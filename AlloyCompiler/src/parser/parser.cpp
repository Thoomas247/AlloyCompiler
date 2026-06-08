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

	// A malformed list (an item not followed by ',' or the end token) must be a
	// reported diagnostic, not a hard crash — e.g. `for (v in c)` where `in` is
	// neither a comma nor ')'.
	ERROR_IF_FALSE(state.it.consume<EndTokenKind>("',' or the closing delimiter after a list item"));

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
	// unary_op = "~" | "!" | "&" | "new" | "move"  (§2.1) — Alloy has no deref operator.
	return
		kind == BitwiseNot ||
		kind == Not ||
		kind == BitwiseAnd ||
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
static Result<Required<AST::ComptimeExpression>> parseComptimeExpression(ParserState& state);

static Result<Required<AST::NamedType>> parseNamedType(ParserState& state)
{
	auto [status, identifier] = parseIdentifierExpression(state);
	ERROR_IF_ERROR(status);

	// Optional generic instantiation: Foo<T1, T2>. Unambiguous in type position.
	Optional<AST::ListNode<AST::Type>> typeArgs;
	if (state.it.peek().kind == Less)
	{
		state.it.consume<Less>();
		auto [taStatus, taList] = commaSeparatedList<Greater, AST::Type>(state,
			[](ParserState& state) -> Result<Required<AST::Type>> { return parseType(state); });
		ERROR_IF_ERROR(taStatus);
		typeArgs = taList;
	}

	return { Ok, state.allocator.allocate<AST::NamedType>(identifier, typeArgs) };
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
		if (size == 0)
		{
			state.logger.logErrorInRange(sizeToken, sizeToken, "Empty arrays are not valid in Alloy — array size must be at least 1. Use the slice type '[T]' if no fixed size is required.");
			return { Error };
		}
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

	case Hash:
	{
		auto [status, comptimeExpr] = parseComptimeExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::BaseType>(comptimeExpr) };
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
	bool isSelf = state.it.peek().kind == Self;
	if (isSelf)
	{
		state.it.consume<Self>();
	}

	auto [status, nameToken] = state.it.consume<Identifier>("parameter name");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<Colon>("':' after parameter name"));

	auto [typeStatus, type] = parseType(state);
	ERROR_IF_ERROR(typeStatus);

	return { Ok, state.allocator.allocate<AST::FunctionParameter>(nameToken, type, isSelf) };
}

static Result<Required<AST::Function>> parseFunction(ParserState& state)
{
	ASSERT(state.it.consume<LParen>());

	auto parametersResult = commaSeparatedList<RParen, AST::FunctionParameter>(state, parseFunctionParameter);
	ERROR_IF_FALSE(parametersResult);

	bool isFirstParam = true;
	parametersResult.value.forEach([&](const Required<AST::FunctionParameter>& param)
		{
			if (!isFirstParam && param.value().isSelf)
				state.logger.logErrorInRange(param.value().name, param.value().name,
					"'self' is only allowed on the first parameter.");
			isFirstParam = false;
		});

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

// allowBitwiseOr=false suppresses '|' as a binary operator so a match-arm pattern
// expression terminates cleanly before a '| capture |' clause (§2.1 match_expr).
static Result<Required<AST::Expression>> parseExpression(ParserState& state, int minPrecedence = 0, bool allowBitwiseOr = true);
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
	auto [status, literalToken] = state.it.consume<CharLiteral, StringLiteral, IntegerLiteral, FloatLiteral, True, False>("a literal");
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

	while (state.it.peek().kind != RBrace)
	{
		// arm pattern: a general expression, or 'else' for the catch-all arm (§2.1)
		Optional<AST::Expression> pattern;
		if (state.it.peek().kind == Else)
		{
			state.it.consume<Else>();
		}
		else
		{
			auto [patternStatus, patternExpr] = parseExpression(state, 0, /*allowBitwiseOr*/ false);
			ERROR_IF_ERROR(patternStatus);
			pattern = patternExpr.ptr();
		}

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
			Required(state.allocator.allocate<AST::MatchArm>(pattern, capture, body)),
			Optional<AST::ListNode<AST::MatchArm>>()
		);
		if (!pArmsTail)
			arms = pNew;
		else
			pArmsTail->next = pNew;
		pArmsTail = pNew;
	}

	ERROR_IF_FALSE(state.it.consume<RBrace>("'}' after match arms"));

	// optional external 'else' block after the closing '}' (§4.3)
	Optional<AST::Statement> externalElse;
	if (state.it.peek().kind == Else)
	{
		state.it.consume<Else>();

		auto [status, statement] = parseStatement(state);
		ERROR_IF_ERROR(status);

		externalElse = statement.ptr();
	}

	return { Ok, state.allocator.allocate<AST::MatchExpression>(subject, arms, externalElse) };
}

