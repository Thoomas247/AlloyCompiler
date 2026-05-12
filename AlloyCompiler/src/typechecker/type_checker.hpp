#pragma once

#include <unordered_map>
#include <vector>

#include "types.hpp"
#include "../resolver/resolver.hpp"
#include "../source/source.hpp"
#include "../util/result.hpp"

struct TypedModule
{
	// Inferred value type of every expression.
	// Value type = the T after stripping one level of pointer/reference modifier.
	std::unordered_map<const AST::Expression*, TypeId> exprTypes;

	// For each function call: the selected overload (after overload resolution).
	std::unordered_map<const AST::FunctionCallExpression*, const ResolvedDeclaration*> selectedOverloads;

	// For each generic call site: concrete TypeId bound for each type parameter token.
	std::unordered_map<const AST::FunctionCallExpression*, std::unordered_map<const Token*, TypeId>> typeArgs;
};

/**
 * Pass 2: Type Checker
 *
 * Infers the type of every expression, checks type compatibility at assignment
 * and call sites, resolves function overloads, and infers generic type arguments.
 *
 * Requires: intern() output and the module's SymbolTable (for method lookup).
 * All sources must outlive the returned TypedModule.
 */
// NOTE: interned is taken by non-const ref — the checker may intern new types
// (e.g. inferred array literal types) that have no corresponding AST annotation node.
Result<TypedModule> typeCheck(
	const Source& source,
	const AST::Module& module,
	const ResolvedModule& resolved,
	InternedTypes& interned,
	const SymbolTable& moduleSymbols
);
