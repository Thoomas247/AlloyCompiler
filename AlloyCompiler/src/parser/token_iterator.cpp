#include "token_iterator.hpp"
using enum Status;

/* -- PUBLIC -- */

TokenIterator::TokenIterator(Logger& logger, const Source& source, const std::vector<Token>& tokens)
	: m_Logger(logger), m_Source(source), m_Tokens(tokens), m_CurrentIndex(0)
{
	skipComments();
}

bool TokenIterator::hasNext(size_t offset) const
{
	return (m_CurrentIndex + offset) < m_Tokens.size();
}

Result<const Token&> TokenIterator::consume()
{
	if (!hasNext())
	{
		return { Error, m_Tokens.back() };
	}

	const auto& token = m_Tokens[m_CurrentIndex++];
	skipComments();
	return { Ok, token };
}

const Token& TokenIterator::peek(size_t offset) const
{
	size_t index = m_CurrentIndex;
	size_t skipped = 0;
	while (skipped <= offset)
	{
		while (index < m_Tokens.size() && m_Tokens[index].kind == TokenKind::Comment)
			index++;
		if (skipped == offset)
			break;
		index++;
		skipped++;
	}
	ASSERT(index < m_Tokens.size());
	return m_Tokens[index];
}

const Token& TokenIterator::previous() const
{
	ASSERT(m_CurrentIndex > 0);
	size_t index = m_CurrentIndex - 1;
	while (index > 0 && m_Tokens[index].kind == TokenKind::Comment)
		index--;
	return m_Tokens[index];
}

std::string_view TokenIterator::createView(const Token& startToken, const Token& endToken) const
{
	const auto startIndex = startToken.start.index;
	const auto endIndex = endToken.end.index;
	ASSERT(startIndex <= m_Source.data.size());
	ASSERT(endIndex <= m_Source.data.size());
	ASSERT(endIndex >= startIndex);
	return std::string_view(&m_Source.data[startIndex], endIndex - startIndex);
}


/* -- PRIVATE -- */

void TokenIterator::skipComments()
{
	while (m_CurrentIndex < m_Tokens.size() && m_Tokens[m_CurrentIndex].kind == TokenKind::Comment)
	{
		m_CurrentIndex++;
	}
}