#include "codegen.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "../util/logger.hpp"
#include "../util/overloaded.hpp"

using enum Status;

namespace
{
	// One emitted construct (loop / match / if) that 'break' can target.
	struct MonoTask
	{
		const AST::FunctionDefinition* fd;
		llvm::Function* fn;
		const std::unordered_map<TypeId, TypeId>* bindings;
	};

	struct BreakTarget
	{
		llvm::BasicBlock* exit = nullptr;
		llvm::Value* resultSlot = nullptr;   // non-null when the construct yields a value
		TypeId resultType = INVALID_TYPE_ID;
	};

	struct Codegen
	{
		const Source& source;
		const AST::Module& astModule;
		const ResolvedModule& resolved;
		const InternedTypes& interned;
		const TypedModule& typed;
		const SymbolTable& syms;

		llvm::LLVMContext ctx;
		std::unique_ptr<llvm::Module> mod;
		llvm::IRBuilder<> builder;
		llvm::TargetMachine* targetMachine = nullptr;

		std::unordered_map<TypeId, llvm::Type*> typeCache;
		llvm::StructType* sliceTy = nullptr;       // { ptr, i64 } fat pointer
		llvm::StructType* ifaceTy = nullptr;       // { data ptr, vtable ptr } fat pointer
		llvm::StructType* closureTy = nullptr;     // { fn ptr, env ptr } function value

		std::unordered_map<const AST::FunctionDefinition*, llvm::Function*> fnMap;
		std::unordered_map<std::string, llvm::Function*> externMap;
		// Cache of synthesized `(env, args) -> ret` thunks wrapping a named function
		// for use as a `(T) -> R` value (closure ABI).
		std::unordered_map<const AST::FunctionDefinition*, llvm::Function*> thunks;
		llvm::Function* mallocFn = nullptr;

		// C3: per-interface method slot table (method name → index in vtable struct).
		std::unordered_map<const AST::InterfaceDefinition*,
			std::unordered_map<std::string, int>> ifaceSlots;
		// C3: LLVM struct type of each interface's vtable.
		std::unordered_map<const AST::InterfaceDefinition*, llvm::StructType*> ifaceVtableTy;
		// C3: vtable globals — key = (concreteTypeId << 32) | interfaceTypeId.
		std::unordered_map<uint64_t, llvm::Constant*> vtables;

		// C4: type-parameter bindings active while lowering a monomorphized body
		// (TypeParam TypeId → concrete TypeId). nullptr during normal lowering.
		const std::unordered_map<TypeId, TypeId>* currentMonoBindings = nullptr;
		// C4: per-call-site monomorphized target (overrides the generic FunctionDef).
		std::unordered_map<const AST::FunctionCallExpression*, llvm::Function*> callMonoTarget;
		// C4: per-call-site bindings — used to substitute param types during arg lowering.
		std::unordered_map<const AST::FunctionCallExpression*,
			const std::unordered_map<TypeId, TypeId>*> callMonoBindings;
		// C4: cache of (generic FunctionDefinition + bindings signature) → mono Fn.
		std::unordered_map<std::string, llvm::Function*> monoCache;
		// C4: cache-key → bindings pointer (parallel to monoCache).
		std::unordered_map<std::string, const std::unordered_map<TypeId, TypeId>*> monoBindingsByKey;
		// C4: stable storage so callMonoBindings can point into a binding map.
		std::deque<std::unordered_map<TypeId, TypeId>> monoBindingStorage;
		// C4: mono bodies still pending codegen — drained iteratively from main().
		std::deque<MonoTask> pendingMonoTasks;

		// Local declaration node -> its stack slot (alloca) and declared storage type.
		std::unordered_map<const void*, llvm::Value*> slots;
		std::unordered_map<const void*, TypeId> slotStorage;

		llvm::Function* currentFn = nullptr;
		TypeId currentReturnType = INVALID_TYPE_ID;
		std::vector<BreakTarget> breakStack;

		bool hadError = false;
		int mangleCounter = 0;
		int lambdaCounter = 0;

		Codegen(const Source& src, const AST::Module& m, const ResolvedModule& r,
			const InternedTypes& i, const TypedModule& t, const SymbolTable& s)
			: source(src), astModule(m), resolved(r), interned(i), typed(t), syms(s),
			builder(ctx)
		{
		}

		// --- diagnostics --------------------------------------------------------

		void warn(const std::string& msg)
		{
			DiagnosticEngine::instance().report(Diagnostic{
				Diagnostic::Severity::Warning, false, 0, 0,
				"codegen: " + msg });
		}

		void fail(const std::string& msg)
		{
			hadError = true;
			DiagnosticEngine::instance().report(Diagnostic{
				Diagnostic::Severity::Error, false, 0, 0,
				"codegen: " + msg });
		}

		std::string_view tokenText(const Token& tok) const
		{
			return { &source.data[tok.start.index], tok.end.index - tok.start.index };
		}

		// C4: leaf substitution of a TypeParam id via the active mono bindings.
		TypeId substT(TypeId id) const
		{
			if (currentMonoBindings)
			{
				auto it = currentMonoBindings->find(id);
				if (it != currentMonoBindings->end()) return it->second;
			}
			return id;
		}

		TypeId exprType(const AST::Expression& e) const
		{
			auto it = typed.exprTypes.find(&e);
			TypeId t = it != typed.exprTypes.end() ? it->second : INVALID_TYPE_ID;
			return substT(t);
		}

		// --- type lowering ------------------------------------------------------

		// Strip Named wrappers and resolve the untyped-literal sentinels.
		TypeId canonical(TypeId id) const
		{
			id = substT(id);
			if (id == TYPE_UNTYPED_INT)   return TYPE_I32;
			if (id == TYPE_UNTYPED_FLOAT) return TYPE_F32;
			if (id == INVALID_TYPE_ID || id >= static_cast<TypeId>(interned.table.size()))
				return id;
			TypeId cur = id;
			while (cur < static_cast<TypeId>(interned.table.size())
				&& interned.get(cur).kind == TypeInfo::Kind::Named)
				cur = interned.get(cur).asNamed().underlying;
			return cur;
		}

		bool isFloat(TypeId id) const
		{
			TypeId c = canonical(id);
			if (c == INVALID_TYPE_ID || c >= static_cast<TypeId>(interned.table.size()))
				return false;
			const TypeInfo& info = interned.get(c);
			return info.kind == TypeInfo::Kind::Primitive && info.asPrimitive().isFloat;
		}

		bool isSigned(TypeId id) const
		{
			TypeId c = canonical(id);
			if (c == INVALID_TYPE_ID || c >= static_cast<TypeId>(interned.table.size()))
				return false;
			const TypeInfo& info = interned.get(c);
			return info.kind == TypeInfo::Kind::Primitive && info.asPrimitive().isSigned;
		}

		bool isIndirection(TypeId id) const
		{
			id = substT(id);
			if (id == INVALID_TYPE_ID || id >= static_cast<TypeId>(interned.table.size()))
				return false;
			return interned.get(id).isIndirection();
		}

		llvm::StructType* getSliceTy()
		{
			if (!sliceTy)
				sliceTy = llvm::StructType::create(
					ctx, { builder.getPtrTy(), builder.getInt64Ty() }, "alloy.slice");
			return sliceTy;
		}

		// C3: { data ptr, vtable ptr } interface object.
		llvm::StructType* getIfaceTy()
		{
			if (!ifaceTy)
				ifaceTy = llvm::StructType::create(
					ctx, { builder.getPtrTy(), builder.getPtrTy() }, "alloy.iface");
			return ifaceTy;
		}

		// Closure / function-value layout: { fn ptr, env ptr }.
		llvm::StructType* getClosureTy()
		{
			if (!closureTy)
				closureTy = llvm::StructType::create(
					ctx, { builder.getPtrTy(), builder.getPtrTy() }, "alloy.closure");
			return closureTy;
		}

		llvm::Type* lowerType(TypeId id)
		{
			id = substT(id);                              // C4: substitute leaf TypeParams
			if (id == TYPE_UNTYPED_INT)   return builder.getInt32Ty();
			if (id == TYPE_UNTYPED_FLOAT) return llvm::Type::getFloatTy(ctx);
			if (id == INVALID_TYPE_ID || id >= static_cast<TypeId>(interned.table.size()))
				return builder.getInt32Ty();

			// Skip the cache while monomorphizing — the same TypeId may lower to
			// different LLVM shapes depending on the active bindings.
			if (!currentMonoBindings)
			{
				if (auto it = typeCache.find(id); it != typeCache.end())
					return it->second;
			}

			const TypeInfo& info = interned.get(id);
			llvm::Type* result = nullptr;

			switch (info.kind)
			{
				case TypeInfo::Kind::Primitive:
				{
					const auto& p = info.asPrimitive();
					if (p.isFloat)
						result = (p.byteWidth == 8) ? llvm::Type::getDoubleTy(ctx)
						: llvm::Type::getFloatTy(ctx);
					else if (p.name == "bool")
						result = builder.getInt1Ty();
					else
						result = llvm::Type::getIntNTy(ctx, p.byteWidth * 8);
					break;
				}
				case TypeInfo::Kind::Pointer:
				case TypeInfo::Kind::Reference:
				case TypeInfo::Kind::PtrMut:
				case TypeInfo::Kind::RefMut:
				{
					// C3: '&I'/'*I' (indirection to an interface) is a fat pointer.
					TypeId inner = info.asIndirection().inner;
					while (inner < (TypeId)interned.table.size()
						&& interned.get(inner).kind == TypeInfo::Kind::Named)
						inner = interned.get(inner).asNamed().underlying;
					if (inner < (TypeId)interned.table.size()
						&& interned.get(inner).kind == TypeInfo::Kind::Interface)
						result = getIfaceTy();
					else
						result = builder.getPtrTy();
					break;
				}
				case TypeInfo::Kind::Slice:
					result = getSliceTy();
					break;
				case TypeInfo::Kind::Array:
				{
					const auto& a = info.asArray();
					result = llvm::ArrayType::get(lowerType(a.elem), a.size);
					break;
				}
				case TypeInfo::Kind::Struct:
				{
					auto* st = llvm::StructType::create(ctx, "alloy.struct");
					if (!currentMonoBindings) typeCache[id] = st;   // register before body for self-reference safety
					std::vector<llvm::Type*> fields;
					for (const auto& m : info.asStruct().members)
						fields.push_back(lowerType(m.type));
					st->setBody(fields);
					return st;
				}
				case TypeInfo::Kind::Enum:
				{
					auto* st = llvm::StructType::create(ctx, "alloy.enum");
					if (!currentMonoBindings) typeCache[id] = st;
					st->setBody({ builder.getInt32Ty(),
						llvm::ArrayType::get(builder.getInt8Ty(), enumPayloadSize(info)) });
					return st;
				}
				case TypeInfo::Kind::Named:
					result = lowerType(info.asNamed().underlying);
					break;
				case TypeInfo::Kind::Function:
					// A function value is a closure: { fn ptr, env ptr }.
					result = getClosureTy();
					break;
				case TypeInfo::Kind::Interface:
					// C3: an interface value is { data ptr, vtable ptr }.
					result = getIfaceTy();
					break;
				case TypeInfo::Kind::TypeParam:
					// Not lowered in the core back-end — represent opaquely.
					result = builder.getPtrTy();
					break;
			}

			if (!result)
				result = builder.getInt32Ty();
			if (!currentMonoBindings) typeCache[id] = result;
			return result;
		}

		// Byte size of an enum's payload area = max over variants. Needs DataLayout.
		uint64_t enumPayloadSize(const TypeInfo& enumInfo)
		{
			uint64_t maxBytes = 1;
			for (const auto& v : enumInfo.asEnum().variants)
			{
				if (!v.payloadType.has_value())
					continue;
				llvm::Type* pt = lowerType(*v.payloadType);
				uint64_t sz = mod->getDataLayout().getTypeAllocSize(pt);
				if (sz > maxBytes)
					maxBytes = sz;
			}
			return maxBytes;
		}

		// --- declaration helpers ------------------------------------------------

		// Underlying AST node pointer for a resolved declaration (slot key).
		const void* declKey(const ResolvedDeclaration& d) const
		{
			return std::visit(Overloaded
				{
					[](const Required<AST::VariableDefinitionStatement>& v) -> const void* { return v.ptr(); },
					[](const Required<AST::FunctionParameter>& v) -> const void* { return v.ptr(); },
					[](const Required<AST::Capture>& v) -> const void* { return v.ptr(); },
					[](const auto&) -> const void* { return nullptr; },
				}, d.definition);
		}

		const ResolvedDeclaration* resolveIdent(const AST::IdentifierExpression& ident) const
		{
			auto it = resolved.names.find(&ident);
			if (it == resolved.names.end() || it->second.empty())
				return nullptr;
			return it->second[0];
		}

		// --- entry-block alloca -------------------------------------------------

		llvm::AllocaInst* entryAlloca(llvm::Type* ty, const llvm::Twine& name)
		{
			llvm::BasicBlock& entry = currentFn->getEntryBlock();
			llvm::IRBuilder<> tmp(&entry, entry.begin());
			return tmp.CreateAlloca(ty, nullptr, name);
		}

		bool blockOpen() const
		{
			return builder.GetInsertBlock() && !builder.GetInsertBlock()->getTerminator();
		}
	};

	// =====================================================================
	//  Forward declarations of the recursive lowering routines.
	// =====================================================================

	llvm::Value* genExpr(Codegen& cg, const AST::Expression& e, TypeId ctxType);
	llvm::Value* genAddr(Codegen& cg, const AST::Expression& e);
	void genStatement(Codegen& cg, const AST::Statement& s);
	llvm::Value* lowerLambda(Codegen& cg, const AST::LambdaExpression& lambda);
	llvm::Function* getOrBuildThunk(Codegen& cg, const AST::FunctionDefinition& fd);

	struct MonoEntry { llvm::Function* fn; const std::unordered_map<TypeId, TypeId>* bindings; };
	MonoEntry getOrCreateMono(Codegen& cg, const AST::FunctionDefinition& gen,
		std::unordered_map<TypeId, TypeId>&& bindings);

	// --- C2: raw declared type of an expression (does NOT strip indirection) ---
	// Used to distinguish *[T] dynamic arrays (whose typechecker exprType is Slice)
	// from true &[T] slices, since the two have different LLVM representations:
	// dynamic arrays are raw element pointers with a hidden length prefix at -8,
	// slices are { ptr, i64 } fat-pointer structs.

	TypeId rawTypeOf(Codegen& cg, const AST::Expression& e)
	{
		if (auto* idReq = std::get_if<Required<AST::IdentifierExpression>>(&e))
		{
			const ResolvedDeclaration* d = cg.resolveIdent(idReq->value());
			if (d)
			{
				const void* k = cg.declKey(*d);
				if (k)
				{
					auto it = cg.slotStorage.find(k);
					if (it != cg.slotStorage.end()) return it->second;
				}
				if (auto* p = std::get_if<Required<AST::FunctionParameter>>(&d->definition))
				{
					auto ti = cg.interned.astTypes.find(&p->value().type.value());
					if (ti != cg.interned.astTypes.end()) return ti->second;
				}
				if (auto* v = std::get_if<Required<AST::VariableDefinitionStatement>>(&d->definition))
				{
					if (v->value().type.hasValue())
					{
						auto ti = cg.interned.astTypes.find(&v->value().type.value());
						if (ti != cg.interned.astTypes.end()) return ti->second;
					}
				}
			}
			return cg.exprType(e);
		}
		if (auto* mReq = std::get_if<Required<AST::MemberAccessExpression>>(&e))
		{
			TypeId cur = rawTypeOf(cg, mReq->value().object.value());
			for (int safety = 0; safety < 32; ++safety)
			{
				if (cur == INVALID_TYPE_ID || cur >= (TypeId)cg.interned.table.size()) break;
				const auto& info = cg.interned.get(cur);
				if (info.kind == TypeInfo::Kind::Named) { cur = info.asNamed().underlying; continue; }
				if (info.isIndirection()) { cur = info.asIndirection().inner; continue; }
				break;
			}
			if (cur != INVALID_TYPE_ID && cur < (TypeId)cg.interned.table.size()
				&& cg.interned.get(cur).kind == TypeInfo::Kind::Struct)
			{
				std::string_view fname = cg.tokenText(mReq->value().memberName);
				for (const auto& m : cg.interned.get(cur).asStruct().members)
					if (m.name == fname) return m.type;
			}
		}
		return cg.exprType(e);
	}

