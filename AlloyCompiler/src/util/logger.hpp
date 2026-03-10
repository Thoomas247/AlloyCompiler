#pragma once

#include <string>
#include <format>
#include <print>

#include "../tokenizer/tokenizer.hpp"

namespace Log
{
	template<typename... Args>
	static void fatal(const std::string& format, Args&&... args)
	{
		std::println("FATAL:{}", std::vformat(format, std::make_format_args(args...)));
	}

	template<typename... Args>
	static void error(const std::string& format, Args&&... args)
	{
		std::println("ERROR:{}", std::vformat(format, std::make_format_args(args...)));
	}

	template<typename... Args>
	static void info(const std::string& format, Args&&... args)
	{
		std::println("INFO:{}", std::vformat(format, std::make_format_args(args...)));
	}
}

struct Token;

class Logger
{
public:
	Logger(const Source& source) : m_Source(source), m_HasError(false) {}

	template<typename... Args>
	void logErrorInRange(TokenPosition startPos, TokenPosition, const std::string& format, Args&&... args)
	{
		m_HasError = true;
		Log::error("{}:{}:{}", startPos.line, startPos.col, makeFormatted(format, args...));
		__debugbreak();
	}

	template<typename... Args>
	void logErrorInRange(Token startToken, Token endToken, const std::string& format, Args&&... args)
	{
		logErrorInRange(startToken.start, endToken.end, format, std::forward<Args>(args)...);
	}

	template<typename E, typename F>
	void logErrorUnexpected(Token startToken, Token endToken, E&& expected, F&& found)
	{
		logErrorInRange(startToken, endToken, "Expected {}, but found '{}'.", expected, found);
	}

	template<typename... Args>
	void logInfo(const std::string& format, Args&&... args)
	{
		Log::info("{}", makeFormatted(format, args...));
	}

	bool hasError() const { return m_HasError; }

private:
	template<typename... Args>
	auto makeFormatted(const std::string& format, Args&&... args) const
	{
		// template magic to make type Token printable
		// overload makePrintable to add more printable types
		auto transformed = std::tuple{
			makePrintable(std::forward<Args>(args))...
		};

		auto formatted = std::vformat(
			format,
			std::apply(
				[](auto&... vals) {
					return std::make_format_args(vals...);
				},
				transformed
			)
		);

		return formatted;
	}

	template<typename T>
	auto makePrintable(const T& value) const
	{
		return value;
	}

	auto makePrintable(const Token& token) const
	{
		return std::string_view(&m_Source.data[token.start.index], token.end.index - token.start.index);
	}

	const Source& m_Source;
	bool m_HasError;
};

#define ASSERT(x) do{ if (!(x)) __debugbreak(); } while(0)