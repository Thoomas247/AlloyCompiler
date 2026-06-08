#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Internal tag for built-in interfaces.
// User-defined interfaces (future) are NOT added here — they are resolved as
// ResolvedDeclaration* from the SymbolTable instead.
enum class BuiltinInterface
{
	Number,   // u8 u16 u32 u64 i8 i16 i32 i64 f32 f64
	Iterable, // fixed arrays, slices &[T], dynamic heap arrays *[T]
};

// map interface name (as it appears in source) to enum value
const std::unordered_map<std::string_view, BuiltinInterface> s_BuiltinInterfaces =
{
	{"Number", BuiltinInterface::Number},
	{"Iterable", BuiltinInterface::Iterable},
};

// map interface to the set of built-in type names it covers
const std::unordered_map<BuiltinInterface, std::unordered_set<std::string_view>> s_BuiltinInterfaceMembers =
{
	{BuiltinInterface::Number, {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64"}},
};

// ---------------------------------------------------------------------------
// Built-in (prelude) types (§5). These behave like user `type` definitions but
// are provided by the compiler with no source declaration — the type system
// synthesises their `TypeInfo` directly. Generic built-ins (e.g. `Option<T>`)
// are monomorphised on use exactly like user generic types.
// ---------------------------------------------------------------------------

enum class BuiltinType
{
	Option,   // Option<T> = enum { Some: T; None; }
};

struct BuiltinTypeInfo
{
	std::string_view name;
	size_t arity;       // number of type parameters
	bool isEnum;        // true → enum (variants), false → struct (members)

	// For an enum: each variant's name and the index of the type-parameter that
	// is its payload (-1 = no payload). For a struct: member name + param index.
	struct Member
	{
		std::string_view name;
		int paramIndex;   // -1 = no payload / unit
	};
	std::vector<Member> members;

	BuiltinType tag;
};

// Registry of built-in types. Order is not significant.
inline const std::vector<BuiltinTypeInfo>& builtinTypes()
{
	static const std::vector<BuiltinTypeInfo> types = {
		{ "Option", 1, true, { { "Some", 0 }, { "None", -1 } }, BuiltinType::Option },
	};
	return types;
}

inline const BuiltinTypeInfo* findBuiltinType(std::string_view name)
{
	for (const auto& bt : builtinTypes())
		if (bt.name == name)
			return &bt;
	return nullptr;
}

struct BuiltinFunction
{
	std::string_view name;
	bool isSelf;                                // first param is self
	std::optional<BuiltinInterface> constraint; // nullopt = available on all types
};

const std::vector<BuiltinFunction> s_BuiltinFunctions =
{
	{"reinterpret", true, std::nullopt},                // reinterpret<T>(self s: &T)
	{"convert",     true, BuiltinInterface::Number},    // convert<T: Number>(self s: &T)
	{"length",      true, BuiltinInterface::Iterable},  // length(self s: &Iterable) -> u64
};
