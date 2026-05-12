#include <vector>

#include "../util/logger.hpp"
#include "../util/allocator.hpp"
#include "../tokenizer/tokenizer.hpp"
#include "../parser/parser.hpp"
#include "token_iterator.hpp"

using enum Status;
using enum TokenKind;

struct ParserState
{
	Logger logger;
	TokenIterator it;
	Allocator allocator;

	ParserState(const Source& source, const std::vector<Token>& tokens)
		: logger(source), it(logger, source, tokens)
	{
	}
};

#pragma region Util

#define ERROR_IF_FALSE(expr)	\
	do {						\
		if (!expr)				\
			return { Error };	\
	} while(0)

#define ERROR_IF_ERROR(status)	\
	do {						\
		if (status == Error)	\
			return { Error };	\
	} while(0)

static AST::Type::Modifier getTypeModifier(ParserState& state)
{
	auto modifier = AST::Type::Modifier::None;

	if (state.it.peek().kind == Multiply)
	{
		state.it.consume<Multiply>();
		if (state.it.peek().kind == Var)
		{
			modifier = AST::Type::Modifier::PointerToMutable;
			state.it.consume<Var>();
		}
		else
		{
			modifier = AST::Type::Modifier::Pointer;
		}
	}
	else if (state.it.peek().kind == BitwiseAnd)
	{
		state.it.consume<BitwiseAnd>();
		if (state.it.peek().kind == Var)
		{
			modifier = AST::Type::Modifier::ReferenceToMutable;
			state.it.consume<Var>();
		}
		else
		{
			modifier = AST::Type::Modifier::Reference;
		}
	}

	return modifier;
}

template <TokenKind EndTokenKind, typename T, typename F>
static Result<Optional<AST::ListNode<T>>> zeroOrMore(ParserState& state, F&& parse)
{
	AST::ListBuilder<T> list;

	while (state.it.peek().kind != EndTokenKind)
	{
		auto [status, node] = parse(state);
		ERROR_IF_ERROR(status);

		list.append(node, state.allocator);
	}

	ASSERT(state.it.consume<EndTokenKind>());

	return { Ok, list.head };
}

template <TokenKind EndTokenKind, typename T, typename F>
static Result<Optional<AST::ListNode<T>>> commaSeparatedList(ParserState& state, F&& parse)
{
	AST::ListBuilder<T> list;

	while (state.it.peek().kind != EndTokenKind)
	{
		auto [status, node] = parse(state);
		ERROR_IF_ERROR(status);

		list.append(node, state.allocator);

		if (state.it.peek().kind == Comma)
		{
			state.it.consume();
		}
		else
		{
			break;
		}
	}

	ASSERT(state.it.consume<EndTokenKind>());

	return { Ok, list.head };
}

static int getBinaryOperatorPrecedence(TokenKind kind)
{
	switch (kind)
	{
	case Multiply:
	case Divide:
	case Modulo:
		return 100;

	case Plus:
	case Minus:
		return 90;

	case ShiftLeft:
	case ShiftRight:
		return 80;

	case Less:
	case LessEqual:
	case Greater:
	case GreaterEqual:
		return 70;

	case Equal:
	case NotEqual:
		return 60;

	case BitwiseAnd:
		return 50;

	case Xor:
		return 40;

	case BitwiseOr:
		return 30;

	case LogicalAnd:
		return 20;


	case LogicalOr:
		return 10;

	default:
		return -1;
	}
}

static bool isUnaryOperator(TokenKind kind)
{
	return
		kind == BitwiseNot ||
		kind == Not ||
		kind == BitwiseAnd ||
		kind == Multiply ||
		kind == New ||
		kind == Move;
}

static bool isAssignmentOperator(TokenKind kind)
{
	return
		kind == TokenKind::Assign ||
		kind == TokenKind::PlusAssign ||
		kind == TokenKind::MinusAssign ||
		kind == TokenKind::MultiplyAssign ||
		kind == TokenKind::DivideAssign ||
		kind == TokenKind::ModuloAssign ||
		kind == TokenKind::ShiftLeftAssign ||
		kind == TokenKind::ShiftRightAssign ||
		kind == TokenKind::AndAssign ||
		kind == TokenKind::OrAssign ||
		kind == TokenKind::XorAssign;
}

static Result<size_t> getOffsetToClosing(const ParserState& state, TokenKind opening, TokenKind closing)
{
	size_t depth = 0;
	size_t offset = 0;
	while (true)
	{
		auto& token = state.it.peek(offset);

		if (token.kind == EndOfFile)
		{
			return { Error };
		}

		if (token.kind == opening)
		{
			++depth;
		}
		else if (token.kind == closing)
		{
			--depth;

			if (depth == 0)
			{
				return { Ok, offset };
			}
		}

		++offset;
	}
}

#pragma endregion

#pragma region Type Nodes

