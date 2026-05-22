#include "resolver.hpp"

#include <unordered_set>

#include "../util/overloaded.hpp"

using enum Status;

struct ResolverState
{
	Logger logger;

	ResolverState(const Source& source) : logger(source) {}

	std::string_view getStringView(const Token& token) const
	{
		return std::string_view(&logger.getSource().data[token.start.index], token.end.index - token.start.index);
	}
};

Result<SymbolTable> declare(const Source& moduleSource, const AST::Module& module)
{
	ResolverState state(moduleSource);
	SymbolTable symbolTable;
	Status status = Ok;

	module.definitions.forEach([&](const Required<AST::Definition>& def)
		{
			auto visibility = def.value().visiblity;

			auto s = std::visit(Overloaded
				{
					[&](const Required<AST::TypeDefinition>& typeDef) -> Status
					{
						auto name = state.getStringView(typeDef.value().name);
						if (symbolTable.add(name, visibility, typeDef) == Error)
						{
							state.logger.logErrorInRange(typeDef.value().name, typeDef.value().name,
								"Duplicate definition '{}'.", name);
							return Error;
						}
						return Ok;
					},
					[&](const Required<AST::FunctionDefinition>& fnDef) -> Status
					{
						auto name = state.getStringView(fnDef.value().name);
						if (symbolTable.add(name, visibility, fnDef) == Error)
						{
							state.logger.logErrorInRange(fnDef.value().name, fnDef.value().name,
								"Duplicate definition '{}'.", name);
							return Error;
						}
						return Ok;
					},
					[&](const Required<AST::ExternDefinition>& externDef) -> Status
					{
						auto name = state.getStringView(externDef.value().name);
						if (symbolTable.add(name, visibility, externDef) == Error)
						{
							state.logger.logErrorInRange(externDef.value().name, externDef.value().name,
								"Duplicate definition '{}'.", name);
							return Error;
						}
						return Ok;
					},
					[&](const Required<AST::InterfaceDefinition>& ifaceDef) -> Status
					{
						auto name = state.getStringView(ifaceDef.value().name);
						if (symbolTable.add(name, visibility, ifaceDef) == Error)
						{
							state.logger.logErrorInRange(ifaceDef.value().name, ifaceDef.value().name,
								"Duplicate definition '{}'.", name);
							return Error;
						}
						return Ok;
					},
					[&](const Required<AST::MacroDefinition>& macroDef) -> Status
					{
						auto name = state.getStringView(macroDef.value().name);
						if (symbolTable.add(name, visibility, macroDef) == Error)
						{
							state.logger.logErrorInRange(macroDef.value().name, macroDef.value().name,
								"Duplicate definition '{}'.", name);
							return Error;
						}
						return Ok;
					},
				}, def.value().definition);

			status &= s;
		});

	return { status, symbolTable };
}

struct ScopedSymbolTable
{
	ScopedSymbolTable* parent;
	// Stores stable pointers into ResolvedModule::localDecls.
	// Never stores values directly — elements of this map do NOT own the declarations.
	std::unordered_map<std::string_view, const ResolvedDeclaration*> locals;

	explicit ScopedSymbolTable(ScopedSymbolTable* parent = nullptr)
		: parent(parent) {
	}

	ScopedSymbolTable(const ScopedSymbolTable&) = delete;
	ScopedSymbolTable& operator=(const ScopedSymbolTable&) = delete;

	const ResolvedDeclaration* lookupLocal(std::string_view name) const
	{
		auto it = locals.find(name);
		if (it != locals.end())
			return it->second;
		if (parent)
			return parent->lookupLocal(name);
		return nullptr;
	}

	// Stores a stable pointer. The pointed-to ResolvedDeclaration must outlive this scope.
	Status declare(std::string_view name, const ResolvedDeclaration* decl)
	{
		if (!locals.try_emplace(name, decl).second)
			return Error;
		return Ok;
	}
};

struct ResolveState : ResolverState
{
	const SymbolTable& moduleScope;
	std::unordered_map<std::string_view, const SymbolTable*> importAliases;
	ResolvedModule result;

	ResolveState(const Source& source, const SymbolTable& moduleScope)
		: ResolverState(source), moduleScope(moduleScope) {
	}

