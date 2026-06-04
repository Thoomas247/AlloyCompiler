#include "type_checker.hpp"

#include <optional>
#include <unordered_map>
#include <variant>

#include "../builtins/builtins.hpp"
#include "../util/logger.hpp"
#include "../util/overloaded.hpp"

using enum Status;

struct ScopedVarMap
{
	ScopedVarMap* parent = nullptr;
	std::unordered_map<const AST::VariableDefinitionStatement*, TypeId> locals;
	std::unordered_map<const AST::FunctionParameter*, TypeId> params;
	std::unordered_map<const AST::Capture*, TypeId> captures;          // B1
	std::unordered_map<const AST::VariableDefinitionStatement*, bool> mutability; // D3

	TypeId lookupVar(const AST::VariableDefinitionStatement* var) const
	{
		auto it = locals.find(var);
		if (it != locals.end()) return it->second;
		return parent ? parent->lookupVar(var) : INVALID_TYPE_ID;
	}

	TypeId lookupParam(const AST::FunctionParameter* param) const
	{
		auto it = params.find(param);
		if (it != params.end()) return it->second;
		return parent ? parent->lookupParam(param) : INVALID_TYPE_ID;
	}

	TypeId lookupCapture(const AST::Capture* cap) const  // B1
	{
		auto it = captures.find(cap);
		if (it != captures.end()) return it->second;
		return parent ? parent->lookupCapture(cap) : INVALID_TYPE_ID;
	}

	bool lookupMutability(const AST::VariableDefinitionStatement* var) const  // D3
	{
		auto it = mutability.find(var);
		if (it != mutability.end()) return it->second;
		return parent ? parent->lookupMutability(var) : true;  // default: mutable (safe fallback)
	}
};

struct CheckState
{
	Logger logger;
	const ResolvedModule& resolved;
	InternedTypes& interned;    // non-const: checker interns inferred types at runtime
	const SymbolTable& moduleSymbols;
	TypedModule result;

	// active type parameter bindings for the current generic call instantiation
	// maps TypeParam TypeId to concrete TypeId
	std::unordered_map<TypeId, TypeId> typeParamBindings;

	// D2: expected return type of the current function being checked (INVALID = void/unknown)
	TypeId currentReturnType = INVALID_TYPE_ID;

	// A2/A3: collects the value types of `break value;` statements targeting the
	// innermost enclosing loop/match. nullptr when no loop/match is active.
	std::vector<TypeId>* breakCollector = nullptr;

	// A2/A3: set by checkStatement when a loop/match is reached in statement
	// position; the construct handler reads and clears it. A trailing `else`
	// clause is only legal when the construct is used as a value expression.
	bool constructInStatementPosition = false;

	explicit CheckState(const Source& source,
		const ResolvedModule& resolved,
		InternedTypes& interned,
		const SymbolTable& moduleSymbols)
		: logger(source), resolved(resolved), interned(interned), moduleSymbols(moduleSymbols)
	{
	}

	// Intern a TypeInfo that has no corresponding AST annotation (e.g. inferred array types).
	// No deduplication — checker-created types are few and structural equality in isAssignable handles correctness.
	TypeId internType(TypeInfo info)
	{
		TypeId id = static_cast<TypeId>(interned.table.size());
		interned.table.push_back(std::move(info));
		return id;
	}

	std::string_view getStringView(const Token& tok) const
	{
		return { &logger.getSource().data[tok.start.index], tok.end.index - tok.start.index };
	}

	TypeId resolveTypeParam(TypeId id) const
	{
		auto it = typeParamBindings.find(id);
		return it != typeParamBindings.end() ? it->second : id;
	}

	void setExprType(const AST::Expression& expr, TypeId id)
	{
		result.exprTypes[&expr] = id;
	}
};

// contextType: optional hint for contextual typing (e.g. array literal element type from annotation).
// Pass INVALID_TYPE_ID when no context is known.
static TypeId checkExpression(CheckState& state, const AST::Expression& expr, ScopedVarMap* scope, TypeId contextType = INVALID_TYPE_ID);
static void checkStatement(CheckState& state, const AST::Statement& stmt, ScopedVarMap* scope);
static void checkFunction(CheckState& state, const AST::Function& fn, const std::unordered_map<TypeId, TypeId>& typeParamBindings, ScopedVarMap* parentScope, TypeId expectedReturnType = INVALID_TYPE_ID);

// --- Char literal byte-width computation (§1.6) ---------------------------

static size_t utf8LeadLength(unsigned char b)
{
	if (b < 0x80)          return 1;
	if ((b & 0xE0) == 0xC0) return 2;
	if ((b & 0xF0) == 0xE0) return 3;
	if ((b & 0xF8) == 0xF0) return 4;
	return 1;
}

static size_t utf8EncodedLength(uint32_t cp)
{
	if (cp <= 0x7F)   return 1;
	if (cp <= 0x7FF)  return 2;
	if (cp <= 0xFFFF) return 3;
	return 4;
}

// Byte width of a char literal's decoded value. `text` includes the surrounding quotes.
static size_t charLiteralByteWidth(std::string_view text)
{
	if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'')
		text = text.substr(1, text.size() - 2);

	size_t bytes = 0;
	for (size_t i = 0; i < text.size(); )
	{
		if (text[i] == '\\' && i + 1 < text.size())
		{
			const char esc = text[i + 1];
			if (esc == 'x' || esc == 'X')
			{
				bytes += 1;
				i += 2;
				for (int k = 0; k < 2 && i < text.size() && isxdigit(static_cast<unsigned char>(text[i])); ++k)
					++i;
			}
			else if (esc == 'u' || esc == 'U')
			{
				i += 2;
				uint32_t cp = 0;
				if (i < text.size() && text[i] == '{')
				{
					++i;
					while (i < text.size() && text[i] != '}')
					{
						const char c = text[i];
						uint32_t digit = (c >= '0' && c <= '9') ? c - '0'
							: (c >= 'a' && c <= 'f') ? c - 'a' + 10
							: (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
						cp = cp * 16 + digit;
						++i;
					}
					if (i < text.size()) ++i;  // skip '}'
				}
				bytes += utf8EncodedLength(cp);
			}
			else
			{
				bytes += 1;
				i += 2;
			}
		}
		else
		{
			const size_t len = utf8LeadLength(static_cast<unsigned char>(text[i]));
			bytes += len;
			i += len;
		}
	}
	return bytes;
}

// Smallest unsigned primitive that holds a char literal of the given byte width (§1.6).
static TypeId charLiteralType(size_t byteWidth)
{
	if (byteWidth <= 1) return TYPE_U8;
	if (byteWidth == 2) return TYPE_U16;
	if (byteWidth <= 4) return TYPE_U32;
	return TYPE_U64;
}

static TypeId declStorageType(const ResolvedDeclaration& decl, const InternedTypes& interned)
{
	return std::visit(Overloaded
		{
			[&](const Required<AST::FunctionParameter>& param) -> TypeId
			{
				auto it = interned.astTypes.find(&param.value().type.value());
				return it != interned.astTypes.end() ? it->second : INVALID_TYPE_ID;
			},
			[&](const Required<AST::VariableDefinitionStatement>& varDef) -> TypeId
			{
				if (varDef.value().type.hasValue())
				{
					auto it = interned.astTypes.find(&varDef.value().type.value());
					if (it != interned.astTypes.end()) return it->second;
				}
				return INVALID_TYPE_ID;  // inferred — caller must use scope
			},
			[&](const Required<AST::TypeParameter>& tp) -> TypeId
			{
				auto it = interned.typeParamIds.find(&tp.value().name);
				return it != interned.typeParamIds.end() ? it->second : INVALID_TYPE_ID;
			},
			[](const auto&) -> TypeId { return INVALID_TYPE_ID; }
		}, decl.definition);
}

// Strip outermost pointer/reference to get the value type.
// Sentinel TypeIds (INVALID, untyped literals) are not in the table and pass through.
static TypeId stripIndirection(TypeId id, const InternedTypes& interned)
{
	if (id == INVALID_TYPE_ID || id == TYPE_UNTYPED_INT || id == TYPE_UNTYPED_FLOAT)
		return id;
	const auto& info = interned.get(id);
	if (info.isIndirection())
	{
		return info.asIndirection().inner;
	}
	return id;
}

// Resolve TYPE_UNTYPED_INT / TYPE_UNTYPED_FLOAT to their concrete defaults.
static TypeId resolveUntypedLiteral(TypeId id)
{
	if (id == TYPE_UNTYPED_INT)   return TYPE_I32;
	if (id == TYPE_UNTYPED_FLOAT) return TYPE_F32;
	return id;
}

// Walk Named chain on `to` to reach its underlying primitive (for untyped coercion).
static bool untypedIntAssignableTo(TypeId to, const InternedTypes& interned)
{
	// A1: guard against sentinel TypeIds that are not in the table
	if (to == INVALID_TYPE_ID || to == TYPE_UNTYPED_INT || to == TYPE_UNTYPED_FLOAT) return false;
	TypeId cur = to;
	while (cur != INVALID_TYPE_ID)
	{
		const TypeInfo& info = interned.get(cur);
		if (info.kind == TypeInfo::Kind::Named) { cur = info.asNamed().underlying; continue; }
		return info.kind == TypeInfo::Kind::Primitive;  // any primitive (int or float) from integer literal
	}
	return false;
}

static bool untypedFloatAssignableTo(TypeId to, const InternedTypes& interned)
{
	// A1: guard against sentinel TypeIds that are not in the table
	if (to == INVALID_TYPE_ID || to == TYPE_UNTYPED_INT || to == TYPE_UNTYPED_FLOAT) return false;
	TypeId cur = to;
	while (cur != INVALID_TYPE_ID)
	{
		const TypeInfo& info = interned.get(cur);
		if (info.kind == TypeInfo::Kind::Named) { cur = info.asNamed().underlying; continue; }
		return info.kind == TypeInfo::Kind::Primitive && info.asPrimitive().isFloat;
	}
	return false;
}

