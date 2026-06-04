#include "tokenizer.hpp"

#include <unordered_map>
#include <array>

#include "../util/logger.hpp"
#include "../source/source.hpp"
#include "unicode_xid.hpp"

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

	struct CodePoint
	{
		uint32_t value;
		size_t length;   // UTF-8 byte length; 0 = no code point available
	};

	// Decodes a UTF-8 code point at the current position (+ byteOffset).
	// An invalid encoding is reported as a single-byte code point.
	CodePoint peekCodePoint(size_t byteOffset = 0) const
	{
		const size_t i = m_CurrentPosition.index + byteOffset;
		if (i >= m_Source.data.size())
			return { 0, 0 };

		const auto b0 = static_cast<unsigned char>(m_Source.data[i]);

		size_t length;
		uint32_t value;
		if (b0 < 0x80)              return { b0, 1 };
		else if ((b0 & 0xE0) == 0xC0) { length = 2; value = b0 & 0x1Fu; }
		else if ((b0 & 0xF0) == 0xE0) { length = 3; value = b0 & 0x0Fu; }
		else if ((b0 & 0xF8) == 0xF0) { length = 4; value = b0 & 0x07u; }
		else                        return { b0, 1 };

		if (i + length > m_Source.data.size())
			return { b0, 1 };

		for (size_t k = 1; k < length; ++k)
		{
			const auto bc = static_cast<unsigned char>(m_Source.data[i + k]);
			if ((bc & 0xC0) != 0x80)
				return { b0, 1 };
			value = (value << 6) | (bc & 0x3Fu);
		}
		return { value, length };
	}

	// Advances past a previously-peeked code point (counts as one column).
	void advanceCodePoint(const CodePoint& cp)
	{
		m_CurrentPosition.index += cp.length;
		m_CurrentPosition.col++;
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
	{"self", Self},

	{"pub", Pub},
	{"exp", Exp},
	{"true", True},
	{"false", False},
	{"interface", Interface},
	{"macro", Macro},
	{"is", Is},

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
	{";", Semicolon},

	{"#", Hash}
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
		// multi-line comment — block comments nest arbitrarily (§1.2)
		state.it.consume('*');

		size_t depth = 1;
		while (depth > 0 && state.it.hasNext())
		{
			if (state.it.peek() == '/' && state.it.hasNext(1) && state.it.peek(1) == '*')
			{
				state.it.consume('/');
				state.it.consume('*');
				++depth;
			}
			else if (state.it.peek() == '*' && state.it.hasNext(1) && state.it.peek(1) == '/')
			{
				state.it.consume('*');
				state.it.consume('/');
				--depth;
			}
			else
			{
				state.it.consume();
			}
		}

		if (depth > 0)
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

static bool tryGetIdentifier(TokenizerState& state)
{
	// Identifiers follow UAX #31 (§1.3): the first code point must be XID_Start
	// (or '_'); subsequent code points must be XID_Continue.
	auto cp = state.it.peekCodePoint();
	if (cp.length == 0 || !unicode::isXidStart(cp.value))
	{
		return false;
	}

	const auto startPos = state.it.currentPosition();
	state.it.advanceCodePoint(cp);

	while (state.it.hasNext())
	{
		auto next = state.it.peekCodePoint();
		if (next.length == 0 || !unicode::isXidContinue(next.value))
		{
			break;
		}
		state.it.advanceCodePoint(next);
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

static uint32_t hexDigitValue(char c)
{
	if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
	if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
	if (c >= 'A' && c <= 'F') return static_cast<uint32_t>(c - 'A' + 10);
	return 0;
}

// F4: validates one escape sequence inside a string/char literal (§1.6). The
// iterator must be positioned at the leading '\'. Consumes the whole sequence.
static void validateEscapeSequence(TokenizerState& state)
{
	const auto escStart = state.it.currentPosition();
	state.it.consume('\\');

	if (!state.it.hasNext())
	{
		state.logger.logErrorInRange(escStart, state.it.currentPosition(),
			"Unterminated escape sequence.");
		return;
	}

	const char esc = state.it.peek();
	switch (esc)
	{
		// simple escapes
		case 'n': case 'r': case 't': case '0':
		case '\\': case '\'': case '"':
			state.it.consume();
			return;

		// \xHH — exactly two hex digits
		case 'x': case 'X':
		{
			state.it.consume();
			int digits = 0;
			while (digits < 2 && state.it.hasNext()
				&& isxdigit(static_cast<unsigned char>(state.it.peek())))
			{
				state.it.consume();
				++digits;
			}
			if (digits != 2)
				state.logger.logErrorInRange(escStart, state.it.currentPosition(),
					"Invalid '\\x' escape: expected exactly two hexadecimal digits.");
			return;
		}

		// \u{HHHH} — braced Unicode scalar value
		case 'u': case 'U':
		{
			state.it.consume();
			if (!state.it.hasNext() || state.it.peek() != '{')
			{
				state.logger.logErrorInRange(escStart, state.it.currentPosition(),
					"Invalid '\\u' escape: expected '{'.");
				return;
			}
			state.it.consume('{');

			uint32_t codepoint = 0;
			int digits = 0;
			while (state.it.hasNext() && state.it.peek() != '}'
				&& isxdigit(static_cast<unsigned char>(state.it.peek())))
			{
				codepoint = codepoint * 16 + hexDigitValue(state.it.peek());
				state.it.consume();
				++digits;
			}

			if (!state.it.hasNext() || state.it.peek() != '}')
			{
				state.logger.logErrorInRange(escStart, state.it.currentPosition(),
					"Invalid '\\u' escape: expected '}'.");
				return;
			}
			state.it.consume('}');

			if (digits == 0)
				state.logger.logErrorInRange(escStart, state.it.currentPosition(),
					"Invalid '\\u' escape: expected at least one hexadecimal digit.");
			else if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				state.logger.logErrorInRange(escStart, state.it.currentPosition(),
					"Invalid '\\u' escape: not a valid Unicode scalar value.");
			return;
		}

		default:
			state.logger.logErrorInRange(escStart, state.it.currentPosition(),
				"Invalid escape sequence '\\{}'.", esc);
			state.it.consume();
			return;
	}
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
			validateEscapeSequence(state);
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

	// A char literal holds one or more chars / escape sequences, up to 8 bytes (§1.6).
	bool endFound = false;
	size_t elementCount = 0;
	while (state.it.hasNext())
	{
		if (state.it.peek() == '\'')
		{
			state.it.consume('\'');
			endFound = true;
			break;
		}

		// handle escape sequences
		if (state.it.peek() == '\\')
		{
			validateEscapeSequence(state);
		}
		else
		{
			state.it.consume();
		}
		++elementCount;
	}

	const auto endPos = state.it.currentPosition();
	if (!endFound)
	{
		state.logger.logErrorInRange(startPos, endPos, "Unterminated char literal.");
	}
	else if (elementCount == 0)
	{
		state.logger.logErrorInRange(startPos, endPos, "Empty char literal.");
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

	// Radix-prefixed integer literals: 0x.. (hex), 0b.. (binary), 0o.. (octal) (§1.6).
	if (state.it.peek() == '0' && state.it.hasNext(1))
	{
		const char radix = state.it.peek(1);
		if (radix == 'x' || radix == 'b' || radix == 'o')
		{
			state.it.consume('0');
			state.it.consume(radix);

			const auto isRadixDigit = [radix](char c) -> bool
			{
				const auto uc = static_cast<unsigned char>(c);
				if (radix == 'x') return isxdigit(uc) != 0;
				if (radix == 'b') return c == '0' || c == '1';
				return c >= '0' && c <= '7';
			};

			bool anyDigit = false;
			while (state.it.hasNext() && isRadixDigit(state.it.peek()))
			{
				state.it.consume();
				anyDigit = true;
			}

			const auto radixEndPos = state.it.currentPosition();
			if (!anyDigit)
			{
				state.logger.logErrorInRange(startPos, radixEndPos,
					"Integer literal has no digits after radix prefix.");
			}

			state.tokens.emplace_back(IntegerLiteral, startPos, radixEndPos);
			return true;
		}
	}

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
		if (tryGetIdentifier(state)) continue;
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