	bool isDynArr(const InternedTypes& interned, TypeId t)
	{
		if (t == INVALID_TYPE_ID || t >= (TypeId)interned.table.size()) return false;
		const auto& info = interned.get(t);
		if (info.kind != TypeInfo::Kind::Pointer && info.kind != TypeInfo::Kind::PtrMut)
			return false;
		TypeId inner = info.asIndirection().inner;
		while (inner < (TypeId)interned.table.size()
			&& interned.get(inner).kind == TypeInfo::Kind::Named)
			inner = interned.get(inner).asNamed().underlying;
		return inner < (TypeId)interned.table.size()
			&& interned.get(inner).kind == TypeInfo::Kind::Slice;
	}

	// Element TypeId of a *[T] dynamic array, given its raw Pointer→Slice TypeId.
	TypeId dynArrElemType(const InternedTypes& interned, TypeId t)
	{
		TypeId inner = interned.get(t).asIndirection().inner;
		while (interned.get(inner).kind == TypeInfo::Kind::Named)
			inner = interned.get(inner).asNamed().underlying;
		return interned.get(inner).asSlice().elem;
	}

	// Emit a runtime bounds check: `idx (i64)` must satisfy 0 <= idx < len.
	// On failure jump to a trap block which executes @llvm.trap and an
	// unreachable terminator. Both idx and len are treated as i64 / unsigned.
	// (Idx is sign-extended from whatever width the source expression used,
	// so a negative index falls through to a very large unsigned compare and
	// still trips the check.)
	void emitBoundsCheck(Codegen& cg, llvm::Value* idx, llvm::Value* len)
	{
		if (!idx || !len) return;
		llvm::Function* fn = cg.builder.GetInsertBlock()->getParent();
		llvm::BasicBlock* ok   = llvm::BasicBlock::Create(cg.ctx, "bounds.ok",   fn);
		llvm::BasicBlock* fail = llvm::BasicBlock::Create(cg.ctx, "bounds.fail", fn);
		llvm::Value* idx64 = cg.builder.CreateIntCast(idx, cg.builder.getInt64Ty(), false, "idx.u64");
		llvm::Value* len64 = cg.builder.CreateIntCast(len, cg.builder.getInt64Ty(), false, "len.u64");
		llvm::Value* cmp = cg.builder.CreateICmpULT(idx64, len64, "bounds.cmp");
		cg.builder.CreateCondBr(cmp, ok, fail);
		cg.builder.SetInsertPoint(fail);
		llvm::Function* trap = llvm::Intrinsic::getOrInsertDeclaration(cg.mod.get(), llvm::Intrinsic::trap);
		cg.builder.CreateCall(trap);
		cg.builder.CreateUnreachable();
		cg.builder.SetInsertPoint(ok);
	}

	// --- C3: interface helpers ---------------------------------------------
	// True iff t is &I / *I / &var I / *var I (an indirection to an interface).
	bool isIfaceIndirection(const InternedTypes& interned, TypeId t)
	{
		if (t == INVALID_TYPE_ID || t >= (TypeId)interned.table.size()) return false;
		const auto& info = interned.get(t);
		if (!info.isIndirection()) return false;
		TypeId inner = info.asIndirection().inner;
		while (inner < (TypeId)interned.table.size()
			&& interned.get(inner).kind == TypeInfo::Kind::Named)
			inner = interned.get(inner).asNamed().underlying;
		return inner < (TypeId)interned.table.size()
			&& interned.get(inner).kind == TypeInfo::Kind::Interface;
	}

	// Canonical Interface TypeId reached through an indirection.
	TypeId ifaceInner(const InternedTypes& interned, TypeId t)
	{
		TypeId inner = interned.get(t).asIndirection().inner;
		while (inner < (TypeId)interned.table.size()
			&& interned.get(inner).kind == TypeInfo::Kind::Named)
			inner = interned.get(inner).asNamed().underlying;
		return inner;
	}

