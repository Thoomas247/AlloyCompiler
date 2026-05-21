# Alloy Compiler — Implementation Status & Handoff

This document is a complete handoff for continuing work on the Alloy compiler. It records
what exists, how it is structured, what has been brought into spec conformance, the
non-obvious pitfalls of the codebase, and a prioritised list of everything still missing
to fully match the language specification.

---

## 1. Project Overview

**Alloy** is a statically-typed systems language. The authoritative specification is
[`AlloyCompiler/LANGUAGE_SPEC.md`](LANGUAGE_SPEC.md) — read it first; everything below assumes
familiarity with it.

The compiler is a C++ project. It currently implements a **front-end only**: source →
tokens → AST → resolved names → interned types → type-checked module. **There is no
back-end** — no IR, no codegen, no executable output. The pipeline stops at a
`TypedModule` value.

- Solution: [`AlloyCompiler.sln`](../AlloyCompiler.sln)
- Project: [`AlloyCompiler/AlloyCompiler.vcxproj`](AlloyCompiler.vcxproj)
- Language standard: C++20/23 (uses `std::print`, `std::println`, `using enum`,
  parenthesized aggregate initialisation, concepts).
- Toolchain: MSVC. Observed MSBuild path on this machine:
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`

### Build

```
MSBuild.exe AlloyCompiler.sln /t:Rebuild /p:Configuration=Debug /p:Platform=x64
```

Output binary: `x64\Debug\AlloyCompiler.exe`. Use `/t:Rebuild` when in doubt — incremental
builds have produced stale binaries during development (see §6).

### Run

The driver ([`src/AlloyCompiler.cpp`](src/AlloyCompiler.cpp)) compiles every `*.alloy`
file found **recursively** under `./examples` (relative to the working directory). Run it
from the `AlloyCompiler/` directory so `./examples` resolves:

```
cd AlloyCompiler
..\x64\Debug\AlloyCompiler.exe
```

---

## 2. Architecture — Pipeline & Files

The driver runs two loops. Loop 1 (per file): `tokenize → parse → declare`. Loop 2
(per module): `resolve → intern → typeCheck`. Modules are matched to imports by path.

| Stage | Files | Responsibility |
| --- | --- | --- |
| Driver | [`src/AlloyCompiler.cpp`](src/AlloyCompiler.cpp) | Orchestrates the pipeline over `./examples`. |
| Source | [`src/source/source.hpp`](src/source/source.hpp), [`source.cpp`](src/source/source.cpp) | Loads files; derives module name from path. |
| Tokenizer | [`src/tokenizer/tokenizer.hpp`](src/tokenizer/tokenizer.hpp), [`tokenizer.cpp`](src/tokenizer/tokenizer.cpp) | Lexing (§1). |
| Unicode | [`src/tokenizer/unicode_xid.hpp`](src/tokenizer/unicode_xid.hpp) | Generated UAX #31 `XID_Start`/`XID_Continue` tables (Unicode 16.0.0). |
| AST | [`src/parser/AST.hpp`](src/parser/AST.hpp) | All AST node definitions. |
| Parser | [`src/parser/parser.cpp`](src/parser/parser.cpp), [`parser.hpp`](src/parser/parser.hpp) | Recursive-descent parsing (§2). |
| Token iterator | [`src/parser/token_iterator.hpp`](src/parser/token_iterator.hpp), [`token_iterator.cpp`](src/parser/token_iterator.cpp) | Token cursor used by the parser. |
| Resolver | [`src/resolver/resolver.hpp`](src/resolver/resolver.hpp), [`resolver.cpp`](src/resolver/resolver.cpp) | `declare()` builds the module `SymbolTable`; `resolve()` binds every name reference, resolves interface markers/constraints. |
| Builtins | [`src/builtins/builtins.hpp`](src/builtins/builtins.hpp) | Built-in interfaces (`Number`, `Iterable`) and functions (`reinterpret`, `convert`, `length`). |
| Types | [`src/typechecker/types.hpp`](src/typechecker/types.hpp) | `TypeInfo`, `TypeId`, `InternedTypes`. |
| Type interner | [`src/typechecker/type_interner.hpp`](src/typechecker/type_interner.hpp), [`type_interner.cpp`](src/typechecker/type_interner.cpp) | Pass 1: assigns a canonical `TypeId` to every type; deduplicates structural types. |
| Type checker | [`src/typechecker/type_checker.hpp`](src/typechecker/type_checker.hpp), [`type_checker.cpp`](src/typechecker/type_checker.cpp) | Pass 2: expression type inference, assignment/call checks, overload resolution, generic inference, §5.2 interface verification. |
| Utilities | [`src/util/allocator.hpp`](src/util/allocator.hpp) (arena), [`pointers.hpp`](src/util/pointers.hpp) (`Required`/`Optional`), [`result.hpp`](src/util/result.hpp) (`Result`/`Status`), [`logger.hpp`](src/util/logger.hpp), [`overloaded.hpp`](src/util/overloaded.hpp) | Support code. |

### Key data structures

- **AST nodes** are arena-allocated ([`Allocator`](src/util/allocator.hpp)); `Required<T>`
  / `Optional<T>` are non-owning pointer wrappers; lists are singly-linked `ListNode<T>`.
- **`SymbolTable`** ([`resolver.hpp`](src/resolver/resolver.hpp)) — module-level declarations;
  functions may overload, everything else may not.
- **`ResolvedModule`** — maps each `IdentifierExpression`/token to its declaration(s);
  records resolved interfaces (`resolvedInterfaces`).
- **`InternedTypes`** ([`types.hpp`](src/typechecker/types.hpp)) — the `TypeInfo` table plus
  index maps (`astTypes`, `namedTypeIds`, `typeParamIds`, `interfaceIds`,
  `implementedInterfaces`).
- **`TypedModule`** — final output: `exprTypes`, `selectedOverloads`, `typeArgs`.

---

## 3. Current Status — What Is Implemented

The front-end has been refactored to conform to the spec. Completed work:

### Lexical (§1)
- Keywords: all of §1.4 including `pub exp true false interface macro`.
- `#` token; `0x`/`0b`/`0o` integer radixes; nested block comments; multi-byte char
  literals (1–8 bytes); UAX #31 Unicode identifiers via `unicode_xid.hpp` + a UTF-8
  decoder in `SourceIterator`.

