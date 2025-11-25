#pragma once

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
	Logger(const Source& source) : m_Source(source), m_HasError(false) {}

	template<typename... Args>
	void logErrorInRange(TokenPosition startPos, TokenPosition endPos, const std::string& format, Args&&... args)
	{
		m_HasError = true;
		logError("{}:{}:{}", startPos.line, startPos.col, std::vformat(format, std::make_format_args(args...)));
	}

	bool hasError() const { return m_HasError; }

private:
	const Source& m_Source;
	bool m_HasError;
};

#define ASSERT(x) do{ if (!(x)) __debugbreak(); } while(0)