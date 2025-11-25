#include <vector>

#include "logger.hpp"
#include "allocator.hpp"
#include "tokenizer.hpp"

class TokenIterator
{
public:
	TokenIterator(const std::vector<Token>& tokens)
		: m_Tokens(tokens), m_CurrentIndex(0)
	{
	}

	bool hasNext(size_t offset = 0) const
	{
		return (m_CurrentIndex + offset) < m_Tokens.size();
	}

	const Token& consume()
	{
		return m_Tokens[m_CurrentIndex++];
	}

	const Token& consume(TokenKind expected)
	{
		ASSERT(m_Tokens[m_CurrentIndex].kind == expected);
		return m_Tokens[m_CurrentIndex++];
	}

	const Token& peek(size_t offset = 0) const
	{
		return m_Tokens[m_CurrentIndex + offset];
	}

private:
	const std::vector<Token>& m_Tokens;
	size_t m_CurrentIndex;
};

struct ParserState
{
	Logger logger;
	TokenIterator it;
	Allocator allocator;
};