static bool isAssignable(TypeId from, TypeId to, const InternedTypes& interned)
{
	if (from == INVALID_TYPE_ID || to == INVALID_TYPE_ID) return false;
	if (from == to) return true;

	// Untyped literals coerce to any matching primitive class (incl. through Named chain).
	if (from == TYPE_UNTYPED_INT)   return untypedIntAssignableTo(to, interned);
	if (from == TYPE_UNTYPED_FLOAT) return untypedFloatAssignableTo(to, interned);

	// An untyped literal as the *target* — its type is not yet pinned (e.g. a
	// binding inferred from an untyped literal, `var total = 0;`). It accepts any
	// untyped or primitive value. Handled before the get() calls below because
	// the untyped sentinels are not entries in the interned-type table.
	if (to == TYPE_UNTYPED_INT || to == TYPE_UNTYPED_FLOAT)
	{
		if (from == TYPE_UNTYPED_INT || from == TYPE_UNTYPED_FLOAT)
			return true;
		if (from < static_cast<TypeId>(interned.table.size()))
			return interned.get(from).kind == TypeInfo::Kind::Primitive;
		return false;
	}

	const TypeInfo& fromInfo = interned.get(from);
	const TypeInfo& toInfo = interned.get(to);

	// §3.2/§5.2: a concrete type that implements interface I coerces to an
	// interface object of I. Checked before the Named unwrap because the
	// 'type T : I' markers are recorded against the named type's TypeId.
	if (toInfo.kind == TypeInfo::Kind::Interface)
	{
		auto it = interned.implementedInterfaces.find(from);
		if (it != interned.implementedInterfaces.end())
		{
			for (TypeId iface : it->second)
				if (iface == to)
					return true;
		}
		return false;
	}

	// named: unwrap transitively (named is subtype of its underlying type)
	if (fromInfo.kind == TypeInfo::Kind::Named)
	{
		return isAssignable(fromInfo.asNamed().underlying, to, interned);
	}

	// numeric widening: same sign class (uint / sint / float), target wider
	if (fromInfo.kind == TypeInfo::Kind::Primitive && toInfo.kind == TypeInfo::Kind::Primitive)
	{
		const auto& fp = fromInfo.asPrimitive();
		const auto& tp = toInfo.asPrimitive();
		bool sameClass = (fp.isFloat == tp.isFloat) &&
			(fp.isFloat || fp.isSigned == tp.isSigned);
		if (sameClass && tp.byteWidth > fp.byteWidth)
			return true;
	}

	// [T; N] is assignable to [T] — a fixed array coerces to a slice of the same element type
	if (fromInfo.kind == TypeInfo::Kind::Array && toInfo.kind == TypeInfo::Kind::Slice)
	{
		return isAssignable(fromInfo.asArray().elem, toInfo.asSlice().elem, interned);
	}

	// [T; N] is assignable to [U; N] when T is assignable to U (structural array equivalence)
	if (fromInfo.kind == TypeInfo::Kind::Array && toInfo.kind == TypeInfo::Kind::Array)
	{
		const auto& fa = fromInfo.asArray();
		const auto& ta = toInfo.asArray();
		return fa.size == ta.size && isAssignable(fa.elem, ta.elem, interned);
	}

	// [T] is assignable to [U] when T is assignable to U (structural slice equivalence)
	if (fromInfo.kind == TypeInfo::Kind::Slice && toInfo.kind == TypeInfo::Kind::Slice)
	{
		return isAssignable(fromInfo.asSlice().elem, toInfo.asSlice().elem, interned);
	}

	// Function-type structural compatibility — needed because the interner does
	// not deduplicate Function TypeInfos, so two `(i32) -> i32` types from
	// different sources (a lambda value vs. a param annotation) get distinct ids.
	if (fromInfo.kind == TypeInfo::Kind::Function && toInfo.kind == TypeInfo::Kind::Function)
	{
		const auto& fF = fromInfo.asFunction();
		const auto& tF = toInfo.asFunction();
		if (fF.params.size() != tF.params.size()) return false;
		for (size_t i = 0; i < fF.params.size(); ++i)
			if (!isAssignable(fF.params[i], tF.params[i], interned)
				&& !isAssignable(tF.params[i], fF.params[i], interned))
				return false;
		if (fF.ret.has_value() != tF.ret.has_value()) return false;
		if (fF.ret.has_value() && !isAssignable(*fF.ret, *tF.ret, interned)
			&& !isAssignable(*tF.ret, *fF.ret, interned)) return false;
		return true;
	}

	// §3.3 rule 6 — chained nominal-structural struct compatibility.
	// An anonymous-layout target (Kind::Struct) accepts any value whose internal
	// shape structurally provides every field the target requires (the source may
	// carry extra fields). Named struct targets stay nominal and are not handled here.
	if (fromInfo.kind == TypeInfo::Kind::Struct && toInfo.kind == TypeInfo::Kind::Struct)
	{
		const auto& fm = fromInfo.asStruct().members;
		const auto& tm = toInfo.asStruct().members;

		for (const auto& target : tm)
		{
			bool found = false;
			for (const auto& source : fm)
			{
				if (source.name != target.name)
					continue;
				if (!isAssignable(source.type, target.type, interned))
					return false;
				found = true;
				break;
			}
			if (!found)
				return false;
		}
		return true;
	}

	return false;
}

// A2/A3: unify the value types yielded by the break statements of a loop/match
// (and its else block). Untyped literals are resolved; the widest compatible
// type wins. Returns INVALID_TYPE_ID when no typed break value was found.
static TypeId unifyBreakTypes(const std::vector<TypeId>& types, const InternedTypes& interned)
{
	TypeId result = INVALID_TYPE_ID;
	for (TypeId raw : types)
	{
		TypeId t = resolveUntypedLiteral(raw);
		if (t == INVALID_TYPE_ID)
			continue;
		if (result == INVALID_TYPE_ID || result == t)
		{
			result = t;
			continue;
		}
		if (isAssignable(result, t, interned))
			result = t;          // t is the wider / more general type
		else if (!isAssignable(t, result, interned))
			result = t;          // incompatible — keep the most recent (lenient)
	}
	return result;
}

// A5: §4.2 — RHS expression forms required when assigning to an indirection-typed binding.
static bool rhsIsAddressOf(const AST::Expression& e)
{
	if (auto* u = std::get_if<Required<AST::UnaryExpression>>(&e))
		return u->value().op == TokenKind::BitwiseAnd;
	return false;
}

static bool rhsIsAllocation(const AST::Expression& e)
{
	if (auto* u = std::get_if<Required<AST::UnaryExpression>>(&e))
		return u->value().op == TokenKind::New || u->value().op == TokenKind::Move;
	return false;
}

// A5: validate the RHS form against an indirection-typed declaration (§4.2).
// `rawDeclType` is the *unstripped* declared TypeId; `loc` is the error location.
static void checkIndirectionAssignment(CheckState& state, TypeId rawDeclType,
	const AST::Expression& rhs, const Token& loc)
{
	if (rawDeclType == INVALID_TYPE_ID || rawDeclType >= static_cast<TypeId>(state.interned.table.size()))
		return;

	switch (state.interned.get(rawDeclType).kind)
	{
		case TypeInfo::Kind::Reference:
		case TypeInfo::Kind::RefMut:
			if (!rhsIsAddressOf(rhs))
				state.logger.logErrorInRange(loc, loc,
					"Assignment to a reference type requires the address-of operator "
					"'&' on the right-hand side.");
			break;
		case TypeInfo::Kind::Pointer:
		case TypeInfo::Kind::PtrMut:
			if (!rhsIsAllocation(rhs))
				state.logger.logErrorInRange(loc, loc,
					"Assignment to a heap pointer type requires 'new' or 'move' "
					"on the right-hand side.");
			break;
		default:
			break;
	}
}

// A6: peel .field / [index] suffixes off an lvalue to reach its root identifier.
static const AST::IdentifierExpression* rootIdentifier(const AST::Expression& e)
{
	const AST::Expression* cur = &e;
	while (true)
	{
		if (auto* id = std::get_if<Required<AST::IdentifierExpression>>(cur))
			return id->ptr();
		if (auto* m = std::get_if<Required<AST::MemberAccessExpression>>(cur))
		{
			cur = &m->value().object.value();
			continue;
		}
		if (auto* a = std::get_if<Required<AST::ArrayAccessExpression>>(cur))
		{
			cur = &a->value().object.value();
			continue;
		}
		return nullptr;
	}
}

// A9: is the call-site receiver provably immutable — a 'const' binding or a value
// behind an immutable '&'/'*' indirection? Conservative: returns false when unknown.
static bool receiverIsImmutable(CheckState& state, const AST::Expression& receiver, ScopedVarMap* scope)
{
	const AST::IdentifierExpression* root = rootIdentifier(receiver);
	if (!root)
		return false;

	auto it = state.resolved.names.find(root);
	if (it == state.resolved.names.end() || it->second.empty())
		return false;

	const ResolvedDeclaration* decl = it->second[0];
	TypeId storage = declStorageType(*decl, state.interned);

	if (auto* vd = std::get_if<Required<AST::VariableDefinitionStatement>>(&decl->definition))
	{
		if (storage == INVALID_TYPE_ID && scope)
			storage = scope->lookupVar(&vd->value());
		if (scope && !scope->lookupMutability(&vd->value()))
			return true;   // 'const' binding
	}

	if (storage != INVALID_TYPE_ID && storage < static_cast<TypeId>(state.interned.table.size()))
	{
		TypeInfo::Kind k = state.interned.get(storage).kind;
		if (k == TypeInfo::Kind::Reference || k == TypeInfo::Kind::Pointer)
			return true;   // behind an immutable indirection
	}
	return false;
}

static const AST::Function* getDeclFunction(const ResolvedDeclaration& decl)
{
	if (auto* fn = std::get_if<Required<AST::FunctionDefinition>>(&decl.definition))
	{
		return fn->value().function.ptr();
	}
	return nullptr;
}

static const AST::ListNode<AST::TypeParameter>* getDeclTypeParams(const ResolvedDeclaration& decl)
{
	if (auto* fn = std::get_if<Required<AST::FunctionDefinition>>(&decl.definition))
	{
		return fn->value().typeParameters.ptr();
	}
	return nullptr;
}

static std::vector<TypeId> getParamTypes(const ResolvedDeclaration& decl, const InternedTypes& interned)
{
	const AST::Function* fn = getDeclFunction(decl);
	if (!fn)
	{
		return {};
	}

	std::vector<TypeId> result;
	fn->parameters.forEach([&](const Required<AST::FunctionParameter>& param)
		{
			auto it = interned.astTypes.find(&param.value().type.value());
			result.push_back(it != interned.astTypes.end() ? it->second : INVALID_TYPE_ID);
		});

	return result;
}

// unify a param TypeId against a concrete arg TypeId, filling bindings for TypeParams
static bool unifyParam(TypeId paramId, TypeId argId, std::unordered_map<TypeId, TypeId>& bindings, const InternedTypes& interned)
{
	if (paramId == INVALID_TYPE_ID || argId == INVALID_TYPE_ID)
	{
		return false;
	}

	// substitute already-bound type params
	if (auto it = bindings.find(paramId); it != bindings.end())
	{
		paramId = it->second;
	}

	const TypeInfo& paramInfo = interned.get(paramId);

	if (paramInfo.kind == TypeInfo::Kind::TypeParam)
	{
		auto it = bindings.find(paramId);
		if (it != bindings.end())
		{
			TypeId bound = it->second;
			// An untyped integer / float literal arg defers to its primitive class
			// rather than to its concrete default — so 'add<u64>(10, 20)' binds the
			// literals to u64 even though the default would have been i32.
			auto reachesPrimitive = [&](bool wantFloat)
				{
					TypeId c = bound;
					while (c < (TypeId)interned.table.size()
						&& interned.get(c).kind == TypeInfo::Kind::Named)
						c = interned.get(c).asNamed().underlying;
					return c < (TypeId)interned.table.size()
						&& interned.get(c).kind == TypeInfo::Kind::Primitive
						&& interned.get(c).asPrimitive().isFloat == wantFloat;
				};
			if (argId == TYPE_UNTYPED_INT   && reachesPrimitive(false)) return true;
			if (argId == TYPE_UNTYPED_FLOAT && reachesPrimitive(true))  return true;
			return bound == argId || isAssignable(argId, bound, interned);
		}
		// Bind on first sighting. Resolve untyped literals to their concrete defaults.
		bindings[paramId] = resolveUntypedLiteral(argId);
		return true;
	}

	// indirection: match value types
	if (paramInfo.isIndirection())
	{
		TypeId paramInner = paramInfo.asIndirection().inner;
		TypeId argInner = stripIndirection(argId, interned);
		return unifyParam(paramInner, argInner, bindings, interned);
	}

	return isAssignable(argId, paramId, interned);
}

// substitute TypeParam TypeIds in a type using bindings
static TypeId substituteTypeParams(TypeId id, const std::unordered_map<TypeId, TypeId>& bindings, const InternedTypes& interned)
{
	if (id == INVALID_TYPE_ID)
	{
		return INVALID_TYPE_ID;
	}

	if (auto it = bindings.find(id); it != bindings.end())
	{
		return it->second;
	}

	const TypeInfo& info = interned.get(id);
	if (info.isIndirection())
	{
		TypeId innerSub = substituteTypeParams(info.asIndirection().inner, bindings, interned);
		return (innerSub == info.asIndirection().inner) ? id : innerSub;
	}
	return id;
}

