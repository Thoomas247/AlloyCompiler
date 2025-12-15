#pragma once

#include <cstdint>
#include <vector>

#include "../source/source.hpp"
#include "../util/result.hpp"

struct TokenPosition
{
	size_t index;	// index of character in the source string
	uint32_t line;
	uint32_t col;
};

enum class TokenKind
{
	Comment,

	Identifier,

	IntegerLiteral,
	FloatLiteral,
	StringLiteral,
	CharLiteral,

	Import,

	Extern,
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

	Break,
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
	BitwiseAnd, LogicalAnd, AndAssign,
	BitwiseOr, LogicalOr, OrAssign,
	BitwiseNot, BitwiseNotAssign,
	Xor, XorAssign,

	Arrow,

	Dot, Ellipsis,
	Colon, DoubleColon,
	Comma,
	Semicolon,

	EndOfFile,
};

struct Token
{
	TokenKind kind;
	TokenPosition start;
	TokenPosition end;
};

Result<std::vector<Token>> tokenize(const Source& source);