	// returns all candidates for name. Local scope (variables/params) always produces at most one
	std::vector<const ResolvedDeclaration*> lookup(std::string_view name, const ScopedSymbolTable* scope) const
	{
		if (scope)
		{
			if (auto* decl = scope->lookupLocal(name))
				return { decl };
		}
		return moduleScope.get(name);  // may return multiple for overloaded functions
	}

	// convenience for contexts where only a single declaration is valid (captures, type names)
	// returns nullptr if not found or if multiple candidates exist
	const ResolvedDeclaration* lookupSingle(std::string_view name, const ScopedSymbolTable* scope) const
	{
		auto candidates = lookup(name, scope);
		return candidates.size() == 1 ? candidates[0] : (candidates.empty() ? nullptr : candidates[0]);
	}

	const SymbolTable* findImport(std::string_view alias) const
	{
		auto it = importAliases.find(alias);
		return it != importAliases.end() ? it->second : nullptr;
	}

	// Allocate a local declaration in stable storage (ResolvedModule::localDecls).
	// std::deque never moves on push_back, so the returned pointer remains valid
	// for the lifetime of the ResolvedModule.
	const ResolvedDeclaration* allocateLocal(AST::Definition::Visibility vis, Declaration def)
	{
		result.localDecls.push_back({ vis, std::move(def) });
		return &result.localDecls.back();
	}
};

// wraps arena-allocated node (const ref) in a non-const Required<T>
// safe because arena allocations are non-const by origin
template<typename T>
static Required<T> asRequired(const T& ref)
{
	return Required<T>(const_cast<T*>(&ref));
}

static void resolveIdentifier(ResolveState& state, const AST::IdentifierExpression& ident, const ScopedSymbolTable* scope);
static void resolveType(ResolveState& state, const AST::Type& type, const ScopedSymbolTable* scope);
static void resolveBaseType(ResolveState& state, const AST::BaseType& baseType, const ScopedSymbolTable* scope);
static void resolveExpression(ResolveState& state, const AST::Expression& expr, ScopedSymbolTable* scope);
static void resolveStatement(ResolveState& state, const AST::Statement& stmt, ScopedSymbolTable* scope);
static void resolveFunction(ResolveState& state, const AST::Function& fn, ScopedSymbolTable* parentScope);
static void resolveComptime(ResolveState& state, const AST::ComptimeExpression& comptime, ScopedSymbolTable* scope);

// Resolves an interface-name token (a generic constraint or a type_def marker) to a
// built-in interface or a user-defined InterfaceDefinition, recording it in the result.
static void resolveInterfaceToken(ResolveState& state, const Token* ifToken)
{
	auto name = state.getStringView(*ifToken);

	auto builtinIt = s_BuiltinInterfaces.find(name);
	if (builtinIt != s_BuiltinInterfaces.end())
	{
		state.result.resolvedInterfaces[ifToken] = builtinIt->second;
		return;
	}

	for (const auto* decl : state.moduleScope.get(name))
	{
		if (std::holds_alternative<Required<AST::InterfaceDefinition>>(decl->definition))
		{
			state.result.resolvedInterfaces[ifToken] = decl;
			return;
		}
	}

	state.logger.logErrorInRange(*ifToken, *ifToken, "'{}' is not a known interface.", name);
}

static const std::unordered_set<std::string_view> s_BuiltinTypeNames = {
	"u8", "u16", "u32", "u64",
	"i8", "i16", "i32", "i64",
	"f32", "f64",
	"bool"
};

// Built-in comptime macros (§6.4) — recognised names so a '#'-call to one is
// not flagged as an undefined identifier. The comptime evaluator handles them.
static const std::unordered_set<std::string_view> s_BuiltinMacroNames = {
	"type_of", "struct_type", "enum_type"
};

