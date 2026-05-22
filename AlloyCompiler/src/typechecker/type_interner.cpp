#include "type_interner.hpp"

#include <functional>
#include <unordered_map>
#include <variant>

#include "../util/logger.hpp"
#include "../util/overloaded.hpp"

using enum Status;

// ---------------------------------------------------------------------------
// TypeInfo equality and hash (for dedup map)
// ---------------------------------------------------------------------------

static bool typeInfoEqual(const TypeInfo& a, const TypeInfo& b)
{
    if (a.kind != b.kind)
        return false;

    switch (a.kind)
    {
        case TypeInfo::Kind::Primitive:
            return a.asPrimitive().name == b.asPrimitive().name;

        case TypeInfo::Kind::Pointer:
        case TypeInfo::Kind::Reference:
        case TypeInfo::Kind::PtrMut:
        case TypeInfo::Kind::RefMut:
            return a.asIndirection().inner == b.asIndirection().inner;

        case TypeInfo::Kind::Slice:
            return a.asSlice().elem == b.asSlice().elem;

        case TypeInfo::Kind::Array:
            return a.asArray().elem == b.asArray().elem
                && a.asArray().size == b.asArray().size;

        case TypeInfo::Kind::Struct:
        {
            const auto& ma = a.asStruct().members;
            const auto& mb = b.asStruct().members;
            if (ma.size() != mb.size()) return false;
            for (size_t i = 0; i < ma.size(); ++i)
                if (ma[i].name != mb[i].name || ma[i].type != mb[i].type)
                    return false;
            return true;
        }

        case TypeInfo::Kind::Enum:
        {
            const auto& va = a.asEnum().variants;
            const auto& vb = b.asEnum().variants;
            if (va.size() != vb.size()) return false;
            for (size_t i = 0; i < va.size(); ++i)
            {
                if (va[i].name != vb[i].name) return false;
                if (va[i].payloadType != vb[i].payloadType) return false;
            }
            return true;
        }

        case TypeInfo::Kind::Function:
        {
            const auto& fa = a.asFunction();
            const auto& fb = b.asFunction();
            return fa.params == fb.params && fa.ret == fb.ret;
        }

        // Named, TypeParam and Interface are never deduplicated — each declaration is unique.
        case TypeInfo::Kind::Named:
        case TypeInfo::Kind::TypeParam:
        case TypeInfo::Kind::Interface:
            return false;
    }
    return false;
}

