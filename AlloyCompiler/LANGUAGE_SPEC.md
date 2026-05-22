# Alloy Language — Formal Language Specification

## Table of Contents

1. [Lexical Grammar](https://www.google.com/search?q=%231-lexical-grammar)
2. [Syntactic Grammar](https://www.google.com/search?q=%232-syntactic-grammar)
3. [Type System & Static Semantics](https://www.google.com/search?q=%233-type-system--static-semantics)
4. [Execution Semantics](https://www.google.com/search?q=%234-execution-semantics)
5. [Standard Library & Primitives](https://www.google.com/search?q=%235-standard-library--primitives)
6. [Compile-Time Evaluation & Macros](https://www.google.com/search?q=%236-compile-time-evaluation--macros)

---

## 1. Lexical Grammar

### 1.1 Character Set

Source files are encoded in **UTF-8**. Identifiers and literal strings fully support Unicode character mappings.

### 1.2 Whitespace & Comments

```
whitespace     ::= ' ' | '\t' | '\n' | '\r' | '\v' | '\f'
line_comment   ::= '//' <any char except '\n'>* '\n'?
block_comment  ::= '/*' ( block_comment | <any sequence not containing '/*' or '*/'> )* '*/'

```

Whitespace and comments are not significant to the grammar and are ignored between tokens. Block comments nest arbitrarily. The compiler tracks the nesting depth, and a block comment is only considered terminated when all opened comment blocks are closed. An unterminated block comment is a compile-time error.

### 1.3 Identifiers

```
identifier     ::= ( letter | '_' ) ( letter | digit | '_' )*
letter         ::= [a-zA-Z] | <Valid UTF-8 identifier characters matching UAX #31>
digit          ::= [0-9]

```

Any identifier that matches a reserved keyword (§1.4) is treated as that keyword and may not be used as a user-defined name.

### 1.4 Keywords

Reserved — cannot be used as identifiers:

```
import   as       extern   type     enum     struct
const    var      fn       if       else     while
for      match    break    return   new      move    self
pub      exp      true     false    interface macro

```

### 1.5 Operators & Punctuation

Complete list of symbolic tokens:

| Category | Symbols |
| --- | --- |
| Arithmetic | `+` `-` `*` `/` `%` |
| Compound assign | `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` ` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` |
| Bitwise | `&` |
| Assignment | `=` |
| Arrow | `->` |
| Path separator | `::` |
| Member access | `.` |
| Spread / variadic | `...` |
| Type annotation | `:` |
| Separator | `,` `;` |
| Delimiters | `(` `)` `{` `}` `[` `]` |
| Comptime / Macro prefix | `#` |

When multiple symbolic tokens share a common prefix, the longest matching token is always chosen.

### 1.6 Literals

```
integer_literal  ::= decimal_literal | hex_literal | binary_literal | octal_literal
decimal_literal  ::= [0-9]+
hex_literal      ::= '0x' [0-9a-fA-F]+
binary_literal   ::= '0b' [01]+
octal_literal    ::= '0o' [0-7]+

float_literal    ::= [0-9]+ '.' [0-9]*
string_literal   ::= '"' ( escape_seq | <any char except '"'> )* '"'
char_literal     ::= '\'' ( escape_seq | <any char except '\''> )+ '\''
escape_seq       ::= '\\' [nrt0\\'"xXuU] | '\\x' digit{2} | '\\u{' digit+ '}'

```

Integer literals support standard **decimal**, **hexadecimal** (`0x`), **binary** (`0b`), and **octal** (`0o`) radix representations.

#### Escape Sequences

The compiler recognizes standard escape sequences within string and character literals:

* `\n` (line feed), `\r` (carriage return), `\t` (tab), `\0` (null byte)
* `\\` (backslash), `\"` (double quote), `\'` (single quote)
* `\xHH` (arbitrary 1-byte hex value)
* `\u{HHHH}` (arbitrary multi-byte Unicode scalar value)

#### Character Literal Typing

The specific underlying primitive type of a character literal dynamically matches its size in bytes:

* Single quotes can bind a single character or a sequence of sequential characters up to 8 bytes long.
* A standard 1-byte ASCII character literal (e.g., `'a'`) evaluates to a `u8`.
* A multi-character packed constant sequence (e.g., `'abcdefgh'`) spans 8 bytes and evaluates to a `u64`.
* Variable-width Unicode characters evaluate to the smallest unsigned primitive integer width capable of holding their entire byte representation (typically `u32` for standard individual scalar sequences).

---

## 2. Syntactic Grammar

### 2.1 Full EBNF

```ebnf
(* Top level *)
module          = { import_decl } { definition } EOF ;

import_decl     = "import" ident { "::" ident } [ "as" ident ] ";" ;

definition      = [ "pub" | "exp" ] ( type_def | fn_def | extern_def | interface_def | macro_def ) ;
type_def        = "type" ident [ ":" ident { "," ident } ] "=" base_type ";" ;
interface_def   = "interface" ident "{" { interface_fn } "}" ;
interface_fn    = "fn" ident "(" [ param { "," param } ] ")" [ "->" type ] ";" ;
fn_def          = "fn" ident [ "<" type_param { "," type_param } ">" ] function ;
macro_def       = "macro" ident "(" [ param { "," param } ] ")" stmt_block ;
extern_def      = "extern" ident "(" extern_params ")" [ "->" type ] ";" ;
extern_params   = /* empty */
                | param { "," param } [ "," "..." ]
                | "..." ;

type_param      = ident [ ":" ident ] ;

(* Functions *)
function        = "(" [ param { "," param } ] ")" [ "->" type ] stmt_block ;
param           = [ "self" ] ident ":" type ;

(* Types *)
type            = type_modifier ( base_type | type ) ;
type_modifier   = /* none */ | "*" | "*" "var" | "&" | "&" "var" ;

base_type       = named_type
                | struct_type
                | enum_type
                | array_type
                | fn_type
                | comptime_expr ;

named_type      = ident { "::" ident } ;
struct_type     = "struct" "{" { ident ":" type ";" } "}" ;
enum_type       = "enum" "{" { ident [ ":" type ] ";" } "}" ;
array_type      = "[" type [ ";" integer_literal ] "]" | "[" type "]" ;
fn_type         = "(" [ type { "," type } ] ")" [ "->" type ] ;

(* Statements *)
statement       = var_def
                | stmt_block
                | if_expr
                | for_expr
                | while_expr
                | match_expr
                | break_stmt
                | return_stmt
                | expr_stmt ;

var_def         = ( "var" | "const" ) ident [ ":" type ] "=" expression [ ";" ] ;
stmt_block      = "{" { statement } "}" ;
break_stmt      = "break" [ expression ] ( ";" | <block-like operand> ) ;
return_stmt     = "return" [ expression ] ";" ;
expr_stmt       = expression ( assign_op expression ";" | ";" ) ;

assign_op       = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                | "<<=" | ">>=" | "&=" | "|=" | "^=" ;

(* Expressions *)
expression      = unary_expr [ binary_op expression ] ;

unary_expr      = unary_op unary_expr | postfix_expr ;
unary_op        = "~" | "!" | "&" | "new" | "move" ;

postfix_expr    = primary_expr { postfix_suffix } ;
postfix_suffix  = "(" [ expr { "," expr } ] ")"                            (* call *)
                | "<" type { "," type } ">" "(" [ expr { "," expr } ] ")"  (* generic call *)
                | "." ident                                                  (* member access *)
                | "[" expression "]" ;                                       (* array index *)

primary_expr    = literal
                | identifier_expr
                | named_struct_init
                | anon_struct_init
                | "(" expression ")"
                | array_fill
                | array_literal
                | if_expr
                | for_expr
                | while_expr
                | match_expr
                | lambda_expr
                | comptime_expr ;

identifier_expr   = ident { "::" ident } ;
literal           = integer_literal | float_literal | string_literal | char_literal | "true" | "false" ;

array_literal     = "[" expression { "," expression } "]" ;
array_fill        = "[" expression ";" integer_literal "]" ;

named_struct_init = ident "{" [ member_init { "," member_init } ] "}" ;
anon_struct_init  = "{" [ member_init { "," member_init } ] "}" ;
member_init       = "." ident "=" expression ;

lambda_expr       = [ "|" [ capture { "," capture } ] "|" ] function ;
capture           = type_modifier ident ;

if_expr     = "if" "(" expression ")" [ "|" capture "|" ]
              statement [ "else" statement ] ;

for_expr    = "for" "(" expression { "," expression } ")"
              [ "|" capture { "," capture } "|" ]
              statement [ "else" statement ] ;

while_expr  = "while" "(" expression ")" statement [ "else" statement ] ;

match_expr  = "match" "(" expression ")" "{"
              { ( expression | "else" ) [ "|" capture "|" ] statement }
              "}" [ "else" statement ] ;

comptime_expr   = "#" postfix_expr ;

```

**Comptime prefix.** The `#` token marks any value-yielding expression for
compile-time evaluation (§6). It binds as a postfix expression: `#f(x)` is
`#(f(x))`, but `#a + b` is `(#a) + b` — parenthesise to mark a whole expression
(`#(a + b)`).

**`break` operand termination.** When a `break`'s operand is a block-like
expression (`if` / `while` / `for` / `match`), that operand is self-terminating
and the trailing `;` is omitted: `break if (c) break a; else break b;`.

### 2.2 Operator Precedence & Associativity

Higher number = tighter binding. All binary operators are **left-associative**.

| Precedence | Operators |
| --- | --- |
| 100 (tightest) | `*`  `/`  `%` |
| 90 | `+`  `-` |
| 80 | `<<`  `>>` |
| 70 | `<`  `<=`  `>`  `>=` |
| 60 | `==`  `!=` |
| 50 | `&` (bitwise AND) |
| 40 | `^` |
| 30 | ` |
| 20 | `&&` |
| 10 (loosest) | ` |

Unary prefix operators (`~`, `!`, `&`, `new`, `move`) bind tighter than all binary operators and are **right-associative**. Postfix operators (call `()`, generic call `<>()`, member `.`, index `[]`) bind tighter than all unary prefix operators.

---

## 3. Type System & Static Semantics

### 3.1 Primitive Types

| Name | Width | Signed | Float |
| --- | --- | --- | --- |
| `u8` | 1 byte | No | No |
| `u16` | 2 bytes | No | No |
| `u32` | 4 bytes | No | No |
| `u64` | 8 bytes | No | No |
| `i8` | 1 byte | Yes | No |
| `i16` | 2 bytes | Yes | No |
| `i32` | 4 bytes | Yes | No |
| `i64` | 8 bytes | Yes | No |
| `f32` | 4 bytes | — | Yes |
| `f64` | 8 bytes | — | Yes |
| `bool` | 1 byte | No | No |

Boolean literals are explicitly reserved via the language keywords `true` and `false`. Character literal primitive types scale automatically to fit their byte layout width (§1.6).

### 3.2 Composite & Derived Types

| Syntax | Kind | Notes |
| --- | --- | --- |
| `struct { f: T; ... }` | Struct | Declaration-order members |
| `enum { A; B: T; ... }` | Enum (sum type) | Variants with optional payload |
| `[T; N]` (N > 0) | Fixed array | Size is a compile-time integer literal allocated on stack/inline |
| `&[T]` | Slice | Unmanaged view (fat pointer containing a raw pointer + a `u64` length) |
| `*[T]` | Dynamically Sized Array | Managed heap pointer to runtime-sized memory layout handle |
| `(T1, T2) -> R` | Function type | First-class function value |
| `type X = BaseType` | Named alias | Nominally distinct; transparently assignable to its underlying type |
| `*T` | Immutable pointer | Managed heap-allocated instance |
| `*var T` | Mutable pointer | Managed mutable heap-allocated instance |
| `&T` | Immutable reference | Unmanaged borrowed reference |
| `&var T` | Mutable reference | Unmanaged mutable borrowed reference |
| `&I` / `*I` (`I` an interface) | Interface object | Dynamic-dispatch fat pointer: a data pointer plus a vtable pointer (§5.2). |

An **interface used as a type** (only behind an indirection — `&I`, `&var I`, `*I`, `*var I`) is an *interface object*: a fat pointer carrying the address of a value together with the vtable of the concrete type's interface implementation. A value of concrete type `T` is implicitly convertible to an interface object of `I` if and only if `T` declares `I` among its interface markers (`type T : I = ...`). The reverse conversion (interface object down to a concrete type) requires an explicit cast.

### 3.3 Type Compatibility & Coercion Rules

1. **Identity** — identical types are always compatible.
2. **Untyped integer literal** — compatible with any primitive type, or any named alias whose underlying chain reaches a primitive. Regardless of radix format (`0x`, `0b`, `0o`, decimal), it resolves to `i32` when no contextual type is available.
3. **Untyped float literal** — compatible with any float primitive (`f32`, `f64`). Resolves to `f32` when no contextual type is available.
4. **Named alias transparency** — a named type is compatible with anything its underlying type is compatible with.
5. **Numeric widening** — a numeric primitive is implicitly compatible with a wider primitive of the **same sign class**: unsigned→unsigned, signed→signed, float→float. Cross-class conversions require explicit `convert<T>()`.
6. **Chained Nominal-Structural struct compatibility** — Two named struct types are fundamentally distinct (**nominal compatibility**). However, type-chaining logic allows implicit casting from more specific (narrower) to more general types layout-wise. A target expecting an anonymous layout (e.g., `struct { a: u8; b: f32; }`) will accept *any* value—named or anonymous—whose internal shape structurally provides a matching set of required fields. Implicit casting up the chain to a more "general" layout is legal at any recursive depth level. Conversely, converting from a general, structurally loose layout down to a narrower/more specific named type requires an explicit cast.

### 3.4 Compile-Time Special Types (`#Type`)

`#Type` is a dedicated system representation primitive available **exclusively during compile-time evaluation**. A `#Type` value is a first-class, mutable description of a type — a struct or enum layout, a primitive, or an interface — that compile-time code may inspect and reconstruct.

* `#Type` maps directly to abstract structures, built-in primitives, or structural layouts.
* It exposes programmable compile-time methods enabling reflection and mutation (see below).
* Any attempt to retain or use `#Type` inside a standard runtime declaration or variable state triggers an immediate compile-time error.

#### Obtaining a `#Type`

* **`#T`** — prefixing a type name with the comptime token `#` yields the `#Type` reflecting `T` (e.g. `#u32`, `#Packet`). `#T.member_names()` reflects on `T` directly.
* **`#type_of(expr)`** — a built-in macro returning the `#Type` of the value `expr`'s type.
* **`#struct_type()` / `#enum_type()`** — built-in macros returning a fresh, empty struct / enum `#Type`, for synthesising a type from scratch.

#### `#Type` methods

All are evaluated at compile time and called with dot syntax on a `#Type` value.

| Method | Signature | Semantics |
| --- | --- | --- |
| `is_struct` | `() -> bool` | True iff the type is a struct. |
| `is_enum` | `() -> bool` | True iff the type is an enum. |
| `is_primitive` | `() -> bool` | True iff the type is a built-in primitive. |
| `is_interface` | `() -> bool` | True iff the type is an interface. |
| `implements_interface` | `(other: #Type) -> bool` | True iff this type declares `other` (an interface) among its interface markers. |
| `name` | `() -> &[u8]` | The type's declared name. |
| `equals` | `(other: #Type) -> bool` | True iff the two `#Type`s denote the same type. |
| `add_member` | `(name: &[u8], type: #Type)` | Appends a member (struct field or enum variant) of the given name and type. |
| `remove_member` | `(name: &[u8])` | Removes the member with the given name. |
| `member_names` | `() -> &[&[u8]]` | Member names, in declaration order. |
| `member_types` | `() -> &[#Type]` | Member types, parallel to `member_names()`. |

A `#Type` is a **value**: `add_member` / `remove_member` mutate the `#Type` value in hand, not the original type it was reflected from. The final `#Type` value, assigned through `type T = <#Type-valued comptime expression>`, becomes the synthesised type `T`.

---

## 4. Execution Semantics

### 4.1 Evaluation Order

**Eager (strict) evaluation.** All sub-expressions are fully evaluated before their result is used. Function arguments are evaluated **left-to-right** before the call.

### 4.2 Memory Model & Pointer Assignment Syntax

Alloy maps memory mechanics transparently using direct, predictable assignment rules:

#### Explicit Assignment Restrictions

* **Assigning to a Reference (`&Type` / `&var Type`)**: The unary address-of operator `&` is **strictly required** on the right-hand side of the assignment (e.g., `var r: &i32 = &stack_var;`).
* **Assigning to a Heap Pointer (`*Type` / `*var Type`) or Dynamically Sized Array (`*[T]`)**: The assignment expression **strictly requires** either the `new` allocation operator or the `move` ownership transfer keyword (e.g., `var p: *i32 = new 5;`, `var p2: *i32 = move p;`).

#### Transparent Pointer/Reference Dereferencing

Accessing or interacting with a `&Type` or a `*Type` target uses **identical syntax to a standard stack value `Type**`. There are no explicit dereference (`*ptr`) or member arrow (`->`) symbols required. The compiler transparently evaluates and routes field access (`.field`), array indices (`[index]`), and basic operators directly to the underlying pointee.

#### Slices (`&[T]`) versus Dynamically Sized Heap Arrays (`*[T]`)

* **Slices (`&[T]`)**: Represent an unmanaged view into a sequence of elements whose bounds are unknown at compile time. Slices are structured internally as a runtime fat pointer pairing an address pointer with a explicit `u64` size boundary.
* **Dynamically Sized Heap Arrays (`*[T]`)**: Represent a completely managed heap instance block instantiated via a `new` allocation expression:
```alloy
var arr: *[u32] = new [0; 120]; // Allocates 120 elements of u32 initialized to 0

```


* **Memory Layout & C-FFI Compatibility:** To retain total binary drop-in compatibility with legacy C ecosystems, a pointer to an Alloy dynamically sized array points directly to the memory address of the first active data element (`element[0]`).
* **Length Metadata Tracking:** The allocation's length value (returned via `arr.length()`) is stored automatically by the runtime in a dedicated metadata prefix block located **immediately before the array data pointer** (i.e., at a negative memory offset from the user-facing pointer address).



---

### 4.3 Control Flow Semantics

**`return [value]`**
Immediately exits the enclosing function, yielding `value` as its result.

**`break [value]`**
Exits the **innermost** value-yielding construct — a `for` loop, a `while` loop, a `match`, or an `if`. When a value is provided, that construct evaluates directly to the value.

#### `if` as a Value-Yielding Construct

An `if` is itself a value-yielding construct: a `break value;` in either branch makes the `if` evaluate to that value, consistent with `for` / `while` / `match`. Because `break` always targets the *innermost* such construct, a `break` placed inside an `if` body yields from that `if` and can never exit an enclosing loop directly. To break an enclosing loop from conditional logic, the condition is written as the loop-break's operand:

```alloy
// breaks the 'while', yielding 'a' or 'b'
break if (cond) break a; else break b;
```

#### Loop Semantics (`for` and `while`)

* Loops are completely interface-driven. Any structural data collection or type implementing the built-in `Iterable` interface (such as a fixed array, a slice `&[T]`, or a dynamically sized heap array `*[T]`) can be utilized inside a `for` loop statement.
* **Expression-Only `else` Clause:** The trailing `else` block on a `for` or `while` loop is **only permitted when the entire loop construct is evaluated as an expression** (e.g., when assigning its value to a variable). When an `else` block is supplied, a value expression is explicitly required along all execution paths: the loop body **must** yield a value via an explicit `break value;` statement, and the `else` block must evaluate to a value matching that same type. Using an `else` arm on a loop that is executed purely as a statement is a compile-time error.

#### Match Expressions

```alloy
var x = match (subject) {
    Pattern1 |payload_capture| { break 10; }
    Pattern2 { break 20; }
} else {
    // External else block
    break 30; // Required expression fallback
};

```

* **Subject Versatility:** The subject of a `match` statement can evaluate to an enum variant, a numeric primitive, a character literal, or a string literal (treated natively as an array of integral numbers).
* **Pattern Captures:** The pattern capture clause (`|capture|`) is **exclusively valid** when matching enum variants containing attached data payloads. Utilizing a pattern capture when matching numbers, characters, or strings results in a compile-time error.
* **Match Evaluation & Value Yielding:** Like loops, distinct match arms can yield an evaluated value from the outer `match` expression block by terminating via a `break value;` statement.
* **Expression-Only External Match `else` Block:** A `match` structure supports an optional **external `else` block** positioned after its closing bracket. This block is **only permitted when the match is evaluated as an expression**. It executes if and only if the selected match arm completes its execution path normally **without returning a value via a `break` statement**. Because it is constrained to expression contexts, the external `else` block must also provide a final value matching the expression's expected return type. Appending an external `else` block to a `match` construct used purely as a statement is a compile-time error.

---

### 4.4 Lambda / Closure Semantics

```alloy
|&var x, y| (param: T) -> R { body }

```

* The optional capture list (`|...|`) names variables from the enclosing scope. Each capture may carry a type modifier (`&`, `&var`, `*`, `*var`) that controls how the outer variable is accessed within the lambda.
* The parameter list and optional return type follow the same syntax as a regular function.
* The type of a lambda expression is the corresponding function type `(T) -> R`.
* A lambda with no captures may omit the capture delimiters: `(param: T) { ... }`.

### 4.5 Extension Functions

Any function whose first parameter is prefixed with `self` is an extension function:

```alloy
fn add(self v: &Vec3, other: &Vec3) -> Vec3 { ... }

```

* Called via dot notation: `v.add(other)`.
* The receiver is treated as an implicit first argument for the purpose of overload resolution.
* `self` must appear only on the **first** parameter; any other position is an error.
* When the `self` receiver's type is an **interface** (e.g. `fn area(self s: &Shape) -> f32`), the extension is the interface function's **default implementation** — see §5.2.

---

## 5. Standard Library & Primitives

### 5.1 Built-in Functions & Methods

| Function / Method | Signature | Semantics |
| --- | --- | --- |
| `reinterpret<T>` | `self s: &S` → `&T` | Reinterprets the bytes of `s` as type `T`. Returns a reference. |
| `convert<T: Number>` | `self s: &S` → `T` | Converts the numeric value of `s` to type `T`. |
| `.length()` | `self s: &Iterable` → `u64` | Built-in collection query method bundled into the core `Iterable` interface definition. Available natively on fixed arrays, dynamic heap arrays `*[T]`, slices `&[T]`, and custom iterator structures. |

### 5.2 Built-in & User-Defined Interfaces

| Name | Satisfied by |
| --- | --- |
| `Number` | `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` `f32` `f64` |
| `Iterable` | Built-in structures, arrays, slices `&[T]`, and dynamic heap arrays `*[T]` providing a `.length()` method and iteration support. |

Used as a type-parameter constraint: `fn foo<T: Number>(...)`.

#### User-Defined Interfaces

Interfaces define traits or constraints as named contract blocks of function signatures:

```alloy
interface Serializable {
    fn serialize(format: u32) -> bool;
}

```

A nominal type alias links itself explicitly to one or more user-defined interfaces using a C++-inspired mapping annotation during its declaration syntax:

```alloy
type Packet : Serializable, Iterable = struct {
    id: u32;
    payload: *[u8];
};

```

#### The Two Roles of an Interface

An interface may be used in **two distinct ways**:

1. **Dynamic dispatch.** An interface used as a type — always behind an indirection (`&I`, `&var I`, `*I`, `*var I`) — produces an *interface object* (§3.2). A value of any concrete type that implements `I` is implicitly convertible to such an interface object. Calling an interface function through an interface object (`handle.do_something()` where `handle: &Shape`) is resolved at **runtime through the vtable** to the concrete type's implementation.

2. **Generic constraint.** An interface used as a type-parameter bound (`fn do<T: I>(...)`) restricts the generic to types that implement `I`. The call is resolved **statically** at each instantiation; no vtable is involved.

#### Default Implementations

An **extension function whose `self` receiver is an interface** is the **default implementation** of that interface function:

```alloy
interface Shape {
    fn area() -> f32;
    fn name() -> *[u8];
}

// default implementation of Shape::name, shared by every implementer
fn name(self s: &Shape) -> *[u8] { return "shape"; }
```

* A default implementation makes the corresponding interface function **optional** for implementing types.
* An extension function written for a **concrete type** *overrides* the default for that type. When resolving a call on a concrete value, a type-specific extension is always preferred over an interface default.

#### Compilation & Verification Mechanics

When a type `T` is flagged with interface markers (`type T : I1, I2 = ...`), the compiler performs a static verification pass over the module scope. For every function declared inside each interface (`I1`, `I2`):

1. A satisfying **extension function** (§4.5) must be visible in the module — either an extension belonging to `T` itself, **or** a default implementation (an extension whose `self` receiver is the interface).
2. The satisfying extension must precisely match the method name, the parameter sequence, and the return type specified by the interface declaration. The parameters following `self` correspond positionally to the interface function's parameter list.
3. The receiver indirection of the satisfying extension's first parameter (`self: &T`, `self: *var T`, etc.) dictates what memory state or qualifier context is permitted when invoking that interface function polymorphically.
4. If neither a type-specific extension nor a default implementation is visible, verification fails with a compile-time error.

---

### 5.3 Extern FFI

External C functions must be explicitly described with fixed signatures that mandate concrete arrow return types:

```alloy
extern functionName(param: Type) -> ReturnType;
extern variadicFunc(...) -> *var u8;

```

* **Architecture Strategy:** The FFI layer is intentionally isolated. Raw `extern` declarations are designed strictly for low-level systems developers contextually porting legacy C libraries to Alloy ecosystems. Standard software applications are expected to consume safe, native Alloy modules that seamlessly wrap and encapsulate these unsafe FFI barriers.

### 5.4 Module System

* **Strict File System Mirroring:** Qualified module pathways match physical disk positioning precisely. An instruction like `import a::b::c;` commands the compiler to look explicitly for a source file located at `a/b/c.alloy` relative to the workspace project root directory.

---

## 6. Compile-Time Evaluation & Macros

### 6.1 The Comptime Modifier (`#`)

Any **value-yielding expression** prefixed by the token `#` — an `#if`, `#while`, `#match`, an arbitrary function call (`#compute(x)`), an identifier, or a parenthesised expression — is intercepted by the compiler and executed at compile time via an internal interpreter. A `#`-marked construct must produce a value; a bare statement block (`{ … }`) is not a value and cannot be marked.

#### Value-Substitution Model

Comptime expressions operate on a pure value-substitution model. Once a compile-time expression completes execution, its entire syntax node tree is stripped from the final runtime code layout and replaced with its final calculated literal value, struct initialization block, or nominal type signature.

A value-yielding construct yields its value via `break` (§4.3), so an `#if` selecting between two values is written:

```alloy
const a = #if (cond) break 50; else break 100;

```

#### Implicit Comptime Inheritance

When a `#` modifier marks an outer expression, all nested child expressions, loop structures, and evaluation flows contained within that outer node scope implicitly inherit compile-time evaluation.

### 6.2 The Compile-Time Pointer Barrier & Sandboxing

To eliminate cross-compilation target safety errors and prevent memory leakage from host architectures into generated binaries, compile-time blocks are bound to strict computational constraints:

* **The Pointer Barrier:** A compile-time evaluation block cannot yield an unmanaged reference (`&`) or a managed pointer (`*`) that escapes into a runtime variable. Any data crossing the boundary from compile-time execution to a runtime variable state must be handled strictly as values. Breaking this constraint triggers a compile-time error.
* **Strict Sandboxing Boundaries:** The compile-time interpreter is strictly restricted to the project's physical root directory workspace (mirroring physical filesystem rules §5.4).
* **Foreign Function Isolation:** Comptime blocks are strictly prohibited from invoking low-level `extern` C functions (§5.3). Compile-time evaluation can only run safe user-defined Alloy code or built-in system macros.

### 6.3 Macros

Macros are specialized compile-time functions used for code reflection, source file introspection, and automated type generation.

```alloy
macro readTypeFromJson(path: &[u8]) {
    // Introspection and type mutation using compile-time features
}

```

* **Signature and Inferred Types:** Macros are defined using the `macro` keyword. While macro input parameters are strictly typed, **macro return types are completely inferred by the compiler** based on the generated AST layout or the underlying type node replacement it yields.
* **Invocation Syntax:** To explicitly distinguish macros from standard functions, all macro calls must be preceded by the `#` character token.
* **Type Synthesis Examples:**
```alloy
type T = #readTypeFromJson("types/T.json");
type P = #if (DEVELOPMENT) break struct { id: u32; }; else break #readTypeFromJson("types/P.json");

```

### 6.4 Built-in Macros

The compiler provides a small set of built-in macros. Like all macros they are invoked with a leading `#`.

| Macro | Signature | Semantics |
| --- | --- | --- |
| `type_of` | `(value) -> #Type` | The `#Type` of the argument expression's type (§3.4). |
| `struct_type` | `() -> #Type` | A fresh, empty struct `#Type`, for synthesising a type. |
| `enum_type` | `() -> #Type` | A fresh, empty enum `#Type`. |

A type may also be reflected directly by prefixing its name with `#` (`#u32`, `#Packet`) — see §3.4.

---

*End of specification.*