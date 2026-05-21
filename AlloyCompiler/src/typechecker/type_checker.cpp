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

// Strip outermost pointer/reference to get the value type
static TypeId stripIndirection(TypeId id, const InternedTypes& interned)
{
	if (id == INVALID_TYPE_ID) return INVALID_TYPE_ID;
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

	const TypeInfo& fromInfo = interned.get(from);
	const TypeInfo& toInfo = interned.get(to);

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

	// structural struct equivalence (same member names + order + assignable types)
	if (fromInfo.kind == TypeInfo::Kind::Struct && toInfo.kind == TypeInfo::Kind::Struct)
	{
		const auto& fm = fromInfo.asStruct().members;
		const auto& tm = toInfo.asStruct().members;
		if (fm.size() != tm.size())
		{
			return false;
		}

		for (size_t i = 0; i < fm.size(); ++i)
		{
			if (fm[i].name != tm[i].name) return false;
			if (!isAssignable(fm[i].type, tm[i].type, interned)) return false;
		}
		return true;
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
			return it->second == argId || isAssignable(argId, it->second, interned);
		bindings[paramId] = argId;
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
		if (info.kind == TypeInfo::Kind::Primitive)
		{
			auto it = s_BuiltinInterfaceMembers.find(iface);
			return it != s_BuiltinInterfaceMembers.end() && it->second.count(info.asPrimitive().name);
		}
		return false;
	}
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
		// Normalize untyped literals to concrete defaults before generic unification
		// so constraint checks (e.g. T: Number) work on real primitive TypeIds.
		TypeId argId = resolveUntypedLiteral(argTypes[i]);
		if (!unifyParam(paramTypes[i], argId, bindings, interned))
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
			if (tpInfo.kind == TypeInfo::Kind::TypeParam && tpInfo.asTypeParam().constraint.has_value())
			{
				if (!satisfiesConstraint(bindIt->second, *tpInfo.asTypeParam().constraint, interned))
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
							resultType = inferred;
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
			},

		// literal
		[&](const Required<AST::LiteralExpression>& lit)
		{
			switch (lit.value().value.kind)
			{
				case TokenKind::IntegerLiteral: resultType = TYPE_UNTYPED_INT;   break;
				case TokenKind::FloatLiteral:   resultType = TYPE_UNTYPED_FLOAT; break;
				case TokenKind::StringLiteral:  resultType = TYPE_U8;  break;  // *[u8], simplified
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
					// &expr → value type stays T (the reference is the storage; callee uses value type).
					resultType = innerType;
					break;
				case TokenKind::Move:
					// move expr — value type is still T (caller assigns to *T variable).
					resultType = innerType;
					break;
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
			const AST::Expression& calleeExpr = call.value().function.value();

			if (auto* identReq = std::get_if<Required<AST::IdentifierExpression>>(&calleeExpr))
			{
				auto it = state.resolved.names.find(&identReq->value());
				if (it != state.resolved.names.end())
					candidates = it->second;
			}
			else if (auto* memberReq = std::get_if<Required<AST::MemberAccessExpression>>(&calleeExpr))
			{
				isMemberCall = true;
				selfType = checkExpression(state, memberReq->value().object.value(), scope);
				auto methodName = state.getStringView(memberReq->value().memberName);
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
					TypeId arg = argTypes[i];
					if (arg != INVALID_TYPE_ID && pv != INVALID_TYPE_ID &&
						!isAssignable(arg, pv, state.interned))
					{
						allMatch = false;
						break;
					}
				}
				if (allMatch) exactMatches.push_back(decl);
			}

			if (exactMatches.size() >= 1)
			{
				selected = exactMatches[0];
				if (exactMatches.size() > 1)
					Log::error("Ambiguous function call — multiple non-generic overloads match.");

				const AST::Function* fn = getDeclFunction(*selected);
				if (fn && fn->returnType.hasValue())
				{
					auto it = state.interned.astTypes.find(&fn->returnType.value());
					if (it != state.interned.astTypes.end())
						selectedRetId = stripIndirection(it->second, state.interned);
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
						Log::error("Ambiguous function call — multiple generic overloads match.");

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
		},

		// --- While expression ---
		[&](const Required<AST::WhileExpression>& whileExpr)
		{
			checkExpression(state, whileExpr.value().condition.value(), scope);
			checkStatement(state, whileExpr.value().body.value(), scope);
			if (whileExpr.value().elseBody.hasValue())
				checkStatement(state, whileExpr.value().elseBody.value(), scope);
		},

		// --- For expression ---
		[&](const Required<AST::ForExpression>& forExpr)
		{
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

			checkStatement(state, forExpr.value().body.value(), &forScope);
			if (forExpr.value().elseBody.hasValue())
				checkStatement(state, forExpr.value().elseBody.value(), &forScope);
		},

		// --- Match expression ---
		[&](const Required<AST::MatchExpression>& match)
		{
			TypeId subjectType = checkExpression(state, match.value().subject.value(), scope);

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
				if (arm.value().capture.hasValue() && enumTypeId != INVALID_TYPE_ID)
				{
					// Find variant payload type — use last path segment (e.g. "Result::Ok" → "Ok")
					std::string_view variantName;
					const AST::ListNode<const Token*>* node = arm.value().variant.value().path.ptr();
					while (node && node->next.hasValue())
						node = node->next.ptr();
					if (node) variantName = state.getStringView(*node->item.value());

					TypeId captureType = INVALID_TYPE_ID;
					const TypeInfo& enumInfo = state.interned.get(enumTypeId);
					for (const auto& variant : enumInfo.asEnum().variants)
					{
						if (variant.name == variantName && variant.payloadType.has_value())
						{
							captureType = *variant.payloadType;
							break;
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

			if (match.value().elseArm.hasValue())
				checkStatement(state, match.value().elseArm.value(), scope);
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
				if (varDef.value().type.hasValue())
				{
					auto it = state.interned.astTypes.find(&varDef.value().type.value());
					if (it != state.interned.astTypes.end())
						annotatedValueType = stripIndirection(it->second, state.interned);
				}

				TypeId valueT = checkExpression(state, varDef.value().value.value(), scope, annotatedValueType);

				if (annotatedValueType != INVALID_TYPE_ID)
				{
					if (valueT != INVALID_TYPE_ID && !isAssignable(valueT, annotatedValueType, state.interned))
					{
						state.logger.logErrorInRange(varDef.value().name, varDef.value().name,
							"Type mismatch in variable declaration '{}'.",
							state.getStringView(varDef.value().name));
					}
					valueT = annotatedValueType;
				}

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

				// D4 + type check: for all assignment operators (= and compound),
				// RHS value type must be assignable to LHS value type.
				if (targetType != INVALID_TYPE_ID && valueT != INVALID_TYPE_ID)
				{
					if (!isAssignable(valueT, targetType, state.interned))
						Log::error("Type mismatch in assignment.");
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
					checkExpression(state, brk.value().value.value(), scope);
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
						if (!isAssignable(retType, expectedVal, state.interned))
							Log::error("Return type mismatch.");
					}
				}
			},

			[&](const Required<AST::IfExpression>& ifExpr)
			{
				checkExpression(state, AST::Expression(ifExpr), scope);
			},
			[&](const Required<AST::ForExpression>& forExpr)
			{
				checkExpression(state, AST::Expression(forExpr), scope);
			},
			[&](const Required<AST::WhileExpression>& whileExpr)
			{
				checkExpression(state, AST::Expression(whileExpr), scope);
			},
			[&](const Required<AST::MatchExpression>& matchExpr)
			{
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
				}, def.value().definition);
		});

	Status status = state.logger.hasError() ? Error : Ok;
	return { status, std::move(state.result) };
}