// A1: result of resolving a qualified identifier as an enum-variant path
// (Enum::Variant). enumTypeId is INVALID unless the first path segment names an
// enum TypeDefinition; variantFound indicates the last segment named a variant.
struct EnumVariantInfo
{
	TypeId enumTypeId = INVALID_TYPE_ID;   // the enum's Named TypeId (the value's type)
	bool variantFound = false;
	bool hasPayload = false;
	TypeId payloadType = INVALID_TYPE_ID;
	const Token* variantToken = nullptr;   // last path segment, for diagnostics
};

// A1: classify a qualified identifier (A::B) as an enum-variant access.
static EnumVariantInfo resolveEnumVariant(CheckState& state, const AST::IdentifierExpression& ident)
{
	EnumVariantInfo result;

	const AST::ListNode<const Token*>* first = ident.path.ptr();
	if (!first || !first->next.hasValue())
		return result;   // not a qualified path

	auto declIt = state.resolved.names.find(&ident);
	if (declIt == state.resolved.names.end() || declIt->second.empty())
		return result;

	auto* typeDefReq = std::get_if<Required<AST::TypeDefinition>>(&declIt->second[0]->definition);
	if (!typeDefReq)
		return result;

	auto namedIt = state.interned.namedTypeIds.find(&typeDefReq->value().name);
	if (namedIt == state.interned.namedTypeIds.end())
		return result;

	// unwrap the Named chain to the underlying type
	TypeId cur = namedIt->second;
	while (cur != INVALID_TYPE_ID && cur < static_cast<TypeId>(state.interned.table.size())
		&& state.interned.get(cur).kind == TypeInfo::Kind::Named)
		cur = state.interned.get(cur).asNamed().underlying;

	if (cur == INVALID_TYPE_ID || cur >= static_cast<TypeId>(state.interned.table.size())
		|| state.interned.get(cur).kind != TypeInfo::Kind::Enum)
		return result;   // first segment is a type, but not an enum

	result.enumTypeId = namedIt->second;

	// the last path segment names the variant
	const AST::ListNode<const Token*>* node = first;
	while (node->next.hasValue())
		node = node->next.ptr();
	result.variantToken = node->item.value();
	std::string_view variantName = state.getStringView(*result.variantToken);

	for (const auto& v : state.interned.get(cur).asEnum().variants)
	{
		if (v.name == variantName)
		{
			result.variantFound = true;
			result.hasPayload = v.payloadType.has_value();
			if (result.hasPayload)
				result.payloadType = *v.payloadType;
			break;
		}
	}
	return result;
}

// check whether a TypeId satisfies a BuiltinInterface constraint
static bool satisfiesConstraint(TypeId id, BuiltinInterface iface, const InternedTypes& interned)
{
	if (id == INVALID_TYPE_ID)
	{
		return false;
	}

	TypeId cur = id;
	while (true)
	{
		const TypeInfo& info = interned.get(cur);
		if (info.kind == TypeInfo::Kind::Named)
		{
			cur = info.asNamed().underlying;
			continue;
		}
		// Nested-generic case: a generic body may bind one TypeParam to another
		// (a caller's). Defer the constraint check to the eventual concrete
		// instantiation by accepting any TypeParam optimistically here.
		if (info.kind == TypeInfo::Kind::TypeParam)
			return true;
		// A4: Iterable has no primitive member set — it is satisfied structurally
		// by fixed arrays and slices (and, transparently, dynamic heap arrays).
		if (iface == BuiltinInterface::Iterable)
			return info.kind == TypeInfo::Kind::Array || info.kind == TypeInfo::Kind::Slice;
		if (info.kind == TypeInfo::Kind::Primitive)
		{
			auto it = s_BuiltinInterfaceMembers.find(iface);
			return it != s_BuiltinInterfaceMembers.end() && it->second.count(info.asPrimitive().name);
		}
		return false;
	}
}

// A4: check whether a concrete type satisfies a user-defined interface bound —
// i.e. its 'type T : I' markers (interned into implementedInterfaces) include I.
static bool satisfiesInterfaceConstraint(TypeId id, TypeId interfaceTypeId, const InternedTypes& interned)
{
	if (id == INVALID_TYPE_ID || interfaceTypeId == INVALID_TYPE_ID)
		return false;
	auto it = interned.implementedInterfaces.find(id);
	if (it == interned.implementedInterfaces.end())
		return false;
	for (TypeId iface : it->second)
		if (iface == interfaceTypeId)
			return true;
	return false;
}

// try a generic overload for a given arg-type list
// on success, fills outBindings and returns the substituted return TypeId
static TypeId tryGenericOverload(const ResolvedDeclaration& decl, const std::vector<TypeId>& argTypes, std::unordered_map<TypeId, TypeId>& outBindings, const InternedTypes& interned)
{
	const AST::Function* fn = getDeclFunction(decl);
	if (!fn)
	{
		return INVALID_TYPE_ID;
	}

	const AST::ListNode<AST::TypeParameter>* tpList = getDeclTypeParams(decl);
	if (!tpList)
	{
		return INVALID_TYPE_ID;
	}

	auto paramTypes = getParamTypes(decl, interned);
	if (paramTypes.size() != argTypes.size())
	{
		return INVALID_TYPE_ID;
	}

	// D1: start with any pre-seeded bindings (explicit type args) passed in via outBindings
	std::unordered_map<TypeId, TypeId> bindings = outBindings;
	for (size_t i = 0; i < paramTypes.size(); ++i)
	{
		// Pass the raw arg TypeId — unifyParam now defers untyped-literal resolution
		// until after consulting any pre-seeded bindings, so 'add<u64>(10, 20)' binds
		// the literals to the explicit primitive class instead of the i32 default.
		if (!unifyParam(paramTypes[i], argTypes[i], bindings, interned))
		{
			return INVALID_TYPE_ID;
		}
	}

	// validate constraints
	bool ok = true;
	tpList->forEach([&](const Required<AST::TypeParameter>& tp)
		{
			if (!tp.value().interface.hasValue())
			{
				return;
			}

			auto tpIt = interned.typeParamIds.find(&tp.value().name);
			if (tpIt == interned.typeParamIds.end())
			{
				return;
			}

			TypeId tpId = tpIt->second;
			auto bindIt = bindings.find(tpId);
			if (bindIt == bindings.end())
			{
				ok = false;
				return;
			}

			const TypeInfo& tpInfo = interned.get(tpId);
			if (tpInfo.kind == TypeInfo::Kind::TypeParam)
			{
				const auto& tpData = tpInfo.asTypeParam();
				if (tpData.constraint.has_value()
					&& !satisfiesConstraint(bindIt->second, *tpData.constraint, interned))
				{
					ok = false;
				}
				// A4: user-defined interface bound (fn f<T: MyInterface>).
				if (tpData.interfaceConstraint.has_value()
					&& !satisfiesInterfaceConstraint(bindIt->second, *tpData.interfaceConstraint, interned))
				{
					ok = false;
				}
			}
		});

	if (!ok)
	{
		return INVALID_TYPE_ID;
	}

	// compute substituted return type
	if (!fn->returnType.hasValue())
	{
		return TYPE_U32;  // void-ish sentinel
	}

	auto retIt = interned.astTypes.find(&fn->returnType.value());
	if (retIt == interned.astTypes.end())
	{
		return INVALID_TYPE_ID;
	}

	outBindings = std::move(bindings);
	return substituteTypeParams(retIt->second, outBindings, interned);
}

