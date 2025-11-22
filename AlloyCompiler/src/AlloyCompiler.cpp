#include <print>
#include <filesystem>
#include <stack>
#include <vector>
#include <array>
#include <fstream>
#include <unordered_map>
#include <format>
#include <string>

namespace fs = std::filesystem;

constexpr auto FILE_EXTENSION = ".alloy";

template<typename... Args>
static void logFatal(const std::string& format, Args&&... args)
{
	std::println("FATAL: {}", std::vformat(format, std::make_format_args(args...)));
}

template<typename... Args>
static void logError(const std::string& format, Args&&... args)
{
	std::println("ERROR: {}", std::vformat(format, std::make_format_args(args...)));
}

static std::vector<fs::path> getFilePaths(const fs::path& rootDir)
{
	std::vector<fs::path> filesToCompile;
	for (const auto& entry : fs::recursive_directory_iterator(rootDir))
	{
		if (entry.is_regular_file() && entry.path().extension() == FILE_EXTENSION)
		{
			filesToCompile.push_back(entry);
		}
	}

	return filesToCompile;
}

static std::string getModuleName(const fs::path& relativePath)
{
	std::string moduleName;
	for (const fs::path& part : relativePath)
	{
		if (!moduleName.empty())
		{
			moduleName += "::";
		}

		moduleName += part.stem().string();
	}

	return moduleName;
}

struct Source
{
	std::string moduleName;
	std::string data;
};

static std::vector<Source> getSources(const fs::path& rootDir, const std::vector<fs::path>& filePaths)
{
	std::vector<Source> sources;
	for (const auto& filePath : filePaths)
	{
		const auto relativePath = fs::relative(filePath, rootDir);

		std::ifstream fileStream(filePath);

		if (!fileStream.is_open())
		{
			logError("Could not read source file '{}'.", filePath.generic_string(), 12);
		}

		std::stringstream stringStream;
		stringStream << fileStream.rdbuf();

		fileStream.close();

		sources.push_back({ getModuleName(relativePath), stringStream.str() });
	}

	return sources;
}

#define ASSERT(x) if (!(x)) __debugbreak()

struct TokenPosition
{
	size_t index;	// index of character in the source string
	uint32_t line;
	uint32_t col;
};

class SourceIterator
{
public:
	SourceIterator(const Source& source) : mSource(source), mCurrentPosition(0, 1, 1) {}

	bool hasNext(size_t offset = 0) const
	{
		return (mCurrentPosition.index + offset) < mSource.data.size();
	}

	char consume(char expected = 0)
	{
		ASSERT(hasNext());
		ASSERT(expected != 0 ? mSource.data[mCurrentPosition.index] == expected : true);

		if (mSource.data[mCurrentPosition.index] == '\n')
		{
			mCurrentPosition.line++;
			mCurrentPosition.col = 1;
		}
		else
		{
			mCurrentPosition.col++;
		}

		return mSource.data[mCurrentPosition.index++];
	}

	char peek(size_t offset = 0) const
	{
		ASSERT(hasNext(offset));
		return mSource.data[mCurrentPosition.index + offset];
	}

	TokenPosition currentPosition() const
	{
		return mCurrentPosition;
	}

	std::string_view createView(TokenPosition startPos, TokenPosition endPos) const
	{
		ASSERT(endPos.index <= mSource.data.size());
		return std::string_view(&mSource.data[startPos.index], endPos.index - startPos.index);
	}

private:
	const Source& mSource;
	TokenPosition mCurrentPosition;
};


template<typename... Args>
static void logErrorInRange(TokenPosition startPos, TokenPosition endPos, const std::string& format, Args&&... args)
{
	logError("{}:{}:{}", startPos.line, startPos.col, std::vformat(format, std::make_format_args(args...)));
}

enum class TokenKind
{
	Comment,

	Identifier,

	IntegerLiteral,
	FloatLiteral,
	StringLiteral,
	CharLiteral,

	Import,

	Type,
	Enum,
	Struct,
	Const,
	Var,
	Fn,
	If,
	Else,
	While,
	For,
	Return,

	LParen, RParen,
	LBrace, RBrace,
	LBracket, RBracket,

	Plus, PlusAssign,
	Minus, MinusAssign,
	Multiply, MultiplyAssign,
	Divide, DivideAssign,
	Modulo, ModuloAssign,
	Assign, Equal,
	Not, NotEqual,
	Less, ShiftLeft, ShiftLeftAssign, LessEqual,
	Greater, ShiftRight, ShiftRightAssign, GreaterEqual,
	And, LogicalAnd, AndAssign,
	Or, LogicalOr, OrAssign,
	BitwiseNot, BitwiseNotAssign,
	Xor, XorAssign,

	Dot, Ellipsis,
	Colon, DoubleColon,
	Comma,
	Semicolon,

	EndOfFile,
};

using enum TokenKind;

struct Token
{
	TokenKind kind;
	TokenPosition start;
	TokenPosition end;
};

