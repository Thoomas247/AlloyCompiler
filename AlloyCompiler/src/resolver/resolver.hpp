#pragma once

#include <variant>
#include <unordered_map>
#include <vector>

#include "../parser/AST.hpp"

using Declaration = std::variant<
	Required<AST::TypeDefinition>,
	Required<AST::FunctionDefinition>,
	Required<AST::ExternDefinition>,
	Required<AST::FunctionParameter>,
	Required<AST::VariableDefinitionStatement>,
	Required<AST::Capture>
>;

struct ResolvedDeclaration
{
	AST::Definition::Visibility visibility;
	Declaration definition;
};

class SymbolTable
{
public:
	template <typename T>
	Status add(std::string_view name, AST::Definition::Visibility visibility, T definition);

	const ResolvedDeclaration* get(std::string_view name) const;

private:
	std::unordered_map<std::string_view, ResolvedDeclaration> m_Symbols;
};

template<typename T>
inline Status SymbolTable::add(std::string_view name, AST::Definition::Visibility visibility, T definition)
{
	if (m_Symbols.try_emplace(name, visibility, Declaration(definition)).second)
	{
		return Status::Ok;
	}
	return Status::Error;
}

inline const ResolvedDeclaration* SymbolTable::get(std::string_view name) const
{
	auto it = m_Symbols.find(name);
	if (it != m_Symbols.end())
	{
		return &it->second;
	}
	return nullptr;
}

struct ResolvedModule
{
	// each IdentifierExpression resolved to its declaration
	std::unordered_map<const AST::IdentifierExpression*, const ResolvedDeclaration*> names;

	// token-keyed resolutions: lambda capture sites and EnumVariantExpression::typeName
	std::unordered_map<const Token*, const ResolvedDeclaration*> tokenNames;
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