static TypeId checkExpression(CheckState& state, const AST::Expression& expr, ScopedVarMap* scope, TypeId contextType)
{
	TypeId resultType = INVALID_TYPE_ID;

	std::visit(Overloaded
		{
			// identifier
			[&](const Required<AST::IdentifierExpression>& ident)
			{
				auto resolvedIt = state.resolved.names.find(&ident.value());
				if (resolvedIt == state.resolved.names.end() || resolvedIt->second.empty())
				{
					return;
				}

				const ResolvedDeclaration* decl = resolvedIt->second[0];

				// variable with explicit annotation
				TypeId storageId = declStorageType(*decl, state.interned);
				if (storageId != INVALID_TYPE_ID)
				{
					resultType = stripIndirection(storageId, state.interned);
					resultType = state.resolveTypeParam(resultType);
					return;
				}

				// variable with inferred type, look up in scope
				if (auto* varDef = std::get_if<Required<AST::VariableDefinitionStatement>>(&decl->definition))
				{
					if (scope)
					{
						TypeId inferred = scope->lookupVar(&varDef->value());
						if (inferred != INVALID_TYPE_ID)
						{
							// An inferred binding stores its full type (e.g. &T for `var r = &x`);
							// using it as a value transparently dereferences to T (§4.2).
							resultType = stripIndirection(inferred, state.interned);
							return;
						}
					}
				}

				// B2: capture variable — type populated by for/if/match/lambda handlers
				if (auto* cap = std::get_if<Required<AST::Capture>>(&decl->definition))
				{
					if (scope) resultType = scope->lookupCapture(&cap->value());
					return;
				}

				// C4: function reference (identifier used as a value, not a call)
				if (auto* fnDef = std::get_if<Required<AST::FunctionDefinition>>(&decl->definition))
				{
					const AST::Function& fn = fnDef->value().function.value();
					TypeInfo::FunctionData data;
					fn.parameters.forEach([&](const Required<AST::FunctionParameter>& param)
					{
						auto it = state.interned.astTypes.find(&param.value().type.value());
						data.params.push_back(it != state.interned.astTypes.end() ? it->second : INVALID_TYPE_ID);
					});
					if (fn.returnType.hasValue())
					{
						auto it = state.interned.astTypes.find(&fn.returnType.value());
						if (it != state.interned.astTypes.end())
							data.ret = it->second;
					}
					TypeInfo fnInfo;
					fnInfo.kind = TypeInfo::Kind::Function;
					fnInfo.data = std::move(data);
					resultType = state.internType(std::move(fnInfo));
					return;
				}

				// A1: enum variant used as a value (Code::Success). A payload-less
				// variant is a complete value of the enum type. A payload-carrying
				// variant referenced without an argument list is incomplete and is
				// left untyped (lenient — see §7.3 of the spec).
				{
					EnumVariantInfo ev = resolveEnumVariant(state, ident.value());
					if (ev.enumTypeId != INVALID_TYPE_ID && ev.variantFound && !ev.hasPayload)
						resultType = ev.enumTypeId;
				}
			},

		// literal
		[&](const Required<AST::LiteralExpression>& lit)
		{
			switch (lit.value().value.kind)
			{
				case TokenKind::IntegerLiteral: resultType = TYPE_UNTYPED_INT;   break;
				case TokenKind::FloatLiteral:   resultType = TYPE_UNTYPED_FLOAT; break;
				case TokenKind::StringLiteral:
				{
					// §1.6/§4.3: a string literal is a sequence of integral numbers —
					// typed as &[u8], an unmanaged view over the literal data.
					TypeInfo sliceInfo;
					sliceInfo.kind = TypeInfo::Kind::Slice;
					sliceInfo.data = TypeInfo::SliceData{ TYPE_U8 };
					TypeId sliceId = state.internType(std::move(sliceInfo));
					TypeInfo refInfo;
					refInfo.kind = TypeInfo::Kind::Reference;
					refInfo.data = TypeInfo::IndirectionData{ sliceId };
					resultType = state.internType(std::move(refInfo));
					break;
				}
				case TokenKind::True:
				case TokenKind::False:          resultType = TYPE_BOOL; break;
				case TokenKind::CharLiteral:
				{
					// Char literal width scales with its byte representation (§1.6).
					const size_t width = charLiteralByteWidth(state.getStringView(lit.value().value));
					if (width == 0 || width > 8)
					{
						state.logger.logErrorInRange(lit.value().value, lit.value().value,
							"Char literal must be between 1 and 8 bytes.");
						resultType = TYPE_U8;
					}
					else
					{
						resultType = charLiteralType(width);
					}
					break;
				}
				default: break;
			}
		},

		// unary
		[&](const Required<AST::UnaryExpression>& unary)
		{
			// new/& propagate contextType so array literals inside can infer element type
			TypeId innerType = checkExpression(state, unary.value().expression.value(), scope, contextType);
			switch (unary.value().op)
			{
				case TokenKind::BitwiseAnd:
				{
					// &expr takes the address of expr — its type is &T, so a binding
					// initialised from it is correctly inferred as a reference type.
					if (innerType == INVALID_TYPE_ID) { resultType = INVALID_TYPE_ID; break; }
					TypeInfo refInfo;
					refInfo.kind = TypeInfo::Kind::Reference;
					refInfo.data = TypeInfo::IndirectionData{ innerType };
					resultType = state.internType(std::move(refInfo));
					break;
				}
				case TokenKind::New:
				case TokenKind::Move:
				{
					// new/move yield a managed heap pointer — the expression's type is *T.
					if (innerType == INVALID_TYPE_ID) { resultType = INVALID_TYPE_ID; break; }
					TypeInfo ptrInfo;
					ptrInfo.kind = TypeInfo::Kind::Pointer;
					ptrInfo.data = TypeInfo::IndirectionData{ innerType };
					resultType = state.internType(std::move(ptrInfo));
					break;
				}
				default:
					resultType = innerType;
					break;
			}
		},

		// binary
		[&](const Required<AST::BinaryExpression>& binary)
		{
			TypeId leftType = checkExpression(state, binary.value().left.value(), scope);
			TypeId rightType = checkExpression(state, binary.value().right.value(), scope);

			switch (binary.value().op)
			{
				case TokenKind::Equal:
				case TokenKind::NotEqual:
				case TokenKind::Less:
				case TokenKind::LessEqual:
				case TokenKind::Greater:
				case TokenKind::GreaterEqual:
					resultType = TYPE_BOOL;
					return;
				default:
					break;
			}

			if (leftType == INVALID_TYPE_ID) { resultType = rightType; return; }
			if (rightType == INVALID_TYPE_ID) { resultType = leftType;  return; }

			// Untyped + concrete → concrete wins; untyped + untyped → propagate untyped.
			if (leftType == TYPE_UNTYPED_INT && rightType == TYPE_UNTYPED_INT)   { resultType = TYPE_UNTYPED_INT;   return; }
			if (leftType == TYPE_UNTYPED_FLOAT && rightType == TYPE_UNTYPED_FLOAT){ resultType = TYPE_UNTYPED_FLOAT; return; }
			if (leftType  == TYPE_UNTYPED_INT || leftType  == TYPE_UNTYPED_FLOAT) { resultType = rightType; return; }
			if (rightType == TYPE_UNTYPED_INT || rightType == TYPE_UNTYPED_FLOAT) { resultType = leftType;  return; }

			// Use wider / target type for arithmetic.
			if (isAssignable(leftType, rightType, state.interned))
				resultType = rightType;
			else if (isAssignable(rightType, leftType, state.interned))
				resultType = leftType;
			else
				resultType = leftType;
		},

		// --- Array access ---
		[&](const Required<AST::ArrayAccessExpression>& access)
		{
			TypeId objType = checkExpression(state, access.value().object.value(), scope);
			checkExpression(state, access.value().index.value(), scope);
			if (objType == INVALID_TYPE_ID || objType >= static_cast<TypeId>(state.interned.table.size())) return;
			const TypeInfo& info = state.interned.get(objType);
			if (info.kind == TypeInfo::Kind::Slice) resultType = info.asSlice().elem;
			else if (info.kind == TypeInfo::Kind::Array) resultType = info.asArray().elem;
		},

		// --- Member access (field) ---
		[&](const Required<AST::MemberAccessExpression>& access)
		{
			TypeId objType = checkExpression(state, access.value().object.value(), scope);
			if (objType == INVALID_TYPE_ID || objType >= static_cast<TypeId>(state.interned.table.size())) return;

			// Unwrap Named types.
			TypeId cur = objType;
			while (state.interned.get(cur).kind == TypeInfo::Kind::Named)
				cur = state.interned.get(cur).asNamed().underlying;

			const TypeInfo& objInfo = state.interned.get(cur);
			if (objInfo.kind != TypeInfo::Kind::Struct) return;  // method call — handled by FunctionCall

			auto memberName = state.getStringView(access.value().memberName);
			for (const auto& member : objInfo.asStruct().members)
			{
				if (member.name == memberName) { resultType = member.type; return; }
			}

			state.logger.logErrorInRange(access.value().memberName, access.value().memberName,
				"No field '{}' on type.", memberName);
		},

		// --- Function call ---
		[&](const Required<AST::FunctionCallExpression>& call)
		{
			// Compute argument types.
			std::vector<TypeId> argTypes;
			call.value().arguments.forEach([&](const Required<AST::Expression>& arg)
			{
				argTypes.push_back(checkExpression(state, arg.value(), scope));
			});

			// Gather candidates and detect method calls.
			std::vector<const ResolvedDeclaration*> candidates;
			bool isMemberCall = false;
			TypeId selfType = INVALID_TYPE_ID;
			const AST::Expression* receiverExpr = nullptr;   // A9: member-call receiver
			const Token* memberCallTok = nullptr;            // A9: method-name token (diagnostics)
			const Token* callTok = nullptr;                  // A10: callee token (diagnostics)
			const AST::Expression& calleeExpr = call.value().function.value();

			// A1: enum variant construction — Enum::Variant(payload). The callee
			// resolves to the enum's TypeDefinition; type the call as the enum
			// type and check the argument against the variant payload type.
			if (auto* enumIdent = std::get_if<Required<AST::IdentifierExpression>>(&calleeExpr))
			{
				EnumVariantInfo ev = resolveEnumVariant(state, enumIdent->value());
				if (ev.enumTypeId != INVALID_TYPE_ID)
				{
					const Token& loc = *ev.variantToken;
					std::string_view variantName = state.getStringView(loc);

					if (!ev.variantFound)
					{
						state.logger.logErrorInRange(loc, loc,
							"'{}' is not a variant of the enum.", variantName);
					}
					else if (ev.hasPayload)
					{
						const AST::Expression* firstArg = nullptr;
						call.value().arguments.forEach([&](const Required<AST::Expression>& a)
						{
							if (!firstArg) firstArg = &a.value();
						});

						if (argTypes.size() != 1 || !firstArg)
						{
							state.logger.logErrorInRange(loc, loc,
								"Enum variant '{}' expects a single payload argument.", variantName);
						}
						else
						{
							// Re-check the argument with the payload type as context so
							// untyped literals inside it concretise correctly.
							TypeId payloadArg = checkExpression(state, *firstArg, scope, ev.payloadType);
							if (payloadArg != INVALID_TYPE_ID &&
								!isAssignable(resolveUntypedLiteral(payloadArg), ev.payloadType, state.interned))
							{
								state.logger.logErrorInRange(loc, loc,
									"Enum variant '{}' payload type mismatch.", variantName);
							}
						}
					}
					else if (!argTypes.empty())
					{
						state.logger.logErrorInRange(loc, loc,
							"Enum variant '{}' carries no payload.", variantName);
					}

					resultType = ev.enumTypeId;
					return;
				}
			}

			if (auto* identReq = std::get_if<Required<AST::IdentifierExpression>>(&calleeExpr))
			{
				auto it = state.resolved.names.find(&identReq->value());
				if (it != state.resolved.names.end())
					candidates = it->second;
				callTok = identReq->value().path.value().item.value();
			}
			else if (auto* memberReq = std::get_if<Required<AST::MemberAccessExpression>>(&calleeExpr))
			{
				isMemberCall = true;
				selfType = checkExpression(state, memberReq->value().object.value(), scope);
				receiverExpr = &memberReq->value().object.value();
				memberCallTok = &memberReq->value().memberName;
				callTok = memberCallTok;
				auto methodName = state.getStringView(memberReq->value().memberName);
				// Built-in methods (5.1): reinterpret<T>, convert<T>, length.
				{
					TypeId selfValue = selfType;
					while (selfValue != INVALID_TYPE_ID && selfValue < static_cast<TypeId>(state.interned.table.size())
						&& state.interned.get(selfValue).kind == TypeInfo::Kind::Named)
						selfValue = state.interned.get(selfValue).asNamed().underlying;

					const bool selfIsIterable = selfValue != INVALID_TYPE_ID
						&& selfValue < static_cast<TypeId>(state.interned.table.size())
						&& (state.interned.get(selfValue).kind == TypeInfo::Kind::Array
							|| state.interned.get(selfValue).kind == TypeInfo::Kind::Slice);

					bool builtinHandled = false;
					for (const auto& bf : s_BuiltinFunctions)
					{
						if (bf.name != methodName)
							continue;

						if (methodName == "length")
						{
							// length is the Iterable query; if the receiver is not
							// iterable, fall through so a user method can still match.
							if (!selfIsIterable)
								break;
							resultType = TYPE_U64;
							builtinHandled = true;
						}
						else
						{
							// reinterpret<T> / convert<T> result is the type argument T
							TypeId taType = INVALID_TYPE_ID;
							if (call.value().typeArguments.hasValue())
							{
								const AST::Type& firstTa = call.value().typeArguments.value().item.value();
								auto taIt = state.interned.astTypes.find(&firstTa);
								if (taIt != state.interned.astTypes.end())
									taType = stripIndirection(taIt->second, state.interned);
							}
							resultType = taType;
							builtinHandled = true;
						}
						break;
					}
					if (builtinHandled)
						return;
				}

				// Dispatch through an interface object: handle.method() where handle: &I.
				{
					TypeId sv = selfType;
					while (sv != INVALID_TYPE_ID && sv < static_cast<TypeId>(state.interned.table.size())
						&& state.interned.get(sv).kind == TypeInfo::Kind::Named)
						sv = state.interned.get(sv).asNamed().underlying;

					if (sv != INVALID_TYPE_ID && sv < static_cast<TypeId>(state.interned.table.size())
						&& state.interned.get(sv).kind == TypeInfo::Kind::Interface)
					{
						const AST::InterfaceDefinition* ifaceDecl = state.interned.get(sv).asInterface().decl;
						bool methodFound = false;
						if (ifaceDecl)
						{
							ifaceDecl->functions.forEach([&](const Required<AST::InterfaceFunction>& f)
							{
								if (methodFound) return;
								if (state.getStringView(f.value().name) != methodName) return;
								methodFound = true;
								if (f.value().returnType.hasValue())
								{
									auto rIt = state.interned.astTypes.find(&f.value().returnType.value());
									if (rIt != state.interned.astTypes.end())
										resultType = stripIndirection(rIt->second, state.interned);
								}
							});
						}
						if (methodFound)
							return;
					}
				}

				auto overloads = state.moduleSymbols.get(methodName);

				for (const auto* decl : overloads)
				{
					const AST::Function* fn = getDeclFunction(*decl);
					if (!fn) continue;
					const auto* paramList = fn->parameters.ptr();
					if (!paramList) continue;
					const auto& firstParam = paramList->item.value();
					if (!firstParam.isSelf) continue;

					// Accept if first param's value type is compatible with selfType,
					// or if first param is a TypeParam (generic extension).
					auto it = state.interned.astTypes.find(&firstParam.type.value());
					if (it == state.interned.astTypes.end()) continue;
					TypeId fpStorageId = it->second;
					TypeId fpValueId = stripIndirection(fpStorageId, state.interned);

					bool selfOk =
						state.interned.get(fpValueId).kind == TypeInfo::Kind::TypeParam ||
						fpValueId == INVALID_TYPE_ID ||
						isAssignable(selfType, fpValueId, state.interned);

					if (selfOk) candidates.push_back(decl);
				}

				// Prepend selfType as implicit first arg.
				argTypes.insert(argTypes.begin(), selfType);
			}
			else
			{
				// Lambda or other expression — evaluate and use its function return type.
				TypeId calleeType = checkExpression(state, calleeExpr, scope);
				if (calleeType != INVALID_TYPE_ID)
				{
					const TypeInfo& info = state.interned.get(calleeType);
					if (info.kind == TypeInfo::Kind::Function)
						resultType = info.asFunction().ret.value_or(INVALID_TYPE_ID);
				}
				return;
			}

			if (candidates.empty()) return;

			// Split generic / non-generic.
			std::vector<const ResolvedDeclaration*> nonGeneric, generic;
			for (const auto* decl : candidates)
			{
				if (getDeclTypeParams(*decl)) generic.push_back(decl);
				else                          nonGeneric.push_back(decl);
			}

			const ResolvedDeclaration* selected = nullptr;
			TypeId                     selectedRetId = INVALID_TYPE_ID;

			// --- Exact match sweep ---
			std::vector<const ResolvedDeclaration*> exactMatches;
			for (const auto* decl : nonGeneric)
			{
				// Extern functions: accept any arg list (variadic / no checking).
				if (std::holds_alternative<Required<AST::ExternDefinition>>(decl->definition))
				{
					exactMatches.push_back(decl);
					continue;
				}

				auto paramTypes = getParamTypes(*decl, state.interned);
				if (paramTypes.size() != argTypes.size()) continue;

				bool allMatch = true;
				for (size_t i = 0; i < paramTypes.size(); ++i)
				{
					TypeId pv  = stripIndirection(paramTypes[i], state.interned);
					TypeId arg = stripIndirection(argTypes[i], state.interned);
					if (arg != INVALID_TYPE_ID && pv != INVALID_TYPE_ID &&
						!isAssignable(arg, pv, state.interned))
					{
						allMatch = false;
						break;
					}
				}
				if (allMatch) exactMatches.push_back(decl);
			}

			// Override (5.2): a type-specific extension wins over an interface default.
			if (isMemberCall && exactMatches.size() > 1)
			{
				std::vector<const ResolvedDeclaration*> specific;
				for (const auto* decl : exactMatches)
				{
					const AST::Function* fn = getDeclFunction(*decl);
					if (!fn || !fn->parameters.ptr()) continue;
					auto pit = state.interned.astTypes.find(&fn->parameters.ptr()->item.value().type.value());
					if (pit == state.interned.astTypes.end()) continue;
					if (stripIndirection(pit->second, state.interned) == selfType)
						specific.push_back(decl);
				}
				if (specific.size() == 1)
					exactMatches = specific;
			}

			if (exactMatches.size() >= 1)
			{
				selected = exactMatches[0];
				if (exactMatches.size() > 1)
				{
					if (callTok)
						state.logger.logErrorInRange(*callTok, *callTok,
							"Ambiguous function call - multiple non-generic overloads match.");
					else
						Log::error("Ambiguous function call - multiple non-generic overloads match.");
				}

				const AST::Function* fn = getDeclFunction(*selected);
				if (fn && fn->returnType.hasValue())
				{
					auto it = state.interned.astTypes.find(&fn->returnType.value());
					if (it != state.interned.astTypes.end())
						selectedRetId = stripIndirection(it->second, state.interned);
				}
				else if (auto* ext = std::get_if<Required<AST::ExternDefinition>>(&selected->definition))
				{
					// extern declarations carry their return type directly (5.3)
					if (ext->value().returnType.hasValue())
					{
						auto it = state.interned.astTypes.find(&ext->value().returnType.value());
						if (it != state.interned.astTypes.end())
							selectedRetId = stripIndirection(it->second, state.interned);
					}
				}
			}

			// --- Generic fallback ---
			if (!selected)
			{
				// D1: if explicit type arguments are present, pre-bind type params before inference
				std::unordered_map<TypeId, TypeId> explicitBindings;
				if (call.value().typeArguments.hasValue())
				{
					// We need a candidate to match against — try all generics, pre-binding explicit args
					// The explicit args are matched positionally against the first generic candidate's TypeParams.
					// (All candidates in the generic set have the same type-param positions by convention.)
					for (const auto* decl : generic)
					{
						const auto* tpList = getDeclTypeParams(*decl);
						if (!tpList) continue;
						size_t argIdx = 0;
						call.value().typeArguments.value().forEach([&](const Required<AST::Type>& ta)
						{
							auto taIt = state.interned.astTypes.find(&ta.value());
							if (taIt == state.interned.astTypes.end()) { ++argIdx; return; }
							TypeId taId = taIt->second;
							// Walk to the argIdx-th TypeParameter
							size_t idx = 0;
							tpList->forEach([&](const Required<AST::TypeParameter>& tp)
							{
								if (idx++ != argIdx) return;
								auto tpIt = state.interned.typeParamIds.find(&tp.value().name);
								if (tpIt != state.interned.typeParamIds.end())
									explicitBindings[tpIt->second] = taId;
							});
							++argIdx;
						});
						break;  // one candidate suffices for positional binding
					}
				}

				std::vector<std::pair<const ResolvedDeclaration*, std::unordered_map<TypeId, TypeId>>> genericMatches;
				for (const auto* decl : generic)
				{
					std::unordered_map<TypeId, TypeId> bindings = explicitBindings;  // seed with explicit args
					TypeId retId = tryGenericOverload(*decl, argTypes, bindings, state.interned);
					if (retId != INVALID_TYPE_ID)
						genericMatches.push_back({ decl, std::move(bindings) });
				}

				if (!genericMatches.empty())
				{
					if (genericMatches.size() > 1)
					{
						if (callTok)
							state.logger.logErrorInRange(*callTok, *callTok,
								"Ambiguous function call - multiple generic overloads match.");
						else
							Log::error("Ambiguous function call - multiple generic overloads match.");
					}

					selected = genericMatches[0].first;
					auto& bindings = genericMatches[0].second;

					// Record token-keyed type args.
					std::unordered_map<const Token*, TypeId> tokenBindings;
					const auto* tpList = getDeclTypeParams(*selected);
					if (tpList)
					{
						tpList->forEach([&](const Required<AST::TypeParameter>& tp)
						{
							auto tpIt = state.interned.typeParamIds.find(&tp.value().name);
							if (tpIt == state.interned.typeParamIds.end()) return;
							auto bindIt = bindings.find(tpIt->second);
							if (bindIt != bindings.end())
								tokenBindings[&tp.value().name] = bindIt->second;
						});
					}
					state.result.typeArgs[&call.value()] = std::move(tokenBindings);

					// Substituted return type.
					const AST::Function* fn = getDeclFunction(*selected);
					if (fn && fn->returnType.hasValue())
					{
						auto it = state.interned.astTypes.find(&fn->returnType.value());
						if (it != state.interned.astTypes.end())
							selectedRetId = substituteTypeParams(it->second, bindings, state.interned);
					}
				}
			}

			if (selected)
				state.result.selectedOverloads[&call.value()] = selected;

			// A9: §5.2 item 3 — a method whose `self` receiver is a mutable
			// indirection (&var/*var) cannot be invoked on an immutable receiver.
			if (isMemberCall && selected && receiverExpr && memberCallTok)
			{
				if (const AST::Function* fn = getDeclFunction(*selected))
				{
					if (const auto* paramList = fn->parameters.ptr())
					{
						const auto& selfParam = paramList->item.value();
						auto it = state.interned.astTypes.find(&selfParam.type.value());
						if (selfParam.isSelf && it != state.interned.astTypes.end()
							&& it->second < static_cast<TypeId>(state.interned.table.size()))
						{
							TypeInfo::Kind k = state.interned.get(it->second).kind;
							const bool mutableSelf =
								k == TypeInfo::Kind::RefMut || k == TypeInfo::Kind::PtrMut;
							if (mutableSelf && receiverIsImmutable(state, *receiverExpr, scope))
								state.logger.logErrorInRange(*memberCallTok, *memberCallTok,
									"Method '{}' requires a mutable receiver ('&var'/'*var' self), "
									"but the receiver is immutable.",
									state.getStringView(*memberCallTok));
						}
					}
				}
			}

			resultType = selectedRetId;
		},

		// --- Array literal ---
		[&](const Required<AST::ArrayLiteralExpression>& lit)
		{
			// Extract expected element type from contextType (Slice or Array annotation).
			TypeId elemContext = INVALID_TYPE_ID;
			if (contextType != INVALID_TYPE_ID)
			{
				TypeId cur = contextType;
				while (state.interned.get(cur).kind == TypeInfo::Kind::Named)
					cur = state.interned.get(cur).asNamed().underlying;
				const TypeInfo& ctxInfo = state.interned.get(cur);
				if (ctxInfo.kind == TypeInfo::Kind::Slice)      elemContext = ctxInfo.asSlice().elem;
				else if (ctxInfo.kind == TypeInfo::Kind::Array) elemContext = ctxInfo.asArray().elem;
			}

			TypeId elemType = INVALID_TYPE_ID;
			size_t count = 0;
			lit.value().elements.forEach([&](const Required<AST::Expression>& elem)
			{
				TypeId t = checkExpression(state, elem.value(), scope, elemContext);
				if (elemType == INVALID_TYPE_ID) elemType = t;
				++count;
			});

			// Resolve untyped literal to concrete type: prefer context, fall back to default.
			if (elemType == TYPE_UNTYPED_INT || elemType == TYPE_UNTYPED_FLOAT)
			{
				if (elemContext != INVALID_TYPE_ID)
					elemType = elemContext;
				else
					elemType = (elemType == TYPE_UNTYPED_INT) ? TYPE_I32 : TYPE_F32;
			}

			if (elemType != INVALID_TYPE_ID)
			{
				TypeInfo arrayInfo;
				arrayInfo.kind = TypeInfo::Kind::Array;
				arrayInfo.data = TypeInfo::ArrayData{ elemType, count };
				resultType = state.internType(std::move(arrayInfo));
			}
		},

		// --- Array fill ---
		[&](const Required<AST::ArrayFillExpression>& fill)
		{
			// Determine element context from contextType annotation (Slice or Array).
			TypeId elemContext = INVALID_TYPE_ID;
			if (contextType != INVALID_TYPE_ID)
			{
				TypeId cur = contextType;
				while (state.interned.get(cur).kind == TypeInfo::Kind::Named)
					cur = state.interned.get(cur).asNamed().underlying;
				const TypeInfo& ctxInfo = state.interned.get(cur);
				if (ctxInfo.kind == TypeInfo::Kind::Slice)      elemContext = ctxInfo.asSlice().elem;
				else if (ctxInfo.kind == TypeInfo::Kind::Array) elemContext = ctxInfo.asArray().elem;
			}

			TypeId elemType = checkExpression(state, fill.value().value.value(), scope, elemContext);

			// Resolve untyped literal to concrete type.
			if (elemType == TYPE_UNTYPED_INT || elemType == TYPE_UNTYPED_FLOAT)
			{
				if (elemContext != INVALID_TYPE_ID)
					elemType = elemContext;
				else
					elemType = (elemType == TYPE_UNTYPED_INT) ? TYPE_I32 : TYPE_F32;
			}

			// Parse size from the integer literal token.
			const Token& sizeTok = fill.value().size;
			std::string_view sizeText = state.getStringView(sizeTok);
			size_t arraySize = 0;
			for (char ch : sizeText)
				arraySize = arraySize * 10 + static_cast<size_t>(ch - '0');

			if (elemType != INVALID_TYPE_ID)
			{
				TypeInfo arrayInfo;
				arrayInfo.kind = TypeInfo::Kind::Array;
				arrayInfo.data = TypeInfo::ArrayData{ elemType, arraySize };
				resultType = state.internType(std::move(arrayInfo));
			}
		},

		// --- Struct initializer ---
		[&](const Required<AST::StructInitializerExpression>& init)
		{
			// C2: if named type known, extract member types for contextual typing
			TypeId namedTypeId = INVALID_TYPE_ID;
			const TypeInfo::StructData* structData = nullptr;
			if (init.value().type.hasValue())
			{
				const auto& nameIdent = init.value().type.value().name.value();
				auto resolvedIt = state.resolved.names.find(&nameIdent);
				if (resolvedIt != state.resolved.names.end() && !resolvedIt->second.empty())
				{
					if (auto* td = std::get_if<Required<AST::TypeDefinition>>(&resolvedIt->second[0]->definition))
					{
						auto namedIt = state.interned.namedTypeIds.find(&td->value().name);
						if (namedIt != state.interned.namedTypeIds.end())
						{
							namedTypeId = namedIt->second;
							// Unwrap Named → Struct to get member types
							TypeId cur = namedTypeId;
							while (cur != INVALID_TYPE_ID && cur < static_cast<TypeId>(state.interned.table.size()))
							{
								const TypeInfo& info = state.interned.get(cur);
								if (info.kind == TypeInfo::Kind::Named) { cur = info.asNamed().underlying; continue; }
								if (info.kind == TypeInfo::Kind::Struct) structData = &info.asStruct();
								break;
							}
						}
					}
				}
			}

			// A1: with no explicit named type, fall back to contextType (e.g. an
			// enum-variant payload or an annotated variable) so member initializers
			// still get contextual typing and untyped literals concretise correctly.
			if (!structData && contextType != INVALID_TYPE_ID)
			{
				TypeId cur = contextType;
				while (cur != INVALID_TYPE_ID && cur < static_cast<TypeId>(state.interned.table.size())
					&& state.interned.get(cur).kind == TypeInfo::Kind::Named)
					cur = state.interned.get(cur).asNamed().underlying;
				if (cur != INVALID_TYPE_ID && cur < static_cast<TypeId>(state.interned.table.size())
					&& state.interned.get(cur).kind == TypeInfo::Kind::Struct)
					structData = &state.interned.get(cur).asStruct();
			}

			// Check each member initializer, using its declared member type as context (C2)
			// and collect inferred types for anonymous struct (C1)
			TypeInfo::StructData inferredStruct;
			init.value().initializers.forEach([&](const Required<AST::StructInitializerExpression::MemberInitializer>& mi)
			{
				auto memberName = state.getStringView(mi.value().name);

				TypeId memberCtx = INVALID_TYPE_ID;
				if (structData)
				{
					for (const auto& m : structData->members)
						if (m.name == memberName) { memberCtx = m.type; break; }
				}

				TypeId memberType = checkExpression(state, mi.value().value.value(), scope, memberCtx);

				// Resolve untyped literals if context provided
				if ((memberType == TYPE_UNTYPED_INT || memberType == TYPE_UNTYPED_FLOAT) && memberCtx != INVALID_TYPE_ID)
					memberType = memberCtx;
				else if (memberType == TYPE_UNTYPED_INT)   memberType = TYPE_I32;
				else if (memberType == TYPE_UNTYPED_FLOAT) memberType = TYPE_F32;

				inferredStruct.members.push_back({ memberName, memberType });
			});

			if (namedTypeId != INVALID_TYPE_ID)
			{
				resultType = namedTypeId;  // named struct initializer → Named TypeId
			}
			else
			{
				// C1: anonymous struct — intern a Struct TypeId from inferred member types
				TypeInfo structInfo;
				structInfo.kind = TypeInfo::Kind::Struct;
				structInfo.data = std::move(inferredStruct);
				resultType = state.internType(std::move(structInfo));
			}
		},

		// --- Lambda ---
		[&](const Required<AST::LambdaExpression>& lambda)
		{
			// B6: resolve capture types from the outer scope via resolved.tokenNames
			// We pass a pre-built captures map into checkFunction via a wrapper scope.
			ScopedVarMap lambdaCaptureScope{ scope };
			if (lambda.value().captures.hasValue())
			{
				lambda.value().captures.value().forEach([&](const Required<AST::Capture>& cap)
				{
					// tokenNames maps the capture's variable token → declaration in outer scope
					auto it = state.resolved.tokenNames.find(&cap.value().variableName);
					if (it == state.resolved.tokenNames.end()) return;
					const ResolvedDeclaration* outerDecl = it->second;

					TypeId capturedType = INVALID_TYPE_ID;
					TypeId storageId = declStorageType(*outerDecl, state.interned);
					if (storageId != INVALID_TYPE_ID)
						capturedType = stripIndirection(storageId, state.interned);
					else if (auto* vd = std::get_if<Required<AST::VariableDefinitionStatement>>(&outerDecl->definition))
						capturedType = scope ? scope->lookupVar(&vd->value()) : INVALID_TYPE_ID;

					lambdaCaptureScope.captures[&cap.value()] = capturedType;
				});
			}

			checkFunction(state, lambda.value().function.value(), {}, &lambdaCaptureScope);

			// C3: intern a Function TypeId for the lambda
			const AST::Function& fn = lambda.value().function.value();
			TypeInfo::FunctionData data;
			fn.parameters.forEach([&](const Required<AST::FunctionParameter>& param)
			{
				auto it = state.interned.astTypes.find(&param.value().type.value());
				data.params.push_back(it != state.interned.astTypes.end() ? it->second : INVALID_TYPE_ID);
			});
			if (fn.returnType.hasValue())
			{
				auto it = state.interned.astTypes.find(&fn.returnType.value());
				if (it != state.interned.astTypes.end())
					data.ret = it->second;
			}
			TypeInfo fnInfo;
			fnInfo.kind = TypeInfo::Kind::Function;
			fnInfo.data = std::move(data);
			resultType = state.internType(std::move(fnInfo));
		},

		// --- If expression ---
		[&](const Required<AST::IfExpression>& ifExpr)
		{
			TypeId condType = checkExpression(state, ifExpr.value().condition.value(), scope);

			// An 'if' is a value-yielding construct: a `break value;` in either
			// branch makes the 'if' evaluate to that value. 'break' targets the
			// innermost loop / match / if, so install a fresh collector here.
			std::vector<TypeId> breaks;
			std::vector<TypeId>* prevCollector = state.breakCollector;
			state.breakCollector = &breaks;

			// B4: if a capture is present, bind it to the condition's value type
			if (ifExpr.value().capture.hasValue())
			{
				ScopedVarMap ifScope{ scope };
				ifScope.captures[&ifExpr.value().capture.value()] = condType;
				checkStatement(state, ifExpr.value().thenBranch.value(), &ifScope);
				if (ifExpr.value().elseBranch.hasValue())
					checkStatement(state, ifExpr.value().elseBranch.value(), scope);
			}
			else
			{
				checkStatement(state, ifExpr.value().thenBranch.value(), scope);
				if (ifExpr.value().elseBranch.hasValue())
					checkStatement(state, ifExpr.value().elseBranch.value(), scope);
			}

			state.breakCollector = prevCollector;
			resultType = unifyBreakTypes(breaks, state.interned);
		},

		// --- While expression ---
		[&](const Required<AST::WhileExpression>& whileExpr)
		{
			const bool isStatement = state.constructInStatementPosition;
			state.constructInStatementPosition = false;

			checkExpression(state, whileExpr.value().condition.value(), scope);

			// A2/A3: collect break values from the body (and else block) so the
			// while can be used as a value-producing expression.
			std::vector<TypeId> breaks;
			std::vector<TypeId>* prevCollector = state.breakCollector;
			state.breakCollector = &breaks;
			checkStatement(state, whileExpr.value().body.value(), scope);
			if (whileExpr.value().elseBody.hasValue())
				checkStatement(state, whileExpr.value().elseBody.value(), scope);
			state.breakCollector = prevCollector;

			const bool hasElse = whileExpr.value().elseBody.hasValue();
			if (hasElse && isStatement)
				Log::error("An 'else' clause on a 'while' loop is only valid when the loop is used as a value expression.");

			resultType = unifyBreakTypes(breaks, state.interned);

			if (hasElse && !isStatement && resultType == INVALID_TYPE_ID)
				Log::error("A 'while' expression with an 'else' clause must yield a value via 'break'.");
		},

		// --- For expression ---
		[&](const Required<AST::ForExpression>& forExpr)
		{
			const bool isStatement = state.constructInStatementPosition;
			state.constructInStatementPosition = false;

			// B3: collect iterable element types in order
			std::vector<TypeId> elemTypes;
			forExpr.value().iterables.value().forEach([&](const Required<AST::Expression>& it)
			{
				TypeId iterType = checkExpression(state, it.value(), scope);
				TypeId elemT = INVALID_TYPE_ID;
				if (iterType != INVALID_TYPE_ID && iterType < static_cast<TypeId>(state.interned.table.size()))
				{
					const TypeInfo& info = state.interned.get(iterType);
					if (info.kind == TypeInfo::Kind::Array)      elemT = info.asArray().elem;
					else if (info.kind == TypeInfo::Kind::Slice) elemT = info.asSlice().elem;
				}
				elemTypes.push_back(elemT);
			});

			// Build a child scope and populate captures paired with their element types
			ScopedVarMap forScope{ scope };
			size_t capIdx = 0;
			if (forExpr.value().iterators.hasValue())
			{
				forExpr.value().iterators.value().forEach([&](const Required<AST::Capture>& cap)
				{
					TypeId elemT = capIdx < elemTypes.size() ? elemTypes[capIdx] : INVALID_TYPE_ID;
					forScope.captures[&cap.value()] = elemT;
					++capIdx;
				});
			}

			// A2/A3: collect break values from the body (and else block).
			std::vector<TypeId> breaks;
			std::vector<TypeId>* prevCollector = state.breakCollector;
			state.breakCollector = &breaks;
			checkStatement(state, forExpr.value().body.value(), &forScope);
			if (forExpr.value().elseBody.hasValue())
				checkStatement(state, forExpr.value().elseBody.value(), &forScope);
			state.breakCollector = prevCollector;

			const bool hasElse = forExpr.value().elseBody.hasValue();
			if (hasElse && isStatement)
				Log::error("An 'else' clause on a 'for' loop is only valid when the loop is used as a value expression.");

			resultType = unifyBreakTypes(breaks, state.interned);

			if (hasElse && !isStatement && resultType == INVALID_TYPE_ID)
				Log::error("A 'for' expression with an 'else' clause must yield a value via 'break'.");
		},

		// --- Match expression ---
		[&](const Required<AST::MatchExpression>& match)
		{
			const bool isStatement = state.constructInStatementPosition;
			state.constructInStatementPosition = false;

			TypeId subjectType = checkExpression(state, match.value().subject.value(), scope);

			// A2/A3: collect break values from every arm body and the external
			// else block so the match can be used as a value-producing expression.
			std::vector<TypeId> breaks;
			std::vector<TypeId>* prevCollector = state.breakCollector;
			state.breakCollector = &breaks;

			// B5: resolve the enum type to get variant payload types
			TypeId enumTypeId = INVALID_TYPE_ID;
			if (subjectType != INVALID_TYPE_ID && subjectType < static_cast<TypeId>(state.interned.table.size()))
			{
				TypeId cur = subjectType;
				while (cur != INVALID_TYPE_ID && cur < static_cast<TypeId>(state.interned.table.size()))
				{
					const TypeInfo& info = state.interned.get(cur);
					if (info.kind == TypeInfo::Kind::Named) { cur = info.asNamed().underlying; continue; }
					if (info.kind == TypeInfo::Kind::Enum)  { enumTypeId = cur; }
					break;
				}
			}

			match.value().arms.forEach([&](const Required<AST::MatchArm>& arm)
			{
				// Identify the variant name when the pattern is an identifier path.
				std::string_view variantName;
				bool patternIsIdent = false;
				if (arm.value().pattern.hasValue())
				{
					const AST::Expression& pat = arm.value().pattern.value();
					if (auto* identReq = std::get_if<Required<AST::IdentifierExpression>>(&pat))
					{
						patternIsIdent = true;
						const AST::ListNode<const Token*>* node = identReq->value().path.ptr();
						while (node && node->next.hasValue())
							node = node->next.ptr();
						if (node) variantName = state.getStringView(*node->item.value());
					}
					checkExpression(state, pat, scope, subjectType);
				}

				if (arm.value().capture.hasValue())
				{
					// §4.3: a pattern capture is valid only on an enum-variant pattern.
					// Only flag it when the subject is typed and is definitively not an
					// enum — an untyped subject cannot be judged here.
					TypeId captureType = INVALID_TYPE_ID;

					if (subjectType != INVALID_TYPE_ID && enumTypeId == INVALID_TYPE_ID)
					{
						const Token& capTok = arm.value().capture.value().variableName;
						state.logger.logErrorInRange(capTok, capTok,
							"Pattern capture is only valid when matching an enum variant.");
					}
					else if (enumTypeId != INVALID_TYPE_ID && patternIsIdent)
					{
						const TypeInfo& enumInfo = state.interned.get(enumTypeId);
						for (const auto& variant : enumInfo.asEnum().variants)
						{
							if (variant.name == variantName)
							{
								if (variant.payloadType.has_value())
									captureType = *variant.payloadType;
								else
								{
									// §7.3: a payload-less variant has no value to capture.
									const Token& capTok =
										arm.value().capture.value().variableName;
									state.logger.logErrorInRange(capTok, capTok,
										"Pattern capture not allowed on payload-less "
										"variant '{}'.", variant.name);
								}
								break;
							}
						}
					}

					ScopedVarMap armScope{ scope };
					armScope.captures[&arm.value().capture.value()] = captureType;
					checkStatement(state, arm.value().body.value(), &armScope);
				}
				else
				{
					checkStatement(state, arm.value().body.value(), scope);
				}
			});

			if (match.value().externalElse.hasValue())
				checkStatement(state, match.value().externalElse.value(), scope);

			state.breakCollector = prevCollector;

			const bool hasElse = match.value().externalElse.hasValue();
			if (hasElse && isStatement)
				Log::error("An external 'else' block on a 'match' is only valid when the match is used as a value expression.");

			resultType = unifyBreakTypes(breaks, state.interned);

			if (hasElse && !isStatement && resultType == INVALID_TYPE_ID)
				Log::error("A 'match' expression with an external 'else' block must yield a value via 'break'.");
		},

		// --- Comptime expression ---
		// A ComptimeExpression surviving to the checker was not replaced by the
		// comptime-evaluation pass (evaluation failed, or it is a deferred macro
		// call). Check the inner expression leniently; the pass already reported
		// any evaluation error. It yields no value type.
		[&](const Required<AST::ComptimeExpression>& comptime)
		{
			checkExpression(state, comptime.value().inner.value(), scope);
		},

		// --- Comptime result (§6.1) ---
		// A compile-time value substituted in by the comptime-evaluation pass.
		[&](const Required<AST::ComptimeResultExpression>& result)
		{
			using RK = AST::ComptimeResultExpression::ResultKind;
			switch (result.value().kind)
			{
				case RK::Integer: resultType = TYPE_UNTYPED_INT;   break;
				case RK::Float:   resultType = TYPE_UNTYPED_FLOAT; break;
				case RK::Bool:    resultType = TYPE_BOOL;          break;
				case RK::Char:
				{
					const uint32_t w = result.value().charWidth;
					resultType = (w >= 1 && w <= 8) ? charLiteralType(w) : TYPE_U8;
					break;
				}
				case RK::String:
				{
					// §1.6/§4.3 — a string value is typed &[u8].
					TypeInfo sliceInfo;
					sliceInfo.kind = TypeInfo::Kind::Slice;
					sliceInfo.data = TypeInfo::SliceData{ TYPE_U8 };
					TypeId sliceId = state.internType(std::move(sliceInfo));
					TypeInfo refInfo;
					refInfo.kind = TypeInfo::Kind::Reference;
					refInfo.data = TypeInfo::IndirectionData{ sliceId };
					resultType = state.internType(std::move(refInfo));
					break;
				}
				case RK::Invalid: resultType = INVALID_TYPE_ID; break;
			}
		},

		}, expr);

	state.setExprType(expr, resultType);
	return resultType;
}

