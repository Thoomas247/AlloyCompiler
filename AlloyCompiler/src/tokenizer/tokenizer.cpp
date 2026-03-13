#include "tokenizer.hpp"

#include <unordered_map>
#include <array>

#include "../util/logger.hpp"
#include "../source/source.hpp"

class SourceIterator
{
public:
	SourceIterator(const Source& source) : m_Source(source), m_CurrentPosition(0, 1, 1) {}

	bool hasNext(size_t offset = 0) const
	{
		return (m_CurrentPosition.index + offset) < m_Source.data.size();
	}

	char consume(char expected = 0)
	{
		ASSERT(hasNext());
		ASSERT(expected != 0 ? m_Source.data[m_CurrentPosition.index] == expected : true);

		if (m_Source.data[m_CurrentPosition.index] == '\n')
		{
			m_CurrentPosition.line++;
			m_CurrentPosition.col = 1;
		}
		else
		{
			m_CurrentPosition.col++;
		}

		return m_Source.data[m_CurrentPosition.index++];
	}

	char peek(size_t offset = 0) const
	{
		ASSERT(hasNext(offset));
		return m_Source.data[m_CurrentPosition.index + offset];
	}

	TokenPosition currentPosition() const
	{
		return m_CurrentPosition;
	}

	std::string_view createView(TokenPosition startPos, TokenPosition endPos) const
	{
		ASSERT(startPos.index <= m_Source.data.size());
		ASSERT(endPos.index <= m_Source.data.size());
		ASSERT(endPos.index >= startPos.index);

		return std::string_view(&m_Source.data[startPos.index], endPos.index - startPos.index);
	}

private:
	const Source& m_Source;
	TokenPosition m_CurrentPosition;
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
	{"as", As},

	{"extern", Extern},
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
	{"match", Match},

	{"break", Break},
	{"return", Return},

	{"new", New},
	{"move", Move},

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
	{"&", BitwiseAnd},			{"&&", LogicalAnd},			{"&=", AndAssign},
	{"|", BitwiseOr},			{"||", LogicalOr},			{"|=", OrAssign},
	{"~", BitwiseNot},
	{"^", Xor},			{"^=", XorAssign},

	{"->", Arrow},

	{".", Dot },		{"...", Ellipsis},
	{":", Colon},		{"::", DoubleColon},
	{",", Comma},
	{";", Semicolon}
};

static const std::unordered_map<char, std::array<std::string_view, 3>> s_OperatorCombinations =
{
	{'+', {"="}},				// +, +=
	{'-', {"=", ">"}},			// -, -=, ->
	{'*', {"="}},				// *, *=
	{'/', {"="}},				// /, /=
	{'%', {"="}},				// %, %=

	{'=', {"="}},				// =, ==
	{'!', {"="}},				// !, !=
	{'<', {"<", "<=", "="}},	// <, <<, <<=, <=
	{'>', {">", ">=", "="}},	// >, >>, >>=, >=

	{'&', {"&", "="}},			// &, &&, &=
	{'|', {"|", "="}},			// |, ||, |=
	{'~', {}},					// ~
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
				Log::fatal("Operator combination '{}' is not defined in known symbols.", currentOp);
				return false;
			}
		}
	}

	return true;
}

static bool trySkipWhitespace(TokenizerState& state)
{
	bool skipped = false;
	while (state.it.hasNext() && isspace(state.it.peek()))
	{
		state.it.consume();
		skipped = true;
	}

	return skipped;
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
	state.it.consume(opIt->first);

	// keep going while we have a matching sequence
	size_t suffixCharIndex = 0;
	bool hasMatch = false;
	do
	{
		hasMatch = false;

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
		if (trySkipWhitespace(state)) continue;
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
	return { state.logger.hasError() ? Status::Error : Status::Ok , std::move(state.tokens) };
}