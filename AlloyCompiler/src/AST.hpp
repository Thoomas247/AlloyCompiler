#pragma once

#include <variant>

#include "tokenizer.hpp"

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
		return m_pValue != nullptr;
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
class Required
{
public:
	Required()
		: m_pValue(nullptr)
	{
	}

	Required(T* pValue)
		: m_pValue(pValue)
	{
	}

	const T& value() const
	{
		ASSERT(m_pValue != nullptr);
		return m_pValue;
	}

private:
	T* m_pValue;
};

namespace AST
{
	template <typename T>
	struct ListNode
	{
		ListNode(Required<T> item, Optional<ListNode<T>> next)
			: item(item), next(next)
		{
		}

		Required<T> item;
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

	struct NamedType;
	struct StructType;
	struct EnumType;
	struct ArrayType;
	struct FunctionType;
	struct StatementBlock;
	struct Statement;
	struct Function;

#pragma region Type Nodes

	using BaseType = std::variant<
		Required<NamedType>,
		Required<StructType>,
		Required<EnumType>,
		Required<ArrayType>,
		Required<FunctionType>
	>;

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
		std::variant<Required<BaseType>, Required<Type>> baseType;
	};

	struct NamedType
	{
		Required<Token> typeName;
		BaseType underlyingType;
	};

	struct StructType
	{
		struct Member
		{
			Required<Token> name;
			Type type;
		};

		Optional<ListNode<Member>> members;
	};

	struct EnumType
	{
		struct Member
		{
			Required<Token> name;
			Optional<Type> payloadType;
		};

		Optional<ListNode<Member>> members;
	};

	struct ArrayType
	{
		Required<Type> elementType;
		size_t size;
	};

	struct FunctionType
	{
		Optional<ListNode<Type>> parameterTypes;
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
		Required<Token> variableName;
	};

	struct IfExpression
	{
		using Branch = std::variant<Required<Expression>, Required<StatementBlock>>;

		Required<Expression> condition;
		Optional<Capture> capture;
		Branch thenBranch;
		Optional<Branch> elseBranch;
	};

	struct ForExpression
	{
		Required<ListNode<Expression>> iterables;
		Required<ListNode<Capture>> iterators;
		Required<StatementBlock> body;
		Optional<Statement> elseBody;
	};

	struct WhileExpression
	{
		Required<Expression> condition;
		Required<StatementBlock> body;
		Optional<Statement> elseBody;
	};

	struct FunctionCallExpression
	{
		Required<Expression> function;
		Required<ListNode<Expression>> arguments;
	};

	struct LambdaExpression
	{
		Optional<ListNode<Capture>> captures;
		Required<Function> function;
	};

#pragma region Statement Nodes

	struct Statement
	{
		// TODO
	};

	struct VariableDefinitionStatement
	{
		Required<Token> name;
		Optional<Type> type;
		Required<Expression> value;
		bool isMutable;
	};

	struct AssignmentStatement
	{
		Required<Expression> target;
		Required<Expression> value;
	};

	struct StatementBlock
	{
		Required<ListNode<Statement>> statements;
	};

#pragma endregion

#pragma region Function Nodes

	struct FunctionParameter
	{
		Required<Token> name;
		Required<Type> type;
	};

	struct Function
	{
		Required<ListNode<FunctionParameter>> parameters;
		Optional<Type> returnType;
		Required<StatementBlock> body;
	};

#pragma endregion

#pragma region Definition Nodes

	struct TypeDefinition
	{
		Required<Token> name;
		Required<BaseType> baseType;
	};

	struct FunctionDefinition
	{
		Required<Token> name;
		Required<Function> function;
	};

	struct ExternDefinition
	{
		Required<Token> name;
		Optional<ListNode<FunctionParameter>> parameters;
		bool isVariadic;
	};

	struct Definition
	{
		enum class Visibility
		{
			Private,
			Public,
			Export
		};

		Visibility visiblity;
		std::variant<TypeDefinition, FunctionDefinition, ExternDefinition> definition;
	};

#pragma endregion

	struct Module
	{
		Optional<ListNode<std::string_view>> imports;
		Optional<ListNode<Definition>> definitions;
	};
}