// ---------------------------------------------------------------------------
// Statement checking
// ---------------------------------------------------------------------------

static void checkStatement(CheckState& state, const AST::Statement& stmt, ScopedVarMap* scope)
{
	std::visit(Overloaded
		{
			[&](const Required<AST::VariableDefinitionStatement>& varDef)
			{
				// Pre-compute annotation type so we can pass it as contextType to the RHS expression
				// (enables contextual typing for array literals and other context-sensitive constructs).
				TypeId annotatedValueType = INVALID_TYPE_ID;
				TypeId rawAnnotated = INVALID_TYPE_ID;
				if (varDef.value().type.hasValue())
				{
					auto it = state.interned.astTypes.find(&varDef.value().type.value());
					if (it != state.interned.astTypes.end())
					{
						rawAnnotated = it->second;
						annotatedValueType = stripIndirection(it->second, state.interned);
					}
				}

				TypeId valueT = checkExpression(state, varDef.value().value.value(), scope, annotatedValueType);

				if (annotatedValueType != INVALID_TYPE_ID)
				{
					// Compare value types: an indirection-typed RHS (&x, new e) is
					// matched against the annotation's stripped value type.
					if (valueT != INVALID_TYPE_ID
						&& !isAssignable(stripIndirection(valueT, state.interned), annotatedValueType, state.interned))
					{
						state.logger.logErrorInRange(varDef.value().name, varDef.value().name,
							"Type mismatch in variable declaration '{}'.",
							state.getStringView(varDef.value().name));
					}
					valueT = annotatedValueType;
				}

				// A5: §4.2 — a reference-typed binding requires '&' on the RHS;
				// a heap-pointer-typed binding requires 'new' or 'move'.
				if (rawAnnotated != INVALID_TYPE_ID)
					checkIndirectionAssignment(state, rawAnnotated,
						varDef.value().value.value(), varDef.value().name);

				if (scope)
				{
					scope->locals[&varDef.value()] = valueT;
					scope->mutability[&varDef.value()] = varDef.value().isMutable;  // D3
				}
			},

			[&](const Required<AST::AssignmentStatement>& assign)
			{
				TypeId targetType = checkExpression(state, assign.value().target.value(), scope);
				TypeId valueT = checkExpression(state, assign.value().value.value(), scope, targetType);

				// D3: flag assignment to immutable const binding
				if (auto* identReq = std::get_if<Required<AST::IdentifierExpression>>(&assign.value().target.value()))
				{
					auto resolvedIt = state.resolved.names.find(&identReq->value());
					if (resolvedIt != state.resolved.names.end() && !resolvedIt->second.empty())
					{
						if (auto* vd = std::get_if<Required<AST::VariableDefinitionStatement>>(&resolvedIt->second[0]->definition))
						{
							if (scope && !scope->lookupMutability(&vd->value()))
							{
								state.logger.logErrorInRange(vd->value().name, vd->value().name,
									"Cannot assign to immutable binding '{}'.",
									state.getStringView(vd->value().name));
							}
						}
					}
				}

				// A6: §4.2 — mutating a field/element *through* an indirection, or
				// through an immutable binding, when the access chain is rooted in
				// an immutable reference/pointer (`&`/`*`) or a `const` binding.
				{
					const AST::Expression& tgt = assign.value().target.value();
					const bool throughAccess =
						std::holds_alternative<Required<AST::MemberAccessExpression>>(tgt) ||
						std::holds_alternative<Required<AST::ArrayAccessExpression>>(tgt);
					if (throughAccess)
					{
						// A6 extension: walk every '.field' along the chain (skipping
						// the outermost step — that's the target slot itself, not a
						// step we mutate THROUGH). At each intermediate MemberAccess
						// look up the field's declared storage type and reject when
						// it is an immutable reference / pointer.
						{
							const AST::Expression* cur =
								std::holds_alternative<Required<AST::MemberAccessExpression>>(tgt)
								? &std::get<Required<AST::MemberAccessExpression>>(tgt).value().object.value()
								: &std::get<Required<AST::ArrayAccessExpression>>(tgt).value().object.value();
							while (true)
							{
								if (auto* m = std::get_if<Required<AST::MemberAccessExpression>>(cur))
								{
									TypeId objTy = checkExpression(state, m->value().object.value(), scope);
									while (objTy != INVALID_TYPE_ID
										&& objTy < static_cast<TypeId>(state.interned.table.size())
										&& state.interned.get(objTy).kind == TypeInfo::Kind::Named)
										objTy = state.interned.get(objTy).asNamed().underlying;
									if (objTy != INVALID_TYPE_ID
										&& objTy < static_cast<TypeId>(state.interned.table.size())
										&& state.interned.get(objTy).kind == TypeInfo::Kind::Struct)
									{
										auto memberName = state.getStringView(m->value().memberName);
										for (const auto& fld : state.interned.get(objTy).asStruct().members)
										{
											if (fld.name != memberName) continue;
											TypeId ft = fld.type;
											if (ft != INVALID_TYPE_ID
												&& ft < static_cast<TypeId>(state.interned.table.size()))
											{
												TypeInfo::Kind k = state.interned.get(ft).kind;
												if (k == TypeInfo::Kind::Reference
													|| k == TypeInfo::Kind::Pointer)
													state.logger.logErrorInRange(
														m->value().memberName, m->value().memberName,
														"Cannot mutate through immutable reference / "
														"pointer field '{}'; declare it as '&var' or '*var'.",
														memberName);
											}
											break;
										}
									}
									cur = &m->value().object.value();
								}
								else if (auto* a = std::get_if<Required<AST::ArrayAccessExpression>>(cur))
								{
									cur = &a->value().object.value();
								}
								else break;
							}
						}

						if (const AST::IdentifierExpression* root = rootIdentifier(tgt))
						{
							auto rIt = state.resolved.names.find(root);
							if (rIt != state.resolved.names.end() && !rIt->second.empty())
							{
								const ResolvedDeclaration* decl = rIt->second[0];
								const Token* tok = root->path.value().item.value();

								TypeId storage = declStorageType(*decl, state.interned);
								if (auto* vd = std::get_if<Required<AST::VariableDefinitionStatement>>(&decl->definition))
								{
									if (storage == INVALID_TYPE_ID && scope)
										storage = scope->lookupVar(&vd->value());
									if (scope && !scope->lookupMutability(&vd->value()))
										state.logger.logErrorInRange(*tok, *tok,
											"Cannot mutate through immutable binding '{}'.",
											state.getStringView(*tok));
								}

								if (storage != INVALID_TYPE_ID && storage < static_cast<TypeId>(state.interned.table.size()))
								{
									TypeInfo::Kind k = state.interned.get(storage).kind;
									if (k == TypeInfo::Kind::Reference || k == TypeInfo::Kind::Pointer)
										state.logger.logErrorInRange(*tok, *tok,
											"Cannot mutate through an immutable reference or pointer; "
											"declare '{}' as '&var' or '*var'.", state.getStringView(*tok));
								}
							}
						}
					}
				}

				// D4 + type check: for all assignment operators (= and compound),
				// RHS value type must be assignable to LHS value type.
				if (targetType != INVALID_TYPE_ID && valueT != INVALID_TYPE_ID)
				{
					if (!isAssignable(stripIndirection(valueT, state.interned),
						stripIndirection(targetType, state.interned), state.interned))
					{
						if (const AST::IdentifierExpression* r = rootIdentifier(assign.value().target.value()))
						{
							const Token* t = r->path.value().item.value();
							state.logger.logErrorInRange(*t, *t, "Type mismatch in assignment.");
						}
						else
							Log::error("Type mismatch in assignment.");
					}
				}

				// A5: §4.2 — plain '=' to an indirection-typed binding requires the
				// matching RHS form ('&' for references, 'new'/'move' for pointers).
				if (assign.value().op == TokenKind::Assign)
				{
					if (auto* identReq = std::get_if<Required<AST::IdentifierExpression>>(&assign.value().target.value()))
					{
						auto resolvedIt = state.resolved.names.find(&identReq->value());
						if (resolvedIt != state.resolved.names.end() && !resolvedIt->second.empty())
						{
							TypeId rawTarget = declStorageType(*resolvedIt->second[0], state.interned);
							const Token* tok = identReq->value().path.value().item.value();
							checkIndirectionAssignment(state, rawTarget,
								assign.value().value.value(), *tok);
						}
					}
				}
			},

			[&](const Required<AST::ExpressionStatement>& exprStmt)
			{
				checkExpression(state, exprStmt.value().expression.value(), scope);
			},

			[&](const Required<AST::StatementBlock>& block)
			{
				ScopedVarMap blockScope{ scope };
				block.value().statements.forEach([&](const Required<AST::Statement>& s)
				{
					checkStatement(state, s.value(), &blockScope);
				});
			},

			[&](const Required<AST::BreakStatement>& brk)
			{
				if (brk.value().value.hasValue())
				{
					TypeId t = checkExpression(state, brk.value().value.value(), scope);
					// A2/A3: contribute this break's value type to the enclosing loop/match.
					if (state.breakCollector && t != INVALID_TYPE_ID)
						state.breakCollector->push_back(t);
				}
			},

			[&](const Required<AST::ReturnStatement>& ret)
			{
				// D2: check return value type against function's declared return type
				if (ret.value().value.hasValue())
				{
					TypeId retCtx = state.currentReturnType != INVALID_TYPE_ID
						? stripIndirection(state.currentReturnType, state.interned)
						: INVALID_TYPE_ID;
					TypeId retType = checkExpression(state, ret.value().value.value(), scope, retCtx);
					if (state.currentReturnType != INVALID_TYPE_ID && retType != INVALID_TYPE_ID)
					{
						TypeId expectedVal = stripIndirection(state.currentReturnType, state.interned);
						if (!isAssignable(stripIndirection(retType, state.interned), expectedVal, state.interned))
						{
							if (const AST::IdentifierExpression* r = rootIdentifier(ret.value().value.value()))
							{
								const Token* t = r->path.value().item.value();
								state.logger.logErrorInRange(*t, *t, "Return type mismatch.");
							}
							else
								Log::error("Return type mismatch.");
						}
					}
				}
			},

			[&](const Required<AST::IfExpression>& ifExpr)
			{
				checkExpression(state, AST::Expression(ifExpr), scope);
			},
			[&](const Required<AST::ForExpression>& forExpr)
			{
				// A2/A3: reached in statement position — a trailing `else` is illegal here.
				state.constructInStatementPosition = true;
				checkExpression(state, AST::Expression(forExpr), scope);
			},
			[&](const Required<AST::WhileExpression>& whileExpr)
			{
				state.constructInStatementPosition = true;
				checkExpression(state, AST::Expression(whileExpr), scope);
			},
			[&](const Required<AST::MatchExpression>& matchExpr)
			{
				state.constructInStatementPosition = true;
				checkExpression(state, AST::Expression(matchExpr), scope);
			},
		}, stmt);
}

