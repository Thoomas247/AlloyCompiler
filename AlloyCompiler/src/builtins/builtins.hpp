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
	//Iterable, // array types
};

// map interface name (as it appears in source) to enum value
const std::unordered_map<std::string_view, BuiltinInterface> s_BuiltinInterfaces =
{
	{"Number", BuiltinInterface::Number},
	//{"Iterable", BuiltinInterface::Iterable},
};

// map interface to the set of built-in type names it covers
const std::unordered_map<BuiltinInterface, std::unordered_set<std::string_view>> s_BuiltinInterfaceMembers =
{
	{BuiltinInterface::Number, {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64"}},
};

struct BuiltinFunction
{
	std::string_view name;
	bool isSelf;                                // first param is self
	std::optional<BuiltinInterface> constraint; // nullopt = available on all types
};

const std::vector<BuiltinFunction> s_BuiltinFunctions =
{
	{"reinterpret", true, std::nullopt},              // reinterpret<T>(self s: &T)
	{"convert",     true, BuiltinInterface::Number},  // convert<T: Number>(self s: &T)
	//{"length", true, BuiltinInterface::Iterable},  // length<T: Iterable>(self s: &T)
};
