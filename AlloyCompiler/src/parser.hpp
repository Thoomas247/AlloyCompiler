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
		std::string_view typeName;
		Type underlyingType;
	};

	struct StructType
	{
		struct Member
		{
			std::string_view name;
			Type type;
		};

		ListNode<Member>& members;
	};

	struct EnumType
	{
		struct Member
		{
			std::string_view name;
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
		std::string_view variableName;
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
		std::string_view name;
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
		std::string_view name;
		Type& type;
	};

	struct Function
	{
		ListNode<FunctionParameter>& parameters;
		Optional<Type> returnType;
		StatementBlock& body;
	};

	struct FunctionDefinition
	{
		std::string_view name;
		Function& function;
	};

	struct ExternDefinition
	{
		std::string_view name;
		ListNode<FunctionParameter>& parameters;
		bool isVariadic;
	};

#pragma endregion

	struct Program
	{
		ListNode<std::string_view>& imports;
	};
}

Result<bool> parse(const Source& source, const std::vector<std::string>& tokens);