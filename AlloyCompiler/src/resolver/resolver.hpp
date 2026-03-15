#pragma once

#include <variant>
#include <unordered_map>

#include "../parser/AST.hpp"

enum class PrimitiveType
{
	Void,
	U8, U16, U32, U64,
	I8, I16, I32, I64,
	F32, F64,
	Bool
};

template <typename T>
using ConstRef = const T&;

struct ResolvedDeclaration
{
	AST::Definition::Visibility visibility;
	std::variant<
		PrimitiveType,
		ConstRef<AST::TypeDefinition>,
		ConstRef<AST::FunctionDefinition>,
		ConstRef<AST::ExternDefinition>,
		ConstRef<AST::FunctionParameter>,
		ConstRef<AST::VariableDefinitionStatement>,
		ConstRef<AST::Capture>,
	> definition;
};

using SymbolTable = std::unordered_map<std::string_view, ResolvedDeclaration>;

/*
* Declares all top-level definitions for the given module.
*/
Result<SymbolTable> declare(const Source& moduleSource, const AST::Module& module);