static const std::unordered_map<std::string_view, TokenKind> s_KnownSymbols =
{
	{"import", Import},

	{"type", Type},
	{"enum", Enum},
	{"struct", Struct},
	{"const", Const},
	{"var", Var},
	{"fn", Fn},
	{"if", If},
	{"else", Else},
	{"while", While},
	{"for", For},
	{"return", Return},

	{"(", LParen},		{")", RParen},
	{"{", LBrace},		{"}", RBrace},
	{"[", LBracket},	{"]", RBracket},

	{"+", Plus},		{"+=", PlusAssign},
	{"-", Minus},		{"-=", MinusAssign},
	{"*", Multiply},	{"*=", MultiplyAssign},
	{"/", Divide},		{"/=", DivideAssign},
	{"%", Modulo},		{"%=", ModuloAssign},
	{"=", Assign},		{"==", Equal},
	{"!", Not},			{"!=", NotEqual},
	{"<", Less},		{"<<", ShiftLeft},			{"<<=", ShiftLeftAssign},	{"<=", LessEqual},
	{">", Greater},		{">>", ShiftRight},			{">>=", ShiftRightAssign},	{">=", GreaterEqual},
	{"&", And},			{"&&", LogicalAnd},			{"&=", AndAssign},
	{"|", Or},			{"||", LogicalOr},			{"|=", OrAssign},
	{"~", BitwiseNot},	{"~=", BitwiseNotAssign},
	{"^", Xor},			{"^=", XorAssign},

	{".", Dot },		{"...", Ellipsis},
	{":", Colon},		{"::", DoubleColon},
	{",", Comma},
	{";", Semicolon}
};

static const std::unordered_map<char, std::array<std::string_view, 3>> s_OperatorCombinations =
{
	{'+', {"="}},				// +, +=
	{'-', {"="}},				// -, -=
	{'*', {"="}},				// *, *=
	{'/', {"="}},				// /, /=
	{'%', {"="}},				// %, %=

	{'=', {"="}},				// =, ==
	{'!', {"="}},				// !, !=
	{'<', {"<", "<=", "="}},	// <, <<, <<=, <=
	{'>', {">", ">=", "="}},	// >, >>, >>=, >=

	{'&', {"&", "="}},			// &, &&, &=
	{'|', {"|", "="}},			// |, ||, |=
	{'~', {"="}},				// ~, ~=
	{'^', {"="}},				// ^, ^=

	{'.', {".."}},				// ., ...
	{':', {":"}}				// :, ::
};

static bool checkOperatorCombinations()
{
	for (const auto& [opChar, combinations] : s_OperatorCombinations)
	{
		for (const auto& suffix : combinations)
		{
			std::string currentOp = std::string(1, opChar);
			currentOp += suffix;
			if (s_KnownSymbols.find(currentOp) == s_KnownSymbols.end())
			{
				logFatal("Operator combination '{}' is not defined in known symbols.", currentOp);
				return false;
			}
		}
	}

	return true;
}

static void skipWhitespace(SourceIterator& it)
{
	while (it.hasNext() && isspace(it.peek()))
	{
		it.consume();
	}
}

static bool tryGetComment(SourceIterator& it, std::vector<Token>& tokens)
{
	if (!(it.peek() == '/' && it.hasNext(1) &&
		(it.peek(1) == '/' || it.peek(1) == '*')))
	{
		return false;
	}

	const auto startPos = it.currentPosition();
	it.consume('/');

	if (it.peek() == '/')
	{
		// single-line comment
		it.consume('/');
		while (it.hasNext() && it.peek() != '\n')
		{
			it.consume();
		}
	}
	else
	{
		// multi-line comment
		bool endFound = false;

		it.consume('*');
		while (it.hasNext())
		{
			if (it.peek() == '*' && it.hasNext(1) && it.peek(1) == '/')
			{
				it.consume('*');
				it.consume('/');
				endFound = true;
				break;
			}
			else
			{
				it.consume();
			}
		}

		if (!endFound)
		{
			logErrorInRange(startPos, it.currentPosition(), "Unterminated multi-line comment.");
		}
	}

	const auto endPos = it.currentPosition();

	tokens.emplace_back(Comment, startPos, endPos);

	return true;
}

static bool tryGetOperator(SourceIterator& it, std::vector<Token>& tokens)
{
	const auto opIt = s_OperatorCombinations.find(it.peek());
	if (opIt == s_OperatorCombinations.end())
	{
		return false;
	}

	const auto startPos = it.currentPosition();
	it.consume();

	// keep going while we have a matching sequence
	size_t suffixCharIndex = 0;
	bool hasMatch = false;
	do
	{
		// check every possible sequence for the next character
		for (const auto& suffix : opIt->second)
		{
			if (suffix.size() > suffixCharIndex && suffix[suffixCharIndex] == it.peek())
			{
				hasMatch = true;
			}
		}

		if (hasMatch)
		{
			it.consume();
			suffixCharIndex++;
		}
	} while (hasMatch);

	const auto endPos = it.currentPosition();

	const auto textView = it.createView(startPos, endPos);

	auto knownSymbolsIt = s_KnownSymbols.find(textView);
	ASSERT(knownSymbolsIt != s_KnownSymbols.end());

	tokens.emplace_back(knownSymbolsIt->second, startPos, endPos);
	return true;
}