static Result<Required<AST::Type>> parseType(ParserState& state);
static Result<Required<AST::IdentifierExpression>> parseIdentifierExpression(ParserState& state);

static Result<Required<AST::NamedType>> parseNamedType(ParserState& state)
{
	auto [status, identifier] = parseIdentifierExpression(state);
	ERROR_IF_ERROR(status);
	return { Ok, state.allocator.allocate<AST::NamedType>(identifier) };
}

static Result<Required<AST::StructType>> parseStructType(ParserState& state)
{
	ASSERT(state.it.consume<Struct>());

	ERROR_IF_FALSE(state.it.consume<LBrace>("'{' after 'struct' keyword"));

	auto [status, members] = zeroOrMore<RBrace, AST::StructType::Member>(
		state,
		[](ParserState& state) -> Result<Required<AST::StructType::Member>>
		{
			auto [status, memberNameToken] = state.it.consume<Identifier>("member name");
			ERROR_IF_ERROR(status);

			ERROR_IF_FALSE(state.it.consume<Colon>("':' after member name"));

			auto [typeStatus, memberType] = parseType(state);
			ERROR_IF_ERROR(typeStatus);

			ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after stuct member definition"));

			return { Ok,  state.allocator.allocate<AST::StructType::Member>(
				memberNameToken,
				memberType
			) };
		}
	);

	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::StructType>(members) };
}

static Result<Required<AST::EnumType>> parseEnumType(ParserState& state)
{
	ASSERT(state.it.consume<Enum>());

	ERROR_IF_FALSE(state.it.consume<LBrace>("'{' after 'enum' keyword"));

	auto [status, members] = zeroOrMore<RBrace, AST::EnumType::Member>(
		state,
		[](ParserState& state) -> Result<Required<AST::EnumType::Member>>
		{
			auto [status, memberNameToken] = state.it.consume<Identifier>("member name");
			ERROR_IF_ERROR(status);

			Optional<AST::Type> payloadType;
			if (state.it.peek().kind == Colon)
			{
				state.it.consume();

				auto [typeStatus, memberType] = parseType(state);
				ERROR_IF_ERROR(typeStatus);

				payloadType = Optional(memberType.ptr());
			}

			ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after enum member definition"));

			return { Ok, state.allocator.allocate<AST::EnumType::Member>(
				memberNameToken,
				payloadType
			) };
		});

	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::EnumType>(members) };
}

static Result<Required<AST::ArrayType>> parseArrayType(ParserState& state)
{
	ASSERT(state.it.consume<LBracket>());

	auto [typeStatus, elementType] = parseType(state);
	ERROR_IF_ERROR(typeStatus);

	size_t size = 0;
	if (state.it.peek().kind == Semicolon)
	{
		state.it.consume<Semicolon>();

		auto [status, sizeToken] = state.it.consume<IntegerLiteral>("integral array size after ';'");
		ERROR_IF_ERROR(status);

		size = std::stoull(std::string(state.it.createView(sizeToken, sizeToken)));
	}

	ERROR_IF_FALSE(state.it.consume<RBracket>("']' after array type"));

	return { Ok, state.allocator.allocate<AST::ArrayType>(elementType, size) };
}

static Result<Required<AST::FunctionType>> parseFunctionType(ParserState& state)
{
	ASSERT(state.it.consume<LParen>());

	AST::ListBuilder<AST::Type> parameterTypes;

	while (state.it.peek().kind != RParen)
	{
		auto [typeStatus, parameterType] = parseType(state);
		ERROR_IF_ERROR(typeStatus);

		parameterTypes.append(parameterType, state.allocator);

		if (state.it.peek().kind == Comma)
		{
			state.it.consume<Comma>();
		}
		else
		{
			break;
		}
	}

	ERROR_IF_FALSE(state.it.consume<RParen>("')' after function parameter types"));

	Optional<AST::Type> returnType;

	if (state.it.peek().kind == Arrow)
	{
		state.it.consume<Arrow>();

		auto [typeStatus, retType] = parseType(state);
		ERROR_IF_ERROR(typeStatus);

		returnType = Optional(retType.ptr());
	}

	return { Ok, state.allocator.allocate<AST::FunctionType>(parameterTypes.head, returnType) };
}

static Result<Required<AST::BaseType>> parseBaseType(ParserState& state)
{
	switch (state.it.peek().kind)
	{
	case Identifier:
	{
		auto [status, namedType] = parseNamedType(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::BaseType>(namedType) };
	}

	case Struct:
	{
		auto [status, structType] = parseStructType(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::BaseType>(structType) };
	}

	case Enum:
	{
		auto [status, enumType] = parseEnumType(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::BaseType>(enumType) };
	}

	case LBracket:
	{
		auto [status, arrayType] = parseArrayType(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::BaseType>(arrayType) };
	}

	case LParen:
	{
		auto [status, functionType] = parseFunctionType(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::BaseType>(functionType) };
	}

	default:
	{
		const auto& token = state.it.peek();
		state.logger.logErrorInRange(
			token,
			token,
			"Expected type, but found '{}'.",
			token
		);
		return { Error };
	}
	}
}

