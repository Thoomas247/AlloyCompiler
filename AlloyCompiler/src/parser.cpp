#include <vector>
#include <functional>

#include "logger.hpp"
#include "allocator.hpp"
#include "tokenizer.hpp"
#include "parser.hpp"

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
		ASSERT((m_Tokens[m_CurrentIndex].kind == Expected || ...));
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
		if ((token.kind == Expected || ...))
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

static Result<Required<AST::Type>> parseType(ParserState& state)
{

}

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

static Result<Required<AST::Module>> parseModule(ParserState& state)
{
	auto module = state.allocator.allocate<AST::Module>();

	Optional<AST::ListNode<std::string_view>> imports;
	Optional<AST::ListNode<AST::Definition>> definitions;

	Status moduleStatus = Ok;

	auto [importsResult, imports] = noneOrMore(state, Import, parseImport);

	while (state.it.hasNext())
	{
		const auto& token = state.it.peek();
		switch (token.kind)
		{
		case Extern:
		{

		}

		case EndOfFile:
		{
			state.it.consume();
			break;
		}
		case Import:
		{
			state.logger.logErrorInRange(token, token, "Import statements must appear before any definitions.");
			moduleStatus = Error;
			break;
		}
		default:
		{
			state.logger.logErrorInRange(token, token, "Unexpected token '{}'.", token);
			moduleStatus = Error;
			break;
		}
		}
	}

	return { moduleStatus, module };
}

Result<Required<AST::Module>> parse(const Source& source, const std::vector<Token>& tokens)
{
	ParserState state(source, tokens);

	return parseModule(state);
}
