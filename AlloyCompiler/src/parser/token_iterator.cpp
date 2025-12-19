#include "token_iterator.hpp"

using enum Status;

TokenIterator::TokenIterator(Logger& logger, const Source& source, const std::vector<Token>& tokens)
	: m_Logger(logger), m_Source(source), m_Tokens(tokens), m_CurrentIndex(0)
{
}

bool TokenIterator::hasNext(size_t offset) const
{
	return (m_CurrentIndex + offset) < m_Tokens.size();
}

Result<const Token&> TokenIterator::consume()
{
	if (!hasNext())
	{
		return { Error, m_Tokens[m_CurrentIndex] };
	}

	return { Ok, m_Tokens[m_CurrentIndex++] };
}

const Token& TokenIterator::peek(size_t offset) const
{
	ASSERT(m_CurrentIndex + offset < m_Tokens.size());
	return m_Tokens[m_CurrentIndex + offset];
}

const Token& TokenIterator::previous() const
{
	ASSERT(m_CurrentIndex > 0);
	return m_Tokens[m_CurrentIndex - 1];
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