static Result<Required<AST::Type>> parseType(ParserState& state)
{
	auto modifier = getTypeModifier(state);

	std::variant<Required<AST::BaseType>, Required<AST::Type>> innerType;

	if (state.it.peek().kind == Multiply || state.it.peek().kind == BitwiseAnd)
	{
		auto [status, type] = parseType(state);
		ERROR_IF_ERROR(status);
		innerType = type;
	}
	else
	{
		auto [status, baseType] = parseBaseType(state);
		ERROR_IF_ERROR(status);
		innerType = baseType;
	}

	return { Ok, state.allocator.allocate<AST::Type>(modifier, innerType) };
}

#pragma endregion

#pragma region Function Nodes

static Result<Required<AST::StatementBlock>> parseStatementBlock(ParserState& state);

static Result<Required<AST::FunctionParameter>> parseFunctionParameter(ParserState& state)
{
	auto [status, nameToken] = state.it.consume<Identifier>("parameter name");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<Colon>("':' after parameter name"));

	auto [typeStatus, type] = parseType(state);
	ERROR_IF_ERROR(typeStatus);

	auto functionParameter = state.allocator.allocate<AST::FunctionParameter>(nameToken, type);

	return { Ok, functionParameter };
}

static Result<Required<AST::Function>> parseFunction(ParserState& state)
{
	ASSERT(state.it.consume<LParen>());

	auto parametersResult = commaSeparatedList<RParen, AST::FunctionParameter>(state, parseFunctionParameter);
	ERROR_IF_FALSE(parametersResult);

	Optional<AST::Type> returnType;
	if (state.it.peek().kind == Arrow)
	{
		state.it.consume();

		auto [status, type] = parseType(state);
		ERROR_IF_ERROR(status);

		returnType = type.ptr();
	}

	auto statementResult = parseStatementBlock(state);
	ERROR_IF_FALSE(statementResult);

	return { Ok, state.allocator.allocate<AST::Function>(parametersResult.value, returnType, statementResult.value) };
}

#pragma endregion

#pragma region Expression Nodes

static Result<Required<AST::Expression>> parseExpression(ParserState& state, int minPrecedence = 0);
static Result<Required<AST::Statement>> parseStatement(ParserState& state);

static Result<Required<AST::IdentifierExpression>> parseIdentifierExpression(ParserState& state)
{
	Optional<AST::ListNode<const Token*>> path;
	AST::ListNode<const Token*>* pPathTail = nullptr;

	while (true)
	{
		auto [status, segment] = state.it.consume<Identifier>("identifier");
		ERROR_IF_ERROR(status);

		auto* pNew = state.allocator.allocate<AST::ListNode<const Token*>>(
			state.allocator.allocate<const Token*>(&segment),
			Optional<AST::ListNode<const Token*>>()
		);
		if (!pPathTail)
			path = pNew;
		else
			pPathTail->next = pNew;
		pPathTail = pNew;

		if (state.it.peek().kind == DoubleColon)
		{
			state.it.consume<DoubleColon>();
		}
		else
		{
			break;
		}
	}

	return { Ok, state.allocator.allocate<AST::IdentifierExpression>(Required(path.ptr())) };
}

static Result<Required<AST::LiteralExpression>> parseLiteralExpression(ParserState& state)
{
	auto [status, literalToken] = state.it.consume<CharLiteral, StringLiteral, IntegerLiteral, FloatLiteral>("a literal");
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::LiteralExpression>(literalToken) };
}

static Result<Required<AST::Capture>> parseCapture(ParserState& state)
{
	auto modifier = getTypeModifier(state);
	auto [status, captureNameToken] = state.it.consume<Identifier>("capture name");
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::Capture>(modifier, captureNameToken) };
}