// ---------------------------------------------------------------------------
// Function checking
// ---------------------------------------------------------------------------

static void checkFunction(CheckState& state, const AST::Function& fn,
	const std::unordered_map<TypeId, TypeId>& typeParamBindings,
	ScopedVarMap* parentScope,
	TypeId expectedReturnType)
{
	auto prevBindings = state.typeParamBindings;
	auto prevReturnType = state.currentReturnType;
	state.typeParamBindings = typeParamBindings;
	state.currentReturnType = expectedReturnType;

	ScopedVarMap fnScope{ parentScope };
	fn.parameters.forEach([&](const Required<AST::FunctionParameter>& param)
		{
			auto it = state.interned.astTypes.find(&param.value().type.value());
			if (it != state.interned.astTypes.end())
			{
				TypeId pv = stripIndirection(it->second, state.interned);
				pv = state.resolveTypeParam(pv);
				fnScope.params[&param.value()] = pv;
			}
		});

	checkStatement(state, AST::Statement(fn.body), &fnScope);

	state.typeParamBindings = prevBindings;
	state.currentReturnType = prevReturnType;
}

// ---------------------------------------------------------------------------
// Interface verification pass (§5.2)
// ---------------------------------------------------------------------------

static TypeId astTypeId(const CheckState& state, const AST::Type& type)
{
	auto it = state.interned.astTypes.find(&type);
	return it != state.interned.astTypes.end() ? it->second : INVALID_TYPE_ID;
}

