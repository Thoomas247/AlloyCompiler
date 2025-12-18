#pragma once
#include "../util/logger.hpp"

class TokenIterator
{
public:
	TokenIterator(Logger& logger, const Source& source, const std::vector<Token>& tokens);

	bool hasNext(size_t offset = 0) const;

	/**
	* Consumes the current token.
	*/
	const Token& consume();

	/**
	* Consumes if current token is one of the expected kinds.
	*/
	template<TokenKind... Expected>
	Result<const Token&> consume();

	/**
	* Consumes if the current token is one of the expected kinds.
	* Displays an error message and does not consume the token if it is not.
	*/
	template<TokenKind... Expected>
	Result<const Token&> consume(const std::string& expectedMessage);

	/**
	* Returns the current token without consuming it.
	*/
	const Token& peek(size_t offset = 0) const;

	/**
	* Returns the previously consumed token.
	*/
	const Token& previous() const;

	/**
	* Creates a view into the source code in the given range.
	* Start token is inclusive, end token is exclusive.
	*/
	std::string_view createView(const Token& startToken, const Token& endToken) const;

private:
	Logger& m_Logger;
	const Source& m_Source;
	const std::vector<Token>& m_Tokens;
	size_t m_CurrentIndex;
};

template<TokenKind ...Expected>
inline Result<const Token&> TokenIterator::consume()
{
	using enum Status;

	const auto& token = peek();
	if (((token.kind == Expected) || ...))
	{
		m_CurrentIndex++;
		return { Ok, token };
	}
	else
	{
		return { Error, token };
	}
}

template<TokenKind ...Expected>
inline Result<const Token&> TokenIterator::consume(const std::string& expectedMessage)
{
	using enum Status;

	const auto& token = peek();
	if (((token.kind == Expected) || ...))
	{
		m_CurrentIndex++;
		return { Ok, token };
	}
	else
	{
		m_Logger.logErrorUnexpected(token, token, expectedMessage, token);
		return { Error, token };
	}
}