static Result<Required<AST::FunctionCallExpression>> parseFunctionCallExpression(
	ParserState& state,
	Required<AST::Expression> left,
	Optional<AST::ListNode<AST::Type>> typeArgs = Optional<AST::ListNode<AST::Type>>())
{
	ASSERT(state.it.consume<LParen>());

	auto [status, arguments] = commaSeparatedList<RParen, AST::Expression>(state, [](ParserState& state) { return parseExpression(state, 0); });
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::FunctionCallExpression>(left, typeArgs, arguments) };
}

// Returns true if the current '<' token is the start of a generic type argument list
// (i.e. '<Types...>' followed by '('), rather than a less-than comparison operator.
static bool isGenericCallAhead(const ParserState& state)
{
	int depth = 1;
	size_t offset = 1;  // start scanning after the '<'

	while (true)
	{
		auto kind = state.it.peek(offset).kind;

		if (kind == EndOfFile)
			return false;

		if (kind == Less)
		{
			++depth;
		}
		else if (kind == Greater)
		{
			--depth;
			if (depth == 0)
				return state.it.peek(offset + 1).kind == LParen;
		}
		else if (
			kind == Identifier ||
			kind == DoubleColon ||
			kind == Multiply ||
			kind == BitwiseAnd ||
			kind == Var ||
			kind == LBracket ||
			kind == RBracket ||
			kind == Comma ||
			kind == Semicolon ||
			kind == IntegerLiteral)
		{
			// valid tokens inside a type expression — continue
		}
		else
		{
			return false;  // value-expression operator or non-integer literal → comparison
		}

		++offset;
	}
}

