#pragma once

#include <string>

#include "../parser/AST.hpp"
#include "../resolver/resolver.hpp"
#include "../source/source.hpp"
#include "../typechecker/type_checker.hpp"
#include "../typechecker/types.hpp"
#include "../util/result.hpp"

/**
 * Back-end (§6.C) — LLVM code generation.
 *
 * Lowers a fully type-checked Alloy module to an LLVM IR module and emits both
 * a textual IR listing (<outBasePath>.ll) and a native object file
 * (<outBasePath>.obj). The object can be linked into an executable.
 *
 * This is the "core lowering" pass: primitives, arithmetic, control flow,
 * structs, enums, fixed arrays, slices, pointers/references, function calls,
 * extension functions and extern FFI. Generics (monomorphisation), interface
 * vtables and closures are not lowered yet — a construct that is not supported
 * is reported as a diagnostic and skipped rather than aborting compilation.
 *
 * Requires the output of intern() + typeCheck(). All inputs must outlive the call.
 */
struct CodegenOptions
{
	// Debug mode emits runtime bounds checks on every `[]` access.
	// Release mode skips them (caller is responsible for staying in bounds).
	bool debug = true;
};

Status codegen(
	const Source& source,
	const AST::Module& module,
	const ResolvedModule& resolved,
	const InternedTypes& interned,
	const TypedModule& typed,
	const SymbolTable& moduleSymbols,
	const std::string& outBasePath,
	const CodegenOptions& options = {});