static void resolveIdentifier(ResolveState& state, const AST::IdentifierExpression& ident, const ScopedSymbolTable* scope)
{
	const auto& firstNode = ident.path.value();
	const Token* firstToken = firstNode.item.value();

	if (!firstNode.next.hasValue())
	{
		auto name = state.getStringView(*firstToken);

		// check if built-in type
		if (s_BuiltinTypeNames.contains(name))
		{
			return;
		}

		// a built-in comptime macro — resolved by the comptime evaluator
		if (s_BuiltinMacroNames.contains(name))
		{
			return;
		}

		auto candidates = state.lookup(name, scope);
		if (candidates.empty())
		{
			state.logger.logErrorInRange(*firstToken, *firstToken, "Undefined name '{}'.", name);
		}
		state.result.names[&ident] = std::move(candidates);
	}
	else
	{
		// qualified path: A::B[::C...]
		// track A — module-qualified: first segment is an import alias
		// track B — type-scoped: first segment is a TypeDefinition; remaining segments resolved by type-checker
		auto firstName = state.getStringView(*firstToken);

		if (auto* importTable = state.findImport(firstName))
		{
			// Track A: walk to last segment and resolve in the imported module's symbol table.
			const AST::ListNode<const Token*>* node = &firstNode;
			while (node->next.hasValue())
				node = node->next.ptr();

			const Token* lastToken = node->item.value();
			auto lastName = state.getStringView(*lastToken);
			auto candidates = importTable->get(lastName);
			if (candidates.empty())
			{
				state.logger.logErrorInRange(*lastToken, *lastToken,
					"Undefined name '{}' in module '{}'.", lastName, firstName);
			}
			else
			{
				// D1: a private definition is not visible across a module boundary —
				// only 'pub'/'exp' declarations may be imported.
				std::vector<const ResolvedDeclaration*> visible;
				for (const auto* decl : candidates)
					if (decl->visibility != AST::Definition::Visibility::Private)
						visible.push_back(decl);
				if (visible.empty())
					state.logger.logErrorInRange(*lastToken, *lastToken,
						"'{}' is private to module '{}' and cannot be imported.", lastName, firstName);
				candidates = std::move(visible);
			}
			state.result.names[&ident] = std::move(candidates);
		}
		else
		{
			// Track B: first segment must be a TypeDefinition.
			// Remaining segments (variant/member names) are resolved by the type-checker.
			auto* decl = state.lookupSingle(firstName, scope);
			if (!decl)
			{
				state.logger.logErrorInRange(*firstToken, *firstToken,
					"Undefined name '{}'.", firstName);
				state.result.names[&ident] = {};
				return;
			}

			if (!std::holds_alternative<Required<AST::TypeDefinition>>(decl->definition))
			{
				state.logger.logErrorInRange(*firstToken, *firstToken,
					"'{}' is not a type; '::' requires a type or module.", firstName);
				state.result.names[&ident] = {};
				return;
			}

			// Store the TypeDefinition. Type-checker validates remaining path segments.
			state.result.names[&ident] = { decl };
		}
	}
}

static void resolveType(ResolveState& state, const AST::Type& type, const ScopedSymbolTable* scope)
{
	std::visit(Overloaded
		{
			[&](const Required<AST::BaseType>& baseType)
			{
				resolveBaseType(state, baseType.value(), scope);
			},
			[&](const Required<AST::Type>& innerType)
			{
				resolveType(state, innerType.value(), scope);
			},
		}, type.innerType);
}

static void resolveBaseType(ResolveState& state, const AST::BaseType& baseType, const ScopedSymbolTable* scope)
{
	std::visit(Overloaded
		{
			[&](const Required<AST::NamedType>& namedType)
			{
				resolveIdentifier(state, namedType.value().name.value(), scope);
			},
			[&](const Required<AST::StructType>& structType)
			{
				structType.value().members.forEach([&](const Required<AST::StructType::Member>& member)
					{
						resolveType(state, member.value().type.value(), scope);
					});
			},
			[&](const Required<AST::EnumType>& enumType)
			{
				enumType.value().members.forEach([&](const Required<AST::EnumType::Member>& member)
					{
						if (member.value().payloadType.hasValue())
						{
							resolveType(state, member.value().payloadType.value(), scope);
						}
					});
			},
			[&](const Required<AST::ArrayType>& arrayType)
			{
				resolveType(state, arrayType.value().elementType.value(), scope);
			},
			[&](const Required<AST::FunctionType>& functionType)
			{
				functionType.value().parameterTypes.forEach([&](const Required<AST::Type>& paramType)
					{
						resolveType(state, paramType.value(), scope);
					});
				if (functionType.value().returnType.hasValue())
				{
					resolveType(state, functionType.value().returnType.value(), scope);
				}
			},
			[&](const Required<AST::ComptimeExpression>& comptime)
			{
				resolveComptime(state, comptime.value(), const_cast<ScopedSymbolTable*>(scope));
			},
		}, baseType);
}