// Returns true if the current Identifier begins a generic struct initializer
// `Ident <Types...> {`, rather than a comparison `Ident < ...`. Mirrors
// isGenericCallAhead but checks for a trailing '{' instead of '('.
static bool isGenericStructInitAhead(const ParserState& state)
{
	if (state.it.peek(1).kind != Less)
		return false;

	int depth = 1;
	size_t offset = 2;  // after the Identifier and its '<'

	while (true)
	{
		auto kind = state.it.peek(offset).kind;

		if (kind == EndOfFile)
			return false;

		if (kind == Less)
		{
			++depth;
		}
		else if (kind == Greater)
		{
			--depth;
			if (depth == 0)
				return state.it.peek(offset + 1).kind == LBrace;
		}
		else if (
			kind == Identifier ||
			kind == DoubleColon ||
			kind == Multiply ||
			kind == BitwiseAnd ||
			kind == Var ||
			kind == LBracket ||
			kind == RBracket ||
			kind == Comma ||
			kind == Semicolon ||
			kind == IntegerLiteral)
		{
			// valid tokens inside a type-argument list — continue
		}
		else
		{
			return false;  // value-expression operator → it's a comparison
		}

		++offset;
	}
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

		// [expr; N] — N is an integer-literal count (fixed array `[T; N]`) or a
		// runtime expression (dynamically-sized array `[T]`, valid under `new`).
		if (state.it.peek().kind == Semicolon)
		{
			state.it.consume<Semicolon>();

			const Token& sizeFirst = state.it.peek();
			auto [sizeStatus, sizeExpr] = parseExpression(state);
			ERROR_IF_ERROR(sizeStatus);

			// Reject a literal zero-sized array (`[v; 0]`). Runtime sizes are
			// unchecked here (a negative/zero runtime size traps at the bounds
			// check on first access).
			if (auto* lit = std::get_if<Required<AST::LiteralExpression>>(&sizeExpr.value()))
			{
				const Token& tok = lit->value().value;
				if (tok.kind == IntegerLiteral)
				{
					std::string_view sizeView = state.it.createView(tok, tok);
					size_t sz = 0;
					for (char ch : sizeView)
						if (ch >= '0' && ch <= '9')
							sz = sz * 10 + static_cast<size_t>(ch - '0');
					if (sz == 0)
					{
						state.logger.logErrorInRange(sizeFirst, sizeFirst, "Empty arrays are not valid in Alloy — array fill size must be at least 1.");
						return { Error };
					}
				}
			}

			ERROR_IF_FALSE(state.it.consume<RBracket>("']' after array fill"));

			return { Ok, state.allocator.allocate<AST::Expression>(
				Required(state.allocator.allocate<AST::ArrayFillExpression>(firstExpr, sizeExpr))
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
		// `Ident { ... }` or `Ident<Types...> { ... }` — a (generic) struct initializer.
		if (state.it.peek(1).kind == LBrace || isGenericStructInitAhead(state))
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
	case True:
	case False:
	{
		auto [status, expression] = parseLiteralExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok,  state.allocator.allocate<AST::Expression>(expression) };
	}

	case Hash:
	{
		auto [status, expression] = parseComptimeExpression(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Expression>(expression) };
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

		if (state.it.peek().kind == Less && isGenericCallAhead(state))
		{
			state.it.consume<Less>();
			auto [taStatus, typeArgs] = commaSeparatedList<Greater, AST::Type>(state,
				[](ParserState& state) -> Result<Required<AST::Type>> { return parseType(state); });
			ERROR_IF_ERROR(taStatus);

			auto callResult = parseFunctionCallExpression(state, primary, typeArgs);
			ERROR_IF_FALSE(callResult);

			primary = state.allocator.allocate<AST::Expression>(callResult.value);
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

static Result<Required<AST::Expression>> parseExpression(ParserState& state, int minPrecedence, bool allowBitwiseOr)
{
	auto [status, left] = parseUnaryExpression(state);
	ERROR_IF_ERROR(status);

	// `expr is Type` — interface-object concrete-type test (§3.2). Postfix
	// binding tighter than every binary operator: it consumes a single
	// NamedType from the input and yields a boolean expression.
	while (state.it.peek().kind == Is)
	{
		const Token& isTok = state.it.peek();
		state.it.consume<Is>();
		auto [typeStatus, typeNode] = parseNamedType(state);
		ERROR_IF_ERROR(typeStatus);
		auto* isExpr = state.allocator.allocate<AST::IsExpression>(left, typeNode, isTok);
		left = state.allocator.allocate<AST::Expression>(Required<AST::IsExpression>(isExpr));
	}

	while (true)
	{
		TokenKind op = state.it.peek().kind;

		if (op == BitwiseOr && !allowBitwiseOr)
		{
			break;
		}

		int precedence = getBinaryOperatorPrecedence(op);
		if (precedence < minPrecedence)
		{
			break;
		}

		state.it.consume();

		auto rightResult = parseExpression(state, precedence + 1, allowBitwiseOr);
		ERROR_IF_FALSE(rightResult);

		auto* pBin = state.allocator.allocate<AST::BinaryExpression>(op, left, rightResult.value);
		left = state.allocator.allocate<AST::Expression>(Required<AST::BinaryExpression>(pBin));
	}

	return { Ok, left };
}

// comptime_expr = "#" postfix_expr
// '#' marks any value-yielding expression (a call, identifier, if/while/match,
// parenthesised expression, ...) for compile-time evaluation (§6.1). It binds as
// a postfix expression, so `#a + b` is `(#a) + b`; use `#(a + b)` for the whole.
static Result<Required<AST::ComptimeExpression>> parseComptimeExpression(ParserState& state)
{
	const Token& hashToken = state.it.peek();
	ASSERT(state.it.consume<Hash>());

	auto [status, inner] = parsePostfixExpression(state);
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::ComptimeExpression>(inner, hashToken) };
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

// A block-like expression (if/while/for/match) is self-terminating.
static bool isBlockLikeExpression(const AST::Expression& expr)
{
	return std::holds_alternative<Required<AST::IfExpression>>(expr)
		|| std::holds_alternative<Required<AST::WhileExpression>>(expr)
		|| std::holds_alternative<Required<AST::ForExpression>>(expr)
		|| std::holds_alternative<Required<AST::MatchExpression>>(expr);
}

static Result<Required<AST::BreakStatement>> parseBreakStatement(ParserState& state)
{
	ASSERT(state.it.consume<Break>());

	Optional<AST::Expression> breakValue;
	bool blockLike = false;
	if (state.it.peek().kind != Semicolon)
	{
		auto [status, value] = parseExpression(state);
		ERROR_IF_ERROR(status);

		breakValue = value.ptr();
		blockLike = isBlockLikeExpression(value.value());
	}

	// When break's operand is a block-like expression it is self-terminating, so
	// the trailing ';' is optional: `break if (c) break a; else break b;`.
	if (blockLike)
	{
		if (state.it.peek().kind == Semicolon)
			state.it.consume();
	}
	else
	{
		ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after break statement"));
	}

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

	// optional type-parameter list: type Name<T, U: I> = ...
	Optional<AST::ListNode<AST::TypeParameter>> typeParams;
	if (state.it.peek().kind == Less)
	{
		state.it.consume<Less>();
		auto [tpStatus, tpList] = commaSeparatedList<Greater, AST::TypeParameter>(
			state,
			[](ParserState& state) -> Result<Required<AST::TypeParameter>>
			{
				auto [nameStatus, nameToken] = state.it.consume<Identifier>("type parameter name");
				ERROR_IF_ERROR(nameStatus);

				Optional<Token> interface;
				if (state.it.peek().kind == Colon)
				{
					state.it.consume<Colon>();
					auto [ifStatus, ifToken] = state.it.consume<Identifier>("interface name after ':'");
					ERROR_IF_ERROR(ifStatus);
					interface = Optional(state.allocator.allocate<Token>(ifToken));
				}
				return { Ok, state.allocator.allocate<AST::TypeParameter>(nameToken, interface) };
			});
		ERROR_IF_ERROR(tpStatus);
		typeParams = tpList;
	}

	// optional interface markers: type Name : I1, I2 = ...  (§2.1 type_def)
	Optional<AST::ListNode<const Token*>> interfaces;
	if (state.it.peek().kind == Colon)
	{
		state.it.consume<Colon>();

		AST::ListBuilder<const Token*> interfaceList;
		while (true)
		{
			auto [ifStatus, ifToken] = state.it.consume<Identifier>("interface name");
			ERROR_IF_ERROR(ifStatus);

			interfaceList.append(state.allocator.allocate<const Token*>(&ifToken), state.allocator);

			if (state.it.peek().kind == Comma)
				state.it.consume<Comma>();
			else
				break;
		}
		interfaces = interfaceList.head;
	}

	ERROR_IF_FALSE(state.it.consume<Assign>("'=' after type name"));

	auto baseTypeResult = parseBaseType(state);
	ERROR_IF_FALSE(baseTypeResult);

	ERROR_IF_FALSE(state.it.consume<Semicolon>("a semicolon after type definition"));

	return { Ok, state.allocator.allocate<AST::TypeDefinition>(typeNameToken, typeParams, interfaces, baseTypeResult.value) };
}

static Result<Required<AST::FunctionDefinition>> parseFunctionDefinition(ParserState& state)
{
	ASSERT(state.it.consume<Fn>());

	auto [status, functionNameToken] = state.it.consume<Identifier>("function name after 'fn' keyword");
	ERROR_IF_ERROR(status);

	// optional type parameter list: fn name<T: Interface, U>(...)
	Optional<AST::ListNode<AST::TypeParameter>> typeParams;
	if (state.it.peek().kind == Less)
	{
		state.it.consume<Less>();
		auto [tpStatus, tpList] = commaSeparatedList<Greater, AST::TypeParameter>(
			state,
			[](ParserState& state) -> Result<Required<AST::TypeParameter>>
			{
				auto [nameStatus, nameToken] = state.it.consume<Identifier>("type parameter name");
				ERROR_IF_ERROR(nameStatus);

				Optional<Token> interface;
				if (state.it.peek().kind == Colon)
				{
					state.it.consume<Colon>();
					auto [ifStatus, ifToken] = state.it.consume<Identifier>("interface name after ':'");
					ERROR_IF_ERROR(ifStatus);
					interface = Optional(state.allocator.allocate<Token>(ifToken));
				}

				return { Ok, state.allocator.allocate<AST::TypeParameter>(nameToken, interface) };
			});
		ERROR_IF_ERROR(tpStatus);
		typeParams = tpList;
	}

	auto functionResult = parseFunction(state);
	ERROR_IF_FALSE(functionResult);

	return { Ok, state.allocator.allocate<AST::FunctionDefinition>(functionNameToken, typeParams, functionResult.value) };
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

	// §5.3 — return type is optional because Alloy has no 'void' keyword.
	// When omitted, the extern is treated as returning nothing (the codegen
	// backend lowers absent → void in the C ABI).
	Optional<AST::Type> returnType;
	if (state.it.peek().kind == Arrow)
	{
		state.it.consume<Arrow>();
		auto [retStatus, retType] = parseType(state);
		ERROR_IF_ERROR(retStatus);
		returnType = retType.ptr();
	}

	ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after extern function declaration"));

	return { Ok, state.allocator.allocate<AST::ExternDefinition>(functionNameToken, functionParameters, returnType, isVariadic) };
}

static Result<Required<AST::InterfaceDefinition>> parseInterfaceDefinition(ParserState& state)
{
	ASSERT(state.it.consume<Interface>());

	auto [status, nameToken] = state.it.consume<Identifier>("interface name after 'interface' keyword");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<LBrace>("'{' after interface name"));

	auto [fnStatus, functions] = zeroOrMore<RBrace, AST::InterfaceFunction>(
		state,
		[](ParserState& state) -> Result<Required<AST::InterfaceFunction>>
		{
			ERROR_IF_FALSE(state.it.consume<Fn>("'fn' in interface body"));

			auto [nameStatus, fnName] = state.it.consume<Identifier>("function name");
			ERROR_IF_ERROR(nameStatus);

			ERROR_IF_FALSE(state.it.consume<LParen>("'(' after interface function name"));
			auto paramsResult = commaSeparatedList<RParen, AST::FunctionParameter>(state, parseFunctionParameter);
			ERROR_IF_FALSE(paramsResult);

			Optional<AST::Type> returnType;
			if (state.it.peek().kind == Arrow)
			{
				state.it.consume<Arrow>();

				auto [retStatus, retType] = parseType(state);
				ERROR_IF_ERROR(retStatus);

				returnType = retType.ptr();
			}

			ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after interface function signature"));

			return { Ok, state.allocator.allocate<AST::InterfaceFunction>(fnName, paramsResult.value, returnType) };
		});
	ERROR_IF_ERROR(fnStatus);

	return { Ok, state.allocator.allocate<AST::InterfaceDefinition>(nameToken, functions) };
}

static Result<Required<AST::MacroDefinition>> parseMacroDefinition(ParserState& state)
{
	ASSERT(state.it.consume<Macro>());

	auto [status, nameToken] = state.it.consume<Identifier>("macro name after 'macro' keyword");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after macro name"));
	auto paramsResult = commaSeparatedList<RParen, AST::FunctionParameter>(state, parseFunctionParameter);
	ERROR_IF_FALSE(paramsResult);

	auto bodyResult = parseStatementBlock(state);
	ERROR_IF_FALSE(bodyResult);

	return { Ok, state.allocator.allocate<AST::MacroDefinition>(nameToken, paramsResult.value, bodyResult.value) };
}

static Result<Required<AST::Definition>> parseDefinition(ParserState& state)
{
	// optional visibility prefix: 'pub' / 'exp'; absent → Private  (§2.1 definition)
	auto visibility = AST::Definition::Visibility::Private;
	if (state.it.peek().kind == Pub)
	{
		state.it.consume<Pub>();
		visibility = AST::Definition::Visibility::Public;
	}
	else if (state.it.peek().kind == Exp)
	{
		state.it.consume<Exp>();
		visibility = AST::Definition::Visibility::Export;
	}

	const auto& token = state.it.peek();
	switch (token.kind)
	{
	case Type:
	{
		auto [status, definition] = parseTypeDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Definition>(visibility, definition) };
	}

	case Fn:
	{
		auto [status, definition] = parseFunctionDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Definition>(visibility, definition) };
	}

	case Extern:
	{
		auto [status, definition] = parseExternDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Definition>(visibility, definition) };
	}

	case Interface:
	{
		auto [status, definition] = parseInterfaceDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Definition>(visibility, definition) };
	}

	case Macro:
	{
		auto [status, definition] = parseMacroDefinition(state);
		ERROR_IF_ERROR(status);
		return { Ok, state.allocator.allocate<AST::Definition>(visibility, definition) };
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