	// LLVM function-pointer signature used to call an interface fn through the
	// vtable: (ptr self, declared_params...) -> ret.
	llvm::FunctionType* interfaceFnLLVMType(Codegen& cg, const AST::InterfaceFunction& ifn)
	{
		std::vector<llvm::Type*> params;
		params.push_back(cg.builder.getPtrTy());   // self (data pointer)
		ifn.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto ti = cg.interned.astTypes.find(&p.value().type.value());
				params.push_back(ti != cg.interned.astTypes.end()
					? cg.lowerType(ti->second) : cg.builder.getPtrTy());
			});
		llvm::Type* ret = cg.builder.getVoidTy();
		if (ifn.returnType.hasValue())
		{
			auto ti = cg.interned.astTypes.find(&ifn.returnType.value());
			if (ti != cg.interned.astTypes.end())
				ret = cg.lowerType(ti->second);
		}
		return llvm::FunctionType::get(ret, params, false);
	}

	// Vtable struct shape for an interface: one ptr per declared fn, in declaration order.
	llvm::StructType* getInterfaceVtableTy(Codegen& cg, const AST::InterfaceDefinition* ifd)
	{
		auto it = cg.ifaceVtableTy.find(ifd);
		if (it != cg.ifaceVtableTy.end()) return it->second;

		int n = 0;
		ifd->functions.forEach([&](const Required<AST::InterfaceFunction>&) { ++n; });
		if (n == 0) n = 1;   // avoid an empty struct
		std::vector<llvm::Type*> elems(n, cg.builder.getPtrTy());
		auto* st = llvm::StructType::create(cg.ctx, elems,
			"alloy.vt." + std::string(cg.tokenText(ifd->name)));
		cg.ifaceVtableTy[ifd] = st;

		int idx = 0;
		ifd->functions.forEach([&](const Required<AST::InterfaceFunction>& f)
			{
				cg.ifaceSlots[ifd][std::string(cg.tokenText(f.value().name))] = idx++;
			});
		return st;
	}

	// Find the extension satisfying interface fn `ifn` for concrete type `namedTypeId`.
	// Prefer a type-specific extension (self: &T) over an interface default (self: &I).
	const AST::FunctionDefinition* findSatisfyingExt(Codegen& cg,
		const AST::InterfaceFunction& ifn, TypeId namedTypeId, TypeId interfaceTypeId)
	{
		std::string_view name = cg.tokenText(ifn.name);
		const AST::FunctionDefinition* specificFn = nullptr;
		const AST::FunctionDefinition* defaultFn = nullptr;
		for (const auto* decl : cg.syms.get(name))
		{
			auto* fd = std::get_if<Required<AST::FunctionDefinition>>(&decl->definition);
			if (!fd) continue;
			const AST::Function& fn = fd->value().function.value();
			const auto* paramList = fn.parameters.ptr();
			if (!paramList) continue;
			const auto& firstParam = paramList->item.value();
			if (!firstParam.isSelf) continue;
			auto pit = cg.interned.astTypes.find(&firstParam.type.value());
			if (pit == cg.interned.astTypes.end()) continue;
			TypeId selfVal = pit->second;
			while (selfVal < (TypeId)cg.interned.table.size()
				&& cg.interned.get(selfVal).isIndirection())
				selfVal = cg.interned.get(selfVal).asIndirection().inner;
			if (selfVal == namedTypeId)        specificFn = &fd->value();
			else if (selfVal == interfaceTypeId) defaultFn = &fd->value();
		}
		return specificFn ? specificFn : defaultFn;
	}

	// Returns (or lazily builds) the global vtable constant for (T, I).
	llvm::Constant* getVtable(Codegen& cg, TypeId concreteT, TypeId ifaceI)
	{
		if (concreteT == INVALID_TYPE_ID || ifaceI == INVALID_TYPE_ID) return nullptr;
		uint64_t key = (uint64_t(concreteT) << 32) | uint64_t(ifaceI);
		auto cached = cg.vtables.find(key);
		if (cached != cg.vtables.end()) return cached->second;

		if (ifaceI >= (TypeId)cg.interned.table.size()) return nullptr;
		const auto& ifInfo = cg.interned.get(ifaceI);
		if (ifInfo.kind != TypeInfo::Kind::Interface) return nullptr;
		const AST::InterfaceDefinition* ifd = ifInfo.asInterface().decl;
		if (!ifd) return nullptr;

		llvm::StructType* vtTy = getInterfaceVtableTy(cg, ifd);

		std::vector<llvm::Constant*> entries;
		ifd->functions.forEach([&](const Required<AST::InterfaceFunction>& f)
			{
				const AST::FunctionDefinition* impl =
					findSatisfyingExt(cg, f.value(), concreteT, ifaceI);
				llvm::Constant* fnPtr = nullptr;
				if (impl)
				{
					auto fit = cg.fnMap.find(impl);
					if (fit != cg.fnMap.end()) fnPtr = fit->second;
				}
				if (!fnPtr)
					fnPtr = llvm::ConstantPointerNull::get(cg.builder.getPtrTy());
				entries.push_back(fnPtr);
			});

		while (entries.size() < vtTy->getNumElements())
			entries.push_back(llvm::ConstantPointerNull::get(cg.builder.getPtrTy()));

		auto* init = llvm::ConstantStruct::get(vtTy, entries);
		auto* gv = new llvm::GlobalVariable(*cg.mod, vtTy, true,
			llvm::GlobalValue::PrivateLinkage, init,
			"alloy.vt." + std::to_string(concreteT) + "." + std::to_string(ifaceI));
		cg.vtables[key] = gv;
		return gv;
	}

	// Forward decl needed by makeIfaceArg.
	bool exprIsAddressForm(const AST::Expression& e);

	// Build a { data, vtable } interface fat pointer from an arg expression and the
	// target interface's TypeId.
	llvm::Value* makeIfaceArg(Codegen& cg, const AST::Expression& arg, TypeId ifaceTypeId)
	{
		TypeId concreteT = INVALID_TYPE_ID;
		llvm::Value* dataPtr = nullptr;

		if (exprIsAddressForm(arg))
		{
			auto* un = std::get_if<Required<AST::UnaryExpression>>(&arg);
			dataPtr = genExpr(cg, arg, INVALID_TYPE_ID);
			if (un) concreteT = cg.exprType(un->value().expression.value());
		}
		else
		{
			TypeId raw = rawTypeOf(cg, arg);
			if (raw < (TypeId)cg.interned.table.size()
				&& cg.interned.get(raw).isIndirection())
			{
				dataPtr = genAddr(cg, arg);
				concreteT = cg.interned.get(raw).asIndirection().inner;
			}
			else
			{
				dataPtr = genAddr(cg, arg);
				if (!dataPtr)
				{
					llvm::Value* v = genExpr(cg, arg, raw);
					dataPtr = cg.entryAlloca(v->getType(), "iface.tmp");
					cg.builder.CreateStore(v, dataPtr);
				}
				concreteT = raw;
			}
		}

		llvm::Constant* vt = getVtable(cg, concreteT, ifaceTypeId);
		if (!vt)
		{
			cg.warn("no vtable for concrete→interface coercion");
			vt = llvm::ConstantPointerNull::get(cg.builder.getPtrTy());
		}
		llvm::Value* agg = llvm::UndefValue::get(cg.getIfaceTy());
		agg = cg.builder.CreateInsertValue(agg, dataPtr, { 0 });
		agg = cg.builder.CreateInsertValue(agg, vt, { 1 });
		return agg;
	}

	// --- numeric coercion ----------------------------------------------------

	llvm::Value* coerce(Codegen& cg, llvm::Value* v, TypeId from, TypeId to)
	{
		if (!v || to == INVALID_TYPE_ID)
			return v;
		llvm::Type* dst = cg.lowerType(to);
		llvm::Type* src = v->getType();
		if (src == dst)
			return v;

		const bool dstFloat = dst->isFloatingPointTy();
		const bool srcFloat = src->isFloatingPointTy();

		if (srcFloat && dstFloat)
			return cg.builder.CreateFPCast(v, dst, "fpcast");
		if (!srcFloat && dstFloat)
		{
			// integer (often an untyped literal) widening into a float target
			return cg.isSigned(from)
				? cg.builder.CreateSIToFP(v, dst, "sitofp")
				: cg.builder.CreateUIToFP(v, dst, "uitofp");
		}
		if (srcFloat && !dstFloat)
			return cg.isSigned(to)
			? cg.builder.CreateFPToSI(v, dst, "fptosi")
			: cg.builder.CreateFPToUI(v, dst, "fptoui");

		// integer <-> integer
		if (src->isIntegerTy() && dst->isIntegerTy())
			return cg.builder.CreateIntCast(v, dst, cg.isSigned(from), "icast");

		return v;
	}

	// =====================================================================
	//  Enum-variant classification (mirrors the type checker's resolveEnumVariant)
	// =====================================================================

	struct VariantInfo
	{
		bool isVariant = false;
		TypeId enumTypeId = INVALID_TYPE_ID;     // the Named enum TypeId
		TypeId enumUnderlying = INVALID_TYPE_ID;  // the Enum TypeId
		int tag = -1;
		bool hasPayload = false;
		TypeId payloadType = INVALID_TYPE_ID;
	};

	VariantInfo classifyVariant(Codegen& cg, const AST::IdentifierExpression& ident)
	{
		VariantInfo vi;
		const AST::ListNode<const Token*>* first = ident.path.ptr();
		if (!first || !first->next.hasValue())
			return vi;   // not a qualified path

		const ResolvedDeclaration* decl = cg.resolveIdent(ident);
		if (!decl)
			return vi;
		auto* td = std::get_if<Required<AST::TypeDefinition>>(&decl->definition);
		if (!td)
			return vi;

		auto namedIt = cg.interned.namedTypeIds.find(&td->value().name);
		if (namedIt == cg.interned.namedTypeIds.end())
			return vi;

		TypeId underlying = cg.canonical(namedIt->second);
		if (underlying == INVALID_TYPE_ID || underlying >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(underlying).kind != TypeInfo::Kind::Enum)
			return vi;

		const AST::ListNode<const Token*>* node = first;
		while (node->next.hasValue())
			node = node->next.ptr();
		std::string_view variantName = cg.tokenText(*node->item.value());

		const auto& variants = cg.interned.get(underlying).asEnum().variants;
		for (size_t i = 0; i < variants.size(); ++i)
		{
			if (variants[i].name == variantName)
			{
				vi.isVariant = true;
				vi.enumTypeId = namedIt->second;
				vi.enumUnderlying = underlying;
				vi.tag = static_cast<int>(i);
				vi.hasPayload = variants[i].payloadType.has_value();
				if (vi.hasPayload)
					vi.payloadType = *variants[i].payloadType;
				break;
			}
		}
		return vi;
	}

	// Build an enum aggregate value: tag + (optional) payload.
	llvm::Value* buildEnum(Codegen& cg, const VariantInfo& vi, llvm::Value* payload)
	{
		llvm::Type* enumTy = cg.lowerType(vi.enumUnderlying);
		llvm::Value* slot = cg.entryAlloca(enumTy, "enum.tmp");

		llvm::Value* tagPtr = cg.builder.CreateStructGEP(enumTy, slot, 0, "enum.tag");
		cg.builder.CreateStore(cg.builder.getInt32(static_cast<uint32_t>(vi.tag)), tagPtr);

		if (payload)
		{
			llvm::Value* payPtr = cg.builder.CreateStructGEP(enumTy, slot, 1, "enum.pay");
			cg.builder.CreateStore(payload, payPtr);
		}
		return cg.builder.CreateLoad(enumTy, slot, "enum.val");
	}

	// =====================================================================
	//  Literals
	// =====================================================================

	uint64_t decodeCharLiteral(std::string_view text)
	{
		if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'')
			text = text.substr(1, text.size() - 2);
		uint64_t value = 0;
		int shift = 0;
		for (size_t i = 0; i < text.size() && shift < 64; )
		{
			uint8_t byte;
			if (text[i] == '\\' && i + 1 < text.size())
			{
				char e = text[i + 1];
				switch (e)
				{
					case 'n': byte = '\n'; i += 2; break;
					case 'r': byte = '\r'; i += 2; break;
					case 't': byte = '\t'; i += 2; break;
					case '0': byte = 0;    i += 2; break;
					case '\\': byte = '\\'; i += 2; break;
					case '\'': byte = '\''; i += 2; break;
					case '"': byte = '"';  i += 2; break;
					case 'x': case 'X':
					{
						i += 2;
						uint32_t v = 0;
						for (int k = 0; k < 2 && i < text.size(); ++k, ++i)
						{
							char c = text[i];
							v = v * 16 + (c >= '0' && c <= '9' ? c - '0'
								: c >= 'a' && c <= 'f' ? c - 'a' + 10
								: c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
						}
						byte = static_cast<uint8_t>(v);
						break;
					}
					default: byte = static_cast<uint8_t>(e); i += 2; break;
				}
			}
			else
			{
				byte = static_cast<uint8_t>(text[i]);
				++i;
			}
			value |= static_cast<uint64_t>(byte) << shift;
			shift += 8;
		}
		return value;
	}

	std::string decodeStringLiteral(std::string_view text)
	{
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
			text = text.substr(1, text.size() - 2);
		std::string out;
		for (size_t i = 0; i < text.size(); )
		{
			if (text[i] == '\\' && i + 1 < text.size())
			{
				char e = text[i + 1];
				switch (e)
				{
					case 'n': out.push_back('\n'); i += 2; break;
					case 'r': out.push_back('\r'); i += 2; break;
					case 't': out.push_back('\t'); i += 2; break;
					case '0': out.push_back('\0'); i += 2; break;
					case '\\': out.push_back('\\'); i += 2; break;
					case '\'': out.push_back('\''); i += 2; break;
					case '"': out.push_back('"');  i += 2; break;
					case 'x': case 'X':
					{
						i += 2;
						uint32_t v = 0;
						for (int k = 0; k < 2 && i < text.size(); ++k, ++i)
						{
							char c = text[i];
							v = v * 16 + (c >= '0' && c <= '9' ? c - '0'
								: c >= 'a' && c <= 'f' ? c - 'a' + 10
								: c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
						}
						out.push_back(static_cast<char>(v));
						break;
					}
					case 'u': case 'U':
					{
						i += 2;
						uint32_t cp = 0;
						if (i < text.size() && text[i] == '{')
						{
							++i;
							while (i < text.size() && text[i] != '}')
							{
								char c = text[i++];
								cp = cp * 16 + (c >= '0' && c <= '9' ? c - '0'
									: c >= 'a' && c <= 'f' ? c - 'a' + 10
									: c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
							}
							if (i < text.size()) ++i;
						}
						// encode as UTF-8
						if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
						else if (cp <= 0x7FF)
						{
							out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
							out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
						}
						else if (cp <= 0xFFFF)
						{
							out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
							out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
							out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
						}
						else
						{
							out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
							out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
							out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
							out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
						}
						break;
					}
					default: out.push_back(e); i += 2; break;
				}
			}
			else
			{
				out.push_back(text[i]);
				++i;
			}
		}
		return out;
	}

	uint64_t parseIntLiteral(std::string_view text)
	{
		uint64_t value = 0;
		if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		{
			for (size_t i = 2; i < text.size(); ++i)
			{
				char c = text[i];
				value = value * 16 + (c >= '0' && c <= '9' ? c - '0'
					: c >= 'a' && c <= 'f' ? c - 'a' + 10
					: c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
			}
		}
		else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
		{
			for (size_t i = 2; i < text.size(); ++i)
				value = value * 2 + (text[i] - '0');
		}
		else if (text.size() > 2 && text[0] == '0' && (text[1] == 'o' || text[1] == 'O'))
		{
			for (size_t i = 2; i < text.size(); ++i)
				value = value * 8 + (text[i] - '0');
		}
		else
		{
			for (char c : text)
				if (c >= '0' && c <= '9')
					value = value * 10 + (c - '0');
		}
		return value;
	}

	llvm::Value* genLiteral(Codegen& cg, const AST::LiteralExpression& lit, TypeId ctxType)
	{
		const Token& tok = lit.value;
		std::string_view text = cg.tokenText(tok);
		TypeId target = cg.canonical(ctxType);

		switch (tok.kind)
		{
			case TokenKind::IntegerLiteral:
			{
				uint64_t v = parseIntLiteral(text);
				if (cg.isFloat(ctxType))
					return llvm::ConstantFP::get(cg.lowerType(ctxType), static_cast<double>(v));
				llvm::Type* ity = (target != INVALID_TYPE_ID
					&& target < static_cast<TypeId>(cg.interned.table.size())
					&& cg.interned.get(target).kind == TypeInfo::Kind::Primitive
					&& !cg.interned.get(target).asPrimitive().isFloat)
					? cg.lowerType(ctxType) : cg.builder.getInt32Ty();
				return llvm::ConstantInt::get(ity, v);
			}
			case TokenKind::FloatLiteral:
			{
				double d = 0.0;
				try { d = std::stod(std::string(text)); }
				catch (...) { d = 0.0; }
				llvm::Type* fty = cg.isFloat(ctxType) ? cg.lowerType(ctxType)
					: llvm::Type::getFloatTy(cg.ctx);
				return llvm::ConstantFP::get(fty, d);
			}
			case TokenKind::True:  return cg.builder.getInt1(true);
			case TokenKind::False: return cg.builder.getInt1(false);
			case TokenKind::CharLiteral:
			{
				uint64_t v = decodeCharLiteral(text);
				llvm::Type* ity = cg.builder.getInt32Ty();
				if (target != INVALID_TYPE_ID && target < static_cast<TypeId>(cg.interned.table.size())
					&& cg.interned.get(target).kind == TypeInfo::Kind::Primitive)
					ity = cg.lowerType(ctxType);
				return llvm::ConstantInt::get(ity, v);
			}
			case TokenKind::StringLiteral:
			{
				std::string bytes = decodeStringLiteral(text);
				llvm::Constant* strPtr = cg.builder.CreateGlobalString(bytes, "str");
				// If a slice is expected, build a { ptr, len } fat pointer.
				TypeId c = cg.canonical(ctxType);
				bool wantSlice = c != INVALID_TYPE_ID
					&& c < static_cast<TypeId>(cg.interned.table.size())
					&& cg.interned.get(c).kind == TypeInfo::Kind::Slice;
				if (wantSlice)
				{
					llvm::Value* agg = llvm::UndefValue::get(cg.getSliceTy());
					agg = cg.builder.CreateInsertValue(agg, strPtr, { 0 });
					agg = cg.builder.CreateInsertValue(
						agg, cg.builder.getInt64(bytes.size()), { 1 });
					return agg;
				}
				return strPtr;
			}
			default:
				cg.warn("unsupported literal kind");
				return cg.builder.getInt32(0);
		}
	}

	// =====================================================================
	//  Address-of (lvalue) lowering
	// =====================================================================

	llvm::Value* genAddr(Codegen& cg, const AST::Expression& e)
	{
		if (auto* idReq = std::get_if<Required<AST::IdentifierExpression>>(&e))
		{
			const ResolvedDeclaration* decl = cg.resolveIdent(idReq->value());
			if (!decl)
				return nullptr;
			const void* key = cg.declKey(*decl);
			auto it = cg.slots.find(key);
			if (it == cg.slots.end())
				return nullptr;
			llvm::Value* slot = it->second;
			// A reference/pointer binding stores a pointer; its lvalue is the pointee.
			TypeId storage = cg.slotStorage.count(key) ? cg.slotStorage[key] : INVALID_TYPE_ID;
			// A lambda capture's slot lives inside the env struct. For a by-ref or
			// by-ptr capture (modifier carries `&`/`&var`/`*`/`*var`) the env field
			// holds a pointer to the outer storage — deref it to reach the lvalue.
			if (auto* capReq = std::get_if<Required<AST::Capture>>(&decl->definition))
			{
				auto m = capReq->value().modifier;
				if (m == AST::Type::Modifier::Reference
					|| m == AST::Type::Modifier::ReferenceToMutable
					|| m == AST::Type::Modifier::Pointer
					|| m == AST::Type::Modifier::PointerToMutable)
					return cg.builder.CreateLoad(cg.builder.getPtrTy(), slot, "cap.deref");
				return slot;
			}
			// C3: an interface-typed binding stores the { data, vtable } fat pointer
			// inline in its slot — no extra deref to reach it.
			if (isIfaceIndirection(cg.interned, storage))
				return slot;
			if (cg.isIndirection(storage))
				return cg.builder.CreateLoad(cg.builder.getPtrTy(), slot, "deref");
			return slot;
		}

		if (auto* memReq = std::get_if<Required<AST::MemberAccessExpression>>(&e))
		{
			const AST::MemberAccessExpression& m = memReq->value();
			llvm::Value* base = genAddr(cg, m.object.value());
			if (!base)
				return nullptr;
			TypeId objTy = cg.canonical(cg.exprType(m.object.value()));
			if (objTy == INVALID_TYPE_ID || objTy >= static_cast<TypeId>(cg.interned.table.size())
				|| cg.interned.get(objTy).kind != TypeInfo::Kind::Struct)
				return nullptr;
			std::string_view field = cg.tokenText(m.memberName);
			const auto& members = cg.interned.get(objTy).asStruct().members;
			for (size_t i = 0; i < members.size(); ++i)
				if (members[i].name == field)
					return cg.builder.CreateStructGEP(cg.lowerType(objTy), base,
						static_cast<unsigned>(i), "field");
			return nullptr;
		}

		if (auto* arrReq = std::get_if<Required<AST::ArrayAccessExpression>>(&e))
		{
			const AST::ArrayAccessExpression& a = arrReq->value();
			TypeId objTy = cg.canonical(cg.exprType(a.object.value()));
			llvm::Value* idx = genExpr(cg, a.index.value(), TYPE_I64);
			idx = cg.builder.CreateIntCast(idx, cg.builder.getInt64Ty(), true, "idx");

			// C2: *[T] dynamic array — index directly off the user-facing pointer.
			// Length is stored as i64 at user_ptr - 8 (see genUnary 'new').
			TypeId rawObj = rawTypeOf(cg, a.object.value());
			if (isDynArr(cg.interned, rawObj))
			{
				llvm::Value* dataPtr = genAddr(cg, a.object.value());
				// Debug bounds check.
				llvm::Value* lenPtr = cg.builder.CreateGEP(cg.builder.getInt8Ty(),
					dataPtr, { cg.builder.getInt64(-8) }, "dyn.len.ptr");
				llvm::Value* len = cg.builder.CreateLoad(cg.builder.getInt64Ty(),
					lenPtr, "dyn.len");
				emitBoundsCheck(cg, idx, len);
				TypeId elem = dynArrElemType(cg.interned, rawObj);
				return cg.builder.CreateGEP(cg.lowerType(elem), dataPtr,
					{ idx }, "dyn.elem");
			}

			if (objTy != INVALID_TYPE_ID && objTy < static_cast<TypeId>(cg.interned.table.size())
				&& cg.interned.get(objTy).kind == TypeInfo::Kind::Slice)
			{
				// slice: load the data pointer, then index it
				llvm::Value* sliceAddr = genAddr(cg, a.object.value());
				if (!sliceAddr) return nullptr;
				llvm::Value* dataPtr = cg.builder.CreateLoad(cg.builder.getPtrTy(),
					cg.builder.CreateStructGEP(cg.getSliceTy(), sliceAddr, 0, "slice.ptr"));
				// Debug bounds check using the slice length field.
				llvm::Value* lenPtr = cg.builder.CreateStructGEP(cg.getSliceTy(),
					sliceAddr, 1, "slice.len.ptr");
				llvm::Value* len = cg.builder.CreateLoad(cg.builder.getInt64Ty(),
					lenPtr, "slice.len");
				emitBoundsCheck(cg, idx, len);
				TypeId elem = cg.interned.get(objTy).asSlice().elem;
				return cg.builder.CreateGEP(cg.lowerType(elem), dataPtr, { idx }, "elem");
			}

			llvm::Value* base = genAddr(cg, a.object.value());
			if (!base) return nullptr;
			if (objTy != INVALID_TYPE_ID && objTy < static_cast<TypeId>(cg.interned.table.size())
				&& cg.interned.get(objTy).kind == TypeInfo::Kind::Array)
			{
				size_t sz = cg.interned.get(objTy).asArray().size;
				emitBoundsCheck(cg, idx, cg.builder.getInt64(sz));
				return cg.builder.CreateGEP(cg.lowerType(objTy), base,
					{ cg.builder.getInt64(0), idx }, "elem");
			}
			return nullptr;
		}

		if (auto* unReq = std::get_if<Required<AST::UnaryExpression>>(&e))
		{
			// &x as an lvalue source: the address is x's address itself.
			if (unReq->value().op == TokenKind::BitwiseAnd)
				return genAddr(cg, unReq->value().expression.value());
		}

		return nullptr;
	}

	// =====================================================================
	//  Calls
	// =====================================================================

	llvm::Function* lookupCallee(Codegen& cg, const AST::FunctionCallExpression& call)
	{
		// C4: a generic call resolves to its monomorphized instance.
		auto mit = cg.callMonoTarget.find(&call);
		if (mit != cg.callMonoTarget.end()) return mit->second;

		auto it = cg.typed.selectedOverloads.find(&call);
		if (it == cg.typed.selectedOverloads.end() || !it->second)
			return nullptr;
		const ResolvedDeclaration& decl = *it->second;
		if (auto* fn = std::get_if<Required<AST::FunctionDefinition>>(&decl.definition))
		{
			auto fit = cg.fnMap.find(fn->ptr());
			return fit != cg.fnMap.end() ? fit->second : nullptr;
		}
		if (auto* ext = std::get_if<Required<AST::ExternDefinition>>(&decl.definition))
		{
			auto eit = cg.externMap.find(std::string(cg.tokenText(ext->value().name)));
			return eit != cg.externMap.end() ? eit->second : nullptr;
		}
		return nullptr;
	}

	// Parameter storage types of the selected overload (self included).
	std::vector<TypeId> calleeParamTypes(Codegen& cg, const AST::FunctionCallExpression& call)
	{
		std::vector<TypeId> out;
		auto it = cg.typed.selectedOverloads.find(&call);
		if (it == cg.typed.selectedOverloads.end() || !it->second)
			return out;
		const ResolvedDeclaration& decl = *it->second;
		const AST::Function* fn = nullptr;
		if (auto* f = std::get_if<Required<AST::FunctionDefinition>>(&decl.definition))
			fn = f->value().function.ptr();
		if (!fn)
			return out;
		fn->parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto ti = cg.interned.astTypes.find(&p.value().type.value());
				out.push_back(ti != cg.interned.astTypes.end() ? ti->second : INVALID_TYPE_ID);
			});
		return out;
	}

	bool exprIsAddressForm(const AST::Expression& e)
	{
		if (auto* u = std::get_if<Required<AST::UnaryExpression>>(&e))
		{
			TokenKind op = u->value().op;
			return op == TokenKind::BitwiseAnd || op == TokenKind::New || op == TokenKind::Move;
		}
		return false;
	}

	// Lower one call argument against the parameter's storage type.
	llvm::Value* genArg(Codegen& cg, const AST::Expression& arg, TypeId paramType)
	{
		// C3: concrete→interface coercion when the parameter is '&I'/'*I'.
		if (isIfaceIndirection(cg.interned, paramType))
			return makeIfaceArg(cg, arg, ifaceInner(cg.interned, paramType));

		if (cg.isIndirection(paramType))
		{
			if (exprIsAddressForm(arg))
				return genExpr(cg, arg, paramType);
			llvm::Value* addr = genAddr(cg, arg);
			if (addr)
				return addr;
			// No addressable form — materialise a temporary whose type matches the
			// parameter's pointee, so a literal argument (e.g. '10' bound to a &u64
			// param via T=u64) is emitted at the right width.
			TypeId paramInner = cg.interned.get(paramType).asIndirection().inner;
			TypeId vt = cg.exprType(arg);
			llvm::Value* v = genExpr(cg, arg, paramInner);
			v = coerce(cg, v, vt, paramInner);
			llvm::Value* tmp = cg.entryAlloca(cg.lowerType(paramInner), "argtmp");
			cg.builder.CreateStore(v, tmp);
			return tmp;
		}
		TypeId vt = cg.exprType(arg);
		llvm::Value* v = genExpr(cg, arg, paramType);
		return coerce(cg, v, vt, paramType);
	}

	llvm::Value* genBuiltinMethodCall(Codegen& cg, const AST::MemberAccessExpression& member,
		TypeId resultType)
	{
		std::string_view name = cg.tokenText(member.memberName);
		TypeId recvTy = cg.canonical(cg.exprType(member.object.value()));

		if (name == "length")
		{
			// C2: *[T] dynamic array — length lives 8 bytes before the user pointer.
			TypeId rawRecv = rawTypeOf(cg, member.object.value());
			if (isDynArr(cg.interned, rawRecv))
			{
				llvm::Value* dataPtr = genExpr(cg, member.object.value(), rawRecv);
				llvm::Value* lenPtr = cg.builder.CreateGEP(cg.builder.getInt8Ty(), dataPtr,
					{ cg.builder.getInt64(-8) }, "dynarr.lenptr");
				return cg.builder.CreateLoad(cg.builder.getInt64Ty(), lenPtr, "dynarr.len");
			}

			if (recvTy != INVALID_TYPE_ID && recvTy < static_cast<TypeId>(cg.interned.table.size()))
			{
				const TypeInfo& info = cg.interned.get(recvTy);
				if (info.kind == TypeInfo::Kind::Array)
					return cg.builder.getInt64(info.asArray().size);
				if (info.kind == TypeInfo::Kind::Slice)
				{
					llvm::Value* addr = genAddr(cg, member.object.value());
					if (addr)
						return cg.builder.CreateLoad(cg.builder.getInt64Ty(),
							cg.builder.CreateStructGEP(cg.getSliceTy(), addr, 1, "slice.len"));
				}
			}
			return cg.builder.getInt64(0);
		}

		if (name == "convert" || name == "reinterpret")
		{
			llvm::Value* src = genExpr(cg, member.object.value(),
				cg.exprType(member.object.value()));
			if (name == "convert")
				return coerce(cg, src, cg.exprType(member.object.value()), resultType);
			// reinterpret: bitcast through memory
			llvm::Value* tmp = cg.entryAlloca(src->getType(), "reinterp");
			cg.builder.CreateStore(src, tmp);
			return cg.builder.CreateLoad(cg.lowerType(resultType), tmp, "reinterp.v");
		}
		return nullptr;
	}

	llvm::Value* genCall(Codegen& cg, const AST::FunctionCallExpression& call, const AST::Expression& self)
	{
		const AST::Expression& callee = call.function.value();

		// Enum-variant construction: Enum::Variant( payload ).
		if (auto* idReq = std::get_if<Required<AST::IdentifierExpression>>(&callee))
		{
			VariantInfo vi = classifyVariant(cg, idReq->value());
			if (vi.isVariant)
			{
				llvm::Value* payload = nullptr;
				if (vi.hasPayload && call.arguments.hasValue())
					payload = genExpr(cg, call.arguments.value().item.value(), vi.payloadType);
				return buildEnum(cg, vi, payload);
			}
		}

		// Builtin method calls (length / convert / reinterpret).
		if (auto* memReq = std::get_if<Required<AST::MemberAccessExpression>>(&callee))
		{
			std::string_view mname = cg.tokenText(memReq->value().memberName);
			if (mname == "length" || mname == "convert" || mname == "reinterpret")
			{
				if (llvm::Value* r = genBuiltinMethodCall(cg, memReq->value(), cg.exprType(self)))
					return r;
			}

			// C3: dynamic dispatch through an interface object.
			TypeId recvTy = cg.canonical(cg.exprType(memReq->value().object.value()));
			if (recvTy < (TypeId)cg.interned.table.size()
				&& cg.interned.get(recvTy).kind == TypeInfo::Kind::Interface)
			{
				const AST::InterfaceDefinition* ifd = cg.interned.get(recvTy).asInterface().decl;
				if (ifd)
				{
					llvm::StructType* vtTy = getInterfaceVtableTy(cg, ifd);   // also fills slots
					std::string method(mname);
					auto& slots = cg.ifaceSlots[ifd];
					auto sit = slots.find(method);
					const AST::InterfaceFunction* ifn = nullptr;
					ifd->functions.forEach([&](const Required<AST::InterfaceFunction>& f)
						{
							if (cg.tokenText(f.value().name) == mname) ifn = &f.value();
						});
					if (sit == slots.end() || !ifn)
					{
						cg.warn("interface method '" + method + "' not found");
						return llvm::UndefValue::get(cg.lowerType(cg.exprType(self)));
					}

					llvm::Value* receiverVal = genExpr(cg, memReq->value().object.value(),
						cg.exprType(memReq->value().object.value()));
					llvm::Value* dataPtr = cg.builder.CreateExtractValue(receiverVal,
						{ 0 }, "iface.data");
					llvm::Value* vtPtr = cg.builder.CreateExtractValue(receiverVal,
						{ 1 }, "iface.vt");
					llvm::Value* slotAddr = cg.builder.CreateStructGEP(vtTy, vtPtr,
						static_cast<unsigned>(sit->second), "iface.slot");
					llvm::Value* fnPtr = cg.builder.CreateLoad(cg.builder.getPtrTy(),
						slotAddr, "iface.fn");

					llvm::FunctionType* fty = interfaceFnLLVMType(cg, *ifn);

					std::vector<TypeId> declParamTypes;
					ifn->parameters.forEach([&](const Required<AST::FunctionParameter>& p)
						{
							auto ti = cg.interned.astTypes.find(&p.value().type.value());
							declParamTypes.push_back(ti != cg.interned.astTypes.end()
								? ti->second : INVALID_TYPE_ID);
						});
					std::vector<llvm::Value*> args;
					args.push_back(dataPtr);
					size_t i = 0;
					call.arguments.forEach([&](const Required<AST::Expression>& a)
						{
							TypeId pt = i < declParamTypes.size() ? declParamTypes[i] : INVALID_TYPE_ID;
							args.push_back(genArg(cg, a.value(), pt));
							++i;
						});

					llvm::Value* result = cg.builder.CreateCall(fty, fnPtr, args);
					if (fty->getReturnType()->isVoidTy())
						return llvm::UndefValue::get(cg.builder.getInt32Ty());
					return result;
				}
			}
		}

		llvm::Function* fn = lookupCallee(cg, call);
		if (!fn)
		{
			// Indirect call: callee value is a function pointer (lambda value or
			// a variable/param whose type is a Function). The typechecker doesn't
			// stash exprType for identifier callees, so consult rawTypeOf, which
			// walks back to the declaration's storage type.
			TypeId calleeTy = rawTypeOf(cg, callee);
			TypeId canon = cg.canonical(calleeTy);
			if (canon != INVALID_TYPE_ID && canon < (TypeId)cg.interned.table.size()
				&& cg.interned.get(canon).kind == TypeInfo::Kind::Function)
			{
				const auto& fd = cg.interned.get(canon).asFunction();
				// Closures always take a hidden env pointer as the first arg.
				std::vector<llvm::Type*> ptys;
				ptys.push_back(cg.builder.getPtrTy());
				for (TypeId p : fd.params) ptys.push_back(cg.lowerType(p));
				llvm::Type* rty = fd.ret.has_value()
					? cg.lowerType(*fd.ret) : cg.builder.getVoidTy();
				llvm::FunctionType* fty = llvm::FunctionType::get(rty, ptys, false);

				llvm::Value* closure = genExpr(cg, callee, calleeTy);
				llvm::Value* fnPtr = cg.builder.CreateExtractValue(closure, { 0 }, "cl.fn");
				llvm::Value* envPtr = cg.builder.CreateExtractValue(closure, { 1 }, "cl.env");

				std::vector<llvm::Value*> args;
				args.push_back(envPtr);
				size_t i = 0;
				call.arguments.forEach([&](const Required<AST::Expression>& a)
					{
						TypeId pt = i < fd.params.size() ? fd.params[i] : INVALID_TYPE_ID;
						args.push_back(genArg(cg, a.value(), pt));
						++i;
					});
				llvm::Value* r = cg.builder.CreateCall(fty, fnPtr, args);
				if (rty->isVoidTy())
					return llvm::UndefValue::get(cg.builder.getInt32Ty());
				return r;
			}

			cg.warn("call target not resolved (generic, lambda or interface dispatch) — skipped");
			TypeId rt = cg.exprType(self);
			llvm::Type* rty = cg.lowerType(rt);
			return llvm::UndefValue::get(rty);
		}

		// C4: when targeting a monomorphized instance, activate that call's bindings
		// so param TypeParams substitute correctly during arg lowering. For a nested
		// generic call (a generic body calling another generic) the recorded callee
		// bindings may reference the CALLER's TypeParams — resolve them through the
		// active mono context and spawn / reuse a fully-concrete mono instance.
		const std::unordered_map<TypeId, TypeId>* prevBindings = cg.currentMonoBindings;
		auto mbIt = cg.callMonoBindings.find(&call);
		if (mbIt != cg.callMonoBindings.end())
		{
			const auto* bindingsForCall = mbIt->second;
			if (prevBindings && !prevBindings->empty())
			{
				// Walk callee binding values through caller's mono chain.
				std::unordered_map<TypeId, TypeId> resolved;
				bool changed = false;
				for (const auto& kv : *mbIt->second)
				{
					TypeId v = kv.second;
					for (int safety = 0; safety < 16; ++safety)
					{
						auto pit = prevBindings->find(v);
						if (pit == prevBindings->end() || pit->second == v) break;
						v = pit->second;
						changed = true;
					}
					resolved[kv.first] = v;
				}
				if (changed)
				{
					auto selIt = cg.typed.selectedOverloads.find(&call);
					if (selIt != cg.typed.selectedOverloads.end() && selIt->second)
					{
						auto* fdReq = std::get_if<Required<AST::FunctionDefinition>>(
							&selIt->second->definition);
						if (fdReq)
						{
							MonoEntry e = getOrCreateMono(cg, fdReq->value(), std::move(resolved));
							fn = e.fn;
							bindingsForCall = e.bindings;
						}
					}
				}
				// Merge caller + callee bindings so arg expressions referencing the
				// caller's TypeParams (typed against the caller's body) still
				// substitute correctly during arg lowering.
				std::unordered_map<TypeId, TypeId> merged = *prevBindings;
				for (const auto& kv : *bindingsForCall) merged[kv.first] = kv.second;
				cg.monoBindingStorage.push_back(std::move(merged));
				cg.currentMonoBindings = &cg.monoBindingStorage.back();
			}
			else
			{
				cg.currentMonoBindings = bindingsForCall;
			}
		}

		std::vector<TypeId> paramTypes = calleeParamTypes(cg, call);
		std::vector<llvm::Value*> args;
		size_t paramIdx = 0;

		// Member call: the receiver is the implicit first ('self') argument.
		if (auto* memReq = std::get_if<Required<AST::MemberAccessExpression>>(&callee))
		{
			TypeId selfParam = paramIdx < paramTypes.size() ? paramTypes[paramIdx] : INVALID_TYPE_ID;
			args.push_back(genArg(cg, memReq->value().object.value(), selfParam));
			++paramIdx;
		}

		const bool isExtern = fn->isVarArg() || paramTypes.empty();
		call.arguments.forEach([&](const Required<AST::Expression>& a)
			{
				TypeId pt = paramIdx < paramTypes.size() ? paramTypes[paramIdx] : INVALID_TYPE_ID;
				if (pt == INVALID_TYPE_ID)
				{
					// extern / variadic argument — apply C default promotions
					llvm::Value* v = genExpr(cg, a.value(), INVALID_TYPE_ID);
					if (v && v->getType()->isFloatTy())
						v = cg.builder.CreateFPExt(v, llvm::Type::getDoubleTy(cg.ctx), "vararg.d");
					else if (v && v->getType()->isIntegerTy()
						&& v->getType()->getIntegerBitWidth() < 32)
						v = cg.builder.CreateIntCast(v, cg.builder.getInt32Ty(),
							cg.isSigned(cg.exprType(a.value())), "vararg.i");
					args.push_back(v);
				}
				else
				{
					args.push_back(genArg(cg, a.value(), pt));
				}
				++paramIdx;
			});
		(void)isExtern;

		llvm::Value* result = cg.builder.CreateCall(fn, args);

		// C4: restore caller's binding context.
		cg.currentMonoBindings = prevBindings;

		if (fn->getReturnType()->isVoidTy())
			return llvm::UndefValue::get(cg.builder.getInt32Ty());
		return result;
	}

	// =====================================================================
	//  Aggregate construction
	// =====================================================================

	llvm::Value* genArrayLiteral(Codegen& cg, const AST::ArrayLiteralExpression& lit,
		TypeId ctxType, TypeId resultType)
	{
		TypeId arrTy = cg.canonical(ctxType);
		if (arrTy == INVALID_TYPE_ID || arrTy >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(arrTy).kind != TypeInfo::Kind::Array)
		{
			arrTy = cg.canonical(resultType);
		}
		if (arrTy == INVALID_TYPE_ID || arrTy >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(arrTy).kind != TypeInfo::Kind::Array)
		{
			cg.warn("array literal without an inferable type — skipped");
			return cg.builder.getInt32(0);
		}

		TypeId elemTy = cg.interned.get(arrTy).asArray().elem;
		llvm::Type* llArr = cg.lowerType(arrTy);
		llvm::Value* slot = cg.entryAlloca(llArr, "arr.tmp");

		unsigned idx = 0;
		lit.elements.forEach([&](const Required<AST::Expression>& el)
			{
				llvm::Value* v = genExpr(cg, el.value(), elemTy);
				v = coerce(cg, v, cg.exprType(el.value()), elemTy);
				llvm::Value* ep = cg.builder.CreateGEP(llArr, slot,
					{ cg.builder.getInt64(0), cg.builder.getInt64(idx) }, "arr.el");
				cg.builder.CreateStore(v, ep);
				++idx;
			});
		return cg.builder.CreateLoad(llArr, slot, "arr.val");
	}

	llvm::Value* genArrayFill(Codegen& cg, const AST::ArrayFillExpression& fill,
		TypeId ctxType, TypeId resultType)
	{
		TypeId arrTy = cg.canonical(ctxType);
		if (arrTy == INVALID_TYPE_ID || arrTy >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(arrTy).kind != TypeInfo::Kind::Array)
			arrTy = cg.canonical(resultType);
		if (arrTy == INVALID_TYPE_ID || arrTy >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(arrTy).kind != TypeInfo::Kind::Array)
		{
			cg.warn("array-fill without an inferable type — skipped");
			return cg.builder.getInt32(0);
		}
		const auto& ad = cg.interned.get(arrTy).asArray();
		llvm::Type* llArr = cg.lowerType(arrTy);
		llvm::Value* slot = cg.entryAlloca(llArr, "fill.tmp");
		llvm::Value* v = genExpr(cg, fill.value.value(), ad.elem);
		v = coerce(cg, v, cg.exprType(fill.value.value()), ad.elem);
		for (size_t i = 0; i < ad.size; ++i)
		{
			llvm::Value* ep = cg.builder.CreateGEP(llArr, slot,
				{ cg.builder.getInt64(0), cg.builder.getInt64(i) }, "fill.el");
			cg.builder.CreateStore(v, ep);
		}
		return cg.builder.CreateLoad(llArr, slot, "fill.val");
	}

	llvm::Value* genStructInit(Codegen& cg, const AST::StructInitializerExpression& init,
		TypeId ctxType, TypeId resultType)
	{
		TypeId structTy = cg.canonical(resultType);
		if (structTy == INVALID_TYPE_ID || structTy >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(structTy).kind != TypeInfo::Kind::Struct)
			structTy = cg.canonical(ctxType);
		if (structTy == INVALID_TYPE_ID || structTy >= static_cast<TypeId>(cg.interned.table.size())
			|| cg.interned.get(structTy).kind != TypeInfo::Kind::Struct)
		{
			cg.warn("struct initializer without an inferable type — skipped");
			return cg.builder.getInt32(0);
		}

		const auto& members = cg.interned.get(structTy).asStruct().members;
		llvm::Type* llStruct = cg.lowerType(structTy);
		llvm::Value* slot = cg.entryAlloca(llStruct, "struct.tmp");

		init.initializers.forEach([&](const Required<AST::StructInitializerExpression::MemberInitializer>& mi)
			{
				std::string_view fname = cg.tokenText(mi.value().name);
				for (size_t i = 0; i < members.size(); ++i)
				{
					if (members[i].name != fname)
						continue;
					llvm::Value* v = genExpr(cg, mi.value().value.value(), members[i].type);
					v = coerce(cg, v, cg.exprType(mi.value().value.value()), members[i].type);
					llvm::Value* fp = cg.builder.CreateStructGEP(llStruct, slot,
						static_cast<unsigned>(i), "init.f");
					cg.builder.CreateStore(v, fp);
					break;
				}
			});
		return cg.builder.CreateLoad(llStruct, slot, "struct.val");
	}

	// =====================================================================
	//  Binary / unary operators
	// =====================================================================

	llvm::Value* genBinary(Codegen& cg, const AST::BinaryExpression& bin, TypeId ctxType)
	{
		using K = TokenKind;
		K op = bin.op;

		// Short-circuit logical operators.
		if (op == K::LogicalAnd || op == K::LogicalOr)
		{
			llvm::Value* l = genExpr(cg, bin.left.value(), TYPE_BOOL);
			l = cg.builder.CreateICmpNE(l, llvm::ConstantInt::get(l->getType(), 0), "tobool");
			llvm::BasicBlock* startBB = cg.builder.GetInsertBlock();
			llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(cg.ctx, "sc.rhs", cg.currentFn);
			llvm::BasicBlock* endBB = llvm::BasicBlock::Create(cg.ctx, "sc.end", cg.currentFn);
			if (op == K::LogicalAnd)
				cg.builder.CreateCondBr(l, rhsBB, endBB);
			else
				cg.builder.CreateCondBr(l, endBB, rhsBB);
			cg.builder.SetInsertPoint(rhsBB);
			llvm::Value* r = genExpr(cg, bin.right.value(), TYPE_BOOL);
			r = cg.builder.CreateICmpNE(r, llvm::ConstantInt::get(r->getType(), 0), "tobool");
			llvm::BasicBlock* rhsEnd = cg.builder.GetInsertBlock();
			cg.builder.CreateBr(endBB);
			cg.builder.SetInsertPoint(endBB);
			llvm::PHINode* phi = cg.builder.CreatePHI(cg.builder.getInt1Ty(), 2, "sc");
			phi->addIncoming(cg.builder.getInt1(op == K::LogicalOr), startBB);
			phi->addIncoming(r, rhsEnd);
			return phi;
		}

		TypeId lt = cg.exprType(bin.left.value());
		TypeId rt = cg.exprType(bin.right.value());
		TypeId common = lt;
		if (cg.isFloat(lt) || cg.isFloat(rt))
			common = cg.isFloat(lt) ? lt : rt;
		else
		{
			// pick the wider integer
			llvm::Type* ltl = cg.lowerType(lt), * rtl = cg.lowerType(rt);
			unsigned lw = ltl->isIntegerTy() ? ltl->getIntegerBitWidth() : 32;
			unsigned rw = rtl->isIntegerTy() ? rtl->getIntegerBitWidth() : 32;
			common = (rw > lw) ? rt : lt;
		}

		llvm::Value* l = genExpr(cg, bin.left.value(), common);
		llvm::Value* r = genExpr(cg, bin.right.value(), common);
		l = coerce(cg, l, lt, common);
		r = coerce(cg, r, rt, common);
		const bool fp = cg.isFloat(common);
		const bool sg = cg.isSigned(common);

		switch (op)
		{
			case K::Plus:     return fp ? cg.builder.CreateFAdd(l, r, "add") : cg.builder.CreateAdd(l, r, "add");
			case K::Minus:    return fp ? cg.builder.CreateFSub(l, r, "sub") : cg.builder.CreateSub(l, r, "sub");
			case K::Multiply: return fp ? cg.builder.CreateFMul(l, r, "mul") : cg.builder.CreateMul(l, r, "mul");
			case K::Divide:   return fp ? cg.builder.CreateFDiv(l, r, "div")
				: sg ? cg.builder.CreateSDiv(l, r, "div") : cg.builder.CreateUDiv(l, r, "div");
			case K::Modulo:   return fp ? cg.builder.CreateFRem(l, r, "rem")
				: sg ? cg.builder.CreateSRem(l, r, "rem") : cg.builder.CreateURem(l, r, "rem");
			case K::BitwiseAnd: return cg.builder.CreateAnd(l, r, "and");
			case K::BitwiseOr:  return cg.builder.CreateOr(l, r, "or");
			case K::Xor:        return cg.builder.CreateXor(l, r, "xor");
			case K::ShiftLeft:  return cg.builder.CreateShl(l, r, "shl");
			case K::ShiftRight: return sg ? cg.builder.CreateAShr(l, r, "shr")
				: cg.builder.CreateLShr(l, r, "shr");
			case K::Equal:    return fp ? cg.builder.CreateFCmpOEQ(l, r, "eq") : cg.builder.CreateICmpEQ(l, r, "eq");
			case K::NotEqual: return fp ? cg.builder.CreateFCmpONE(l, r, "ne") : cg.builder.CreateICmpNE(l, r, "ne");
			case K::Less:     return fp ? cg.builder.CreateFCmpOLT(l, r, "lt")
				: sg ? cg.builder.CreateICmpSLT(l, r, "lt") : cg.builder.CreateICmpULT(l, r, "lt");
			case K::LessEqual: return fp ? cg.builder.CreateFCmpOLE(l, r, "le")
				: sg ? cg.builder.CreateICmpSLE(l, r, "le") : cg.builder.CreateICmpULE(l, r, "le");
			case K::Greater:  return fp ? cg.builder.CreateFCmpOGT(l, r, "gt")
				: sg ? cg.builder.CreateICmpSGT(l, r, "gt") : cg.builder.CreateICmpUGT(l, r, "gt");
			case K::GreaterEqual: return fp ? cg.builder.CreateFCmpOGE(l, r, "ge")
				: sg ? cg.builder.CreateICmpSGE(l, r, "ge") : cg.builder.CreateICmpUGE(l, r, "ge");
			default:
				cg.warn("unsupported binary operator");
				return l;
		}
	}

	llvm::Value* genUnary(Codegen& cg, const AST::UnaryExpression& un, TypeId ctxType)
	{
		using K = TokenKind;
		switch (un.op)
		{
			case K::BitwiseAnd:
			{
				llvm::Value* a = genAddr(cg, un.expression.value());
				if (!a)
				{
					// address-of a temporary
					llvm::Value* v = genExpr(cg, un.expression.value(),
						cg.exprType(un.expression.value()));
					a = cg.entryAlloca(v->getType(), "addr.tmp");
					cg.builder.CreateStore(v, a);
				}
				return a;
			}
			case K::New:
			{
				TypeId inner = cg.exprType(un.expression.value());

				// C2: *[T] dynamic array — length-prefix layout (§4.2). User-facing
				// pointer points at element[0]; a u64 length sits 8 bytes BEFORE it.
				if (isDynArr(cg.interned, ctxType))
				{
					TypeId innerC = cg.canonical(inner);
					if (innerC != INVALID_TYPE_ID && innerC < (TypeId)cg.interned.table.size()
						&& cg.interned.get(innerC).kind == TypeInfo::Kind::Array)
					{
						const auto& ad = cg.interned.get(innerC).asArray();
						uint64_t elemBytes = cg.mod->getDataLayout()
							.getTypeAllocSize(cg.lowerType(ad.elem));
						uint64_t total = 8 + elemBytes * ad.size;
						llvm::Value* base = cg.builder.CreateCall(cg.mallocFn,
							{ cg.builder.getInt64(total) }, "dynarr.mem");
						cg.builder.CreateStore(cg.builder.getInt64(ad.size), base);
						llvm::Value* userPtr = cg.builder.CreateGEP(
							cg.builder.getInt8Ty(), base,
							{ cg.builder.getInt64(8) }, "dynarr.data");
						llvm::Value* arrVal = genExpr(cg, un.expression.value(), inner);
						cg.builder.CreateStore(arrVal, userPtr);
						return userPtr;
					}
				}

				llvm::Type* innerTy = cg.lowerType(inner);
				uint64_t size = cg.mod->getDataLayout().getTypeAllocSize(innerTy);
				llvm::Value* mem = cg.builder.CreateCall(cg.mallocFn,
					{ cg.builder.getInt64(size) }, "heap");
				llvm::Value* v = genExpr(cg, un.expression.value(), inner);
				cg.builder.CreateStore(v, mem);
				return mem;
			}
			case K::Move:
				return genExpr(cg, un.expression.value(), ctxType);
			case K::Minus:
			{
				TypeId t = cg.exprType(un.expression.value());
				llvm::Value* v = genExpr(cg, un.expression.value(), ctxType);
				return cg.isFloat(t) ? cg.builder.CreateFNeg(v, "neg")
					: cg.builder.CreateNeg(v, "neg");
			}
			case K::Not:
			{
				llvm::Value* v = genExpr(cg, un.expression.value(), TYPE_BOOL);
				if (v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() != 1)
					v = cg.builder.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0), "tobool");
				return cg.builder.CreateNot(v, "not");
			}
			case K::BitwiseNot:
			{
				llvm::Value* v = genExpr(cg, un.expression.value(), ctxType);
				return cg.builder.CreateNot(v, "bnot");
			}
			default:
				cg.warn("unsupported unary operator");
				return genExpr(cg, un.expression.value(), ctxType);
		}
	}

	// =====================================================================
	//  Value-yielding constructs (if / while / for / match)
	// =====================================================================

	// `resultSlot` is non-null when the construct is in expression position.
	void genIf(Codegen& cg, const AST::IfExpression& ifx, llvm::Value* resultSlot, TypeId resultType);
	void genWhile(Codegen& cg, const AST::WhileExpression& wx, llvm::Value* resultSlot, TypeId resultType);
	void genFor(Codegen& cg, const AST::ForExpression& fx, llvm::Value* resultSlot, TypeId resultType);
	void genMatch(Codegen& cg, const AST::MatchExpression& mx, llvm::Value* resultSlot, TypeId resultType);

	// =====================================================================
	//  Expression dispatch
	// =====================================================================

	llvm::Value* genExpr(Codegen& cg, const AST::Expression& e, TypeId ctxType)
	{
		if (auto* p = std::get_if<Required<AST::LiteralExpression>>(&e))
			return genLiteral(cg, p->value(), ctxType);

		if (auto* p = std::get_if<Required<AST::IdentifierExpression>>(&e))
		{
			// payload-less enum variant used as a value
			VariantInfo vi = classifyVariant(cg, p->value());
			if (vi.isVariant && !vi.hasPayload)
				return buildEnum(cg, vi, nullptr);

			const ResolvedDeclaration* decl = cg.resolveIdent(p->value());
			if (decl)
			{
				// Named function used as a `(T) -> R` value — synthesise a thunk
				// with the closure ABI `(env, args) -> ret` that ignores env and
				// calls the named fn. Returned as a `{ thunk_ptr, null }` closure.
				if (auto* fd = std::get_if<Required<AST::FunctionDefinition>>(&decl->definition))
				{
					if (llvm::Function* thunk = getOrBuildThunk(cg, fd->value()))
					{
						llvm::Value* cl = llvm::UndefValue::get(cg.getClosureTy());
						cl = cg.builder.CreateInsertValue(cl, thunk, { 0 });
						cl = cg.builder.CreateInsertValue(cl,
							llvm::ConstantPointerNull::get(cg.builder.getPtrTy()), { 1 });
						return cl;
					}
					auto fit = cg.fnMap.find(fd->ptr());
					if (fit != cg.fnMap.end()) return fit->second;
				}
				// C2: a *[T] binding's value is the raw user-facing element pointer,
				// not a slice struct. Bypass the generic value-type load below.
				const void* key = cg.declKey(*decl);
				if (key)
				{
					auto storIt = cg.slotStorage.find(key);
					if (storIt != cg.slotStorage.end()
						&& isDynArr(cg.interned, storIt->second))
					{
						auto slotIt = cg.slots.find(key);
						if (slotIt != cg.slots.end())
							return cg.builder.CreateLoad(cg.builder.getPtrTy(),
								slotIt->second, "dynarr.load");
					}
				}
			}
			llvm::Value* addr = genAddr(cg, e);
			if (!addr)
			{
				cg.warn("unresolved identifier in expression");
				return llvm::UndefValue::get(cg.lowerType(ctxType));
			}
			TypeId vt = cg.exprType(e);
			// The typechecker doesn't always set exprType for identifiers used as
			// callees — fall back to the declaration's storage type for the load.
			if (vt == INVALID_TYPE_ID) vt = rawTypeOf(cg, e);
			return cg.builder.CreateLoad(cg.lowerType(vt), addr, "load");
		}

		if (auto* p = std::get_if<Required<AST::BinaryExpression>>(&e))
			return genBinary(cg, p->value(), ctxType);

		if (auto* p = std::get_if<Required<AST::UnaryExpression>>(&e))
			return genUnary(cg, p->value(), ctxType);

		if (auto* p = std::get_if<Required<AST::FunctionCallExpression>>(&e))
			return genCall(cg, p->value(), e);

		if (std::get_if<Required<AST::MemberAccessExpression>>(&e)
			|| std::get_if<Required<AST::ArrayAccessExpression>>(&e))
		{
			llvm::Value* addr = genAddr(cg, e);
			if (!addr)
			{
				cg.warn("unresolved access expression");
				return llvm::UndefValue::get(cg.lowerType(ctxType));
			}
			TypeId vt = cg.exprType(e);
			return cg.builder.CreateLoad(cg.lowerType(vt), addr, "load");
		}

		if (auto* p = std::get_if<Required<AST::ArrayLiteralExpression>>(&e))
			return genArrayLiteral(cg, p->value(), ctxType, cg.exprType(e));

		if (auto* p = std::get_if<Required<AST::ArrayFillExpression>>(&e))
			return genArrayFill(cg, p->value(), ctxType, cg.exprType(e));

		if (auto* p = std::get_if<Required<AST::StructInitializerExpression>>(&e))
			return genStructInit(cg, p->value(), ctxType, cg.exprType(e));

		if (auto* p = std::get_if<Required<AST::ComptimeResultExpression>>(&e))
		{
			using RK = AST::ComptimeResultExpression::ResultKind;
			const auto& r = p->value();
			switch (r.kind)
			{
				case RK::Integer:
					if (cg.isFloat(ctxType))
						return llvm::ConstantFP::get(cg.lowerType(ctxType),
							static_cast<double>(r.intValue));
					return llvm::ConstantInt::get(
						cg.canonical(ctxType) != INVALID_TYPE_ID && !cg.isFloat(ctxType)
						? cg.lowerType(ctxType) : cg.builder.getInt32Ty(),
						static_cast<uint64_t>(r.intValue));
				case RK::Float:
					return llvm::ConstantFP::get(
						cg.isFloat(ctxType) ? cg.lowerType(ctxType)
						: llvm::Type::getFloatTy(cg.ctx), r.floatValue);
				case RK::Bool:
					return cg.builder.getInt1(r.intValue != 0);
				case RK::Char:
					return llvm::ConstantInt::get(cg.builder.getInt32Ty(),
						static_cast<uint64_t>(r.intValue));
				default:
					cg.warn("invalid comptime result");
					return cg.builder.getInt32(0);
			}
		}

		if (auto* p = std::get_if<Required<AST::IfExpression>>(&e))
		{
			TypeId rt = cg.exprType(e);
			llvm::Value* slot = cg.entryAlloca(cg.lowerType(rt), "if.res");
			genIf(cg, p->value(), slot, rt);
			return cg.builder.CreateLoad(cg.lowerType(rt), slot, "if.val");
		}
		if (auto* p = std::get_if<Required<AST::WhileExpression>>(&e))
		{
			TypeId rt = cg.exprType(e);
			llvm::Value* slot = cg.entryAlloca(cg.lowerType(rt), "while.res");
			genWhile(cg, p->value(), slot, rt);
			return cg.builder.CreateLoad(cg.lowerType(rt), slot, "while.val");
		}
		if (auto* p = std::get_if<Required<AST::ForExpression>>(&e))
		{
			TypeId rt = cg.exprType(e);
			llvm::Value* slot = cg.entryAlloca(cg.lowerType(rt), "for.res");
			genFor(cg, p->value(), slot, rt);
			return cg.builder.CreateLoad(cg.lowerType(rt), slot, "for.val");
		}
		if (auto* p = std::get_if<Required<AST::MatchExpression>>(&e))
		{
			TypeId rt = cg.exprType(e);
			llvm::Value* slot = cg.entryAlloca(cg.lowerType(rt), "match.res");
			genMatch(cg, p->value(), slot, rt);
			return cg.builder.CreateLoad(cg.lowerType(rt), slot, "match.val");
		}

		if (auto* p = std::get_if<Required<AST::LambdaExpression>>(&e))
			return lowerLambda(cg, p->value());

		if (auto* p = std::get_if<Required<AST::ComptimeExpression>>(&e))
			return genExpr(cg, p->value().inner.value(), ctxType);

		cg.warn("unsupported expression kind — emitting placeholder");
		return llvm::UndefValue::get(cg.lowerType(ctxType));
	}

	// =====================================================================
	//  Constructs
	// =====================================================================

	void genIf(Codegen& cg, const AST::IfExpression& ifx, llvm::Value* resultSlot, TypeId resultType)
	{
		llvm::Value* cond = genExpr(cg, ifx.condition.value(), TYPE_BOOL);
		if (cond->getType() != cg.builder.getInt1Ty())
			cond = cg.builder.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "ifcond");

		llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(cg.ctx, "if.then", cg.currentFn);
		llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(cg.ctx, "if.else", cg.currentFn);
		llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(cg.ctx, "if.end", cg.currentFn);

		cg.builder.CreateCondBr(cond, thenBB, elseBB);

		cg.breakStack.push_back({ mergeBB, resultSlot, resultType });

		cg.builder.SetInsertPoint(thenBB);
		genStatement(cg, ifx.thenBranch.value());
		if (cg.blockOpen())
			cg.builder.CreateBr(mergeBB);

		cg.builder.SetInsertPoint(elseBB);
		if (ifx.elseBranch.hasValue())
			genStatement(cg, ifx.elseBranch.value());
		if (cg.blockOpen())
			cg.builder.CreateBr(mergeBB);

		cg.breakStack.pop_back();
		cg.builder.SetInsertPoint(mergeBB);
	}

	void genWhile(Codegen& cg, const AST::WhileExpression& wx, llvm::Value* resultSlot, TypeId resultType)
	{
		llvm::BasicBlock* condBB = llvm::BasicBlock::Create(cg.ctx, "while.cond", cg.currentFn);
		llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(cg.ctx, "while.body", cg.currentFn);
		llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(cg.ctx, "while.exit", cg.currentFn);

		cg.builder.CreateBr(condBB);
		cg.builder.SetInsertPoint(condBB);
		llvm::Value* cond = genExpr(cg, wx.condition.value(), TYPE_BOOL);
		if (cond->getType() != cg.builder.getInt1Ty())
			cond = cg.builder.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "wcond");
		cg.builder.CreateCondBr(cond, bodyBB, exitBB);

		cg.breakStack.push_back({ exitBB, resultSlot, resultType });
		cg.builder.SetInsertPoint(bodyBB);
		genStatement(cg, wx.body.value());
		if (cg.blockOpen())
			cg.builder.CreateBr(condBB);
		cg.breakStack.pop_back();

		cg.builder.SetInsertPoint(exitBB);
		if (wx.elseBody.hasValue())
			genStatement(cg, wx.elseBody.value());
	}

	void genFor(Codegen& cg, const AST::ForExpression& fx, llvm::Value* resultSlot, TypeId resultType)
	{
		// Collect iterables and their element types.
		struct Iter { const AST::Expression* expr; TypeId type; TypeId elem; };
		std::vector<Iter> iters;
		fx.iterables.value().forEach([&](const Required<AST::Expression>& it)
			{
				TypeId t = cg.canonical(cg.exprType(it.value()));
				TypeId elem = INVALID_TYPE_ID;
				if (t != INVALID_TYPE_ID && t < static_cast<TypeId>(cg.interned.table.size()))
				{
					const TypeInfo& info = cg.interned.get(t);
					if (info.kind == TypeInfo::Kind::Array) elem = info.asArray().elem;
					else if (info.kind == TypeInfo::Kind::Slice) elem = info.asSlice().elem;
				}
				iters.push_back({ &it.value(), t, elem });
			});

		if (iters.empty())
		{
			cg.warn("'for' with no iterables — skipped");
			return;
		}

		// Loop length = minimum of all iterable lengths.
		llvm::Value* length = nullptr;
		std::vector<llvm::Value*> iterAddrs;
		for (auto& it : iters)
		{
			llvm::Value* addr = genAddr(cg, *it.expr);
			iterAddrs.push_back(addr);
			llvm::Value* len = nullptr;
			if (it.type != INVALID_TYPE_ID && it.type < static_cast<TypeId>(cg.interned.table.size()))
			{
				const TypeInfo& info = cg.interned.get(it.type);
				if (info.kind == TypeInfo::Kind::Array)
					len = cg.builder.getInt64(info.asArray().size);
				else if (info.kind == TypeInfo::Kind::Slice && addr)
					len = cg.builder.CreateLoad(cg.builder.getInt64Ty(),
						cg.builder.CreateStructGEP(cg.getSliceTy(), addr, 1), "slen");
			}
			if (!len) len = cg.builder.getInt64(0);
			length = length ? cg.builder.CreateSelect(
				cg.builder.CreateICmpULT(len, length), len, length, "minlen") : len;
		}

		// Capture slots.
		std::vector<llvm::Value*> capSlots;
		std::vector<TypeId> capTypes;
		size_t ci = 0;
		if (fx.iterators.hasValue())
		{
			fx.iterators.value().forEach([&](const Required<AST::Capture>& cap)
				{
					TypeId elem = ci < iters.size() ? iters[ci].elem : INVALID_TYPE_ID;
					llvm::Value* slot = cg.entryAlloca(cg.lowerType(elem), "cap");
					cg.slots[cap.ptr()] = slot;
					cg.slotStorage[cap.ptr()] = elem;
					capSlots.push_back(slot);
					capTypes.push_back(elem);
					++ci;
				});
		}

		llvm::Value* idxSlot = cg.entryAlloca(cg.builder.getInt64Ty(), "for.i");
		cg.builder.CreateStore(cg.builder.getInt64(0), idxSlot);

		llvm::BasicBlock* condBB = llvm::BasicBlock::Create(cg.ctx, "for.cond", cg.currentFn);
		llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(cg.ctx, "for.body", cg.currentFn);
		llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(cg.ctx, "for.exit", cg.currentFn);

		cg.builder.CreateBr(condBB);
		cg.builder.SetInsertPoint(condBB);
		llvm::Value* idx = cg.builder.CreateLoad(cg.builder.getInt64Ty(), idxSlot, "i");
		cg.builder.CreateCondBr(cg.builder.CreateICmpULT(idx, length, "for.more"), bodyBB, exitBB);

		cg.builder.SetInsertPoint(bodyBB);
		// Load each iterable's current element into its capture slot.
		for (size_t k = 0; k < capSlots.size() && k < iters.size(); ++k)
		{
			llvm::Value* elemPtr = nullptr;
			if (iters[k].type < static_cast<TypeId>(cg.interned.table.size())
				&& cg.interned.get(iters[k].type).kind == TypeInfo::Kind::Array)
			{
				elemPtr = cg.builder.CreateGEP(cg.lowerType(iters[k].type), iterAddrs[k],
					{ cg.builder.getInt64(0), idx }, "for.elptr");
			}
			else if (iters[k].type < static_cast<TypeId>(cg.interned.table.size())
				&& cg.interned.get(iters[k].type).kind == TypeInfo::Kind::Slice && iterAddrs[k])
			{
				llvm::Value* dp = cg.builder.CreateLoad(cg.builder.getPtrTy(),
					cg.builder.CreateStructGEP(cg.getSliceTy(), iterAddrs[k], 0), "for.dp");
				elemPtr = cg.builder.CreateGEP(cg.lowerType(iters[k].elem), dp, { idx }, "for.elptr");
			}
			if (elemPtr)
			{
				llvm::Value* v = cg.builder.CreateLoad(cg.lowerType(capTypes[k]), elemPtr, "for.el");
				cg.builder.CreateStore(v, capSlots[k]);
			}
		}

		cg.breakStack.push_back({ exitBB, resultSlot, resultType });
		genStatement(cg, fx.body.value());
		cg.breakStack.pop_back();

		if (cg.blockOpen())
		{
			llvm::Value* next = cg.builder.CreateAdd(
				cg.builder.CreateLoad(cg.builder.getInt64Ty(), idxSlot), cg.builder.getInt64(1), "i.next");
			cg.builder.CreateStore(next, idxSlot);
			cg.builder.CreateBr(condBB);
		}

		cg.builder.SetInsertPoint(exitBB);
		if (fx.elseBody.hasValue())
			genStatement(cg, fx.elseBody.value());
	}

	void genMatch(Codegen& cg, const AST::MatchExpression& mx, llvm::Value* resultSlot, TypeId resultType)
	{
		TypeId subjTy = cg.canonical(cg.exprType(mx.subject.value()));
		llvm::Value* subjVal = genExpr(cg, mx.subject.value(), cg.exprType(mx.subject.value()));

		const bool isEnum = subjTy != INVALID_TYPE_ID
			&& subjTy < static_cast<TypeId>(cg.interned.table.size())
			&& cg.interned.get(subjTy).kind == TypeInfo::Kind::Enum;

		// Materialise the subject so the payload area is addressable.
		llvm::Value* subjSlot = cg.entryAlloca(subjVal->getType(), "match.subj");
		cg.builder.CreateStore(subjVal, subjSlot);

		llvm::Value* discr = nullptr;
		if (isEnum)
			discr = cg.builder.CreateLoad(cg.builder.getInt32Ty(),
				cg.builder.CreateStructGEP(cg.lowerType(subjTy), subjSlot, 0), "tag");
		else
			discr = subjVal;

		llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(cg.ctx, "match.exit", cg.currentFn);
		llvm::BasicBlock* defaultBB = llvm::BasicBlock::Create(cg.ctx, "match.default", cg.currentFn);

		llvm::SwitchInst* sw = cg.builder.CreateSwitch(discr, defaultBB);

		cg.breakStack.push_back({ exitBB, resultSlot, resultType });

		const AST::Statement* elseArm = nullptr;
		mx.arms.forEach([&](const Required<AST::MatchArm>& armReq)
			{
				const AST::MatchArm& arm = armReq.value();
				if (!arm.pattern.hasValue())
				{
					elseArm = &arm.body.value();
					return;
				}

				llvm::BasicBlock* armBB = llvm::BasicBlock::Create(cg.ctx, "match.arm", cg.currentFn);

				// Determine the case constant.
				llvm::ConstantInt* caseVal = nullptr;
				VariantInfo vi;
				if (auto* idReq = std::get_if<Required<AST::IdentifierExpression>>(&arm.pattern.value()))
				{
					vi = classifyVariant(cg, idReq->value());
					if (vi.isVariant)
						caseVal = cg.builder.getInt32(static_cast<uint32_t>(vi.tag));
				}
				if (!caseVal && !isEnum)
				{
					// integer / char pattern
					llvm::Value* pv = genExpr(cg, arm.pattern.value(), subjTy);
					if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(pv))
						caseVal = llvm::cast<llvm::ConstantInt>(
							cg.builder.CreateIntCast(ci, discr->getType(), false));
				}

				if (caseVal)
					sw->addCase(caseVal, armBB);
				else
				{
					cg.warn("unsupported match pattern — arm skipped");
					return;
				}

				cg.builder.SetInsertPoint(armBB);

				// Bind a payload capture, if present.
				if (arm.capture.hasValue() && vi.isVariant && vi.hasPayload)
				{
					llvm::Value* payPtr = cg.builder.CreateStructGEP(
						cg.lowerType(subjTy), subjSlot, 1, "match.pay");
					llvm::Value* slot = cg.entryAlloca(cg.lowerType(vi.payloadType), "cap");
					llvm::Value* v = cg.builder.CreateLoad(cg.lowerType(vi.payloadType), payPtr, "pay.v");
					cg.builder.CreateStore(v, slot);
					cg.slots[&arm.capture.value()] = slot;
					cg.slotStorage[&arm.capture.value()] = vi.payloadType;
				}

				genStatement(cg, arm.body.value());
				if (cg.blockOpen())
					cg.builder.CreateBr(exitBB);
			});

		// Default block: the internal 'else' arm, if any.
		cg.builder.SetInsertPoint(defaultBB);
		if (elseArm)
			genStatement(cg, *elseArm);
		if (cg.blockOpen())
			cg.builder.CreateBr(exitBB);

		cg.breakStack.pop_back();
		cg.builder.SetInsertPoint(exitBB);

		// External 'else' runs when an arm fell through without 'break'.
		if (mx.externalElse.hasValue())
			genStatement(cg, mx.externalElse.value());
	}

	// =====================================================================
	//  Statement dispatch
	// =====================================================================

	void genStatement(Codegen& cg, const AST::Statement& s)
	{
		if (!cg.blockOpen())
			return;   // unreachable code after a terminator

		if (auto* p = std::get_if<Required<AST::VariableDefinitionStatement>>(&s))
		{
			const AST::VariableDefinitionStatement& vd = p->value();
			TypeId storage = INVALID_TYPE_ID;
			if (vd.type.hasValue())
			{
				auto it = cg.interned.astTypes.find(&vd.type.value());
				if (it != cg.interned.astTypes.end())
					storage = it->second;
			}
			if (storage == INVALID_TYPE_ID)
				storage = cg.exprType(vd.value.value());

			const bool ifaceStore = isIfaceIndirection(cg.interned, storage);
			llvm::Value* init = nullptr;
			llvm::Type*  slotTy = nullptr;
			if (ifaceStore)
			{
				// Coerce RHS into a { data, vtable } fat pointer using the
				// interface's TypeId (the inner of the &I/*I storage).
				init   = makeIfaceArg(cg, vd.value.value(), ifaceInner(cg.interned, storage));
				slotTy = cg.getIfaceTy();
			}
			else
			{
				TypeId valueCtx = cg.isIndirection(storage)
					? storage : cg.canonical(storage);
				init = genExpr(cg, vd.value.value(), valueCtx);
				slotTy = cg.isIndirection(storage)
					? static_cast<llvm::Type*>(cg.builder.getPtrTy())
					: cg.lowerType(storage);
			}
			llvm::Value* slot = cg.entryAlloca(slotTy, cg.tokenText(vd.name));

			if (!cg.isIndirection(storage))
				init = coerce(cg, init, cg.exprType(vd.value.value()), storage);
			cg.builder.CreateStore(init, slot);

			cg.slots[&vd] = slot;
			cg.slotStorage[&vd] = storage;
			return;
		}

		if (auto* p = std::get_if<Required<AST::AssignmentStatement>>(&s))
		{
			const AST::AssignmentStatement& as = p->value();
			llvm::Value* addr = genAddr(cg, as.target.value());
			if (!addr)
			{
				cg.warn("assignment target is not addressable — skipped");
				return;
			}
			TypeId tt = cg.exprType(as.target.value());
			llvm::Value* rhs = nullptr;
			if (isIfaceIndirection(cg.interned, tt))
			{
				rhs = makeIfaceArg(cg, as.value.value(), ifaceInner(cg.interned, tt));
			}
			else
			{
				rhs = genExpr(cg, as.value.value(), tt);
				rhs = coerce(cg, rhs, cg.exprType(as.value.value()), tt);
			}

			if (as.op != TokenKind::Assign)
			{
				llvm::Value* cur = cg.builder.CreateLoad(cg.lowerType(tt), addr, "cur");
				const bool fp = cg.isFloat(tt);
				const bool sg = cg.isSigned(tt);
				using K = TokenKind;
				switch (as.op)
				{
					case K::PlusAssign:    rhs = fp ? cg.builder.CreateFAdd(cur, rhs) : cg.builder.CreateAdd(cur, rhs); break;
					case K::MinusAssign:   rhs = fp ? cg.builder.CreateFSub(cur, rhs) : cg.builder.CreateSub(cur, rhs); break;
					case K::MultiplyAssign:rhs = fp ? cg.builder.CreateFMul(cur, rhs) : cg.builder.CreateMul(cur, rhs); break;
					case K::DivideAssign:  rhs = fp ? cg.builder.CreateFDiv(cur, rhs)
						: sg ? cg.builder.CreateSDiv(cur, rhs) : cg.builder.CreateUDiv(cur, rhs); break;
					case K::ModuloAssign:  rhs = fp ? cg.builder.CreateFRem(cur, rhs)
						: sg ? cg.builder.CreateSRem(cur, rhs) : cg.builder.CreateURem(cur, rhs); break;
					case K::AndAssign:     rhs = cg.builder.CreateAnd(cur, rhs); break;
					case K::OrAssign:      rhs = cg.builder.CreateOr(cur, rhs); break;
					case K::XorAssign:     rhs = cg.builder.CreateXor(cur, rhs); break;
					case K::ShiftLeftAssign:  rhs = cg.builder.CreateShl(cur, rhs); break;
					case K::ShiftRightAssign: rhs = sg ? cg.builder.CreateAShr(cur, rhs)
						: cg.builder.CreateLShr(cur, rhs); break;
					default: break;
				}
			}
			cg.builder.CreateStore(rhs, addr);
			return;
		}

		if (auto* p = std::get_if<Required<AST::ExpressionStatement>>(&s))
		{
			genExpr(cg, p->value().expression.value(), INVALID_TYPE_ID);
			return;
		}

		if (auto* p = std::get_if<Required<AST::StatementBlock>>(&s))
		{
			p->value().statements.forEach([&](const Required<AST::Statement>& st)
				{
					genStatement(cg, st.value());
				});
			return;
		}

		if (auto* p = std::get_if<Required<AST::IfExpression>>(&s))
		{
			genIf(cg, p->value(), nullptr, INVALID_TYPE_ID);
			return;
		}
		if (auto* p = std::get_if<Required<AST::WhileExpression>>(&s))
		{
			genWhile(cg, p->value(), nullptr, INVALID_TYPE_ID);
			return;
		}
		if (auto* p = std::get_if<Required<AST::ForExpression>>(&s))
		{
			genFor(cg, p->value(), nullptr, INVALID_TYPE_ID);
			return;
		}
		if (auto* p = std::get_if<Required<AST::MatchExpression>>(&s))
		{
			genMatch(cg, p->value(), nullptr, INVALID_TYPE_ID);
			return;
		}

		if (auto* p = std::get_if<Required<AST::BreakStatement>>(&s))
		{
			if (cg.breakStack.empty())
			{
				cg.warn("'break' outside any construct — skipped");
				return;
			}
			const BreakTarget& tgt = cg.breakStack.back();
			if (p->value().value.hasValue() && tgt.resultSlot)
			{
				llvm::Value* v = genExpr(cg, p->value().value.value(), tgt.resultType);
				v = coerce(cg, v, cg.exprType(p->value().value.value()), tgt.resultType);
				cg.builder.CreateStore(v, tgt.resultSlot);
			}
			else if (p->value().value.hasValue())
			{
				genExpr(cg, p->value().value.value(), INVALID_TYPE_ID);
			}
			cg.builder.CreateBr(tgt.exit);
			return;
		}

		if (auto* p = std::get_if<Required<AST::ReturnStatement>>(&s))
		{
			if (p->value().value.hasValue() && cg.currentReturnType != INVALID_TYPE_ID)
			{
				llvm::Value* v = genExpr(cg, p->value().value.value(), cg.currentReturnType);
				v = coerce(cg, v, cg.exprType(p->value().value.value()), cg.currentReturnType);
				cg.builder.CreateRet(v);
			}
			else if (p->value().value.hasValue())
			{
				genExpr(cg, p->value().value.value(), INVALID_TYPE_ID);
				cg.builder.CreateRetVoid();
			}
			else
			{
				cg.builder.CreateRetVoid();
			}
			return;
		}

		cg.warn("unsupported statement kind — skipped");
	}

	// =====================================================================
	//  Function declaration & definition
	// =====================================================================

	llvm::FunctionType* functionType(Codegen& cg, const AST::Function& fn, bool& retVoid)
	{
		std::vector<llvm::Type*> params;
		fn.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto it = cg.interned.astTypes.find(&p.value().type.value());
				TypeId t = it != cg.interned.astTypes.end() ? it->second : INVALID_TYPE_ID;
				params.push_back(cg.lowerType(t));
			});
		llvm::Type* ret = cg.builder.getVoidTy();
		retVoid = true;
		if (fn.returnType.hasValue())
		{
			auto it = cg.interned.astTypes.find(&fn.returnType.value());
			if (it != cg.interned.astTypes.end())
			{
				ret = cg.lowerType(it->second);
				retVoid = false;
			}
		}
		return llvm::FunctionType::get(ret, params, false);
	}

	// §5.3 / FFI: lower a TypeId for an extern signature. Alloy references and
	// pointers (&T / *T / &var T / *var T) map to a plain C pointer regardless
	// of the inner type — the C ABI has only one notion of pointer. Slice and
	// interface (fat-pointer) types cannot be represented by a single C
	// pointer; we lower them as a plain pointer with a diagnostic so the
	// programmer notices.
	llvm::Type* lowerExternType(Codegen& cg, TypeId t, const char* role)
	{
		if (t == INVALID_TYPE_ID || t >= (TypeId)cg.interned.table.size())
			return cg.builder.getInt32Ty();
		const auto& info = cg.interned.get(t);
		if (info.isIndirection())
			return cg.builder.getPtrTy();
		if (info.kind == TypeInfo::Kind::Slice)
		{
			cg.warn(std::string("extern ") + role + " cannot be a slice — C has no slice ABI; lowering as plain pointer");
			return cg.builder.getPtrTy();
		}
		if (info.kind == TypeInfo::Kind::Interface)
		{
			cg.warn(std::string("extern ") + role + " cannot be an interface — C has no fat-pointer ABI; lowering as plain pointer");
			return cg.builder.getPtrTy();
		}
		return cg.lowerType(t);
	}

	void declareExtern(Codegen& cg, const AST::ExternDefinition& ext)
	{
		std::string name(cg.tokenText(ext.name));
		if (cg.externMap.count(name))
			return;
		std::vector<llvm::Type*> params;
		ext.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto it = cg.interned.astTypes.find(&p.value().type.value());
				TypeId t = it != cg.interned.astTypes.end() ? it->second : INVALID_TYPE_ID;
				params.push_back(lowerExternType(cg, t, "parameter"));
			});
		// Alloy has no 'void' keyword, so absence of '-> T' on an extern
		// means the C function returns nothing.
		llvm::Type* ret = cg.builder.getVoidTy();
		if (ext.returnType.hasValue())
		{
			auto it = cg.interned.astTypes.find(&ext.returnType.value());
			if (it != cg.interned.astTypes.end())
				ret = lowerExternType(cg, it->second, "return type");
		}
		auto* fty = llvm::FunctionType::get(ret, params, ext.isVariadic || params.empty());
		auto* fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, *cg.mod);
		cg.externMap[name] = fn;
	}

	void declareFunction(Codegen& cg, const AST::FunctionDefinition& fd)
	{
		// C4: generic functions are not declared directly — a monomorphized copy is
		// emitted for each distinct binding set seen at call sites.
		if (fd.typeParameters.hasValue())
			return;
		bool retVoid = false;
		llvm::FunctionType* fty = functionType(cg, fd.function.value(), retVoid);

		std::string name(cg.tokenText(fd.name));
		std::string symbol = (name == "main") ? "main"
			: "alloy." + name + "." + std::to_string(cg.mangleCounter++);

		auto* fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, symbol, *cg.mod);
		cg.fnMap[&fd] = fn;
	}

	// Lowers a function body into the supplied llvm::Function. Shared by named
	// function definitions, generic monomorphisations and lambda expressions.
	void lowerFunctionBody(Codegen& cg, const AST::Function& fn,
		const char* debugName, llvm::Function* lfn,
		const std::unordered_map<TypeId, TypeId>* bindings)
	{
		// Save outer per-function state so re-entry (e.g. lambda nested in a body,
		// or two instances of the same generic) doesn't trample shared maps.
		auto savedSlots = std::move(cg.slots);
		auto savedStorage = std::move(cg.slotStorage);
		auto* savedFn = cg.currentFn;
		TypeId savedRet = cg.currentReturnType;
		const auto* savedBindings = cg.currentMonoBindings;
		llvm::BasicBlock* savedBB = cg.builder.GetInsertBlock();
		cg.slots.clear();
		cg.slotStorage.clear();

		cg.currentMonoBindings = bindings;
		cg.currentFn = lfn;

		cg.currentReturnType = INVALID_TYPE_ID;
		if (fn.returnType.hasValue())
		{
			auto rit = cg.interned.astTypes.find(&fn.returnType.value());
			if (rit != cg.interned.astTypes.end())
				cg.currentReturnType = rit->second;
		}

		llvm::BasicBlock* entry = llvm::BasicBlock::Create(cg.ctx, "entry", lfn);
		cg.builder.SetInsertPoint(entry);

		unsigned i = 0;
		fn.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto ti = cg.interned.astTypes.find(&p.value().type.value());
				TypeId storage = ti != cg.interned.astTypes.end() ? ti->second : INVALID_TYPE_ID;
				llvm::Argument* arg = lfn->getArg(i);
				arg->setName(cg.tokenText(p.value().name));
				llvm::Value* slot = cg.builder.CreateAlloca(arg->getType(), nullptr,
					std::string(cg.tokenText(p.value().name)) + ".addr");
				cg.builder.CreateStore(arg, slot);
				cg.slots[&p.value()] = slot;
				cg.slotStorage[&p.value()] = storage;
				++i;
			});

		genStatement(cg, AST::Statement(fn.body));

		if (cg.blockOpen())
		{
			if (lfn->getReturnType()->isVoidTy())
				cg.builder.CreateRetVoid();
			else
				cg.builder.CreateRet(llvm::Constant::getNullValue(lfn->getReturnType()));
		}

		std::string verr;
		llvm::raw_string_ostream os(verr);
		if (llvm::verifyFunction(*lfn, &os))
			cg.fail("LLVM verification failed for '"
				+ std::string(debugName ? debugName : "<anon>") + "': " + os.str());

		cg.currentFn = savedFn;
		cg.currentReturnType = savedRet;
		cg.currentMonoBindings = savedBindings;
		cg.slots = std::move(savedSlots);
		cg.slotStorage = std::move(savedStorage);
		if (savedBB) cg.builder.SetInsertPoint(savedBB);
	}

	void defineFunctionImpl(Codegen& cg, const AST::FunctionDefinition& fd,
		llvm::Function* fn, const std::unordered_map<TypeId, TypeId>* bindings)
	{
		std::string name(cg.tokenText(fd.name));
		lowerFunctionBody(cg, fd.function.value(), name.c_str(), fn, bindings);
	}

	void defineFunction(Codegen& cg, const AST::FunctionDefinition& fd)
	{
		auto it = cg.fnMap.find(&fd);
		if (it == cg.fnMap.end())
			return;
		defineFunctionImpl(cg, fd, it->second, nullptr);
	}

	// Capture info collected at lambda-creation time (caller side).
	struct CaptureInfo
	{
		const AST::Capture* cap;
		const ResolvedDeclaration* outerDecl;
		AST::Type::Modifier modifier;
		TypeId outerValueTy;       // value type of the captured outer binding
		llvm::Type* fieldTy;       // env struct field type for this capture
	};

	// Lower the lambda body. lfn has signature `(ptr env, declared_params...) -> R`.
	// If `envTy` is non-null, the body wires up capture slots via GEPs off env.
	void lowerLambdaBody(Codegen& cg, const AST::LambdaExpression& lambda,
		llvm::Function* lfn, llvm::StructType* envTy,
		const std::vector<CaptureInfo>& caps)
	{
		auto savedSlots = std::move(cg.slots);
		auto savedStorage = std::move(cg.slotStorage);
		auto* savedFn = cg.currentFn;
		TypeId savedRet = cg.currentReturnType;
		const auto* savedBindings = cg.currentMonoBindings;
		llvm::BasicBlock* savedBB = cg.builder.GetInsertBlock();

		cg.slots.clear();
		cg.slotStorage.clear();
		cg.currentFn = lfn;

		const AST::Function& fn = lambda.function.value();
		cg.currentReturnType = INVALID_TYPE_ID;
		if (fn.returnType.hasValue())
		{
			auto rit = cg.interned.astTypes.find(&fn.returnType.value());
			if (rit != cg.interned.astTypes.end())
				cg.currentReturnType = rit->second;
		}

		llvm::BasicBlock* entry = llvm::BasicBlock::Create(cg.ctx, "entry", lfn);
		cg.builder.SetInsertPoint(entry);

		llvm::Argument* envArg = lfn->getArg(0);
		envArg->setName("env");
		if (envTy)
		{
			for (size_t i = 0; i < caps.size(); ++i)
			{
				llvm::Value* fieldAddr = cg.builder.CreateStructGEP(envTy, envArg,
					static_cast<unsigned>(i),
					std::string("cap.") + std::to_string(i));
				cg.slots[caps[i].cap] = fieldAddr;
				// slotStorage = value type for by-value; for by-ref the field holds a
				// pointer and genAddr inspects the Capture modifier directly.
				cg.slotStorage[caps[i].cap] = caps[i].outerValueTy;
			}
		}

		unsigned argIdx = 1;
		fn.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto ti = cg.interned.astTypes.find(&p.value().type.value());
				TypeId storage = ti != cg.interned.astTypes.end() ? ti->second : INVALID_TYPE_ID;
				llvm::Argument* arg = lfn->getArg(argIdx);
				arg->setName(cg.tokenText(p.value().name));
				llvm::Value* slot = cg.builder.CreateAlloca(arg->getType(), nullptr,
					std::string(cg.tokenText(p.value().name)) + ".addr");
				cg.builder.CreateStore(arg, slot);
				cg.slots[&p.value()] = slot;
				cg.slotStorage[&p.value()] = storage;
				++argIdx;
			});

		genStatement(cg, AST::Statement(fn.body));

		if (cg.blockOpen())
		{
			if (lfn->getReturnType()->isVoidTy())
				cg.builder.CreateRetVoid();
			else
				cg.builder.CreateRet(llvm::Constant::getNullValue(lfn->getReturnType()));
		}

		std::string verr;
		llvm::raw_string_ostream os(verr);
		if (llvm::verifyFunction(*lfn, &os))
			cg.fail("LLVM verification failed for lambda: " + os.str());

		cg.currentFn = savedFn;
		cg.currentReturnType = savedRet;
		cg.currentMonoBindings = savedBindings;
		cg.slots = std::move(savedSlots);
		cg.slotStorage = std::move(savedStorage);
		if (savedBB) cg.builder.SetInsertPoint(savedBB);
	}

	// Synthesise (and cache) a `(env, args) -> ret` thunk that forwards to a named
	// function. Used when a named fn is taken as a `(T) -> R` value so closures
	// always carry a fn pointer matching the closure-ABI signature.
	llvm::Function* getOrBuildThunk(Codegen& cg, const AST::FunctionDefinition& fd)
	{
		if (auto it = cg.thunks.find(&fd); it != cg.thunks.end()) return it->second;
		auto fnIt = cg.fnMap.find(&fd);
		if (fnIt == cg.fnMap.end()) return nullptr;
		llvm::Function* target = fnIt->second;
		llvm::FunctionType* tfty = target->getFunctionType();

		std::vector<llvm::Type*> ptys;
		ptys.push_back(cg.builder.getPtrTy());   // env (ignored)
		for (unsigned i = 0; i < tfty->getNumParams(); ++i)
			ptys.push_back(tfty->getParamType(i));
		auto* thunkFty = llvm::FunctionType::get(tfty->getReturnType(), ptys, false);

		std::string name = "alloy.thunk." + std::to_string(cg.lambdaCounter++);
		auto* thunk = llvm::Function::Create(thunkFty,
			llvm::Function::PrivateLinkage, name, *cg.mod);

		llvm::BasicBlock* savedBB = cg.builder.GetInsertBlock();
		auto* entry = llvm::BasicBlock::Create(cg.ctx, "entry", thunk);
		cg.builder.SetInsertPoint(entry);

		std::vector<llvm::Value*> fwd;
		for (unsigned i = 1; i < thunk->arg_size(); ++i)
			fwd.push_back(thunk->getArg(i));
		llvm::Value* result = cg.builder.CreateCall(target, fwd);
		if (thunkFty->getReturnType()->isVoidTy()) cg.builder.CreateRetVoid();
		else cg.builder.CreateRet(result);

		if (savedBB) cg.builder.SetInsertPoint(savedBB);
		cg.thunks[&fd] = thunk;
		return thunk;
	}

	llvm::Value* lowerLambda(Codegen& cg, const AST::LambdaExpression& lambda)
	{
		// --- gather capture info from the current (caller's) scope -------------
		std::vector<CaptureInfo> caps;
		if (lambda.captures.hasValue())
		{
			lambda.captures.value().forEach([&](const Required<AST::Capture>& cap)
				{
					CaptureInfo ci;
					ci.cap = cap.ptr();
					ci.modifier = cap.value().modifier;
					auto tnIt = cg.resolved.tokenNames.find(&cap.value().variableName);
					if (tnIt == cg.resolved.tokenNames.end())
					{
						cg.warn("lambda capture references unresolved name");
						return;
					}
					ci.outerDecl = tnIt->second;

					const void* outerKey = cg.declKey(*ci.outerDecl);
					TypeId outerStorage = INVALID_TYPE_ID;
					if (outerKey && cg.slotStorage.count(outerKey))
						outerStorage = cg.slotStorage.at(outerKey);

					// Compute the captured value type — peel one level of indirection
					// off the outer binding when present.
					ci.outerValueTy = outerStorage;
					if (ci.outerValueTy < (TypeId)cg.interned.table.size()
						&& cg.interned.get(ci.outerValueTy).isIndirection())
						ci.outerValueTy = cg.interned.get(ci.outerValueTy).asIndirection().inner;

					switch (ci.modifier)
					{
						case AST::Type::Modifier::None:
							ci.fieldTy = cg.lowerType(ci.outerValueTy);
							break;
						default:
							ci.fieldTy = cg.builder.getPtrTy();
							break;
					}
					caps.push_back(ci);
				});
		}

		// --- env struct + alloca + field initialisation ------------------------
		llvm::StructType* envTy = nullptr;
		llvm::Value* envAlloc = llvm::ConstantPointerNull::get(cg.builder.getPtrTy());
		if (!caps.empty())
		{
			std::vector<llvm::Type*> fields;
			for (auto& c : caps) fields.push_back(c.fieldTy);
			envTy = llvm::StructType::create(cg.ctx, fields,
				"alloy.env." + std::to_string(cg.lambdaCounter));
			// Heap-promote the env so closures that escape their creator's frame
			// stay valid. malloc bytes = DataLayout alloc size of the env struct.
			uint64_t envBytes = cg.mod->getDataLayout().getTypeAllocSize(envTy);
			envAlloc = cg.builder.CreateCall(cg.mallocFn,
				{ cg.builder.getInt64(envBytes) }, "env.heap");

			for (size_t i = 0; i < caps.size(); ++i)
			{
				auto& ci = caps[i];
				const void* outerKey = cg.declKey(*ci.outerDecl);
				auto sIt = cg.slots.find(outerKey);
				if (sIt == cg.slots.end()) continue;
				llvm::Value* outerSlot = sIt->second;
				TypeId outerStorage = cg.slotStorage.count(outerKey)
					? cg.slotStorage.at(outerKey) : INVALID_TYPE_ID;

				llvm::Value* fieldAddr = cg.builder.CreateStructGEP(envTy, envAlloc,
					static_cast<unsigned>(i), "env.f");
				llvm::Value* val = nullptr;
				switch (ci.modifier)
				{
					case AST::Type::Modifier::None:
						// By-value: copy the outer var's value into env.
						if (cg.isIndirection(outerStorage))
						{
							llvm::Value* ptr = cg.builder.CreateLoad(cg.builder.getPtrTy(),
								outerSlot, "outer.deref");
							val = cg.builder.CreateLoad(ci.fieldTy, ptr, "outer.val");
						}
						else
						{
							val = cg.builder.CreateLoad(ci.fieldTy, outerSlot, "outer.val");
						}
						break;
					default:
						// By-ref / by-ptr: env field is a pointer to (or just is) the outer storage.
						if (cg.isIndirection(outerStorage))
							val = cg.builder.CreateLoad(cg.builder.getPtrTy(),
								outerSlot, "outer.ptr");
						else
							val = outerSlot;
						break;
				}
				if (val) cg.builder.CreateStore(val, fieldAddr);
			}
		}

		// --- build lambda fn type (env first) and the function -----------------
		const AST::Function& fn = lambda.function.value();
		std::vector<llvm::Type*> ptys;
		ptys.push_back(cg.builder.getPtrTy());   // env
		fn.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				auto ti = cg.interned.astTypes.find(&p.value().type.value());
				ptys.push_back(ti != cg.interned.astTypes.end()
					? cg.lowerType(ti->second) : cg.builder.getPtrTy());
			});
		llvm::Type* ret = cg.builder.getVoidTy();
		if (fn.returnType.hasValue())
		{
			auto ti = cg.interned.astTypes.find(&fn.returnType.value());
			if (ti != cg.interned.astTypes.end()) ret = cg.lowerType(ti->second);
		}
		auto* fty = llvm::FunctionType::get(ret, ptys, false);

		std::string name = "alloy.lambda." + std::to_string(cg.lambdaCounter++);
		auto* lfn = llvm::Function::Create(fty, llvm::Function::PrivateLinkage,
			name, *cg.mod);

		lowerLambdaBody(cg, lambda, lfn, envTy, caps);

		// --- assemble the closure value ----------------------------------------
		llvm::Value* closure = llvm::UndefValue::get(cg.getClosureTy());
		closure = cg.builder.CreateInsertValue(closure, lfn,      { 0 });
		closure = cg.builder.CreateInsertValue(closure, envAlloc, { 1 });
		return closure;
	}

	// C4: walks every generic call site, instantiates the needed monomorphized
	// llvm::Function (deduplicated by (genericFn, bindings)), and records the
	// per-call target in callMonoTarget. Returns the list of mono tasks whose
	// bodies must be lowered in pass 2.
	// Returns (or builds + queues for body lowering) a mono instance of `gen`
	// keyed by `bindings`. The bindings vector is dedup'd into Codegen's stable
	// storage; the returned pointer remains valid for the rest of codegen.
	MonoEntry getOrCreateMono(Codegen& cg, const AST::FunctionDefinition& gen,
		std::unordered_map<TypeId, TypeId>&& bindings)
	{
		// Build a stable cache key (sorted bindings) and a symbol-suffix string.
		std::vector<std::pair<TypeId, TypeId>> sorted(bindings.begin(), bindings.end());
		std::sort(sorted.begin(), sorted.end());
		std::string bindKey;
		for (auto& kv : sorted)
			bindKey += "." + std::to_string(kv.first) + "_" + std::to_string(kv.second);
		std::string cacheKey = std::to_string((uintptr_t)&gen) + bindKey;

		auto cIt = cg.monoCache.find(cacheKey);
		if (cIt != cg.monoCache.end())
			return { cIt->second, cg.monoBindingsByKey[cacheKey] };

		cg.monoBindingStorage.push_back(std::move(bindings));
		auto* bptr = &cg.monoBindingStorage.back();

		const auto* prev = cg.currentMonoBindings;
		cg.currentMonoBindings = bptr;
		bool retVoid = false;
		llvm::FunctionType* fty = functionType(cg, gen.function.value(), retVoid);
		cg.currentMonoBindings = prev;

		std::string symbol = "alloy." + std::string(cg.tokenText(gen.name)) + bindKey;
		auto* fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage,
			symbol, *cg.mod);
		cg.monoCache[cacheKey] = fn;
		cg.monoBindingsByKey[cacheKey] = bptr;
		cg.pendingMonoTasks.push_back({ &gen, fn, bptr });
		return { fn, bptr };
	}

	void monomorphize(Codegen& cg)
	{
		for (const auto& [call, tokenBindings] : cg.typed.typeArgs)
		{
			auto selIt = cg.typed.selectedOverloads.find(call);
			if (selIt == cg.typed.selectedOverloads.end() || !selIt->second) continue;
			auto* fdReq = std::get_if<Required<AST::FunctionDefinition>>(&selIt->second->definition);
			if (!fdReq) continue;
			const AST::FunctionDefinition& gen = fdReq->value();
			if (!gen.typeParameters.hasValue()) continue;

			std::unordered_map<TypeId, TypeId> bindings;
			gen.typeParameters.value().forEach([&](const Required<AST::TypeParameter>& tp)
				{
					auto tpidIt = cg.interned.typeParamIds.find(&tp.value().name);
					if (tpidIt == cg.interned.typeParamIds.end()) return;
					auto tbIt = tokenBindings.find(&tp.value().name);
					if (tbIt == tokenBindings.end()) return;
					bindings[tpidIt->second] = tbIt->second;
				});

			MonoEntry e = getOrCreateMono(cg, gen, std::move(bindings));
			cg.callMonoTarget[call] = e.fn;
			cg.callMonoBindings[call] = e.bindings;
		}
	}

	// =====================================================================
	//  Object-file emission
	// =====================================================================

	bool emitObject(Codegen& cg, const std::string& objPath)
	{
		std::error_code ec;
		llvm::raw_fd_ostream dest(objPath, ec, llvm::sys::fs::OF_None);
		if (ec)
		{
			cg.fail("could not open object file '" + objPath + "': " + ec.message());
			return false;
		}
		llvm::legacy::PassManager pm;
		if (cg.targetMachine->addPassesToEmitFile(pm, dest, nullptr,
			llvm::CodeGenFileType::ObjectFile))
		{
			cg.fail("target machine cannot emit an object file");
			return false;
		}
		pm.run(*cg.mod);
		dest.flush();
		return true;
	}

}   // namespace

