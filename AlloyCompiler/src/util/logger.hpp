#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <format>
#include <print>
#include <vector>

#include "../tokenizer/tokenizer.hpp"

struct Token;

// A single compiler diagnostic, with an optional source location.
struct Diagnostic
{
	enum class Severity { Error, Warning, Info };

	Severity severity;
	bool hasLocation;
	uint32_t line;
	uint32_t col;
	std::string message;
};

// Process-wide diagnostic sink. Every compiler stage reports here instead of
// aborting on the first error; the driver prints the accumulated list at the
// end of compilation and derives a meaningful exit code from getErrorCount().
class DiagnosticEngine
{
public:
	static DiagnosticEngine& instance()
	{
		static DiagnosticEngine engine;
		return engine;
	}

	void report(Diagnostic diagnostic)
	{
		if (diagnostic.severity == Diagnostic::Severity::Error)
			++m_ErrorCount;
		m_Diagnostics.push_back(std::move(diagnostic));
	}

	const std::vector<Diagnostic>& diagnostics() const { return m_Diagnostics; }
	size_t getErrorCount() const { return m_ErrorCount; }
	bool hasError() const { return m_ErrorCount > 0; }

	// Resets the sink — used by the test runner to isolate per-test diagnostics.
	void clear()
	{
		m_Diagnostics.clear();
		m_ErrorCount = 0;
	}

	// Prints every accumulated diagnostic to stderr (unbuffered — survives a crash).
	void printAll() const
	{
		for (const auto& d : m_Diagnostics)
		{
			const char* tag =
				d.severity == Diagnostic::Severity::Error   ? "ERROR" :
				d.severity == Diagnostic::Severity::Warning ? "WARNING" : "INFO";
			if (d.hasLocation)
				std::fprintf(stderr, "%s:%u:%u: %s\n", tag, d.line, d.col, d.message.c_str());
			else
				std::fprintf(stderr, "%s: %s\n", tag, d.message.c_str());
		}
		std::fflush(stderr);
	}

private:
	std::vector<Diagnostic> m_Diagnostics;
	size_t m_ErrorCount = 0;
};

namespace Log
{
	// Reports a fatal (location-less) error into the diagnostic engine.
	template<typename... Args>
	static void fatal(const std::string& format, Args&&... args)
	{
		DiagnosticEngine::instance().report(Diagnostic{
			Diagnostic::Severity::Error, false, 0, 0,
			std::vformat(format, std::make_format_args(args...))
		});
	}

	// Reports a location-less error into the diagnostic engine.
	template<typename... Args>
	static void error(const std::string& format, Args&&... args)
	{
		DiagnosticEngine::instance().report(Diagnostic{
			Diagnostic::Severity::Error, false, 0, 0,
			std::vformat(format, std::make_format_args(args...))
		});
	}

	template<typename... Args>
	static void info(const std::string& format, Args&&... args)
	{
		std::println("INFO:{}", std::vformat(format, std::make_format_args(args...)));
	}
}

class Logger
{
public:
	Logger(const Source& source) : m_Source(source), m_HasError(false) {}

	const Source& getSource() const
	{
		return m_Source;
	}

	template<typename... Args>
	void logErrorInRange(TokenPosition startPos, TokenPosition, const std::string& format, Args&&... args)
	{
		m_HasError = true;
		DiagnosticEngine::instance().report(Diagnostic{
			Diagnostic::Severity::Error, true, startPos.line, startPos.col,
			makeFormatted(format, args...)
		});
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
