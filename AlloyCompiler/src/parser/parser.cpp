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


#define ERROR()				\
	do {					\
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

#pragma endregion

#pragma region Type Nodes

static Result<Required<AST::Type>> parseType(ParserState& state);

static Result<Required<AST::NamedType>> parseNamedType(ParserState& state)
{
	auto [status, typeNameToken] = state.it.consume<Identifier>("type name");
	ERROR_IF_ERROR(status);

	return { Ok, state.allocator.allocate<AST::NamedType>(Required(&typeNameToken)) };
}

static Result<Required<AST::StructType>> parseStructType(ParserState& state)
{
	ASSERT(state.it.consume<Struct>());

	ERROR_IF_FALSE(state.it.consume<LBrace>("'{' after 'struct' keyword"));

	Optional<AST::ListNode<AST::StructType::Member>> members;
	while (state.it.peek().kind != RBrace)
	{
		auto [status, memberNameToken] = state.it.consume<Identifier>("member name");
		ERROR_IF_ERROR(status);

		ERROR_IF_FALSE(state.it.consume<Colon>("':' after member name"));

		auto [typeStatus, memberType] = parseType(state);
		ERROR_IF_ERROR(typeStatus);

		ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after stuct member definition"));

		auto member = state.allocator.allocate<AST::StructType::Member>(
			Required(&memberNameToken),
			memberType
		);

		members = state.allocator.allocate<AST::ListNode<AST::StructType::Member>>(
			member,
			members
		);
	}

	ERROR_IF_FALSE(state.it.consume<RBrace>("'}' after struct members"));

	return { Ok, state.allocator.allocate<AST::StructType>(members) };
}

static Result<Required<AST::EnumType>> parseEnumType(ParserState& state)
{
	ASSERT(state.it.consume<Enum>());

	ERROR_IF_FALSE(state.it.consume<LBrace>("'{' after 'enum' keyword"));

	Optional<AST::ListNode<AST::EnumType::Member>> members;
	while (state.it.peek().kind != RBrace)
	{
		auto [status, memberNameToken] = state.it.consume<Identifier>("member name");
		ERROR_IF_ERROR(status);

		Optional<AST::Type> payloadType;
		if (state.it.peek().kind == Colon)
		{
			auto [typeStatus, memberType] = parseType(state);
			ERROR_IF_ERROR(typeStatus);

			payloadType = Optional(memberType.ptr());
		}

		ERROR_IF_FALSE(state.it.consume<Semicolon>("';' after enum member definition"));

		auto member = state.allocator.allocate<AST::EnumType::Member>(
			Required(&memberNameToken),
			payloadType
		);

		members = state.allocator.allocate<AST::ListNode<AST::EnumType::Member>>(
			member,
			members
		);
	}

	ERROR_IF_FALSE(state.it.consume<RBrace>("'}' after struct members"));

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

#pragma region Expression Nodes

static Result<Required<AST::Expression>> parseExpression(ParserState& state);
static Result<Required<AST::StatementBlock>> parseStatementBlock(ParserState& state);
static Result<Required<AST::Statement>> parseStatement(ParserState& state);

static Result<Required<AST::Capture>> parseCapture(ParserState& state)
{
	ASSERT(state.it.consume<BitwiseOr>());

	auto modifier = getTypeModifier(state);
	auto [success, captureNameToken] = state.it.consume<Identifier>("capture name");

	return { Ok, state.allocator.allocate<AST::Capture>(modifier, Required(&captureNameToken)) };
}

static Result<Required<AST::IfExpression>> parseIfExpression(ParserState& state)
{
	ASSERT(state.it.consume<If>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'if' keyword"));

	auto [status, condition] = parseExpression(state);
	ERROR_IF_ERROR(status);

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

	auto [status, thenBranch] = parseStatement(state);
	ERROR_IF_ERROR(status);

	Optional<AST::Statement> elseBranch;
	if (state.it.peek().kind == Else)
	{
		state.it.consume();

		auto [status, branch] = parseStatement(state);
		ERROR_IF_ERROR(status);

		elseBranch = branch.ptr();
	}

	return { Ok, state.allocator.allocate<AST::IfExpression>(condition, ifCapture, thenBranch, elseBranch) };
}

template <TokenKind EndTokenKind, typename T, typename F>
static Optional<AST::ListNode<T>> zeroOrMore(ParserState& state, F&& parse)
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

	return listHead;
}

static Result<Required<AST::ForExpression>> parseForExpression(ParserState& state)
{
	ASSERT(state.it.consume<For>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'for' keyword"));

	auto iterables = zeroOrMore<RParen, AST::Expression>(state, parseExpression);
	if (!iterables.hasValue())
	{
		state.logger.logErrorInRange(state.it.previous(), state.it.previous(), expectedError("an iterable expression", ")"));
		ERROR();
	}

	Optional<AST::ListNode<AST::Capture>> iterators;
	if (state.it.peek().kind == BitwiseOr)
	{
		state.it.consume();

		iterators = zeroOrMore<BitwiseOr, AST::Capture>(state, parseCapture);
		if (!iterators.hasValue())
		{
			state.logger.logErrorInRange(state.it.previous(), state.it.previous(), expectedError("a capture name", "|"));
			ERROR();
		}
	}

	auto [status, body] = parseStatement(state);
	ERROR_IF_ERROR(status);

	Optional<AST::Statement> elseBody;
	if (state.it.peek().kind == Else)
	{
		state.it.consume();

		auto [status, statement] = parseStatement(state);
		ERROR_IF_ERROR(status);

		elseBody = statement.ptr();
	}

	return { Ok, state.allocator.allocate<AST::ForExpression>(Required(iterables.ptr()), iterators, body, elseBody) };
}

static Result<Required<AST::WhileExpression>> parseWhileExpression(ParserState& state)
{
	ASSERT(state.it.consume<While>());

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after 'while' keyword"));

	auto [status, condition] = parseExpression(state);
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<RParen>("')' after condition"));

	auto [status, body] = parseStatement(state);
	ERROR_IF_ERROR(status);

	Optional<AST::Statement> elseBody;
	if (state.it.peek().kind == Else)
	{
		state.it.consume();

		auto [status, statement] = parseStatement(state);
		ERROR_IF_ERROR(status);

		elseBody = statement.ptr();
	}

	return { Ok, state.allocator.allocate<AST::WhileExpression>(condition, body, elseBody) };
}

#pragma endregion


#pragma region Statement Nodes

#pragma endregion

#pragma region Function Nodes

static Result<Required<AST::FunctionParameter>> parseFunctionParameter(ParserState& state)
{
	auto [status, nameToken] = state.it.consume<Identifier>("parameter name");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<Colon>("':' after parameter name"));

	auto [typeStatus, type] = parseType(state);
	ERROR_IF_ERROR(typeStatus);

	auto functionParameter = state.allocator.allocate<AST::FunctionParameter>(Required(&nameToken), type);

	return { Ok, functionParameter };
}

#pragma endregion

#pragma region Definition Nodes

static Result<Required<AST::ExternDefinition>> parseExternDefinition(ParserState& state)
{
	state.it.consume<Extern>();

	auto [status, functionNameToken] = state.it.consume<Identifier>("C function name after 'extern' keyword");
	ERROR_IF_ERROR(status);

	ERROR_IF_FALSE(state.it.consume<LParen>("'(' after extern function name"));

	bool isVariadic = false;
	Optional<AST::ListNode<AST::FunctionParameter>> functionParameters;
	while (state.it.peek().kind != RParen)
	{
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
		else if (state.it.peek().kind == Ellipsis)
		{
			state.it.consume();
			isVariadic = true;
			break;
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

	return { Ok, state.allocator.allocate<AST::ExternDefinition>(Required(&functionNameToken), functionParameters, isVariadic) };
}

#pragma endregion

static Result<std::string_view> parseImport(ParserState& state)
{
	state.it.consume<Import>();

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
		switch (token.kind)
		{
		case Extern:
		{
			auto [status, externDefinition] = parseExternDefinition(state);

			if (status == Ok)
			{
				const auto definition = state.allocator.allocate<AST::Definition>(
					AST::Definition::Visibility::Public,
					externDefinition
				);
				definitions = state.allocator.allocate<AST::ListNode<AST::Definition>>(
					definition,
					definitions
				);
			}
			break;
		}
		case Comment:
		{
			// skip comments
			state.it.consume();
			break;
		}
		case EndOfFile:
		{
			state.it.consume();
			break;
		}
		case Import:
		{
			auto [status, importString] = parseImport(state);
			if (status == Ok)
			{
				auto import = state.allocator.allocate<std::string_view>(importString);
				imports = state.allocator.allocate<AST::ListNode<std::string_view>>(import, imports);
			}
			break;
		}
		default:
		{
			state.logger.logErrorInRange(token, token, "Unexpected token '{}'.", token);
			moduleStatus = Error;
			state.it.consume();
			break;
		}
		}
	}

	return { moduleStatus, state.allocator.allocate<AST::Module>(imports, definitions) };
}

Result<std::pair<Required<AST::Module>, Allocator>> parse(const Source& source, const std::vector<Token>& tokens)
{
	ParserState state(source, tokens);

	auto [status, module] = parseModule(state);

	return { status, { module, std::move(state.allocator) } };
}