static Result<Required<AST::IfExpression>> parseIfExpression(ParserState& state)
{
	ASSERT(state.it.consume<If>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'if' keyword"));

	auto expressionResult = parseExpression(state);
	ERROR_IF_FALSE(expressionResult);

	ERROR_IF_FALSE(state.it.consume<RParen>("')' after condition"));

	Optional<AST::Capture> ifCapture;
	if (state.it.peek().kind == BitwiseOr)
	{
		state.it.consume();

		auto [status, capture] = parseCapture(state);
		ERROR_IF_ERROR(status);

		ERROR_IF_FALSE(state.it.consume<BitwiseOr>("'|' after capture name"));

		ifCapture = capture.ptr();
	}

	auto thenResult = parseStatement(state);
	ERROR_IF_FALSE(thenResult);

	Optional<AST::Statement> elseBranch;
	if (state.it.peek().kind == Else)
	{
		state.it.consume();

		auto [status, branch] = parseStatement(state);
		ERROR_IF_ERROR(status);

		elseBranch = branch.ptr();
	}

	return { Ok, state.allocator.allocate<AST::IfExpression>(expressionResult.value, ifCapture, thenResult.value, elseBranch) };
}

static Result<Required<AST::ForExpression>> parseForExpression(ParserState& state)
{
	ASSERT(state.it.consume<For>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'for' keyword"));

	auto iterablesResult = commaSeparatedList<RParen, AST::Expression>(state, [](ParserState& state) { return parseExpression(state, 0); });
	ERROR_IF_FALSE(iterablesResult);
	if (!iterablesResult.value.hasValue())
	{
		state.logger.logErrorUnexpected(state.it.previous(), state.it.previous(), "an iterable expression", ")");
		return { Error };
	}

	Optional<AST::ListNode<AST::Capture>> iterators;
	if (state.it.peek().kind == BitwiseOr)
	{
		state.it.consume();

		auto [status, captures] = commaSeparatedList<BitwiseOr, AST::Capture>(state, parseCapture);
		ERROR_IF_ERROR(status);

		iterators = captures;
	}

	auto bodyResult = parseStatement(state);
	ERROR_IF_FALSE(bodyResult);

	Optional<AST::Statement> elseBody;
	if (state.it.peek().kind == Else)
	{
		state.it.consume();

		auto [status, statement] = parseStatement(state);
		ERROR_IF_ERROR(status);

		elseBody = statement.ptr();
	}

	return { Ok, state.allocator.allocate<AST::ForExpression>(Required(iterablesResult.value.ptr()), iterators, bodyResult.value, elseBody) };
}

static Result<Required<AST::WhileExpression>> parseWhileExpression(ParserState& state)
{
	ASSERT(state.it.consume<While>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'while' keyword"));

	auto conditionResult = parseExpression(state);
	ERROR_IF_FALSE(conditionResult);

	ERROR_IF_FALSE(state.it.consume<RParen>("')' after condition"));

	auto bodyResult = parseStatement(state);
	ERROR_IF_FALSE(bodyResult);

	Optional<AST::Statement> elseBody;
	if (state.it.peek().kind == Else)
	{
		state.it.consume();

		auto [status, statement] = parseStatement(state);
		ERROR_IF_ERROR(status);

		elseBody = statement.ptr();
	}

	return { Ok, state.allocator.allocate<AST::WhileExpression>(conditionResult.value, bodyResult.value, elseBody) };
}

static Result<Required<AST::MatchExpression>> parseMatchExpression(ParserState& state)
{
	ASSERT(state.it.consume<Match>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'match'"));

	auto [subjectStatus, subject] = parseExpression(state);
	ERROR_IF_ERROR(subjectStatus);

	ERROR_IF_FALSE(state.it.consume<RParen>("')' after match subject"));
	ERROR_IF_FALSE(state.it.consume<LBrace>("'{' after match subject"));

	Optional<AST::ListNode<AST::MatchArm>> arms;
	AST::ListNode<AST::MatchArm>* pArmsTail = nullptr;
	Optional<AST::Statement> elseArm;

	while (state.it.peek().kind != RBrace)
	{
		if (state.it.peek().kind == Else)
		{
			state.it.consume<Else>();

			auto [status, statement] = parseStatement(state);
			ERROR_IF_ERROR(status);

			elseArm = statement.ptr();
			break;
		}

		auto [variantStatus, variant] = parseIdentifierExpression(state);
		ERROR_IF_ERROR(variantStatus);

		Optional<AST::Capture> capture;
		if (state.it.peek().kind == BitwiseOr)
		{
			state.it.consume();

			auto [captureStatus, cap] = parseCapture(state);
			ERROR_IF_ERROR(captureStatus);

			ERROR_IF_FALSE(state.it.consume<BitwiseOr>("'|' after capture name"));

			capture = cap.ptr();
		}

		auto [bodyStatus, body] = parseStatement(state);
		ERROR_IF_ERROR(bodyStatus);

		auto* pNew = state.allocator.allocate<AST::ListNode<AST::MatchArm>>(
			Required(state.allocator.allocate<AST::MatchArm>(variant, capture, body)),
			Optional<AST::ListNode<AST::MatchArm>>()
		);
		if (!pArmsTail)
			arms = pNew;
		else
			pArmsTail->next = pNew;
		pArmsTail = pNew;
	}

	ERROR_IF_FALSE(state.it.consume<RBrace>("'}' after match arms"));

	return { Ok, state.allocator.allocate<AST::MatchExpression>(subject, arms, elseArm) };
}

static Result<Required<AST::FunctionCallExpression>> parseFunctionCallExpression(ParserState& state, Required<AST::Expression> left)
{
	ASSERT(state.it.consume<LParen>());

	auto [status, arguments] = commaSeparatedList<RParen, AST::Expression>(state, [](ParserState& state) { return parseExpression(state, 0); });
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::FunctionCallExpression>(left, arguments) };
}

static Result<Required<AST::MemberAccessExpression>> parseMemberAccessExpression(ParserState& state, Required<AST::Expression> left)
{
	ASSERT(state.it.consume<Dot>());

	auto [status, memberName] = state.it.consume<Identifier>("an identifier");
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::MemberAccessExpression>(left, memberName) };
}

static Result<Required<AST::ArrayAccessExpression>> parseArrayAccessExpression(ParserState& state, Required<AST::Expression> left)
{
	ASSERT(state.it.consume<LBracket>());

	auto [status, index] = parseExpression(state);
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<RBracket>("a ']' after the expression"));

	return { Ok, state.allocator.allocate<AST::ArrayAccessExpression>(left, index) };
}

static Result<Required<AST::LambdaExpression>> parseLambdaExpression(ParserState& state)
{
	Optional<AST::ListNode<AST::Capture>> captures;
	if (state.it.peek().kind == BitwiseOr)
	{
		state.it.consume();

		auto [status, captureNames] = commaSeparatedList<BitwiseOr, AST::Capture>(state, parseCapture);
		ERROR_IF_ERROR(status);

		captures = captureNames;
	}

	auto [status, function] = parseFunction(state);
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::LambdaExpression>(captures, function) };
}

