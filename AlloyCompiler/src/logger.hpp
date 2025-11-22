#include <string>
#include <format>
#include <print>

#include "tokenizer.hpp"

template<typename... Args>
static void logFatal(const std::string& format, Args&&... args)
{
	std::println("FATAL:{}", std::vformat(format, std::make_format_args(args...)));
}

template<typename... Args>
static void logError(const std::string& format, Args&&... args)
{
	std::println("ERROR:{}", std::vformat(format, std::make_format_args(args...)));
}

template<typename... Args>
static void logInfo(const std::string& format, Args&&... args)
{
	std::println("INFO:{}", std::vformat(format, std::make_format_args(args...)));
}

class Logger
{
public:
	Logger(const Source& source) : mSource(source), mHasError(false) {}

	template<typename... Args>
	void logErrorInRange(TokenPosition startPos, TokenPosition endPos, const std::string& format, Args&&... args)
	{
		mHasError = true;
		logError("{}:{}:{}", startPos.line, startPos.col, std::vformat(format, std::make_format_args(args...)));
	}

	bool hasError() const { return mHasError; }

private:
	const Source& mSource;
	bool mHasError;
};

#define ASSERT(x) do{ if (!(x)) __debugbreak(); } while(0)