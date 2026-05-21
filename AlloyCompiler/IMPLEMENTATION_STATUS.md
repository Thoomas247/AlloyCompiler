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

**A4. User-defined interface as a generic constraint.** (§5.2)
`resolveInterfaceToken` ([`resolver.cpp`](src/resolver/resolver.cpp)) resolves
`fn f<T: MyInterface>` constraints, but `TypeInfo::TypeParamData::constraint` is only
`std::optional<BuiltinInterface>` ([`types.hpp`](src/typechecker/types.hpp)). At a generic
call site, `satisfiesConstraint` in [`type_checker.cpp`](src/typechecker/type_checker.cpp)
therefore cannot enforce a user-interface bound. Extend `TypeParamData::constraint` to hold
a user interface (e.g. an interface `TypeId`) and check, at the call site, that the bound
concrete type's `implementedInterfaces` set contains it. Also handle `Iterable` as a
constraint structurally (it has no primitive member set).

**A5. Memory-model assignment rules.** (§4.2)
Not enforced: assigning to a `&`/`&var` requires `&` on the RHS; assigning to a `*`/`*var`
or `*[T]` requires `new` or `move`. Add these checks in the `VariableDefinitionStatement`
and `AssignmentStatement` handlers of [`type_checker.cpp`](src/typechecker/type_checker.cpp),
keyed on the declared type's `TypeInfo::Kind` (`Reference`/`RefMut` vs `Pointer`/`PtrMut`).

**A6. Mutability through indirections.** `&var`/`*var` vs `&`/`*` are interned but mutation
through an immutable reference/pointer is not rejected. `var`/`const` *local* mutability is
checked (see `ScopedVarMap::mutability`); extend that to indirections.

**A7. Bare interface value type.** An interface used as a type *without* an indirection
(`var x: Shape`) is unsized and should be a compile-time error. Currently interned and
silently allowed. Reject in `type_checker` / `type_interner`.

**A8. String literal typing.** Currently simplified to `u8`. Spec treats a string as an
array of integral numbers — type it as `&[u8]` or `*[u8]` consistently and update `match`
on string subjects.

**A9. `self` receiver indirection context.** (§5.2 item 3) The receiver qualifier
(`self: &T` vs `self: *var T` …) is not validated against the call-site value's mutability.

**A10. Unify the error model.** Replace `Log::error` soft-errors in `type_checker.cpp`
with `logErrorInRange` (or a new accumulating diagnostic API — see F1) so all type errors
are actually fatal and reported with source locations.

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

**D1. Enforce visibility.** `pub`/`exp`/private are parsed and stored
(`AST::Definition::Visibility`) but the resolver does **not** restrict cross-module access.
A private definition is currently importable. Enforce in `resolve()` when resolving names
through an import alias ([`resolver.cpp`](src/resolver/resolver.cpp), the import-alias
branch of `resolveIdentifier`).

**D2. Import validation.** Detect missing modules cleanly (currently a soft `Log::error`),
and detect/handle circular imports. `import a::b::c` ⇒ `a/b/c.alloy` mapping (§5.4) is
implemented in `source.cpp`/the driver.

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

**F3. Test harness.** Add a real test runner: positive cases (must compile clean) and
negative cases (must produce a specific diagnostic). Currently testing is manual via
`examples/`. The diagnostic rework (F1/F2) unblocks this — a batch run no longer aborts
on the first error and the exit code is meaningful.

**F4. Escape-sequence validation.** The tokenizer counts but does not validate
`\xHH` / `\u{...}` contents; the checker decodes char widths but does not validate scalar
values.

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

There is no automated test suite. To verify a change:

1. Build with `/t:Rebuild` (see gotcha on stale incremental builds).
2. Place `*.alloy` files under `AlloyCompiler/examples/` and run the binary from the
   `AlloyCompiler/` directory.
3. Diagnostics print to `stderr` at the end of the run; the process exits `0` (clean) or
   `1` (errors). No `__debugbreak()` dance is needed any more (F1/F2 done).
4. `examples/main.alloy` is the canonical smoke test; it must compile without errors and
   exit `0`. It exercises A1 (enum construction) and A2/A3 (`for`/`match` as expressions).

---

## 9. Suggested Order of Work

1. ~~**F1 + F2** — proper diagnostics and exit codes.~~ **DONE.**
2. ~~**A1** — enum variant construction.~~ **DONE.**
3. ~~**A2/A3** — loop/match as expressions.~~ **DONE.**
4. **F3** — test harness. Next highest-leverage item now that F1/F2 unblock it.
5. **A4–A10, D1–D2** — remaining front-end semantic gaps and visibility.
6. **B1–B4** — comptime/macro evaluation.
7. **C1–C5** — back-end. The largest effort; everything above is a prerequisite for a
   correct one.