static size_t hashTypeInfo(const TypeInfo& info)
{
    auto h = [](size_t seed, size_t v) -> size_t {
        return seed ^ (v + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    };

    size_t seed = static_cast<size_t>(info.kind);

    switch (info.kind)
    {
        case TypeInfo::Kind::Primitive:
            return h(seed, std::hash<std::string_view>{}(info.asPrimitive().name));

        case TypeInfo::Kind::Pointer:
        case TypeInfo::Kind::Reference:
        case TypeInfo::Kind::PtrMut:
        case TypeInfo::Kind::RefMut:
            return h(seed, info.asIndirection().inner);

        case TypeInfo::Kind::Slice:
            return h(seed, info.asSlice().elem);

        case TypeInfo::Kind::Array:
            return h(h(seed, info.asArray().elem), info.asArray().size);

        case TypeInfo::Kind::Struct:
            for (const auto& m : info.asStruct().members)
            {
                seed = h(seed, std::hash<std::string_view>{}(m.name));
                seed = h(seed, m.type);
            }
            return seed;

        case TypeInfo::Kind::Enum:
            for (const auto& v : info.asEnum().variants)
            {
                seed = h(seed, std::hash<std::string_view>{}(v.name));
                if (v.payloadType) seed = h(seed, *v.payloadType);
            }
            return seed;

        case TypeInfo::Kind::Function:
        {
            const auto& f = info.asFunction();
            for (TypeId p : f.params) seed = h(seed, p);
            if (f.ret) seed = h(seed, *f.ret);
            return seed;
        }

        case TypeInfo::Kind::Named:
        case TypeInfo::Kind::TypeParam:
        case TypeInfo::Kind::Interface:
            // Never hashed for dedup — should not reach here.
            return seed;
    }
    return seed;
}

// ---------------------------------------------------------------------------
// Interning state
// ---------------------------------------------------------------------------

struct InternState
{
    Logger logger;
    InternedTypes result;

    // Dedup map for structural types (excludes Named and TypeParam).
    struct TypeInfoHasher {
        size_t operator()(const TypeInfo& info) const { return hashTypeInfo(info); }
    };
    struct TypeInfoEqual {
        bool operator()(const TypeInfo& a, const TypeInfo& b) const { return typeInfoEqual(a, b); }
    };
    std::unordered_map<TypeInfo, TypeId, TypeInfoHasher, TypeInfoEqual> dedupMap;

    // Maps TypeDefinition* → Named TypeId (prevents re-interning the same named type).
    std::unordered_map<const AST::TypeDefinition*, TypeId> namedTypeCache;

    // The current generic function's TypeParam scope (name → TypeId).
    // Set while interning a FunctionDefinition with type parameters.
    std::unordered_map<std::string_view, TypeId> typeParamScope;

    // B3-5: types synthesised by type-position comptime expressions (§3.4).
    const SynthTypeMap* synthTypes = nullptr;

    explicit InternState(const Source& source) : logger(source) {}

    std::string_view getStringView(const Token& tok) const
    {
        return { &logger.getSource().data[tok.start.index],
                 tok.end.index - tok.start.index };
    }

    // Insert a structural TypeInfo, deduplicating.
    TypeId internStructural(TypeInfo info)
    {
        auto [it, inserted] = dedupMap.emplace(info, static_cast<TypeId>(result.table.size()));
        if (inserted)
            result.table.push_back(std::move(info));
        return it->second;
    }

    // Insert a unique TypeInfo (Named, TypeParam) — no dedup.
    TypeId internUnique(TypeInfo info)
    {
        TypeId id = static_cast<TypeId>(result.table.size());
        result.table.push_back(std::move(info));
        return id;
    }
};

// Forward declarations.
static TypeId internASTType(InternState& state, const AST::Type& type, const ResolvedModule& resolved);
static TypeId internBaseType(InternState& state, const AST::BaseType& base, const ResolvedModule& resolved);

// ---------------------------------------------------------------------------
// Intern an interface used as a type (dynamic-dispatch object). Cached per decl.
// ---------------------------------------------------------------------------

static TypeId internInterfaceType(InternState& state, const AST::InterfaceDefinition& ifaceDef)
{
    auto it = state.result.interfaceIds.find(&ifaceDef);
    if (it != state.result.interfaceIds.end())
        return it->second;

    TypeInfo info;
    info.kind = TypeInfo::Kind::Interface;
    info.data = TypeInfo::InterfaceData{ state.getStringView(ifaceDef.name), &ifaceDef };

    TypeId id = state.internUnique(std::move(info));
    state.result.interfaceIds[&ifaceDef] = id;
    return id;
}

// ---------------------------------------------------------------------------
// Intern a named type from a TypeDefinition (may recurse for chains).
// ---------------------------------------------------------------------------

static TypeId internNamedType(InternState& state, const AST::TypeDefinition& typeDef, const ResolvedModule& resolved)
{
    auto cacheIt = state.namedTypeCache.find(&typeDef);
    if (cacheIt != state.namedTypeCache.end())
        return cacheIt->second;

    // To break cycles (if ever introduced), reserve a slot with a placeholder.
    TypeId placeholderId = static_cast<TypeId>(state.result.table.size());
    state.result.table.emplace_back(); // placeholder
    state.namedTypeCache[&typeDef] = placeholderId;

    TypeId underlying = internBaseType(state, typeDef.baseType.value(), resolved);
    auto name = state.getStringView(typeDef.name);

    TypeInfo info;
    info.kind = TypeInfo::Kind::Named;
    info.data = TypeInfo::NamedData{ name, underlying };

    // Overwrite the placeholder.
    state.result.table[placeholderId] = std::move(info);

    state.result.namedTypeIds[&typeDef.name] = placeholderId;

    // Record which user-defined interfaces this type implements ('type T : I1, I2').
    typeDef.interfaces.forEach([&](const Required<const Token*>& markerReq)
    {
        auto riIt = resolved.resolvedInterfaces.find(markerReq.value());
        if (riIt == resolved.resolvedInterfaces.end())
            return;
        if (auto* declPtr = std::get_if<const ResolvedDeclaration*>(&riIt->second))
        {
            if (auto* ifReq = std::get_if<Required<AST::InterfaceDefinition>>(&(*declPtr)->definition))
            {
                TypeId ifId = internInterfaceType(state, ifReq->value());
                state.result.implementedInterfaces[placeholderId].push_back(ifId);
            }
        }
    });

    return placeholderId;
}

// ---------------------------------------------------------------------------
// B3-5: intern a type synthesised by a type-position comptime expression.
// ---------------------------------------------------------------------------

static TypeId primitiveTypeId(std::string_view name)
{
    if (name == "u8")   return TYPE_U8;
    if (name == "u16")  return TYPE_U16;
    if (name == "u32")  return TYPE_U32;
    if (name == "u64")  return TYPE_U64;
    if (name == "i8")   return TYPE_I8;
    if (name == "i16")  return TYPE_I16;
    if (name == "i32")  return TYPE_I32;
    if (name == "i64")  return TYPE_I64;
    if (name == "f32")  return TYPE_F32;
    if (name == "f64")  return TYPE_F64;
    if (name == "bool") return TYPE_BOOL;
    return INVALID_TYPE_ID;
}

static TypeId internSynthType(InternState& state, const SynthType& st)
{
    if (st.isEnum)
    {
        TypeInfo::EnumData data;
        for (const SynthType::Member& m : st.members)
        {
            std::optional<TypeId> payload;
            if (m.typeName != "void")
            {
                TypeId t = primitiveTypeId(m.typeName);
                if (t == INVALID_TYPE_ID)
                    Log::error("Synthesised enum variant '{}' has a non-primitive payload type "
                        "'{}' (B3-5 supports primitive member types only).", m.name, m.typeName);
                payload = t;
            }
            data.variants.push_back({ std::string_view(m.name), payload });
        }
        TypeInfo info;
        info.kind = TypeInfo::Kind::Enum;
        info.data = std::move(data);
        return state.internStructural(std::move(info));
    }

    TypeInfo::StructData data;
    for (const SynthType::Member& m : st.members)
    {
        TypeId t = primitiveTypeId(m.typeName);
        if (t == INVALID_TYPE_ID)
            Log::error("Synthesised struct member '{}' has a non-primitive type "
                "'{}' (B3-5 supports primitive member types only).", m.name, m.typeName);
        data.members.push_back({ std::string_view(m.name), t });
    }
    TypeInfo info;
    info.kind = TypeInfo::Kind::Struct;
    info.data = std::move(data);
    return state.internStructural(std::move(info));
}

// ---------------------------------------------------------------------------
// Intern a base type node.
// ---------------------------------------------------------------------------

static TypeId internBaseType(InternState& state, const AST::BaseType& base, const ResolvedModule& resolved)
{
    return std::visit(Overloaded
    {
        [&](const Required<AST::NamedType>& namedType) -> TypeId
        {
            const auto& ident = namedType.value().name.value();
            const auto& firstNode = ident.path.value();
            const Token* firstToken = firstNode.item.value();
            auto name = state.getStringView(*firstToken);

            // Built-in primitive?
            static const std::unordered_map<std::string_view, TypeId> s_primitives =
            {
                {"u8",  TYPE_U8},  {"u16", TYPE_U16}, {"u32", TYPE_U32}, {"u64", TYPE_U64},
                {"i8",  TYPE_I8},  {"i16", TYPE_I16}, {"i32", TYPE_I32}, {"i64", TYPE_I64},
                {"f32", TYPE_F32}, {"f64", TYPE_F64}, {"bool", TYPE_BOOL},
            };
            if (auto it = s_primitives.find(name); it != s_primitives.end())
                return it->second;

            // Generic type parameter?
            if (auto it = state.typeParamScope.find(name); it != state.typeParamScope.end())
                return it->second;

            // Named user-defined type — look up via resolved names.
            auto resolvedIt = resolved.names.find(&ident);
            if (resolvedIt == resolved.names.end() || resolvedIt->second.empty())
            {
                // Already reported by resolver; return a sentinel.
                return INVALID_TYPE_ID;
            }

            const ResolvedDeclaration* decl = resolvedIt->second[0];
            if (auto* td = std::get_if<Required<AST::TypeDefinition>>(&decl->definition))
                return internNamedType(state, td->value(), resolved);
            if (auto* ifd = std::get_if<Required<AST::InterfaceDefinition>>(&decl->definition))
                return internInterfaceType(state, ifd->value());  // interface used as a type
            return INVALID_TYPE_ID;
        },

        [&](const Required<AST::StructType>& structType) -> TypeId
        {
            TypeInfo::StructData data;
            structType.value().members.forEach([&](const Required<AST::StructType::Member>& m)
            {
                TypeId memberTypeId = internASTType(state, m.value().type.value(), resolved);
                data.members.push_back({ state.getStringView(m.value().name), memberTypeId });
            });

            TypeInfo info;
            info.kind = TypeInfo::Kind::Struct;
            info.data = std::move(data);
            return state.internStructural(std::move(info));
        },

        [&](const Required<AST::EnumType>& enumType) -> TypeId
        {
            TypeInfo::EnumData data;
            enumType.value().members.forEach([&](const Required<AST::EnumType::Member>& m)
            {
                std::optional<TypeId> payloadId;
                if (m.value().payloadType.hasValue())
                    payloadId = internASTType(state, m.value().payloadType.value(), resolved);
                data.variants.push_back({ state.getStringView(m.value().name), payloadId });
            });

            TypeInfo info;
            info.kind = TypeInfo::Kind::Enum;
            info.data = std::move(data);
            return state.internStructural(std::move(info));
        },

        [&](const Required<AST::ArrayType>& arrayType) -> TypeId
        {
            TypeId elemId = internASTType(state, arrayType.value().elementType.value(), resolved);
            size_t sz = arrayType.value().size;

            TypeInfo info;
            if (sz == 0)
            {
                info.kind = TypeInfo::Kind::Slice;
                info.data = TypeInfo::SliceData{ elemId };
            }
            else
            {
                info.kind = TypeInfo::Kind::Array;
                info.data = TypeInfo::ArrayData{ elemId, sz };
            }
            return state.internStructural(std::move(info));
        },

        [&](const Required<AST::FunctionType>& fnType) -> TypeId
        {
            TypeInfo::FunctionData data;
            fnType.value().parameterTypes.forEach([&](const Required<AST::Type>& pt)
            {
                data.params.push_back(internASTType(state, pt.value(), resolved));
            });
            if (fnType.value().returnType.hasValue())
                data.ret = internASTType(state, fnType.value().returnType.value(), resolved);

            TypeInfo info;
            info.kind = TypeInfo::Kind::Function;
            info.data = std::move(data);
            return state.internStructural(std::move(info));
        },

        [&](const Required<AST::ComptimeExpression>& comptime) -> TypeId
        {
            // B3-5 (§3.4): a type-position comptime expression — intern the type
            // the comptime-evaluation pass synthesised for it.
            if (state.synthTypes)
            {
                auto it = state.synthTypes->find(&comptime.value());
                if (it != state.synthTypes->end())
                    return internSynthType(state, it->second);
            }
            // Not evaluated (evaluation failed) — no concrete TypeId.
            return INVALID_TYPE_ID;
        },
    }, base);
}

// ---------------------------------------------------------------------------
// Intern an AST::Type (handles modifier wrapping).
// ---------------------------------------------------------------------------

// A7: first identifier token of a type annotation, for diagnostics. Returns
// nullptr when the type's base is not a named type.
static const Token* firstTokenOfType(const AST::Type& type)
{
    const AST::Type* t = &type;
    while (true)
    {
        if (auto* inner = std::get_if<Required<AST::Type>>(&t->innerType))
        {
            t = inner->ptr();
            continue;
        }
        auto* base = std::get_if<Required<AST::BaseType>>(&t->innerType);
        if (!base)
            return nullptr;
        if (auto* nt = std::get_if<Required<AST::NamedType>>(&base->value()))
            return nt->value().name.value().path.value().item.value();
        return nullptr;
    }
}

static TypeId internASTType(InternState& state, const AST::Type& type, const ResolvedModule& resolved)
{
    // Recursively intern whatever is inside first.
    TypeId innerId = std::visit(Overloaded
    {
        [&](const Required<AST::BaseType>& base) -> TypeId
        {
            return internBaseType(state, base.value(), resolved);
        },
        [&](const Required<AST::Type>& inner) -> TypeId
        {
            return internASTType(state, inner.value(), resolved);
        },
    }, type.innerType);

    if (innerId == INVALID_TYPE_ID)
        return INVALID_TYPE_ID;

    // Wrap with modifier.
    TypeInfo::Kind wrapKind = TypeInfo::Kind::Pointer;
    switch (type.modifier)
    {
        case AST::Type::Modifier::None:
            // A7: an interface is unsized — it may only be used as a type behind
            // an indirection (&I / *I, the interface-object form, §3.2). A bare
            // interface value type is a compile-time error.
            if (innerId < static_cast<TypeId>(state.result.table.size())
                && state.result.table[innerId].kind == TypeInfo::Kind::Interface)
            {
                if (const Token* tok = firstTokenOfType(type))
                    state.logger.logErrorInRange(*tok, *tok,
                        "Interface '{}' cannot be used as a value type; an interface "
                        "type must appear behind '&' or '*'.", state.getStringView(*tok));
            }
            // Cache the mapping and return directly (no wrapper).
            state.result.astTypes[&type] = innerId;
            return innerId;
        case AST::Type::Modifier::Pointer:           wrapKind = TypeInfo::Kind::Pointer;   break;
        case AST::Type::Modifier::Reference:         wrapKind = TypeInfo::Kind::Reference; break;
        case AST::Type::Modifier::PointerToMutable:  wrapKind = TypeInfo::Kind::PtrMut;    break;
        case AST::Type::Modifier::ReferenceToMutable:wrapKind = TypeInfo::Kind::RefMut;    break;
    }

    TypeInfo wrapInfo;
    wrapInfo.kind = wrapKind;
    wrapInfo.data = TypeInfo::IndirectionData{ innerId };
    TypeId wrappedId = state.internStructural(std::move(wrapInfo));
    state.result.astTypes[&type] = wrappedId;
    return wrappedId;
}

// ---------------------------------------------------------------------------
// Walk a Function node (parameters + return type + body type annotations).
// Body type annotations are inside statements/expressions — only explicit
// variable annotations are walked here; expression types are inferred by
// the type checker.
// ---------------------------------------------------------------------------

static void internFunction(InternState& state, const AST::Function& fn, const ResolvedModule& resolved);
static void internStatement(InternState& state, const AST::Statement& stmt, const ResolvedModule& resolved);
static void internExpression(InternState& state, const AST::Expression& expr, const ResolvedModule& resolved);
static void internComptime(InternState& state, const AST::ComptimeExpression& comptime, const ResolvedModule& resolved);

static void internFunction(InternState& state, const AST::Function& fn, const ResolvedModule& resolved)
{
    fn.parameters.forEach([&](const Required<AST::FunctionParameter>& param)
    {
        internASTType(state, param.value().type.value(), resolved);
    });

    if (fn.returnType.hasValue())
        internASTType(state, fn.returnType.value(), resolved);

    internStatement(state, AST::Statement(fn.body), resolved);
}

static void internExpression(InternState& state, const AST::Expression& expr, const ResolvedModule& resolved)
{
    std::visit(Overloaded
    {
        [&](const Required<AST::IdentifierExpression>&) {},
        [&](const Required<AST::LiteralExpression>&) {},

        [&](const Required<AST::FunctionCallExpression>& call)
        {
            internExpression(state, call.value().function.value(), resolved);
            // Intern explicit type arguments (e.g., add<u64>(...)).
            call.value().typeArguments.forEach([&](const Required<AST::Type>& ta)
            {
                internASTType(state, ta.value(), resolved);
            });
            call.value().arguments.forEach([&](const Required<AST::Expression>& arg)
            {
                internExpression(state, arg.value(), resolved);
            });
        },

        [&](const Required<AST::MemberAccessExpression>& access)
        {
            internExpression(state, access.value().object.value(), resolved);
        },

        [&](const Required<AST::ArrayAccessExpression>& access)
        {
            internExpression(state, access.value().object.value(), resolved);
            internExpression(state, access.value().index.value(), resolved);
        },

        [&](const Required<AST::UnaryExpression>& unary)
        {
            internExpression(state, unary.value().expression.value(), resolved);
        },

        [&](const Required<AST::BinaryExpression>& binary)
        {
            internExpression(state, binary.value().left.value(), resolved);
            internExpression(state, binary.value().right.value(), resolved);
        },

        [&](const Required<AST::ArrayLiteralExpression>& lit)
        {
            lit.value().elements.forEach([&](const Required<AST::Expression>& elem)
            {
                internExpression(state, elem.value(), resolved);
            });
        },

        [&](const Required<AST::ArrayFillExpression>& fill)
        {
            internExpression(state, fill.value().value.value(), resolved);
        },

        [&](const Required<AST::StructInitializerExpression>& init)
        {
            init.value().initializers.forEach([&](const Required<AST::StructInitializerExpression::MemberInitializer>& mi)
            {
                internExpression(state, mi.value().value.value(), resolved);
            });
        },

        [&](const Required<AST::LambdaExpression>& lambda)
        {
            // Lambda captures have no explicit type annotation — types inferred by checker.
            internFunction(state, lambda.value().function.value(), resolved);
        },

        [&](const Required<AST::IfExpression>& ifExpr)
        {
            internExpression(state, ifExpr.value().condition.value(), resolved);
            internStatement(state, ifExpr.value().thenBranch.value(), resolved);
            if (ifExpr.value().elseBranch.hasValue())
                internStatement(state, ifExpr.value().elseBranch.value(), resolved);
        },

        [&](const Required<AST::WhileExpression>& whileExpr)
        {
            internExpression(state, whileExpr.value().condition.value(), resolved);
            internStatement(state, whileExpr.value().body.value(), resolved);
            if (whileExpr.value().elseBody.hasValue())
                internStatement(state, whileExpr.value().elseBody.value(), resolved);
        },

        [&](const Required<AST::ForExpression>& forExpr)
        {
            forExpr.value().iterables.value().forEach([&](const Required<AST::Expression>& it)
            {
                internExpression(state, it.value(), resolved);
            });
            internStatement(state, forExpr.value().body.value(), resolved);
            if (forExpr.value().elseBody.hasValue())
                internStatement(state, forExpr.value().elseBody.value(), resolved);
        },

        [&](const Required<AST::MatchExpression>& match)
        {
            internExpression(state, match.value().subject.value(), resolved);
            match.value().arms.forEach([&](const Required<AST::MatchArm>& arm)
            {
                if (arm.value().pattern.hasValue())
                    internExpression(state, arm.value().pattern.value(), resolved);
                internStatement(state, arm.value().body.value(), resolved);
            });
            if (match.value().externalElse.hasValue())
                internStatement(state, match.value().externalElse.value(), resolved);
        },

        [&](const Required<AST::ComptimeExpression>& comptime)
        {
            internComptime(state, comptime.value(), resolved);
        },

        [&](const Required<AST::ComptimeResultExpression>&)
        {
            // A substituted comptime result carries no nested AST types to intern;
            // its value type is assigned by the type checker.
        },
    }, expr);
}

static void internStatement(InternState& state, const AST::Statement& stmt, const ResolvedModule& resolved)
{
    std::visit(Overloaded
    {
        [&](const Required<AST::VariableDefinitionStatement>& varDef)
        {
            if (varDef.value().type.hasValue())
                internASTType(state, varDef.value().type.value(), resolved);
            internExpression(state, varDef.value().value.value(), resolved);
        },

        [&](const Required<AST::AssignmentStatement>& assign)
        {
            internExpression(state, assign.value().target.value(), resolved);
            internExpression(state, assign.value().value.value(), resolved);
        },

        [&](const Required<AST::ExpressionStatement>& exprStmt)
        {
            internExpression(state, exprStmt.value().expression.value(), resolved);
        },

        [&](const Required<AST::StatementBlock>& block)
        {
            block.value().statements.forEach([&](const Required<AST::Statement>& s)
            {
                internStatement(state, s.value(), resolved);
            });
        },

        [&](const Required<AST::BreakStatement>& brk)
        {
            if (brk.value().value.hasValue())
                internExpression(state, brk.value().value.value(), resolved);
        },

        [&](const Required<AST::ReturnStatement>& ret)
        {
            if (ret.value().value.hasValue())
                internExpression(state, ret.value().value.value(), resolved);
        },

        [&](const Required<AST::IfExpression>& ifExpr)
        {
            internExpression(state, AST::Expression(ifExpr), resolved);
        },

        [&](const Required<AST::ForExpression>& forExpr)
        {
            internExpression(state, AST::Expression(forExpr), resolved);
        },

        [&](const Required<AST::WhileExpression>& whileExpr)
        {
            internExpression(state, AST::Expression(whileExpr), resolved);
        },

        [&](const Required<AST::MatchExpression>& matchExpr)
        {
            internExpression(state, AST::Expression(matchExpr), resolved);
        },
    }, stmt);
}

static void internComptime(InternState& state, const AST::ComptimeExpression& comptime, const ResolvedModule& resolved)
{
    // A ComptimeExpression surviving to the interner was not evaluated (eval
    // failed or it is a deferred macro call); intern its inner expression so any
    // nested type annotations are still seen.
    internExpression(state, comptime.inner.value(), resolved);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

Result<InternedTypes> intern(
    const Source& source,
    const AST::Module& module,
    const ResolvedModule& resolved,
    const SynthTypeMap& synthTypes)
{
    InternState state(source);
    state.synthTypes = &synthTypes;

    // Pre-allocate the 11 primitive TypeIds (0..10).
    // Ordered to match TYPE_U8 ... TYPE_BOOL constants.
    static const TypeInfo::PrimitiveData PRIMITIVES[] =
    {
        {"u8",   false, false, 1},
        {"u16",  false, false, 2},
        {"u32",  false, false, 4},
        {"u64",  false, false, 8},
        {"i8",   false, true,  1},
        {"i16",  false, true,  2},
        {"i32",  false, true,  4},
        {"i64",  false, true,  8},
        {"f32",  true,  false, 4},
        {"f64",  true,  false, 8},
        {"bool", false, false, 1},
    };
    for (const auto& prim : PRIMITIVES)
    {
        TypeInfo info;
        info.kind = TypeInfo::Kind::Primitive;
        info.data = prim;
        state.result.table.push_back(std::move(info));
    }

    // Walk top-level definitions.
    module.definitions.forEach([&](const Required<AST::Definition>& def)
    {
        std::visit(Overloaded
        {
            [&](const Required<AST::TypeDefinition>& typeDef)
            {
                // Intern the named type (and its underlying structural type).
                internNamedType(state, typeDef.value(), resolved);
            },

            [&](const Required<AST::FunctionDefinition>& fnDef)
            {
                // Build TypeParam scope for this function.
                state.typeParamScope.clear();
                fnDef.value().typeParameters.forEach([&](const Required<AST::TypeParameter>& tp)
                {
                    TypeInfo info;
                    info.kind = TypeInfo::Kind::TypeParam;
                    auto tpName = state.getStringView(tp.value().name);
                    std::optional<BuiltinInterface> constraint;
                    std::optional<TypeId> interfaceConstraint;   // A4: user-defined interface bound
                    if (tp.value().interface.hasValue())
                    {
                        // Prefer the resolver's classification (built-in vs user interface);
                        // fall back to a built-in name lookup.
                        auto riIt = resolved.resolvedInterfaces.find(&tp.value().interface.value());
                        if (riIt != resolved.resolvedInterfaces.end())
                        {
                            std::visit(Overloaded
                            {
                                [&](BuiltinInterface bi) { constraint = bi; },
                                [&](const ResolvedDeclaration* decl)
                                {
                                    if (auto* ifReq = std::get_if<Required<AST::InterfaceDefinition>>(&decl->definition))
                                        interfaceConstraint = internInterfaceType(state, ifReq->value());
                                },
                            }, riIt->second);
                        }
                        else
                        {
                            auto ifName = state.getStringView(tp.value().interface.value());
                            auto it = s_BuiltinInterfaces.find(ifName);
                            if (it != s_BuiltinInterfaces.end())
                                constraint = it->second;
                        }
                    }
                    info.data = TypeInfo::TypeParamData{ tpName, constraint, interfaceConstraint };
                    TypeId tpId = state.internUnique(std::move(info));
                    state.typeParamScope[tpName] = tpId;
                    state.result.typeParamIds[&tp.value().name] = tpId;
                });

                internFunction(state, fnDef.value().function.value(), resolved);

                state.typeParamScope.clear();
            },

            [&](const Required<AST::ExternDefinition>& externDef)
            {
                externDef.value().parameters.forEach([&](const Required<AST::FunctionParameter>& param)
                {
                    internASTType(state, param.value().type.value(), resolved);
                });
                if (externDef.value().returnType.hasValue())
                    internASTType(state, externDef.value().returnType.value(), resolved);
            },

            [&](const Required<AST::InterfaceDefinition>& ifaceDef)
            {
                // Give every interface a TypeId (it may be used as a dispatch type).
                internInterfaceType(state, ifaceDef.value());

                // Intern interface-function signature types so the §5.2
                // verification pass can compare them against extension functions.
                ifaceDef.value().functions.forEach([&](const Required<AST::InterfaceFunction>& fn)
                {
                    fn.value().parameters.forEach([&](const Required<AST::FunctionParameter>& param)
                    {
                        internASTType(state, param.value().type.value(), resolved);
                    });
                    if (fn.value().returnType.hasValue())
                        internASTType(state, fn.value().returnType.value(), resolved);
                });
            },

            [&](const Required<AST::MacroDefinition>& macroDef)
            {
                macroDef.value().parameters.forEach([&](const Required<AST::FunctionParameter>& param)
                {
                    internASTType(state, param.value().type.value(), resolved);
                });
                internStatement(state, AST::Statement(macroDef.value().body), resolved);
            },
        }, def.value().definition);
    });

    Status status = state.logger.hasError() ? Error : Ok;
    return { status, std::move(state.result) };
}
