#pragma once

#include <variant>

#include "../tokenizer/tokenizer.hpp"
#include "../util/pointers.hpp"
#include "../util/allocator.hpp"

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

	template <typename T>
	struct ListBuilder
	{
		Optional<ListNode<T>> head;

		void append(Required<T> item, Allocator& allocator)
		{
			auto* pNew = allocator.allocate<ListNode<T>>(item, Optional<ListNode<T>>());
			if (!m_pTail)
				head = pNew;
			else
				m_pTail->next = pNew;
			m_pTail = pNew;
		}

	private:
		ListNode<T>* m_pTail = nullptr;
	};

	struct NamedType;
	struct StructType;
	struct EnumType;
	struct ArrayType;
	struct FunctionType;

	struct IdentifierExpression;
	struct LiteralExpression;
	struct IfExpression;
	struct ForExpression;
	struct WhileExpression;
	struct MatchArm;
	struct MatchExpression;
	struct FunctionCallExpression;
	struct MemberAccessExpression;
	struct ArrayAccessExpression;
	struct ArrayLiteralExpression;
	struct ArrayFillExpression;
	struct StructInitializerExpression;
	struct LambdaExpression;
	struct BinaryExpression;
	struct UnaryExpression;

	struct VariableDefinitionStatement;
	struct AssignmentStatement;
	struct ExpressionStatement;
	struct StatementBlock;
	struct BreakStatement;
	struct ReturnStatement;

	using Expression = std::variant<
		Required<IdentifierExpression>,
		Required<LiteralExpression>,
		Required<IfExpression>,
		Required<ForExpression>,
		Required<WhileExpression>,
		Required<MatchExpression>,
		Required<FunctionCallExpression>,
		Required<MemberAccessExpression>,
		Required<ArrayAccessExpression>,
		Required<ArrayLiteralExpression>,
		Required<ArrayFillExpression>,
		Required<StructInitializerExpression>,
		Required<LambdaExpression>,
		Required<BinaryExpression>,
		Required<UnaryExpression>
	>;

	using Statement = std::variant<
		Required<VariableDefinitionStatement>,
		Required<AssignmentStatement>,
		Required<ExpressionStatement>,
		Required<StatementBlock>,
		Required<IfExpression>,
		Required<ForExpression>,
		Required<WhileExpression>,
		Required<MatchExpression>,
		Required<BreakStatement>,
		Required<ReturnStatement>
	>;

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
		std::variant<Required<BaseType>, Required<Type>> innerType;
	};

	struct NamedType
	{
		Required<IdentifierExpression> name;
	};

	struct StructType
	{
		struct Member
		{
			const Token& name;
			Required<Type> type;
		};

		Optional<ListNode<Member>> members;
	};

	struct EnumType
	{
		struct Member
		{
			const Token& name;
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

#pragma region Function Nodes

	struct FunctionParameter
	{
		const Token& name;
		Required<Type> type;
		bool isSelf;   // true iff declared with 'self' keyword
	};

	struct TypeParameter
	{
		const Token& name;
		Optional<Token> interface;
	};

	struct Function
	{
		Optional<ListNode<FunctionParameter>> parameters;
		Optional<Type> returnType;
		Required<StatementBlock> body;
	};

#pragma endregion

#pragma region Expression Nodes

	struct IdentifierExpression
	{
		Required<ListNode<const Token*>> path;
	};

	struct LiteralExpression
	{
		const Token& value;
	};

	struct Capture
	{
		Type::Modifier modifier;
		Token variableName;
	};

	struct IfExpression
	{
		Required<Expression> condition;
		Optional<Capture> capture;
		Required<Statement> thenBranch;
		Optional<Statement> elseBranch;
	};

	struct ForExpression
	{
		Required<ListNode<Expression>> iterables;
		Optional<ListNode<Capture>> iterators;
		Required<Statement> body;
		Optional<Statement> elseBody;
	};

	struct WhileExpression
	{
		Required<Expression> condition;
		Required<Statement> body;
		Optional<Statement> elseBody;
	};

	struct MatchArm
	{
		Required<IdentifierExpression> variant;
		Optional<Capture> capture;
		Required<Statement> body;
	};

	struct MatchExpression
	{
		Required<Expression> subject;
		Optional<ListNode<MatchArm>> arms;
		Optional<Statement> elseArm;
	};

	struct FunctionCallExpression
	{
		Required<Expression> function;
		Optional<ListNode<Type>> typeArguments;
		Optional<ListNode<Expression>> arguments;
	};

	struct MemberAccessExpression
	{
		Required<Expression> object;
		const Token& memberName;
	};

	struct ArrayAccessExpression
	{
		Required<Expression> object;
		Required<Expression> index;
	};

	struct ArrayLiteralExpression
	{
		Optional<ListNode<Expression>> elements;
	};

	struct ArrayFillExpression
	{
		Required<Expression> value;
		const Token& size;
	};

	struct StructInitializerExpression
	{
		struct MemberInitializer
		{
			const Token& name;
			Required<Expression> value;
		};

		Optional<NamedType> type;
		Optional<ListNode<MemberInitializer>> initializers;
	};

	struct LambdaExpression
	{
		Optional<ListNode<Capture>> captures;
		Required<Function> function;
	};

	struct UnaryExpression
	{
		TokenKind op;
		Required<Expression> expression;
	};

	struct BinaryExpression
	{
		TokenKind op;
		Required<Expression> left;
		Required<Expression> right;
	};

#pragma endregion

#pragma region Statement Nodes

	struct VariableDefinitionStatement
	{
		const Token& name;
		Optional<Type> type;
		Required<Expression> value;
		bool isMutable;
	};

	struct AssignmentStatement
	{
		TokenKind op;
		Required<Expression> target;
		Required<Expression> value;
	};

	struct ExpressionStatement
	{
		Required<Expression> expression;
	};

	struct StatementBlock
	{
		Optional<ListNode<Statement>> statements;
	};

	struct BreakStatement
	{
		Optional<Expression> value;
	};

	struct ReturnStatement
	{
		Optional<Expression> value;
	};

#pragma endregion

#pragma region Definition Nodes

	struct TypeDefinition
	{
		const Token& name;
		Required<BaseType> baseType;
	};

	struct FunctionDefinition
	{
		const Token& name;
		Optional<ListNode<TypeParameter>> typeParameters;
		Required<Function> function;
	};

	struct ExternDefinition
	{
		const Token& name;
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
		std::variant<Required<TypeDefinition>, Required<FunctionDefinition>, Required<ExternDefinition>> definition;
	};

#pragma endregion

	struct Import
	{
		std::string_view path;   // full module path, e.g. "a::b::c"
		std::string_view alias;  // user-given alias; same as path if no alias
	};

	struct Module
	{
		Optional<ListNode<Import>> imports;
		Optional<ListNode<Definition>> definitions;
	};
}