// =====================================================================
//  Entry point
// =====================================================================

Status codegen(
	const Source& source,
	const AST::Module& module,
	const ResolvedModule& resolved,
	const InternedTypes& interned,
	const TypedModule& typed,
	const SymbolTable& moduleSymbols,
	const std::string& outBasePath)
{
	Codegen cg(source, module, resolved, interned, typed, moduleSymbols);

	// --- target setup -------------------------------------------------------
	// Only the X86 back-end is linked (see the project's LLVM library list).
	LLVMInitializeX86TargetInfo();
	LLVMInitializeX86Target();
	LLVMInitializeX86TargetMC();
	LLVMInitializeX86AsmParser();
	LLVMInitializeX86AsmPrinter();

	cg.mod = std::make_unique<llvm::Module>(source.moduleName, cg.ctx);

	std::string triple = llvm::sys::getDefaultTargetTriple();
	// LLVM 19+ requires a Triple, not a StringRef.
	cg.mod->setTargetTriple(llvm::Triple(triple));

	std::string tErr;
	const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, tErr);
	if (!target)
	{
		Log::error("codegen: no LLVM target for triple '{}': {}", triple, tErr);
		return Error;
	}
	llvm::TargetOptions opts;
	cg.targetMachine = target->createTargetMachine(
		triple, "generic", "", opts, llvm::Reloc::PIC_);
	cg.mod->setDataLayout(cg.targetMachine->createDataLayout());

	// --- malloc (for 'new') -------------------------------------------------
	{
		auto* mty = llvm::FunctionType::get(
			cg.builder.getPtrTy(), { cg.builder.getInt64Ty() }, false);
		cg.mallocFn = llvm::Function::Create(
			mty, llvm::Function::ExternalLinkage, "malloc", *cg.mod);
	}

	// --- pass 1: declare every extern and function --------------------------
	module.definitions.forEach([&](const Required<AST::Definition>& def)
		{
			std::visit(Overloaded
				{
					[&](const Required<AST::ExternDefinition>& e) { declareExtern(cg, e.value()); },
					[&](const Required<AST::FunctionDefinition>& f) { declareFunction(cg, f.value()); },
					[&](const auto&) {},
				}, def.value().definition);
		});

	// --- pass 1.5: seed mono tasks for every generic call site --------------
	monomorphize(cg);

	// --- pass 2: lower every non-generic function body ----------------------
	module.definitions.forEach([&](const Required<AST::Definition>& def)
		{
			if (auto* f = std::get_if<Required<AST::FunctionDefinition>>(&def.value().definition))
				defineFunction(cg, f->value());
		});

	// --- pass 3: drain mono task queue. Lowering a body may discover further
	// generic calls (the nested case) and spawn additional tasks; iterate
	// until the queue is empty.
	while (!cg.pendingMonoTasks.empty())
	{
		MonoTask t = cg.pendingMonoTasks.front();
		cg.pendingMonoTasks.pop_front();
		defineFunctionImpl(cg, *t.fd, t.fn, t.bindings);
	}

	// --- write textual IR ---------------------------------------------------
	{
		std::error_code ec;
		llvm::raw_fd_ostream ll(outBasePath + ".ll", ec, llvm::sys::fs::OF_Text);
		if (!ec)
			cg.mod->print(ll, nullptr);
	}

	// --- emit the object file ----------------------------------------------
	if (!cg.hadError)
		emitObject(cg, outBasePath + ".obj");

	return cg.hadError ? Error : Ok;
}
