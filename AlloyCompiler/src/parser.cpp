#include <vector>

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

	const Token& consume()
	{
		return m_Tokens[m_CurrentIndex++];
	}

	template<TokenKind... Expected>
	const Token& consume()
	{
		ASSERT((m_Tokens[m_CurrentIndex].kind == Expected || ...));
		return m_Tokens[m_CurrentIndex++];
	}

	template<TokenKind... Expected>
	std::pair<bool, TokenRef> consume(const std::string& expectedMessage)
	{
		if (!hasNext())
		{
			m_Logger.logErrorInRange(
				m_Tokens.back(),
				m_Tokens.back(),
				"Expected {}, but found end of file.",
				expectedMessage
			);
			return { false, m_Tokens.back() };
		}

		const auto& token = peek();
		if ((token.kind == Expected || ...))
		{
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

	template<TokenKind... Expected>
	bool expect(const std::string& expectedMessage)
	{
		auto [success, token] = consume<Expected...>(expectedMessage);
		if (success)
		{
			m_CurrentIndex++;
		}
		return success;
	}

	const Token& peek(size_t offset = 0) const
	{
		ASSERT(m_CurrentIndex + offset < m_Tokens.size());
		return m_Tokens[m_CurrentIndex + offset];
	}

	const Token& previous() const
	{
		ASSERT(m_CurrentIndex > 0);
		return m_Tokens[m_CurrentIndex - 1];
	}

	std::string_view createView(TokenRef startToken, TokenRef endToken) const
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

using enum Status;
using enum TokenKind;

#define ERROR_IF_FALSE(expr) \
	do { \
		if (!expr) \
			return { Error }; \
	} while(0)

static Result<std::string_view> parseImport(ParserState& state)
{
	TokenRef errorToken = state.it.consume<Import>();

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

	TokenRef endToken = state.it.previous();

	state.it.consume<Semicolon>();

	return { Ok, state.it.createView(startToken, endToken) };
}

static Result<Required<AST::Program>> parseProgram(ParserState& state)
{
	auto program = state.allocator.allocate<AST::Program>();

	Status status = Ok;
	while (state.it.hasNext())
	{
		const auto& token = state.it.peek();
		switch (token.kind)
		{
		case Import:

			break;

		case EndOfFile:
			state.it.consume();
			break;

		default:
			state.logger.logErrorInRange(token.start, token.end, "Unexpected token '{}'.", token);
			status = Error;
			break;
		}
	}

	return { status, Required(program) };
}

Result<Required<AST::Program>> parse(const Source& source, const std::vector<Token>& tokens)
{
	ParserState state(source, tokens);

	return parseProgram(state);
}