static void resolveExpression(ResolveState& state, const AST::Expression& expr, ScopedSymbolTable* scope)
{
	std::visit(Overloaded
		{
			[&](const Required<AST::IdentifierExpression>& ident)
			{
				resolveIdentifier(state, ident.value(), scope);
			},
			[&](const Required<AST::LiteralExpression>&) {},
			[&](const Required<AST::FunctionCallExpression>& call)
			{
				resolveExpression(state, call.value().function.value(), scope);
				call.value().arguments.forEach([&](const Required<AST::Expression>& arg)
					{
						resolveExpression(state, arg.value(), scope);
					});
			},
			[&](const Required<AST::MemberAccessExpression>& access)
			{
				resolveExpression(state, access.value().object.value(), scope);
				// memberName resolved in type-checking pass
			},
			[&](const Required<AST::ArrayAccessExpression>& access)
			{
				resolveExpression(state, access.value().object.value(), scope);
				resolveExpression(state, access.value().index.value(), scope);
			},
			[&](const Required<AST::UnaryExpression>& unary)
			{
				resolveExpression(state, unary.value().expression.value(), scope);
			},
			[&](const Required<AST::BinaryExpression>& binary)
			{
				resolveExpression(state, binary.value().left.value(), scope);
				resolveExpression(state, binary.value().right.value(), scope);
			},
			[&](const Required<AST::ArrayLiteralExpression>& arrayLit)
			{
				arrayLit.value().elements.forEach([&](const Required<AST::Expression>& elem)
					{
						resolveExpression(state, elem.value(), scope);
					});
			},
			[&](const Required<AST::ArrayFillExpression>& fill)
			{
				resolveExpression(state, fill.value().value.value(), scope);
			},
			[&](const Required<AST::StructInitializerExpression>& structInit)
			{
				if (structInit.value().type.hasValue())
				{
					resolveIdentifier(state, structInit.value().type.value().name.value(), scope);
				}
				structInit.value().initializers.forEach(
					[&](const Required<AST::StructInitializerExpression::MemberInitializer>& init)
					{
						resolveExpression(state, init.value().value.value(), scope);
						// init.value().name is a member field — resolved in type-checking pass
					});
			},
			[&](const Required<AST::LambdaExpression>& lambda)
			{
				ScopedSymbolTable lambdaScope(scope);
				lambda.value().captures.forEach([&](const Required<AST::Capture>& capture)
					{
						auto name = state.getStringView(capture.value().variableName);
						auto* outerDecl = state.lookupSingle(name, scope);
						if (!outerDecl)
						{
							state.logger.logErrorInRange(capture.value().variableName, capture.value().variableName, "Undefined name '{}' in capture.", name);
						}
						state.result.tokenNames[&capture.value().variableName] = outerDecl;
						lambdaScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, capture));
					});
				resolveFunction(state, lambda.value().function.value(), &lambdaScope);
			},
			[&](const Required<AST::IfExpression>& ifExpr)
			{
				resolveExpression(state, ifExpr.value().condition.value(), scope);
				{
					ScopedSymbolTable thenScope(scope);
					if (ifExpr.value().capture.hasValue())
					{
						const auto& cap = ifExpr.value().capture.value();
						auto name = state.getStringView(cap.variableName);
						thenScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, asRequired(cap)));
					}
					resolveStatement(state, ifExpr.value().thenBranch.value(), &thenScope);
				}
				if (ifExpr.value().elseBranch.hasValue())
				{
					ScopedSymbolTable elseScope(scope);
					resolveStatement(state, ifExpr.value().elseBranch.value(), &elseScope);
				}
			},
			[&](const Required<AST::WhileExpression>& whileExpr)
			{
				resolveExpression(state, whileExpr.value().condition.value(), scope);
				resolveStatement(state, whileExpr.value().body.value(), scope);
				if (whileExpr.value().elseBody.hasValue())
				{
					resolveStatement(state, whileExpr.value().elseBody.value(), scope);
				}
			},
			[&](const Required<AST::ForExpression>& forExpr)
			{
			// resolve iterables in outer scope, iterators bound in inner scope
			forExpr.value().iterables.value().forEach([&](const Required<AST::Expression>& iterable)
				{
					resolveExpression(state, iterable.value(), scope);
				});

			ScopedSymbolTable forScope(scope);
			forExpr.value().iterators.forEach([&](const Required<AST::Capture>& capture)
				{
					auto name = state.getStringView(capture.value().variableName);
					forScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, capture));
				});

			resolveStatement(state, forExpr.value().body.value(), &forScope);
			if (forExpr.value().elseBody.hasValue())
			{
				resolveStatement(state, forExpr.value().elseBody.value(), scope);
			}
			},
			[&](const Required<AST::MatchExpression>& match)
			{
				resolveExpression(state, match.value().subject.value(), scope);
				match.value().arms.forEach([&](const Required<AST::MatchArm>& arm)
					{
						// pattern resolves in the outer scope (capture is not yet bound)
						if (arm.value().pattern.hasValue())
						{
							resolveExpression(state, arm.value().pattern.value(), scope);
						}

						ScopedSymbolTable armScope(scope);
						if (arm.value().capture.hasValue())
						{
							const auto& cap = arm.value().capture.value();
							auto name = state.getStringView(cap.variableName);
							armScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, asRequired(cap)));
						}
						resolveStatement(state, arm.value().body.value(), &armScope);
					});
				if (match.value().externalElse.hasValue())
				{
					ScopedSymbolTable elseScope(scope);
					resolveStatement(state, match.value().externalElse.value(), &elseScope);
				}
			},
			[&](const Required<AST::ComptimeExpression>& comptime)
			{
				resolveComptime(state, comptime.value(), scope);
			},
			[&](const Required<AST::ComptimeResultExpression>&)
			{
				// Produced only by the comptime pass, which runs after resolution.
			},
		}, expr);
}