### Syntax (§2)
- `pub`/`exp` visibility prefixes (absent ⇒ `Private`).
- `extern` return types; `type` interface markers (`type T : I1, I2 = ...`).
- `interface` and `macro` definitions; `comptime_expr` (`#if/#while/#match/#{}/#macroCall`)
  as primary expressions and base types; `macro_call`.
- `match` arms are general expressions (numbers/chars/strings/enum paths), with an
  internal catch-all `else` arm and an external post-`}` `else`.
- Unary `*` removed (Alloy has no dereference operator); `true`/`false` literals.

### Types & semantics (§3, §5)
- Char literals typed by byte width; `true`/`false` ⇒ `bool`.
- §3.3 rule 6 — anonymous-struct structural subset compatibility.
- Built-in methods `reinterpret<T>` / `convert<T>` / `length`; `Iterable` interface enabled.
- **Interfaces** — both roles:
  - *Dynamic dispatch*: an interface used behind `&`/`*` is an interface object
    (`TypeInfo::Kind::Interface`); a concrete type that declares the interface marker is
    assignable to it; method calls on an interface object are typed via the interface's
    function signatures.
  - *Generic constraint*: `fn f<T: I>(...)` resolves built-in and user interfaces.
  - *Default implementations*: an extension function whose `self` receiver is an interface
    is the default impl; a type-specific extension overrides it (member-call tie-break).
  - §5.2 verification pass: every interface-marked type must provide each interface
    function via a type-specific extension **or** a default.

### Out-of-scope-but-parsed (front-end only)
Comptime/macro constructs (§6) are **parsed into the AST and name-resolved**, but **not
evaluated**: comptime type expressions intern to `INVALID_TYPE_ID`, macro calls yield no
value type, macro bodies are not type-checked.

---

## 4. The Spec File

`LANGUAGE_SPEC.md` has been updated for interface semantics (§3.2 interface objects, §4.5
default implementations, §5.2 two roles + defaults + override + verification). The rest of
the spec predates this work and is otherwise unchanged.

---

## 5. Critical Implementation Notes / Gotchas

Read this section before touching the code — these cost real debugging time.

1. **Diagnostics now accumulate (F1 done).** `logErrorInRange` and the free `Log::error` /
   `Log::fatal` no longer `__debugbreak()` — they report a `Diagnostic` into the
   process-wide `DiagnosticEngine` ([`logger.hpp`](src/util/logger.hpp)). Compilation runs
   to completion and reports *every* error. The driver prints the accumulated list to
   `stderr` via `DiagnosticEngine::instance().printAll()` and exits `0` (clean) or `1`
   (errors). One error no longer aborts the batch.