static Result<Required<AST::StructInitializerExpression>> parseStructInitializerExpression(ParserState& state, Optional<AST::NamedType> type)
{
	ERROR_IF_FALSE(state.it.consume<LBrace>("'{'"));

	auto [status, initializers] = commaSeparatedList<RBrace, AST::StructInitializerExpression::MemberInitializer>(
		state,
		[](ParserState& state) -> Result<Required<AST::StructInitializerExpression::MemberInitializer>>
		{
			ERROR_IF_FALSE(state.it.consume<Dot>("'.' before member name"));
			auto [nameStatus, name] = state.it.consume<Identifier>("member name");
			ERROR_IF_ERROR(nameStatus);
			ERROR_IF_FALSE(state.it.consume<Assign>("'='"));
			auto [valueStatus, value] = parseExpression(state);
			ERROR_IF_ERROR(valueStatus);
			return { Ok, state.allocator.allocate<AST::StructInitializerExpression::MemberInitializer>(name, value) };
		}
	);
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::StructInitializerExpression>(type, initializers) };
}

static Result<Required<AST::Expression>> parsePrimaryExpression(ParserState& state)
{
	switch (state.it.peek().kind)
	{
	case If:
	{
		auto [status, expression] = parseIfExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	case For:
	{
		auto [status, expression] = parseForExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	case While:
	{
		auto [status, expression] = parseWhileExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	case Match:
	{
		auto [status, expression] = parseMatchExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Expression>(expression) };
	}

	case BitwiseOr:
	{
		// |...| (...) {...} or |...| (...) -> ... {...}
		auto [status, expression] = parseLambdaExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	case LParen:
	{
		auto seekResult = getOffsetToClosing(state, LParen, RParen);
		ERROR_IF_FALSE(seekResult);

		auto tokenAfterParen = state.it.peek(seekResult.value + 1).kind;
		if (tokenAfterParen == Arrow || tokenAfterParen == LBrace)
		{
			// (...) {...} or (...) -> ... {...}
			auto [status, expression] = parseLambdaExpression(state);
			ERROR_IF_ERROR(status);
			return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
		}

		state.it.consume<LParen>();
		auto [status, expression] = parseExpression(state);
		ERROR_IF_ERROR(status);
		ERROR_IF_FALSE(state.it.consume<RParen>("')'"));

		return { Ok,  expression };
	}

	case LBracket:
	{
		state.it.consume<LBracket>();

		auto [firstStatus, firstExpr] = parseExpression(state);
		ERROR_IF_ERROR(firstStatus);

		// [expr; N]
		if (state.it.peek().kind == Semicolon)
		{
			state.it.consume<Semicolon>();

			auto [sizeStatus, sizeToken] = state.it.consume<IntegerLiteral>("integral array size after ';'");
			ERROR_IF_ERROR(sizeStatus);

			ERROR_IF_FALSE(state.it.consume<RBracket>("']' after array fill"));

			return { Ok, state.allocator.allocate<AST::Expression>(
				Required(state.allocator.allocate<AST::ArrayFillExpression>(firstExpr, sizeToken))
			) };
		}

		// [a, b, c]
		auto* pElemsTail = state.allocator.allocate<AST::ListNode<AST::Expression>>(firstExpr, Optional<AST::ListNode<AST::Expression>>());
		Optional<AST::ListNode<AST::Expression>> elements = pElemsTail;

		while (state.it.peek().kind == Comma)
		{
			state.it.consume<Comma>();

			auto [elemStatus, elem] = parseExpression(state);
			ERROR_IF_ERROR(elemStatus);

			auto* pNew = state.allocator.allocate<AST::ListNode<AST::Expression>>(elem, Optional<AST::ListNode<AST::Expression>>());
			pElemsTail->next = pNew;
			pElemsTail = pNew;
		}

		ERROR_IF_FALSE(state.it.consume<RBracket>("']' after array literal"));

		return { Ok, state.allocator.allocate<AST::Expression>(
			Required(state.allocator.allocate<AST::ArrayLiteralExpression>(elements))
		) };
	}

	case LBrace:
	{
		auto [status, expression] = parseStructInitializerExpression(state, Optional<AST::NamedType>());
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Expression>(expression) };
	}

	case Identifier:
	{
		if (state.it.peek(1).kind == LBrace)
		{
			auto [typeStatus, type] = parseNamedType(state);
			ERROR_IF_ERROR(typeStatus);
			auto [status, expression] = parseStructInitializerExpression(state, Optional<AST::NamedType>(type.ptr()));
			ERROR_IF_ERROR(status);
			return { Ok, state.allocator.allocate<AST::Expression>(expression) };
		}

		auto [status, identifier] = parseIdentifierExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Expression>(identifier) };
	}

	case CharLiteral:
	case StringLiteral:
	case IntegerLiteral:
	case FloatLiteral:
	{
		auto [status, expression] = parseLiteralExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	default:
		state.logger.logErrorUnexpected(state.it.peek(), state.it.peek(), "an expression", state.it.peek());
		return { Error };
	}
}

static Result<Required<AST::Expression>> parsePostfixExpression(ParserState& state)
{
	auto [status, primary] = parsePrimaryExpression(state);
	ERROR_IF_ERROR(status);

	while (true)
	{
		if (state.it.peek().kind == LParen)
		{
			auto functionCallResult = parseFunctionCallExpression(state, primary);
			ERROR_IF_FALSE(functionCallResult);

			primary = state.allocator.allocate<AST::Expression>(functionCallResult.value);
			continue;
		}

		if (state.it.peek().kind == Dot)
		{
			auto memberAccessResult = parseMemberAccessExpression(state, primary);
			ERROR_IF_FALSE(memberAccessResult);

			primary = state.allocator.allocate<AST::Expression>(memberAccessResult.value);
			continue;
		}


		if (state.it.peek().kind == LBracket)
		{
			auto arrayAccessResult = parseArrayAccessExpression(state, primary);
			ERROR_IF_FALSE(arrayAccessResult);

			primary = state.allocator.allocate<AST::Expression>(arrayAccessResult.value);
			continue;
		}

		break;
	}

	return { Ok, primary };
}

static Result<Required<AST::Expression>> parseUnaryExpression(ParserState& state)
{
	TokenKind op = state.it.peek().kind;

	if (isUnaryOperator(op))
	{
		state.it.consume();

		auto [status, operand] = parseUnaryExpression(state);
		ERROR_IF_ERROR(status);

		return { Ok,  state.allocator.allocate<AST::Expression>(
			Required(state.allocator.allocate<AST::UnaryExpression>(
				op, operand)
			)
		) };
	}

	return parsePostfixExpression(state);
}

static Result<Required<AST::Expression>> parseExpression(ParserState& state, int minPrecedence)
{
	auto [status, left] = parseUnaryExpression(state);
	ERROR_IF_ERROR(status);

	while (true)
	{
		TokenKind op = state.it.peek().kind;

		int precedence = getBinaryOperatorPrecedence(op);
		if (precedence < minPrecedence)
		{
			break;
		}

		state.it.consume();

		auto rightResult = parseExpression(state, precedence + 1);
		ERROR_IF_FALSE(rightResult);

		auto* pBin = state.allocator.allocate<AST::BinaryExpression>(op, left, rightResult.value);
		left = state.allocator.allocate<AST::Expression>(Required<AST::BinaryExpression>(pBin));
	}

	return { Ok, left };
}

#pragma endregion

#pragma region Statement Nodes

static Result<Required<AST::VariableDefinitionStatement>> parseVariableDefinitionStatement(ParserState& state)
{
	auto keywordResult = state.it.consume<Var, Const>();
	ASSERT(keywordResult);

	bool isMutable = keywordResult.value.kind == Var;

	auto nameResult = state.it.consume<Identifier>("a variable name");
	ERROR_IF_FALSE(nameResult);

	Optional<AST::Type> type;
	if (state.it.peek().kind == Colon)
	{
		state.it.consume();

		auto [status, variableType] = parseType(state);
		ERROR_IF_ERROR(status);

		type = variableType.ptr();
	}

	ERROR_IF_FALSE(state.it.consume<Assign>("an initializer"));

	auto valueResult = parseExpression(state);
	ERROR_IF_FALSE(valueResult);

	// block-like expressions (for, while, if) may have already consumed
	// the semicolon via a break/return in their else branch
	if (state.it.peek().kind == Semicolon)
	{
		state.it.consume<Semicolon>();
	}

	return { Ok, state.allocator.allocate<AST::VariableDefinitionStatement>(nameResult.value, type, valueResult.value, isMutable) };
}

static Result<Required<AST::StatementBlock>> parseStatementBlock(ParserState& state)
{
	ASSERT(state.it.consume<LBrace>());

	auto [status, statements] = zeroOrMore<RBrace, AST::Statement>(state, parseStatement);
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::StatementBlock>(statements) };
}

static Result<Required<AST::BreakStatement>> parseBreakStatement(ParserState& state)
{
	ASSERT(state.it.consume<Break>());

	Optional<AST::Expression> breakValue;
	if (state.it.peek().kind != Semicolon)
	{
		auto [status, value] = parseExpression(state);
		ERROR_IF_ERROR(status);

		breakValue = value.ptr();;
	}

	ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after break statement"));

	return { Ok, state.allocator.allocate<AST::BreakStatement>(breakValue) };
}

static Result<Required<AST::ReturnStatement>> parseReturnStatement(ParserState& state)
{
	ASSERT(state.it.consume<Return>());

	Optional<AST::Expression> returnValue;
	if (state.it.peek().kind != Semicolon)
	{
		auto [status, value] = parseExpression(state);
		ERROR_IF_ERROR(status);

		returnValue = value.ptr();
	}

	ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after return statement"));

	return { Ok, state.allocator.allocate<AST::ReturnStatement>(returnValue) };
}

static Result<Required<AST::Statement>> parseStatement(ParserState& state)
{
	switch (state.it.peek().kind)
	{
	case Var:
	case Const:
	{
		auto [status, statement] = parseVariableDefinitionStatement(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case LBrace:
	{
		auto [status, statement] = parseStatementBlock(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case If:
	{
		auto [status, statement] = parseIfExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case For:
	{
		auto [status, statement] = parseForExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case While:
	{
		auto [status, statement] = parseWhileExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case Match:
	{
		auto [status, statement] = parseMatchExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case Break:
	{
		auto [status, statement] = parseBreakStatement(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	case Return:
	{
		auto [status, statement] = parseReturnStatement(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
	}

	default:
	{
		auto [status, expression] = parseExpression(state);
		ERROR_IF_ERROR(status);

		if (isAssignmentOperator(state.it.peek().kind))
		{
			auto [opStatus, opToken] = state.it.consume();
			ERROR_IF_ERROR(opStatus);

			auto [valueStatus, value] = parseExpression(state);
			ERROR_IF_ERROR(valueStatus);

			ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after assignment statement"));

			return { Ok, state.allocator.allocate<AST::Statement>(
				Required(state.allocator.allocate<AST::AssignmentStatement>(opToken.kind, expression, value))
			) };
		}

		ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after expression statement"));

		return { Ok, state.allocator.allocate<AST::Statement>(
			Required(state.allocator.allocate<AST::ExpressionStatement>(expression))
		) };
	}
	}
}

#pragma endregion

#pragma region Definition Nodes

static Result<Required<AST::TypeDefinition>> parseTypeDefinition(ParserState& state)
{
	ASSERT(state.it.consume<Type>());

	auto [status, typeNameToken] = state.it.consume<Identifier>("type name after 'type' keyword");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<Assign>("'=' after type name"));

	auto baseTypeResult = parseBaseType(state);
	ERROR_IF_FALSE(baseTypeResult);

	ERROR_IF_FALSE(state.it.consume<Semicolon>("a semicolon after type definition"));

	return { Ok, state.allocator.allocate<AST::TypeDefinition>(typeNameToken, baseTypeResult.value) };
}

static Result<Required<AST::FunctionDefinition>> parseFunctionDefinition(ParserState& state)
{
	ASSERT(state.it.consume<Fn>());

	auto [status, functionNameToken] = state.it.consume<Identifier>("function name after 'fn' keyword");
	ERROR_IF_ERROR(status);

	auto functionResult = parseFunction(state);
	ERROR_IF_FALSE(functionResult);

	return { Ok, state.allocator.allocate<AST::FunctionDefinition>(functionNameToken, functionResult.value) };
}

static Result<Required<AST::ExternDefinition>> parseExternDefinition(ParserState& state)
{
	ASSERT(state.it.consume<Extern>());

	auto [status, functionNameToken] = state.it.consume<Identifier>("C function name after 'extern' keyword");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after extern function name"));

	bool isVariadic = false;
	Optional<AST::ListNode<AST::FunctionParameter>> functionParameters;
	AST::ListNode<AST::FunctionParameter>* pParamsTail = nullptr;

	while (state.it.peek().kind != RParen)
	{
		if (state.it.peek().kind == Ellipsis)
		{
			state.it.consume();
			isVariadic = true;
			break;
		}

		auto [paramStatus, functionParameter] = parseFunctionParameter(state);
		ERROR_IF_ERROR(paramStatus);

		auto* pNew = state.allocator.allocate<AST::ListNode<AST::FunctionParameter>>(functionParameter, Optional<AST::ListNode<AST::FunctionParameter>>());
		if (!pParamsTail)
			functionParameters = pNew;
		else
			pParamsTail->next = pNew;
		pParamsTail = pNew;

		if (state.it.peek().kind == Comma)
		{
			state.it.consume();
		}
		else
		{
			break;
		}
	}

	if (!state.it.consume<RParen>("')' after extern function parameters"))
	{
		if (isVariadic)
		{
			state.logger.logInfo("'...' should always be the at the end of the parameter list.");
		}

		return { Error };
	}

	ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after extern function declaration"));

	return { Ok, state.allocator.allocate<AST::ExternDefinition>(functionNameToken, functionParameters, isVariadic) };
}

static Result<Required<AST::Definition>> parseDefinition(ParserState& state)
{
	const auto& token = state.it.peek();
	switch (token.kind)
	{
	case Type:
	{
		auto [status, definition] = parseTypeDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Definition>(
			AST::Definition::Visibility::Public,
			definition
		) };
	}

	case Fn:
	{
		auto [status, definition] = parseFunctionDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Definition>(
			AST::Definition::Visibility::Public,
			definition
		) };
	}

	case Extern:
	{
		auto [status, definition] = parseExternDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Definition>(
			AST::Definition::Visibility::Public,
			definition
		) };
	}

	default:
	{
		state.logger.logErrorUnexpected(token, token, "a definition", token);
		return { Error };
	}
	}
}

#pragma endregion

static Result<AST::Import> parseImport(ParserState& state)
{
	ASSERT(state.it.consume<Import>());

	auto [status, startToken] = state.it.consume<Identifier>("module name after 'import' keyword");
	ERROR_IF_ERROR(status);

	while (state.it.peek().kind != Semicolon && state.it.peek().kind != As)
	{
		ERROR_IF_FALSE(state.it.consume<DoubleColon>("'::' or ';' or 'as' after module name"));
		ERROR_IF_FALSE(state.it.consume<Identifier>("module name after '::'"));
	}

	const Token& endToken = state.it.previous();
	std::string_view path = state.it.createView(startToken, endToken);

	std::string_view alias = path;
	if (state.it.peek().kind == As)
	{
		state.it.consume<As>();
		auto [aliasStatus, aliasToken] = state.it.consume<Identifier>("alias name after 'as'");
		ERROR_IF_ERROR(aliasStatus);
		alias = state.it.createView(aliasToken, aliasToken);
	}

	ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after import statement"));

	return { Ok, AST::Import{ path, alias } };
}

static Result<Required<AST::Module>> parseModule(ParserState& state)
{
	Optional<AST::ListNode<AST::Import>> imports;
	AST::ListNode<AST::Import>* pImportsTail = nullptr;
	Optional<AST::ListNode<AST::Definition>> definitions;
	AST::ListNode<AST::Definition>* pDefinitionsTail = nullptr;

	while (state.it.hasNext())
	{
		const auto& token = state.it.peek();
		if (token.kind == EndOfFile)
		{
			state.it.consume();
			continue;
		}

		if (token.kind == Import)
		{
			auto [status, importNode] = parseImport(state);
			if (status == Ok)
			{
				auto* pNew = state.allocator.allocate<AST::ListNode<AST::Import>>(
					state.allocator.allocate<AST::Import>(importNode),
					Optional<AST::ListNode<AST::Import>>()
				);
				if (!pImportsTail)
					imports = pNew;
				else
					pImportsTail->next = pNew;
				pImportsTail = pNew;
			}
			continue;
		}

		auto [status, definition] = parseDefinition(state);
		if (status == Ok)
		{
			auto* pNew = state.allocator.allocate<AST::ListNode<AST::Definition>>(definition, Optional<AST::ListNode<AST::Definition>>());
			if (!pDefinitionsTail)
				definitions = pNew;
			else
				pDefinitionsTail->next = pNew;
			pDefinitionsTail = pNew;
			continue;
		}

		state.it.consume();
	}

	return { state.logger.hasError() ? Error : Ok, state.allocator.allocate<AST::Module>(imports, definitions) };
}

Result<std::pair<Required<AST::Module>, Allocator>> parse(const Source& source, const std::vector<Token>& tokens)
{
	ParserState state(source, tokens);

	auto [status, module] = parseModule(state);

	return { status, { module, std::move(state.allocator) } };
}