static bool tryGetDelimiter(SourceIterator& it, std::vector<Token>& tokens)
{
	auto knownSymbolsIt = s_KnownSymbols.find(std::string(1, it.peek()));

	if (knownSymbolsIt == s_KnownSymbols.end())
	{
		return false;
	}

	const auto startPos = it.currentPosition();
	it.consume();
	const auto endPos = it.currentPosition();

	tokens.emplace_back(knownSymbolsIt->second, startPos, endPos);
	return true;
}

static bool tryGetKeyword(SourceIterator& it, std::vector<Token>& tokens)
{
	if (!(isalpha(it.peek()) || it.peek() == '_'))
	{
		return false;
	}

	const auto startPos = it.currentPosition();

	it.consume();
	while (it.hasNext() && (isalnum(it.peek()) || it.peek() == '_'))
	{
		it.consume();
	}

	const auto endPos = it.currentPosition();

	const auto textView = it.createView(startPos, endPos);

	auto knownSymbolIt = s_KnownSymbols.find(textView);
	if (knownSymbolIt != s_KnownSymbols.end())
	{
		tokens.emplace_back(knownSymbolIt->second, startPos, endPos);
	}
	else
	{
		tokens.emplace_back(Identifier, startPos, endPos);
	}

	return true;

}

static bool tryGetStringLiteral(SourceIterator& it, std::vector<Token>& tokens)
{
	if (it.peek() != '"')
	{
		return false;
	}

	const auto startPos = it.currentPosition();

	it.consume('"');
	bool endFound = false;

	while (it.hasNext())
	{
		if (it.peek() == '"')
		{
			it.consume('"');
			endFound = true;
			break;
		}
		// handle escape sequences
		else if (it.peek() == '\\')
		{
			it.consume('\\');
			if (it.hasNext())
			{
				it.consume();
			}
		}
		else
		{
			it.consume();
		}
	}

	const auto endPos = it.currentPosition();

	if (!endFound)
	{
		logErrorInRange(startPos, endPos, "Unterminated string literal.");
	}

	tokens.emplace_back(StringLiteral, startPos, endPos);
	return true;
}

static bool tryGetCharLiteral(SourceIterator& it, std::vector<Token>& tokens)
{
	if (it.peek() != '\'')
	{
		return false;
	}

	const auto startPos = it.currentPosition();

	it.consume('\'');

	bool endFound = false;
	if (it.hasNext())
	{
		// handle escape sequences
		if (it.peek() == '\\')
		{
			it.consume('\\');
			if (it.hasNext())
			{
				it.consume();
			}
		}
		else
		{
			it.consume();
		}
		if (it.hasNext() && it.peek() == '\'')
		{
			it.consume('\'');
			endFound = true;
		}
	}

	const auto endPos = it.currentPosition();
	if (!endFound)
	{
		logErrorInRange(startPos, endPos, "Unterminated char literal.");
	}

	tokens.emplace_back(CharLiteral, startPos, endPos);
	return true;
}

static bool tryGetNumberLiteral(SourceIterator& it, std::vector<Token>& tokens)
{
	if (!isdigit(it.peek()))
	{
		return false;
	}

	const auto startPos = it.currentPosition();

	bool isFloat = false;

	while (it.hasNext() && isdigit(it.peek()))
	{
		it.consume();
	}

	if (it.hasNext() && it.peek() == '.')
	{
		isFloat = true;
		it.consume('.');
		while (it.hasNext() && isdigit(it.peek()))
		{
			it.consume();
		}
	}

	const auto endPos = it.currentPosition();

	tokens.emplace_back(isFloat ? FloatLiteral : IntegerLiteral, startPos, endPos);
	return true;
}

static std::vector<Token> tokenize(const Source& source)
{
	ASSERT(checkOperatorCombinations());

	SourceIterator it(source);
	std::vector<Token> tokens;

	while (it.hasNext())
	{
		skipWhitespace(it);

		if (tryGetComment(it, tokens)) continue;
		if (tryGetOperator(it, tokens)) continue;
		if (tryGetDelimiter(it, tokens)) continue;
		if (tryGetKeyword(it, tokens)) continue;
		if (tryGetStringLiteral(it, tokens)) continue;
		if (tryGetCharLiteral(it, tokens)) continue;
		if (tryGetNumberLiteral(it, tokens)) continue;

		const auto currentPos = it.currentPosition();
		logErrorInRange(currentPos, currentPos, "Unrecognized character '{}'.", it.peek());
	}

	tokens.emplace_back(EndOfFile, it.currentPosition(), it.currentPosition());
	return tokens;
}

static std::vector<std::vector<Token>> getTokens(const std::vector<Source>& sources)
{
	std::vector<std::vector<Token>> tokens;

	for (const auto& source : sources)
	{
		tokens.push_back(std::move(tokenize(source)));
	}

	return tokens;
}

int main()
{
	fs::path rootDir = "./examples";
	const auto filePaths = getFilePaths(rootDir);
	const auto sources = getSources(rootDir, filePaths);
	const auto tokens = getTokens(sources);

	__debugbreak();
}