// Does `decl` name an extension function satisfying interface function `ifFn` for type `namedTypeId`?
// `namedTypeId` is the implementing type T; `interfaceTypeId` is the interface I itself
// (a self receiver of I marks a default implementation, §5.2).
static bool extensionSatisfiesInterfaceFn(const CheckState& state, const ResolvedDeclaration& decl,
	const AST::InterfaceFunction& ifFn, TypeId namedTypeId, TypeId interfaceTypeId)
{
	const AST::Function* fn = getDeclFunction(decl);
	if (!fn)
		return false;

	const auto* paramList = fn->parameters.ptr();
	if (!paramList)
		return false;  // an extension function needs at least the 'self' receiver

	const auto& firstParam = paramList->item.value();
	if (!firstParam.isSelf)
		return false;

	// the receiver's value type must be the implementing type T (a type-specific
	// extension) or the interface I itself (a shared default implementation)
	TypeId selfValue = stripIndirection(astTypeId(state, firstParam.type.value()), state.interned);
	if (selfValue != namedTypeId && selfValue != interfaceTypeId)
		return false;

	// extension parameters following 'self' must match the interface signature exactly
	std::vector<TypeId> extParams;
	for (const AST::ListNode<AST::FunctionParameter>* node = paramList->next.ptr(); node; node = node->next.ptr())
		extParams.push_back(astTypeId(state, node->item.value().type.value()));

	std::vector<TypeId> ifParams;
	ifFn.parameters.forEach([&](const Required<AST::FunctionParameter>& p)
		{
			ifParams.push_back(astTypeId(state, p.value().type.value()));
		});

	if (extParams.size() != ifParams.size())
		return false;
	for (size_t i = 0; i < extParams.size(); ++i)
		if (extParams[i] != ifParams[i])
			return false;

	TypeId extRet = fn->returnType.hasValue() ? astTypeId(state, fn->returnType.value()) : INVALID_TYPE_ID;
	TypeId ifRet = ifFn.returnType.hasValue() ? astTypeId(state, ifFn.returnType.value()) : INVALID_TYPE_ID;
	return extRet == ifRet;
}