static void resolveComptime(ResolveState& state, const AST::ComptimeExpression& comptime, ScopedSymbolTable* scope)
{
	// '#' wraps an ordinary expression; resolve it normally. Whether the callee
	// of a '#'-call is a function or a macro is determined here by name lookup.
	resolveExpression(state, comptime.inner.value(), scope);
}

static void resolveStatement(ResolveState& state, const AST::Statement& stmt, ScopedSymbolTable* scope)
{
	std::visit(Overloaded
		{
			[&](const Required<AST::VariableDefinitionStatement>& varDef)
			{
			// resolve type annotation and value before adding name to scope
			// (prevents `var x = x` from binding to itself)
			if (varDef.value().type.hasValue())
			{
				resolveType(state, varDef.value().type.value(), scope);
			}
			resolveExpression(state, varDef.value().value.value(), scope);
			auto name = state.getStringView(varDef.value().name);
			if (scope->declare(name, state.allocateLocal(AST::Definition::Visibility::Private, varDef)) == Error)
			{
				state.logger.logErrorInRange(varDef.value().name, varDef.value().name,
					"Duplicate variable declaration '{}'.", name);
			}
		},
		[&](const Required<AST::AssignmentStatement>& assign)
		{
			resolveExpression(state, assign.value().target.value(), scope);
			resolveExpression(state, assign.value().value.value(), scope);
		},
		[&](const Required<AST::ExpressionStatement>& exprStmt)
		{
			resolveExpression(state, exprStmt.value().expression.value(), scope);
		},
		[&](const Required<AST::StatementBlock>& block)
		{
			ScopedSymbolTable blockScope(scope);
			block.value().statements.forEach([&](const Required<AST::Statement>& s)
				{
					resolveStatement(state, s.value(), &blockScope);
				});
		},
		[&](const Required<AST::BreakStatement>& breakStmt)
		{
			if (breakStmt.value().value.hasValue())
			{
				resolveExpression(state, breakStmt.value().value.value(), scope);
			}
		},
		[&](const Required<AST::ReturnStatement>& retStmt)
		{
			if (retStmt.value().value.hasValue())
			{
				resolveExpression(state, retStmt.value().value.value(), scope);
			}
		},

		// control-flow expressions used as statements — delegate to resolveExpression
		[&](const Required<AST::IfExpression>& ifExpr)
		{
			resolveExpression(state, AST::Expression(ifExpr), scope);
		},
		[&](const Required<AST::ForExpression>& forExpr)
		{
			resolveExpression(state, AST::Expression(forExpr), scope);
		},
		[&](const Required<AST::WhileExpression>& whileExpr)
		{
			resolveExpression(state, AST::Expression(whileExpr), scope);
		},
		[&](const Required<AST::MatchExpression>& matchExpr)
		{
			resolveExpression(state, AST::Expression(matchExpr), scope);
		},
		}, stmt);
}

