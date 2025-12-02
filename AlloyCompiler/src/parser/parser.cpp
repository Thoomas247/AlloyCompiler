#include <vector>
#include <functional>

#include "../util/logger.hpp"
#include "../util/allocator.hpp"
#include "../tokenizer/tokenizer.hpp"
#include "../parser/parser.hpp"

class TokenIterator
{
public:
	TokenIterator(Logger& logger, const Source& source, const std::vector<Token>& tokens)
		: m_Logger(logger), m_Source(source), m_Tokens(tokens), m_CurrentIndex(0)
	{
	}

	bool hasNext(size_t offset = 0) const
	{
		return (m_CurrentIndex + offset) < m_Tokens.size();
	}

	bool done() const
	{
		return m_CurrentIndex >= m_Tokens.size();
	}

	/**
	* Consumes the current token.
	*/
	const Token& consume()
	{
		return m_Tokens[m_CurrentIndex++];
	}

	/**
	* Asserts that the current token is one of the expected kinds, then consumes it.
	*/
	template<TokenKind... Expected>
	const Token& consume()
	{
		ASSERT(((m_Tokens[m_CurrentIndex].kind == Expected) || ...));
		return m_Tokens[m_CurrentIndex++];
	}

	/**
	* Checks if the current token is one of the expected kinds.
	* Displays an error message and does not consume the token if it is not.
	*/
	template<TokenKind... Expected>
	std::pair<bool, const Token&> consume(const std::string& expectedMessage)
	{
		const auto& token = peek();
		if (((token.kind == Expected) || ...))
		{
			m_CurrentIndex++;
			return { true, token };
		}
		else
		{
			m_Logger.logErrorInRange(
				token,
				token,
				"Expected {}, but found '{}'.",
				expectedMessage,
				token
			);
			return { false, token };
		}
	}

	/**
	* Returns true if the current token is one of the expected kinds.
	* Prints the given error message if it is not.
	*/
	template<TokenKind... Expected>
	bool expect(const std::string& expectedMessage)
	{
		auto [success, _] = consume<Expected...>(expectedMessage);
		return success;
	}

	/**
	* Returns the current token without consuming it.
	*/
	const Token& peek(size_t offset = 0) const
	{
		ASSERT(m_CurrentIndex + offset < m_Tokens.size());
		return m_Tokens[m_CurrentIndex + offset];
	}

	/**
	* Returns the previously consumed token.
	*/
	const Token& previous() const
	{
		ASSERT(m_CurrentIndex > 0);
		return m_Tokens[m_CurrentIndex - 1];
	}

	/**
	* Creates a view into the source code in the given range.
	* Start token is inclusive, end token is exclusive.
	*/
	std::string_view createView(const Token& startToken, const Token& endToken) const
	{
		const auto startIndex = startToken.start.index;
		const auto endIndex = endToken.end.index;

		ASSERT(startIndex <= m_Source.data.size());
		ASSERT(endIndex <= m_Source.data.size());
		ASSERT(endIndex >= startIndex);

		return std::string_view(&m_Source.data[startIndex], endIndex - startIndex);
	}

private:
	Logger& m_Logger;
	const Source& m_Source;
	const std::vector<Token>& m_Tokens;
	size_t m_CurrentIndex;
};

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

using enum Status;
using enum TokenKind;

#define ERROR_IF_FALSE(expr) \
	do { \
		if (!expr) \
			return { Error }; \
	} while(0)

#define ERROR_IF_ERROR(status) \
	do { \
		if (status == Error) \
			return { Error }; \
	} while(0)

#pragma endregion

#pragma region Type Nodes

static Result<Required<AST::Type>> parseType(ParserState& state);

static Result<Required<AST::NamedType>> parseNamedType(ParserState& state)
{
	auto [success, typeNameToken] = state.it.consume<Identifier>("type name");
	ERROR_IF_FALSE(success);

	return { Ok, state.allocator.allocate<AST::NamedType>(Required(&typeNameToken)) };
}

static Result<Required<AST::StructType>> parseStructType(ParserState& state)
{
	state.it.consume<Struct>();

	ERROR_IF_FALSE(state.it.expect<LBrace>("'{' after 'struct' keyword"));

	Optional<AST::ListNode<AST::StructType::Member>> members;
	while (state.it.peek().kind != RBrace)
	{
		auto [success, memberNameToken] = state.it.consume<Identifier>("member name");
		ERROR_IF_FALSE(success);

		ERROR_IF_FALSE(state.it.expect<Colon>("':' after member name"));

		auto [typeStatus, memberType] = parseType(state);
		ERROR_IF_ERROR(typeStatus);

		ERROR_IF_FALSE(state.it.expect<Semicolon>("';' after stuct member definition"));

		auto member = state.allocator.allocate<AST::StructType::Member>(
			Required(&memberNameToken),
			memberType
		);

		members = state.allocator.allocate<AST::ListNode<AST::StructType::Member>>(
			member,
			members
		);
	}

	ERROR_IF_FALSE(state.it.expect<RBrace>("'}' after struct members"));

	return { Ok, state.allocator.allocate<AST::StructType>(members) };
}

