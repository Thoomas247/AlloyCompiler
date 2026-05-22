#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../source/source.hpp"
#include "../parser/AST.hpp"
#include "../resolver/resolver.hpp"
#include "../util/allocator.hpp"
#include "../util/result.hpp"

// A type synthesised by a type-position comptime expression — `type T = #...`
// (§3.4 / §6.1). The comptime interpreter produces it; the type interner turns
// it into a real interned struct/enum TypeId. Member/payload types are given by
// name (B3-5 supports primitive member types).
struct SynthType
{
	bool isEnum = false;
	std::string name;

	struct Member
	{
		std::string name;
		std::string typeName;   // a primitive type name; "void" = enum variant with no payload
	};

	std::vector<Member> members;
};

// Maps each type-position ComptimeExpression node to the type it synthesised.
using SynthTypeMap = std::unordered_map<const AST::ComptimeExpression*, SynthType>;

// B1/B3 (§6) — the compile-time evaluation pass.
//
// Walks the module: evaluates expression-position '#' constructs and rewrites
// each into its computed value (value-substitution); evaluates type-position
// '#' constructs (`type T = #...`) and records the synthesised type in
// `synthOut` for the interner to consume.
//
// The AST is mutated in place. Evaluation failures are reported as diagnostics;
// the pass always returns Status::Ok so the rest of the pipeline keeps running.
//
// Runs after resolve() and before intern().
Status comptimeEval(
	const Source& source,
	AST::Module& module,
	const ResolvedModule& resolved,
	Allocator& allocator,
	SynthTypeMap& synthOut);