static void resolveFunction(ResolveState& state, const AST::Function& fn, ScopedSymbolTable* parentScope)
{
	ScopedSymbolTable fnScope(parentScope);

	fn.parameters.forEach([&](const Required<AST::FunctionParameter>& param)
		{
			resolveType(state, param.value().type.value(), &fnScope);
			auto name = state.getStringView(param.value().name);
			fnScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, param));
		});

	if (fn.returnType.hasValue())
	{
		resolveType(state, fn.returnType.value(), &fnScope);
	}

	resolveStatement(state, AST::Statement(fn.body), &fnScope);
}

Result<ResolvedModule> resolve(
	const Source& moduleSource,
	const AST::Module& module,
	const SymbolTable& moduleSymbols,
	const std::vector<const SymbolTable*>& importedSymbols)
{
	ResolveState state(moduleSource, moduleSymbols);

	// build import alias map (imports and importedSymbols are in the same order)
	size_t importIdx = 0;
	module.imports.forEach([&](const Required<AST::Import>& imp)
		{
			if (importIdx < importedSymbols.size())
			{
				state.importAliases[imp.value().alias] = importedSymbols[importIdx++];
			}
		});

	// resolve each top-level definition
	module.definitions.forEach([&](const Required<AST::Definition>& def)
		{
			std::visit(Overloaded
				{
					[&](const Required<AST::TypeDefinition>& typeDef)
					{
						// Resolve ': I1, I2' interface markers.
						typeDef.value().interfaces.forEach([&](const Required<const Token*>& ifaceTok)
							{
								resolveInterfaceToken(state, ifaceTok.value());
							});

						resolveBaseType(state, typeDef.value().baseType.value(), nullptr);
					},
					[&](const Required<AST::FunctionDefinition>& fnDef)
					{
					// Resolve type-parameter interface constraints (built-in or user-defined).
					fnDef.value().typeParameters.forEach([&](const Required<AST::TypeParameter>& tp)
						{
							if (tp.value().interface.hasValue())
								resolveInterfaceToken(state, tp.value().interface.ptr());
						});

					// Inject type parameters into a scope so resolveType can find 'T', 'U', etc.
					ScopedSymbolTable typeParamScope(nullptr);
					fnDef.value().typeParameters.forEach([&](const Required<AST::TypeParameter>& tp)
						{
							auto name = state.getStringView(tp.value().name);
							typeParamScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, tp));
						});

					resolveFunction(state, fnDef.value().function.value(), &typeParamScope);
				},
				[&](const Required<AST::ExternDefinition>& externDef)
				{
					externDef.value().parameters.forEach([&](const Required<AST::FunctionParameter>& param)
						{
							resolveType(state, param.value().type.value(), nullptr);
						});
					if (externDef.value().returnType.hasValue())
					{
						resolveType(state, externDef.value().returnType.value(), nullptr);
					}
				},
				[&](const Required<AST::InterfaceDefinition>& ifaceDef)
				{
					ifaceDef.value().functions.forEach([&](const Required<AST::InterfaceFunction>& fn)
						{
							fn.value().parameters.forEach([&](const Required<AST::FunctionParameter>& param)
								{
									resolveType(state, param.value().type.value(), nullptr);
								});
							if (fn.value().returnType.hasValue())
							{
								resolveType(state, fn.value().returnType.value(), nullptr);
							}
						});
				},
				[&](const Required<AST::MacroDefinition>& macroDef)
				{
					ScopedSymbolTable macroScope(nullptr);
					macroDef.value().parameters.forEach([&](const Required<AST::FunctionParameter>& param)
						{
							resolveType(state, param.value().type.value(), &macroScope);
							auto name = state.getStringView(param.value().name);
							macroScope.declare(name, state.allocateLocal(AST::Definition::Visibility::Private, param));
						});
					resolveStatement(state, AST::Statement(macroDef.value().body), &macroScope);
				},
				}, def.value().definition);
		});

	Status status = state.logger.hasError() ? Error : Ok;
	return { status, std::move(state.result) };
}
