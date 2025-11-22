#include "tokenizer.hpp"

#include <unordered_map>
#include <array>

#include "logger.hpp"
#include "source.hpp"

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

struct TokenizerState
{
	Logger logger;
	SourceIterator it;
	std::vector<Token> tokens;

	TokenizerState(const Source& source)
		: logger(source), it(source)
	{
	}
};

using enum TokenKind;

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

static void skipWhitespace(TokenizerState& state)
{
	while (state.it.hasNext() && isspace(state.it.peek()))
	{
		state.it.consume();
	}
}

static bool tryGetComment(TokenizerState& state)
{
	if (!(state.it.peek() == '/' && state.it.hasNext(1) &&
		(state.it.peek(1) == '/' || state.it.peek(1) == '*')))
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();
	state.it.consume('/');

	if (state.it.peek() == '/')
	{
		// single-line comment
		state.it.consume('/');
		while (state.it.hasNext() && state.it.peek() != '\n')
		{
			state.it.consume();
		}
	}
	else
	{
		// multi-line comment
		bool endFound = false;

		state.it.consume('*');
		while (state.it.hasNext())
		{
			if (state.it.peek() == '*' && state.it.hasNext(1) && state.it.peek(1) == '/')
			{
				state.it.consume('*');
				state.it.consume('/');
				endFound = true;
				break;
			}
			else
			{
				state.it.consume();
			}
		}

		if (!endFound)
		{
			state.logger.logErrorInRange(startPos, state.it.currentPosition(), "Unterminated multi-line comment.");
		}
	}

	const auto endPos = state.it.currentPosition();

	state.tokens.emplace_back(Comment, startPos, endPos);

	return true;
}

static bool tryGetOperator(TokenizerState& state)
{
	const auto opIt = s_OperatorCombinations.find(state.it.peek());
	if (opIt == s_OperatorCombinations.end())
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();
	state.it.consume();

	// keep going while we have a matching sequence
	size_t suffixCharIndex = 0;
	bool hasMatch = false;
	do
	{
		// check every possible sequence for the next character
		for (const auto& suffix : opIt->second)
		{
			if (suffix.size() > suffixCharIndex && suffix[suffixCharIndex] == state.it.peek())
			{
				hasMatch = true;
			}
		}

		if (hasMatch)
		{
			state.it.consume();
			suffixCharIndex++;
		}
	} while (hasMatch);

	const auto endPos = state.it.currentPosition();

	const auto textView = state.it.createView(startPos, endPos);

	auto knownSymbolsIt = s_KnownSymbols.find(textView);
	ASSERT(knownSymbolsIt != s_KnownSymbols.end());

	state.tokens.emplace_back(knownSymbolsIt->second, startPos, endPos);
	return true;
}

static bool tryGetDelimiter(TokenizerState& state)
{
	auto knownSymbolsIt = s_KnownSymbols.find(std::string(1, state.it.peek()));

	if (knownSymbolsIt == s_KnownSymbols.end())
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();
	state.it.consume();
	const auto endPos = state.it.currentPosition();

	state.tokens.emplace_back(knownSymbolsIt->second, startPos, endPos);
	return true;
}

static bool tryGetKeyword(TokenizerState& state)
{
	if (!(isalpha(state.it.peek()) || state.it.peek() == '_'))
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();

	state.it.consume();
	while (state.it.hasNext() && (isalnum(state.it.peek()) || state.it.peek() == '_'))
	{
		state.it.consume();
	}

	const auto endPos = state.it.currentPosition();

	const auto textView = state.it.createView(startPos, endPos);

	auto knownSymbolIt = s_KnownSymbols.find(textView);
	if (knownSymbolIt != s_KnownSymbols.end())
	{
		state.tokens.emplace_back(knownSymbolIt->second, startPos, endPos);
	}
	else
	{
		state.tokens.emplace_back(Identifier, startPos, endPos);
	}

	return true;

}

static bool tryGetStringLiteral(TokenizerState& state)
{
	if (state.it.peek() != '"')
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();

	state.it.consume('"');
	bool endFound = false;

	while (state.it.hasNext())
	{
		if (state.it.peek() == '"')
		{
			state.it.consume('"');
			endFound = true;
			break;
		}
		// handle escape sequences
		else if (state.it.peek() == '\\')
		{
			state.it.consume('\\');
			if (state.it.hasNext())
			{
				state.it.consume();
			}
		}
		else
		{
			state.it.consume();
		}
	}

	const auto endPos = state.it.currentPosition();

	if (!endFound)
	{
		state.logger.logErrorInRange(startPos, endPos, "Unterminated string literal.");
	}

	state.tokens.emplace_back(StringLiteral, startPos, endPos);
	return true;
}

static bool tryGetCharLiteral(TokenizerState& state)
{
	if (state.it.peek() != '\'')
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();

	state.it.consume('\'');

	bool endFound = false;
	if (state.it.hasNext())
	{
		// handle escape sequences
		if (state.it.peek() == '\\')
		{
			state.it.consume('\\');
			if (state.it.hasNext())
			{
				state.it.consume();
			}
		}
		else
		{
			state.it.consume();
		}
		if (state.it.hasNext() && state.it.peek() == '\'')
		{
			state.it.consume('\'');
			endFound = true;
		}
	}

	const auto endPos = state.it.currentPosition();
	if (!endFound)
	{
		state.logger.logErrorInRange(startPos, endPos, "Unterminated char literal.");
	}

	state.tokens.emplace_back(CharLiteral, startPos, endPos);
	return true;
}

static bool tryGetNumberLiteral(TokenizerState& state)
{
	if (!isdigit(state.it.peek()))
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();

	bool isFloat = false;

	while (state.it.hasNext() && isdigit(state.it.peek()))
	{
		state.it.consume();
	}

	if (state.it.hasNext() && state.it.peek() == '.')
	{
		isFloat = true;
		state.it.consume('.');
		while (state.it.hasNext() && isdigit(state.it.peek()))
		{
			state.it.consume();
		}
	}

	const auto endPos = state.it.currentPosition();

	state.tokens.emplace_back(isFloat ? FloatLiteral : IntegerLiteral, startPos, endPos);
	return true;
}

Result<std::vector<Token>> tokenize(const Source& source)
{
	ASSERT(checkOperatorCombinations());

	TokenizerState state(source);

	while (state.it.hasNext())
	{
		skipWhitespace(state);

		if (tryGetComment(state)) continue;
		if (tryGetOperator(state)) continue;
		if (tryGetDelimiter(state)) continue;
		if (tryGetKeyword(state)) continue;
		if (tryGetStringLiteral(state)) continue;
		if (tryGetCharLiteral(state)) continue;
		if (tryGetNumberLiteral(state)) continue;

		const auto currentPos = state.it.currentPosition();
		state.logger.logErrorInRange(currentPos, currentPos, "Unrecognized character '{}'.", state.it.peek());
		state.it.consume();
	}

	state.tokens.emplace_back(EndOfFile, state.it.currentPosition(), state.it.currentPosition());
	return { state.logger.hasError(), std::move(state.tokens) };
}