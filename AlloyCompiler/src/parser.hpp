#pragma once

#include <string>
#include <vector>
#include <variant>

#include "source.hpp"

namespace AST
{

	template <typename T>
	class Optional
	{
	public:
		Optional()
			: m_pValue(nullptr)
		{
		}
		Optional(T* pValue)
			: m_pValue(pValue)
		{
		}

		bool hasValue() const
		{
			return m_Ptr != nullptr;
		}

		const T& value() const
		{
			ASSERT(m_pValue != nullptr);
			return *m_pValue;
		}

		const T* ptr() const
		{
			return m_pValue;
		}

	private:
		T* m_pValue;
	};

	template <typename T>
	struct ListNode
	{
		T item;
		Optional<ListNode<T>> next;

		template <typename Function>
		void forEach(Function&& func) const
		{
			const ListNode<T>* current = this;
			while (current)
			{
				func(current->item);
				current = current->next.ptr();
			}
		}
	};

#pragma region Type Nodes

	using BaseType = std::variant<NamedType, StructType, EnumType, ArrayType, FunctionType>;

	struct Type
	{
		enum class Modifier
		{
			None,
			Pointer,
			Reference,
			PointerToMutable,
			ReferenceToMutable
		};

		Modifier modifier;
		std::variant<BaseType, Type> baseType;
	};

	struct NamedType
	{
		const Token& typeName;
		BaseType underlyingType;
	};

	struct StructType
	{
		struct Member
		{
			const Token& name;
			Type type;
		};

		ListNode<Member>& members;
	};

	struct EnumType
	{
		struct Member
		{
			const Token& name;
			Optional<Type> payloadType;
		};

		ListNode<Member>& members;
	};

	struct ArrayType
	{
		Type elementType;
		size_t size;
	};

	struct FunctionType
	{
		ListNode<Type>& parameterTypes;
		Optional<Type> returnType;
	};

#pragma endregion

	struct Expression
	{
		// TODO
	};

	struct Capture
	{
		Type::Modifier modifier;
		const Token& variableName;
	};

	struct IfExpression
	{
		using Branch = std::variant<Expression, StatementBlock>;

		Expression& condition;
		Optional<Capture> capture;
		Branch thenBranch;
		Optional<Branch> elseBranch;
	};

	struct ForExpression
	{
		ListNode<Expression>& iterables;
		ListNode<Capture>& iterators;
		StatementBlock& body;
		Optional<Statement> elseBody;
	};

	struct WhileExpression
	{
		Expression& condition;
		StatementBlock& body;
		Optional<Statement> elseBody;
	};

	struct FunctionCallExpression
	{
		Expression& function;
		ListNode<Expression>& arguments;
	};

	struct LambdaExpression
	{
		Optional<ListNode<Capture>> captures;
		Function& function;
	};

#pragma region Statement Nodes

	struct Statement
	{
		// TODO
	};

	struct VariableDefinitionStatement
	{
		const Token& name;
		Optional<Type> type;
		Expression& value;
		bool isMutable;
	};

	struct AssignmentStatement
	{
		Expression& target;
		Expression& value;
	};

	struct StatementBlock
	{
		ListNode<Statement>& statements;
	};

#pragma endregion

#pragma region Function Nodes

	struct FunctionParameter
	{
		const Token& name;
		Type& type;
	};

	struct Function
	{
		ListNode<FunctionParameter>& parameters;
		Optional<Type> returnType;
		StatementBlock& body;
	};

#pragma endregion

#pragma region Definition Nodes

	struct TypeDefinition
	{
		const Token& name;
		BaseType baseType;
	};

	struct FunctionDefinition
	{
		const Token& name;
		Function& function;
	};

	struct ExternDefinition
	{
		const Token& name;
		ListNode<FunctionParameter>& parameters;
		bool isVariadic;
	};

	struct Definition
	{
		using BaseDefinition = std::variant<TypeDefinition, FunctionDefinition, ExternDefinition>;

		enum class Visibility
		{
			Private,
			Public,
			Export
		};

		Visibility visiblity;
		BaseDefinition definition;
	};

#pragma endregion

	struct Program
	{
		ListNode<Definition>& definitions;
	};
}

Result<bool> parse(const Source& source, const std::vector<std::string>& tokens);