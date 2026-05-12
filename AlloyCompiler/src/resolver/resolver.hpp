#pragma once

#include <deque>
#include <variant>
#include <unordered_map>
#include <vector>

#include "../parser/AST.hpp"
#include "../builtins/builtins.hpp"

using Declaration = std::variant<
	Required<AST::TypeDefinition>,
	Required<AST::FunctionDefinition>,
	Required<AST::ExternDefinition>,
	Required<AST::FunctionParameter>,
	Required<AST::VariableDefinitionStatement>,
	Required<AST::Capture>,
	Required<AST::TypeParameter>
>;

struct ResolvedDeclaration
{
	AST::Definition::Visibility visibility;
	Declaration definition;
};

// A resolved interface constraint — either a built-in interface or (future) a user-defined
// interface declaration from the SymbolTable.
using ResolvedInterface = std::variant<BuiltinInterface, const ResolvedDeclaration*>;

class SymbolTable
{
public:
	// Functions (FunctionDefinition, ExternDefinition) may overload: same name is allowed.
	// All other declarations (types, variables) may not: duplicate name returns Error.
	// Also returns Error if a function name collides with an existing non-function declaration.
	Status add(std::string_view name, AST::Definition::Visibility visibility, Declaration definition);

	// Returns all declarations for this name. Empty = not found.
	std::vector<const ResolvedDeclaration*> get(std::string_view name) const;

private:
	std::unordered_map<std::string_view, std::vector<ResolvedDeclaration>> m_Symbols;
};

inline Status SymbolTable::add(std::string_view name, AST::Definition::Visibility visibility, Declaration definition)
{
	bool isFunction =
		std::holds_alternative<Required<AST::FunctionDefinition>>(definition) ||
		std::holds_alternative<Required<AST::ExternDefinition>>(definition);

	auto it = m_Symbols.find(name);
	if (it != m_Symbols.end())
	{
		// Name already exists — only function overloads are allowed.
		if (!isFunction)
			return Status::Error;  // non-function can't overload

		for (const auto& existing : it->second)
		{
			bool existingIsFunction =
				std::holds_alternative<Required<AST::FunctionDefinition>>(existing.definition) ||
				std::holds_alternative<Required<AST::ExternDefinition>>(existing.definition);
			if (!existingIsFunction)
				return Status::Error;  // function collides with an existing type/variable
		}

		it->second.push_back({ visibility, std::move(definition) });
		return Status::Ok;
	}

	m_Symbols[name].push_back({ visibility, std::move(definition) });
	return Status::Ok;
}

inline std::vector<const ResolvedDeclaration*> SymbolTable::get(std::string_view name) const
{
	auto it = m_Symbols.find(name);
	if (it == m_Symbols.end())
		return {};

	std::vector<const ResolvedDeclaration*> result;
	result.reserve(it->second.size());
	for (const auto& decl : it->second)
		result.push_back(&decl);
	return result;
}

struct ResolvedModule
{
	// Maps each IdentifierExpression to all matching declarations (overload set).
	// Single-declaration names (types, variables) have a 1-element vector.
	// Empty vector means resolution failed (error already logged).
	std::unordered_map<const AST::IdentifierExpression*, std::vector<const ResolvedDeclaration*>> names;

	// Token-keyed resolutions for lambda captures (always single, variables can't overload).
	std::unordered_map<const Token*, const ResolvedDeclaration*> tokenNames;

	// Maps each TypeParameter interface token → its resolved interface.
	// Built-in interfaces → BuiltinInterface enum.
	// User-defined interfaces (future) → ResolvedDeclaration* of the InterfaceDefinition.
	std::unordered_map<const Token*, ResolvedInterface> resolvedInterfaces;

	// Stable storage for function-scope declarations (parameters, local variables, captures,
	// type parameters). std::deque never moves elements on push_back, so all stored
	// ResolvedDeclaration* pointers into this container remain valid for the
	// lifetime of the ResolvedModule.
	std::deque<ResolvedDeclaration> localDecls;
};

/**
* Declares all top-level definitions for the given module.
* Source must outlive the returned SymbolTable (keys are string_views into source.data).
*/
Result<SymbolTable> declare(const Source& moduleSource, const AST::Module& module);

/**
* Resolves all name references in the module.
* moduleSymbols must be the result of declare() for this module.
* importedSymbols contains pre-resolved symbol tables for each imported module, in import order.
* All sources must outlive the returned ResolvedModule.
*/
Result<ResolvedModule> resolve(
	const Source& moduleSource,
	const AST::Module& module,
	const SymbolTable& moduleSymbols,
	const std::vector<const SymbolTable*>& importedSymbols
);
