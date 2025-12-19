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
	Optional<AST::ListNode<T>> listHead;
	while (state.it.peek().kind != EndTokenKind)
	{
		auto [status, node] = parse(state);
		ERROR_IF_ERROR(status);

		listHead = state.allocator.allocate<AST::ListNode<T>>(
			node,
			listHead
		);
	}

	ASSERT(state.it.consume<EndTokenKind>());

	return { Ok, listHead };
}

template <TokenKind EndTokenKind, typename T, typename F>
static Result<Optional<AST::ListNode<T>>> commaSeparatedList(ParserState& state, F&& parse)
{
	Optional<AST::ListNode<T>> listHead;
	while (state.it.peek().kind != EndTokenKind)
	{
		auto [status, node] = parse(state);
		ERROR_IF_ERROR(status);

		listHead = state.allocator.allocate<AST::ListNode<T>>(
			node,
			listHead
		);

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

	return { Ok, listHead };
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
		kind == Not;
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
	size_t depth = 1;
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

static Result<Required<AST::NamedType>> parseNamedType(ParserState& state)
{
	auto [status, typeNameToken] = state.it.consume<Identifier>("type name");
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::NamedType>(typeNameToken) };
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

	Optional<AST::ListNode<AST::Type>> parameterTypes;

	while (state.it.peek().kind != RParen)
	{
		auto [typeStatus, parameterType] = parseType(state);
		ERROR_IF_ERROR(typeStatus);

		parameterTypes = state.allocator.allocate<AST::ListNode<AST::Type>>(
			parameterType,
			parameterTypes
		);

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

	return { Ok, state.allocator.allocate<AST::FunctionType>(parameterTypes, returnType) };
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

static Result<Required<AST::LiteralExpression>> parseLiteralExpression(ParserState& state)
{
	auto [status, literalToken] = state.it.consume<CharLiteral, StringLiteral, IntegerLiteral, FloatLiteral>("a literal");
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::LiteralExpression>(literalToken) };
}

static Result<Required<AST::VariableExpression>> parseVariableExpression(ParserState& state)
{
	auto [status, name] = state.it.consume<Identifier>();
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::VariableExpression>(name) };
}

static Result<Required<AST::Capture>> parseCapture(ParserState& state)
{
	ASSERT(state.it.consume<BitwiseOr>());

	auto modifier = getTypeModifier(state);
	auto [success, captureNameToken] = state.it.consume<Identifier>("capture name");

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

	case BitwiseOr:
	{
		// |...| (...) -> {...}
		auto [status, expression] = parseLambdaExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	case LParen:
	{
		auto seekResult = getOffsetToClosing(state, LParen, RParen);
		ERROR_IF_FALSE(seekResult);

		if (state.it.peek(seekResult.value + 1).kind == Arrow)
		{
			// (...) -> {...}
			auto [status, expression] = parseLambdaExpression(state);
			ERROR_IF_ERROR(status);
			return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
		}

		state.it.consume<LParen>();
		auto [status, expression] = parseExpression(state);
		ERROR_IF_ERROR(status);
		state.it.consume<RParen>("')'");

		return { Ok,  expression };
	}

	case Identifier:
	{
		auto [status, expression] = parseVariableExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
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

			return { Ok, state.allocator.allocate<AST::Expression>(functionCallResult.value) };
		}

		if (state.it.peek().kind == Dot)
		{
			auto memberAccessResult = parseMemberAccessExpression(state, primary);
			ERROR_IF_FALSE(memberAccessResult);

			return { Ok, state.allocator.allocate<AST::Expression>(memberAccessResult.value) };
		}


		if (state.it.peek().kind == LBracket)
		{
			auto arrayAccessResult = parseArrayAccessExpression(state, primary);
			ERROR_IF_FALSE(arrayAccessResult);

			return { Ok, state.allocator.allocate<AST::Expression>(arrayAccessResult.value) };
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

		left.value() = state.allocator.allocate<AST::BinaryExpression>(op, left, rightResult.value);
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

	ERROR_IF_FALSE(state.it.consume<Semicolon>());

	return { Ok, state.allocator.allocate<AST::VariableDefinitionStatement>(nameResult.value, type, valueResult.value, isMutable) };
}

static Result<Required<AST::AssignmentStatement>> parseAssignmentStatement(ParserState& state)
{
	auto targetResult = parseExpression(state);
	ERROR_IF_FALSE(targetResult);

	auto [status, opToken] = state.it.consume();
	ERROR_IF_ERROR(status);

	if (!isAssignmentOperator(opToken.kind))
	{
		state.logger.logErrorUnexpected(opToken, opToken, "an assignment operator", opToken);
	}

	auto valueResult = parseExpression(state);
	ERROR_IF_FALSE(valueResult);

	return { Ok, state.allocator.allocate<AST::AssignmentStatement>(opToken.kind, targetResult.value, valueResult.value) };
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
		auto [status, statement] = parseAssignmentStatement(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Statement>(statement) };
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

		functionParameters = state.allocator.allocate<AST::ListNode<AST::FunctionParameter>>(
			functionParameter,
			functionParameters
		);

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

static Result<std::string_view> parseImport(ParserState& state)
{
	ASSERT(state.it.consume<Import>());

	auto [status, startToken] = state.it.consume<Identifier>("module name after 'import' keyword");
	ERROR_IF_ERROR(status);

	while (true)
	{
		ERROR_IF_FALSE(state.it.consume<DoubleColon>("'::' or ';' after module name"));
		ERROR_IF_FALSE(state.it.consume<Identifier>("module name after '::'"));

		if (state.it.peek().kind == Semicolon)
		{
			break;
		}
	}

	const Token& endToken = state.it.previous();

	ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after import statement"));

	return { Ok, state.it.createView(startToken, endToken) };
}

static Result<Required<AST::Module>> parseModule(ParserState& state)
{
	Optional<AST::ListNode<std::string_view>> imports;
	Optional<AST::ListNode<AST::Definition>> definitions;

	Status moduleStatus = Ok;

	while (state.it.hasNext())
	{
		const auto& token = state.it.peek();
		if (token.kind == Comment || token.kind == EndOfFile)
		{
			state.it.consume();
			continue;
		}

		if (token.kind == Import)
		{
			auto [status, importString] = parseImport(state);
			if (status == Ok)
			{
				auto import = state.allocator.allocate<std::string_view>(importString);
				imports = state.allocator.allocate<AST::ListNode<std::string_view>>(import, imports);
			}
			continue;
		}

		auto [status, definition] = parseDefinition(state);
		if (status == Ok)
		{
			definitions = state.allocator.allocate<AST::ListNode<AST::Definition>>(
				definition,
				definitions
			);
			continue;
		}

		state.it.consume();
	}

	return { moduleStatus, state.allocator.allocate<AST::Module>(imports, definitions) };
}

Result<std::pair<Required<AST::Module>, Allocator>> parse(const Source& source, const std::vector<Token>& tokens)
{
	ParserState state(source, tokens);

	auto [status, module] = parseModule(state);

	return { status, { module, std::move(state.allocator) } };
}