2. **`Log::error` / `Log::fatal` are now real, counted errors.** They were "soft" before
   (printed, but did not set `hasError()`). They now feed the `DiagnosticEngine` and count
   toward the exit code. They carry **no source location** — prefer
   `logger.logErrorInRange(token, token, ...)` when a token is available; `Log::error` is
   acceptable only for genuinely location-less diagnostics (e.g. construct-level errors
   with no representative token).
3. **`ASSERT`** ([`logger.hpp`](src/util/logger.hpp)) still expands to `__debugbreak()` —
   it is for internal compiler-invariant violations, not user errors. That is intentional.
6. **Incremental builds can be stale.** During this work an incremental build silently
   kept an old `type_checker.obj`. Prefer `/t:Rebuild` when verifying behaviour changes.
7. **Arena allocation + parenthesized aggregate init.** `Allocator::allocate<T>(args...)`
   does `new T(args...)`. For aggregate structs this relies on C++20 parenthesized
   aggregate initialisation. When you add a field to an AST node, every `allocate<Node>(...)`
   call site must be updated to match the new field order.
8. **`InternedTypes::table` must not reallocate mid-check.** `typeCheck` reserves headroom
   (`interned.table.reserve(... + 4096)`) because live `const TypeInfo&` references are held
   across `internType()` pushes. If you intern many new types during checking, revisit this.
9. **Source files use TAB indentation.**
10. **No test harness exists.** Verification is manual: drop `*.alloy` files in `examples/`
    and run. A batch now compiles every module even when one has errors (see gotcha 1),
    so multiple positive/negative cases can coexist in `examples/`.

---

## 6. Missing — Next Steps to Match the Spec

Prioritised. Each item lists the spec section, the files involved, and concrete guidance.

### A. Semantic gaps in the existing front-end (tractable now, no new subsystems)

**A1. Enum variant construction — DONE.** (§3.2, §4.3)
`resolveEnumVariant` in [`type_checker.cpp`](src/typechecker/type_checker.cpp) classifies a
qualified identifier (`Enum::Variant`) whose first segment resolves to an enum
`TypeDefinition`. The function-call handler types `Enum::Variant(payload)` as the enum's
Named `TypeId` and checks the argument against the variant's payload type (re-checking the
argument with the payload as `contextType` so untyped literals concretise); the identifier
handler types a payload-less variant (`Code::Success`) as the enum type. Bad variant
names, payload mismatches, and wrong arity are reported. The struct-initializer handler
now also derives member contexts from `contextType` when there is no explicit named type,
so an anonymous-struct payload typechecks against the variant's declared struct.

**A2/A3. Loop / match as value-producing expressions — DONE.** (§4.3)
`CheckState::breakCollector` (a `std::vector<TypeId>*`) collects the value types of
`break value;` statements targeting the innermost loop/match; the `for`/`while`/`match`
handlers swap it around their body + else block, then `unifyBreakTypes` computes the
construct's result type. `CheckState::constructInStatementPosition` is set by
`checkStatement` when a construct is reached in statement position; a trailing `else` on a
statement-position loop/match is a compile-time error, and an expression-position
construct *with* an `else` that yields no `break` value is also an error. Note: full
all-paths-yield-a-value control-flow analysis is **not** implemented — only the gross
"no break value at all" case is caught. Break-type mismatch is unified leniently (no
error) to avoid false positives.