static void verifyInterfaces(CheckState& state, const AST::Module& module)
{
	module.definitions.forEach([&](const Required<AST::Definition>& def)
		{
			auto* typeDefReq = std::get_if<Required<AST::TypeDefinition>>(&def.value().definition);
			if (!typeDefReq)
				return;

			const AST::TypeDefinition& typeDef = typeDefReq->value();
			if (!typeDef.interfaces.hasValue())
				return;

			auto namedIt = state.interned.namedTypeIds.find(&typeDef.name);
			if (namedIt == state.interned.namedTypeIds.end())
				return;
			const TypeId namedTypeId = namedIt->second;

			typeDef.interfaces.forEach([&](const Required<const Token*>& markerReq)
				{
					const Token* marker = markerReq.value();
					auto riIt = state.resolved.resolvedInterfaces.find(marker);
					if (riIt == state.resolved.resolvedInterfaces.end())
						return;  // unresolved — resolver already reported it

					std::visit(Overloaded
						{
							[&](BuiltinInterface bi)
							{
								// structural check on the named type's underlying layout
								TypeId cur = namedTypeId;
								while (cur != INVALID_TYPE_ID && cur < static_cast<TypeId>(state.interned.table.size())
									&& state.interned.get(cur).kind == TypeInfo::Kind::Named)
									cur = state.interned.get(cur).asNamed().underlying;

								if (cur == INVALID_TYPE_ID || cur >= static_cast<TypeId>(state.interned.table.size()))
									return;

								const TypeInfo& info = state.interned.get(cur);
								bool ok = false;
								if (bi == BuiltinInterface::Number)
								{
									auto memIt = s_BuiltinInterfaceMembers.find(BuiltinInterface::Number);
									ok = info.kind == TypeInfo::Kind::Primitive && memIt != s_BuiltinInterfaceMembers.end()
										&& memIt->second.count(info.asPrimitive().name) != 0;
								}
								else if (bi == BuiltinInterface::Iterable)
								{
									ok = info.kind == TypeInfo::Kind::Array || info.kind == TypeInfo::Kind::Slice;
								}

								if (!ok)
									state.logger.logErrorInRange(typeDef.name, typeDef.name,
										"Type '{}' does not satisfy its built-in interface marker.",
										state.getStringView(typeDef.name));
							},
							[&](const ResolvedDeclaration* ifaceDecl)
							{
								auto* ifaceReq = std::get_if<Required<AST::InterfaceDefinition>>(&ifaceDecl->definition);
								if (!ifaceReq)
									return;

								// the interface's own TypeId — a self receiver of this type
								// marks a default implementation (§5.2).
								TypeId interfaceTypeId = INVALID_TYPE_ID;
								auto ifaceIdIt = state.interned.interfaceIds.find(&ifaceReq->value());
								if (ifaceIdIt != state.interned.interfaceIds.end())
									interfaceTypeId = ifaceIdIt->second;

								ifaceReq->value().functions.forEach([&](const Required<AST::InterfaceFunction>& ifFn)
									{
										auto fnName = state.getStringView(ifFn.value().name);

										bool found = false;
										for (const auto* candidate : state.moduleSymbols.get(fnName))
										{
											if (extensionSatisfiesInterfaceFn(state, *candidate, ifFn.value(), namedTypeId, interfaceTypeId))
											{
												found = true;
												break;
											}
										}

										if (!found)
											state.logger.logErrorInRange(typeDef.name, typeDef.name,
												"Type '{}' does not satisfy interface: no matching extension function '{}'.",
												state.getStringView(typeDef.name), fnName);
									});
							},
						}, riIt->second);
				});
		});
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

Result<TypedModule> typeCheck(
	const Source& source,
	const AST::Module& module,
	const ResolvedModule& resolved,
	InternedTypes& interned,
	const SymbolTable& moduleSymbols)
{
	// A2: reserve headroom so internType() pushes don't reallocate the vector
	// and invalidate any live const TypeInfo& references held during expression checking.
	interned.table.reserve(interned.table.size() + 4096);

	CheckState state(source, resolved, interned, moduleSymbols);

	module.definitions.forEach([&](const Required<AST::Definition>& def)
		{
			std::visit(Overloaded
				{
					[&](const Required<AST::TypeDefinition>&) {},
					[&](const Required<AST::FunctionDefinition>& fnDef)
					{
					// Parametric: check body with unbound type params (identity bindings).
					const AST::Function& fn = fnDef.value().function.value();
					TypeId retTypeId = INVALID_TYPE_ID;
					if (fn.returnType.hasValue())
					{
						auto it = state.interned.astTypes.find(&fn.returnType.value());
						if (it != state.interned.astTypes.end())
							retTypeId = it->second;
					}
					checkFunction(state, fn, {}, nullptr, retTypeId);
				},
				[&](const Required<AST::ExternDefinition>&) {},
				[&](const Required<AST::InterfaceDefinition>&) {},
				[&](const Required<AST::MacroDefinition>&) {},
				}, def.value().definition);
		});

	// 5.2: every interface-marked type must provide the required extension functions.
	verifyInterfaces(state, module);

	Status status = state.logger.hasError() ? Error : Ok;
	return { status, std::move(state.result) };
}