static Result<Required<AST::EnumType>> parseEnumType(ParserState& state)
{
	state.it.consume<Enum>();

	ERROR_IF_FALSE(state.it.expect<LBrace>("'{' after 'enum' keyword"));

	Optional<AST::ListNode<AST::EnumType::Member>> members;
	while (state.it.peek().kind != RBrace)
	{
		auto [success, memberNameToken] = state.it.consume<Identifier>("member name");
		ERROR_IF_FALSE(success);

		Optional<AST::Type> payloadType;
		if (state.it.peek().kind == Colon)
		{
			auto [typeStatus, memberType] = parseType(state);
			ERROR_IF_ERROR(typeStatus);

			payloadType = Optional(memberType.ptr());
		}

		ERROR_IF_FALSE(state.it.expect<Semicolon>("';' after enum member definition"));

		auto member = state.allocator.allocate<AST::EnumType::Member>(
			Required(&memberNameToken),
			payloadType
		);

		members = state.allocator.allocate<AST::ListNode<AST::EnumType::Member>>(
			member,
			members
		);
	}

	ERROR_IF_FALSE(state.it.expect<RBrace>("'}' after struct members"));

	return { Ok, state.allocator.allocate<AST::EnumType>(members) };
}

static Result<Required<AST::ArrayType>> parseArrayType(ParserState& state)
{
	state.it.consume<LBracket>();

	auto [typeStatus, elementType] = parseType(state);
	ERROR_IF_ERROR(typeStatus);

	size_t size = 0;
	if (state.it.peek().kind == Semicolon)
	{
		state.it.consume<Semicolon>();

		auto [success, sizeToken] = state.it.consume<IntegerLiteral>("integral array size after ';'");
		ERROR_IF_FALSE(success);

		size = std::stoull(std::string(state.it.createView(sizeToken, sizeToken)));
	}

	ERROR_IF_FALSE(state.it.expect<RBracket>("']' after array type"));

	return { Ok, state.allocator.allocate<AST::ArrayType>(elementType, size) };
}

static Result<Required<AST::FunctionType>> parseFunctionType(ParserState& state)
{
	state.it.consume<LParen>();

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

	ERROR_IF_FALSE(state.it.expect<RParen>("')' after function parameter types"));

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
	AST::Type::Modifier modifier = AST::Type::Modifier::None;

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
	else if (state.it.peek().kind == And)
	{
		state.it.consume<And>();
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

	std::variant<Required<AST::BaseType>, Required<AST::Type>> innerType;

	if (state.it.peek().kind == Multiply || state.it.peek().kind == And)
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

#pragma endregion


#pragma region Statement Nodes

#pragma endregion

#pragma region Function Nodes

static Result<Required<AST::FunctionParameter>> parseFunctionParameter(ParserState& state)
{
	auto [success, nameToken] = state.it.consume<Identifier>("parameter name");
	ERROR_IF_FALSE(success);

	ERROR_IF_FALSE(state.it.expect<Colon>("':' after parameter name"));

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

	auto [success, functionNameToken] = state.it.consume<Identifier>("C function name after 'extern' keyword");
	ERROR_IF_FALSE(success);

	ERROR_IF_FALSE(state.it.expect<LParen>("'(' after extern function name"));

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
			ERROR_IF_FALSE((state.it.expect<Identifier, Ellipsis>("function parameter declaration after ','")));
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

	if (!state.it.expect<RParen>("')' after extern function parameters"))
	{
		if (isVariadic)
		{
			state.logger.logInfo("'...' should always be the at the end of the parameter list.");
		}

		return { Error };
	}

	ERROR_IF_FALSE(state.it.expect<Semicolon>("';' after extern function declaration"));

	return { Ok, state.allocator.allocate<AST::ExternDefinition>(Required(&functionNameToken), functionParameters, isVariadic) };
}

#pragma endregion

static Result<std::string_view> parseImport(ParserState& state)
{
	state.it.consume<Import>();

	auto [success, startToken] = state.it.consume<Identifier>("module name after 'import' keyword");
	ERROR_IF_FALSE(success);

	while (true)
	{
		ERROR_IF_FALSE(state.it.expect<DoubleColon>("'::' or ';' after module name"));
		ERROR_IF_FALSE(state.it.expect<Identifier>("module name after '::'"));

		if (state.it.peek().kind == Semicolon)
		{
			break;
		}
	}

	const Token& endToken = state.it.previous();

	ERROR_IF_FALSE(state.it.expect<Semicolon>("';' after import statement"));

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
