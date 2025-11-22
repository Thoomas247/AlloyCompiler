#pragma once

#include <string>
#include <vector>

#include "logger.hpp"
#include "result.hpp"
#include "source.hpp"
#include "tokenizer.hpp"

class TokenIterator
{
public:
	TokenIterator(const std::vector<Token>& tokens)
		: mTokens(tokens), mCurrentIndex(0)
	{
	}

	bool hasNext(size_t offset = 0) const
	{
		return (mCurrentIndex + offset) < mTokens.size();
	}

	const Token& consume()
	{
		return mTokens[mCurrentIndex++];
	}

	const Token& consume(TokenKind expected)
	{
		ASSERT(mTokens[mCurrentIndex].kind == expected);
		return mTokens[mCurrentIndex++];
	}

	const Token& peek(size_t offset = 0) const
	{
		return mTokens[mCurrentIndex + offset];
	}

private:
	const std::vector<Token>& mTokens;
	size_t mCurrentIndex;
};

struct ParserState
{
	Logger logger;
	TokenIterator it;
};

Result<> parse(const Source& source, const std::vector<std::string>& tokens)
{

}