**A4. User-defined interface as a generic constraint — DONE.** (§5.2)
`TypeInfo::TypeParamData` now carries `interfaceConstraint` (`std::optional<TypeId>`, the
user interface's Interface `TypeId`) alongside the built-in `constraint`
([`types.hpp`](src/typechecker/types.hpp)). The interner fills it from
`resolved.resolvedInterfaces` ([`type_interner.cpp`](src/typechecker/type_interner.cpp)).
At a generic call site, `tryGenericOverload` checks `satisfiesInterfaceConstraint` — the
bound concrete type's `implementedInterfaces` set must contain the interface. `Iterable`
is now satisfied structurally (any `Array`/`Slice`). NOTE: a generic call whose constraint
fails simply selects no overload and yields `INVALID_TYPE_ID` — there is still no
"no matching overload" diagnostic (blocked by the absence of function-type assignability;
`examples/main.alloy`'s `call(...)` already relies on that silence).

**A5. Memory-model assignment rules — DONE.** (§4.2)
`checkIndirectionAssignment` in [`type_checker.cpp`](src/typechecker/type_checker.cpp)
enforces, against the *unstripped* declared `TypeInfo::Kind`: a `Reference`/`RefMut`
binding requires a `&` (address-of) RHS; a `Pointer`/`PtrMut` binding requires a `new`/
`move` RHS. Checked in the `VariableDefinitionStatement` handler and, for plain `=`, in
the `AssignmentStatement` handler (target must be an identifier resolving to a declared
binding). Note: per the literal §4.2 wording the RHS must *syntactically* be `&`/`new`/
`move` — copying an existing pointer/reference variable is rejected. If that proves too
strict it is a spec question (§7), not a checker bug.

**A6. Mutability through indirections — DONE.** The `AssignmentStatement` handler in
[`type_checker.cpp`](src/typechecker/type_checker.cpp), for any target that is a `.field`
/ `[index]` access (a mutation *through* something), walks to the root identifier
(`rootIdentifier`) and reports an error when that root is an immutable reference/pointer
(`Reference`/`Pointer` kind) or a `const` binding. `&var`/`*var` (`RefMut`/`PtrMut`) and
`var` bindings are permitted. This also closes a D3 gap — `const v; v.x = …;` (an access
chain, not a bare identifier) is now caught. Limitation: only the *root* of the chain is
checked; an immutable reference *field* mid-chain (`r.immutRefField.x = …`) is not yet
rejected.

*Reference/pointer expression typing — DONE (prerequisite for A6).* The `&` operator now
yields `&T` and `new`/`move` yield `*T` (see the unary handler in
[`type_checker.cpp`](src/typechecker/type_checker.cpp)), so `var r = &x;` correctly infers
`r : &T` and `var p = new e;` infers `p : *T`. Using such a binding as a value
transparently dereferences to `T` (§4.2) — the identifier handler strips one indirection
for both annotated and inferred bindings. Comparison sites that may receive an
indirection-typed expression (`var`-init mismatch, assignment, `return`, overload
exact-match) strip before calling `isAssignable`. `stripIndirection` is sentinel-safe.
Note: a few non-load-bearing sites (struct-member initialisers, array-literal elements,
generic `unifyParam`) do **not** yet strip — fine today since nothing exercises an
indirection expression there, but revisit if it does.

**A7. Bare interface value type — DONE.** An interface used as a type without an
indirection (`var x: Shape`, `fn f(s: Shape)`) is now a compile-time error, reported in
`internASTType` ([`type_interner.cpp`](src/typechecker/type_interner.cpp)) in the
`Modifier::None` branch when the inner type is `Kind::Interface`. `&I`/`*I` forms are
unaffected. (Because this is an interner error, the F2 loop-2 guard skips `typeCheck` for
that module — so a file with a bare-interface error suppresses its *type* errors. Keep
positive/negative cases in separate files when testing.)

**A8. String literal typing — DONE.** A string literal is now typed `&[u8]` (the literal
handler in [`type_checker.cpp`](src/typechecker/type_checker.cpp) interns
`Reference(Slice(u8))`) rather than the old `u8` simplification. Used as a value it
transparently derefs to `[u8]`. `match` on a string subject still falls through the
enum path leniently (string-pattern matching is not specially typed).

**A9. `self` receiver indirection context — DONE.** (§5.2 item 3) When a member call's
selected overload has a `self` parameter of *mutable* indirection kind (`&var`/`*var` —
`RefMut`/`PtrMut`), the type checker now rejects the call if the receiver is provably
immutable — a `const` binding or a value behind an immutable `&`/`*` indirection
(`receiverIsImmutable` in [`type_checker.cpp`](src/typechecker/type_checker.cpp), reusing
the A6 root-identifier walk). Immutable-`self` methods accept any receiver.

**A10. Unify the error model — DONE.** F1 already made `Log::error` a counted, fatal
diagnostic. The remaining location-less type errors in `type_checker.cpp` (ambiguous call
×2, assignment mismatch, return mismatch) now use `logErrorInRange` with a token position
when one is recoverable (the callee token, or the target/return expression's root
identifier); they fall back to location-less `Log::error` only for token-less expressions
(e.g. `return 5;`).

### B. Compile-time evaluation & macros (§6) — currently front-end only

**B1. Comptime interpreter.** Evaluate `#if/#while/#match/#{}` at compile time over a
value-substitution model (§6.1): execute the construct, replace the AST node with the
resulting literal/struct/type. New subsystem; consumes the `ComptimeExpression` AST nodes
already produced by the parser.

**B2. Macro expansion.** Execute `macro` bodies; replace `#macroCall(...)` sites with the
generated AST / type node. Macro return types are inferred from the generated node (§6.3).

**B3. `#Type` reflection.** (§3.4) The compile-time-only `#Type` representation with
reflection/mutation methods (`@members`, etc.). Needed for `type T = #readTypeFromJson(...)`.

**B4. Comptime sandboxing.** (§6.2) Pointer barrier (no `&`/`*` may escape comptime into
runtime), filesystem sandbox to the project root, prohibition on calling `extern` from
comptime.

Until B1–B4 exist, keep the current behaviour: comptime types ⇒ `INVALID_TYPE_ID`, macro
calls ⇒ no value type.

### C. Back-end / code generation — entirely absent (largest effort)

The pipeline ends at `TypedModule`. To produce running programs:

**C1. Lowering / IR.** Choose an IR (custom, or target LLVM). Lower the typed AST.

**C2. Memory model codegen.** (§4.2) Managed heap pointers (`new`/`move` semantics),
unmanaged references, slices as `{ptr, u64 len}` fat pointers, dynamically-sized arrays
`*[T]` with the length metadata stored in a prefix block immediately *before* the data
pointer (C-FFI-compatible layout).

**C3. Interface vtables.** Dynamic dispatch is *typed* but not *generated*. Each
concrete-type/interface pair needs a vtable; `&I`/`*I` values are `{data ptr, vtable ptr}`
fat pointers. Default implementations populate vtable slots not overridden by a
type-specific extension.

**C4. Generic monomorphisation.** Instantiate generic functions per concrete type set
(`TypedModule::typeArgs` already records the bindings per call site).

**C5. Extension-function & overload lowering**, `extern` FFI linkage, object/executable
emission.

### D. Module system & visibility

**D1. Enforce visibility — DONE.** `resolveIdentifier`'s Track A (import-alias branch,
[`resolver.cpp`](src/resolver/resolver.cpp)) now filters out `Private` declarations when
resolving `alias::name` through an imported module; only `pub`/`exp` declarations cross
the boundary. An all-private match reports "`X` is private to module `M`". Same-module
lookup is unaffected (a module still sees its own privates).

**D2. Import validation — mostly addressed.** Missing imported modules are reported by the
driver's `Log::error` ([`AlloyCompiler.cpp`](src/AlloyCompiler.cpp)), which since F1 is a
counted, exit-code-affecting error — no longer "soft". Circular imports are *benign* in
the current architecture: `declare()` runs per-module independently and `resolve()`
consumes already-built `SymbolTable`s, so a cycle cannot cause infinite recursion. A
dedicated cycle *diagnostic* is still absent but not load-bearing. `import a::b::c` ⇒
`a/b/c.alloy` mapping (§5.4) is implemented in `source.cpp`/the driver.

### E. Standard library

No stdlib modules exist. `Number`/`Iterable` are built-in interface *tags* only;
`length`/`reinterpret`/`convert` are typed but have no implementation. A real `String`,
collections, etc. (§5) are all missing and depend on the back-end (C).

### F. Tooling, diagnostics, tests

**F1. Diagnostic system — DONE.** [`logger.hpp`](src/util/logger.hpp) now defines a
`Diagnostic` struct and a process-wide `DiagnosticEngine` singleton. `logErrorInRange`,
`Log::error`, and `Log::fatal` report into it instead of `__debugbreak()`-ing. The
compiler reports *all* errors. (`Log::info` still prints inline.)

**F2. Driver exit codes — DONE.** [`AlloyCompiler.cpp`](src/AlloyCompiler.cpp) no longer
`__debugbreak()`s. It prints `DiagnosticEngine::instance().printAll()` and returns `0`
(clean) / `1` (errors). Loop-2 stages are guarded: a failed `resolve`/`intern` skips the
dependent stages for that module but other modules still compile.

**F3. Test harness — DONE.** The driver ([`AlloyCompiler.cpp`](src/AlloyCompiler.cpp))
takes a `--test` flag: `AlloyCompiler.exe --test` compiles every `*.alloy` under `./tests`
**in isolation** (each as a standalone module, no cross-file imports), resetting the
`DiagnosticEngine` between files, and checks each against `//@expect-error <substring>`
annotations embedded as comments. A file with no annotations is a positive test (must
compile clean). A test passes when expected and actual error counts match and every
expected substring matches a distinct diagnostic. Prints `PASS`/`FAIL` per file and
`N passed, M failed`; exit `0` iff all pass. Without `--test` the driver compiles
`./examples` as before. Corpus in `tests/` is **29 cases** (all passing) covering: char
width, numeric widening/narrowing, cross-sign-class, undefined name, duplicate definition,
missing field, interface satisfied/unsatisfied/built-in marker, enum match + bad variant +
payload mismatch, match-capture-on-non-enum, loop/while/match `else` placement, ref/ptr
assignment forms, mutate-through-immutable, `self`-receiver mutability, struct structural
compatibility, generic `Number` constraint, string literal, extension call, arrays, escape
sequences. Add a new `tests/*.alloy` per future feature/fix.

**F4. Escape-sequence validation — DONE.** `validateEscapeSequence` in
[`tokenizer.cpp`](src/tokenizer/tokenizer.cpp) validates every escape inside a string/char
literal (§1.6): simple escapes (`\n \r \t \0 \\ \' \"`); `\xHH` requires exactly two hex
digits; `\u{…}` requires `{` … `}` with ≥1 hex digit and a valid Unicode scalar value
(≤ `0x10FFFF`, no surrogates). Anything else → "Invalid escape sequence". Diagnostics carry
the escape's source span.

---

## 7. Spec Ambiguities to Resolve

These should be clarified with the language designer before the affected code is finalised:

1. **`if`/`while`/`for` branch grammar.** §2.1 EBNF makes the branch a `statement`, but the
   §6 examples (`var a = #if (#isDevelopment()) 50 else 100;`) use a bare expression with no
   `;`. Either the grammar needs an "expression branch" form, or the examples are wrong. The
   parser currently follows the EBNF (branch = `statement`), so `#if (c) 50 else 100` does
   not parse — only block/`;`-terminated branches do.
2. **`extern` return type.** §5.3 prose says extern declarations "mandate concrete arrow
   return types", but the EBNF and the `examples/main.alloy` `extern printf(...);` make it
   optional. The parser follows the EBNF (optional).
3. **Capture on payload-less enum variants.** §4.3 says a capture is valid only on variants
   "containing attached data payloads". The checker currently only errors when the subject
   is provably a non-enum; a capture on a known payload-less variant is not yet rejected
   (deliberately lenient to avoid false positives while A1 is unimplemented).

---

## 8. Verification Methodology

To verify a change:

1. Build with `/t:Rebuild` (see gotcha on stale incremental builds).
2. **Run the test suite:** `AlloyCompiler.exe --test` from the `AlloyCompiler/` directory.
   It compiles every `tests/*.alloy` in isolation and checks `//@expect-error` annotations;
   exit `0` iff all pass. Add a `tests/*.alloy` for each new feature/fix (positive case =
   no annotations; negative case = one `//@expect-error <substring>` per expected error).
3. **Smoke test:** run the binary with no arguments to compile `./examples`. Diagnostics
   print to `stderr`; the process exits `0` (clean) or `1` (errors). `examples/main.alloy`
   must compile clean and exit `0`.

---

## 9. Suggested Order of Work

1. ~~**F1 + F2** — proper diagnostics and exit codes.~~ **DONE.**
2. ~~**A1** — enum variant construction.~~ **DONE.**
3. ~~**A2/A3** — loop/match as expressions.~~ **DONE.**
4. ~~**A5** — memory-model assignment rules.~~ **DONE.**
5. ~~**A7** — bare interface value type.~~ **DONE.**
6. ~~**A6** — mutability through indirections (+ reference/pointer expression typing).~~ **DONE.**
7. ~~**A4** — user-defined interface as a generic constraint.~~ **DONE.**
8. ~~**A8** — string literal typing.~~ **DONE.**
9. ~~**D1/D2** — cross-module visibility + import validation.~~ **DONE.**
10. ~~**A9/A10** — `self`-receiver indirection context; located error model.~~ **DONE.**
11. ~~**F3** — test harness (`--test`, `tests/` corpus).~~ **DONE.**
12. ~~**F4** — escape-sequence validation.~~ **DONE.**
13. **B1–B4** — comptime/macro evaluation.
14. **C1–C5** — back-end. The largest effort; everything above is a prerequisite for a
    correct one.
