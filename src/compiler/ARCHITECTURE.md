# Compiler Architecture

The `compiler` static library translates a restricted Lua-like source language into GLSL 3.30 core fragment shader code. It has **no dependency on Lua, OpenGL, or any third-party library** — only the C++ standard library — making it straightforward to test in isolation.

## Pipeline Overview

```
Lua source text
      │
      ▼
  [Lexer]          Tokenize into a flat stream of tokens
      │
      ▼
  [Parser]         Build an Abstract Syntax Tree (AST)
      │
      ▼
  [Compiler]
    Pass 1: collect_signatures   Register all local function names/arities
    Pass 2: type_expr            Assign TypeInfo to every expression node
                                 (type variables for unresolved literals)
    Pass 3: solve                Union-find constraint solving; default to float
    validate                     Semantic checks (no recursion, no closures, …)
    Pass 4: monomorphize         DFS call-graph walk; specialize each function
                                 per unique argument-type tuple
    Pass 5: dedup_structs        Merge structurally identical anonymous structs;
                                 topo-sort struct definitions for emission
      │
      ▼
  [Emitter]        Walk monomorphized instances in callee-before-caller order;
                   produce a self-contained GLSL string
      │
      ▼
GLSL source text
```

The public entry point is:

```cpp
// src/compiler/Compiler.h
CompileResult Compiler::compile(ShaderFunc sf,
                                std::vector<std::string> uniform_names);
```

`CompileResult` carries `{ bool ok, std::string glsl, std::vector<UniformDesc>, std::vector<Diagnostic> errors }`.

---

## Lexer (`Lexer.h / Lexer.cpp`)

A straightforward single-pass tokenizer. It exposes a one-token lookahead interface:

```cpp
Token Lexer::peek();   // return current token without consuming
Token Lexer::next();   // consume and return current token
```

Token kinds (`enum class TK`) cover:
- Keywords: `and`, `break`, `do`, `else`, `elseif`, `end`, `false`, `for`, `function`, `if`, `in`, `local`, `nil`, `not`, `or`, `repeat`, `return`, `then`, `true`, `until`, `while`
- Multi-character symbols: `==`, `~=`, `<=`, `>=`, `..`
- Literals: `Number` (IEEE 754 double), `Name`, `String`
- Long strings: `[[ ... ]]`
- `Eof`

Errors are reported as `LexError { line, message }`.

---

## AST (`AST.h`)

All nodes use `std::variant` for a tagged-union representation with no virtual dispatch.

**Expressions** (`ExprKind`): `NumberLit`, `StringLit`, `BoolLit`, `NilLit`, `NameExpr`, `FieldExpr` (swizzle / struct field), `CallExpr`, `UnaryExpr`, `BinaryExpr`, `TableExpr` (anonymous struct constructor), `FuncExpr`.

**Statements** (`StmtKind`): `LocalStmt`, `LocalFuncStmt`, `AssignStmt`, `ExprStmt`, `ReturnStmt`, `IfStmt`, `WhileStmt`, `ForStmt`, `BreakStmt`, `StructDeclStmt`.

Every `Expr` and `Stmt` carries a `SrcLoc { file, line }` for error messages.

`ShaderFunc` is the top-level node produced by the parser. It holds:
- The list of uniform parameter names (from the outer closure)
- The block of statements forming the shader body

---

## Type System (`Types.h / Types.cpp`)

### `GlslType` enum

All supported GLSL types: `Float`, `Int`, `Bool`, `Vec2`–`Vec4`, `IVec2`–`IVec4`, `Mat2`–`Mat4`, `Sampler2D`, `Void`, `Struct`, `Unknown`.

`Unknown` is a poison value that propagates through expressions so that multiple type errors can be reported in a single compilation.

### `TypeInfo`

```cpp
struct TypeInfo {
    GlslType tag;       // concrete type, Struct, or Unknown
    int      struct_id; // valid when tag == Struct; indexes Compiler::structs_
    int      tvar_id;   // union-find index; -1 for concrete types
};
```

Factory helpers: `TypeInfo::make(GlslType)`, `::make_tvar(int)`, `::make_struct(int)`, `::unknown()`.

### `UnionFind`

A standard union-find (path compression, union by rank) that operates on type variables:

```cpp
int  UnionFind::new_tvar();                        // allocate a fresh type variable
int  UnionFind::find(int i);                       // representative (path-compressed)
bool UnionFind::unify(int a, int b, SrcLoc loc);   // merge two tvars
bool UnionFind::constrain(int var, TypeInfo ty);   // bind tvar to concrete type
TypeInfo UnionFind::resolve(TypeInfo t);           // follow chain to concrete type
```

Numeric literals receive fresh type variables during Pass 2. When those variables appear in typed contexts (e.g., as an argument to `vec3()`) they are constrained to the concrete type. Unresolved variables default to `Float` at the end of Pass 3.

---

## Symbol Table (`SymbolTable.h / SymbolTable.cpp`)

A chain of lexical scopes, each holding a `std::unordered_map<std::string, Binding>`. `lookup()` walks the chain from innermost to outermost.

### `BindingKind`

`Local`, `Uniform`, `Function`, `ShaderLib`, `Builtin`, `Constructor`, `StructType`

### Pre-installed global scope

The constructor installs all GLSL built-in functions (`sin`, `cos`, `mix`, `clamp`, `smoothstep`, `length`, `dot`, `cross`, `normalize`, `reflect`, `refract`, `texture`, …), their `math.*` aliases, and all vector/matrix constructors (`vec2`–`vec4`, `ivec2`–`ivec4`, `mat2`–`mat4`, `float`, `int`, `bool`).

---

## Compiler Passes (`Compiler.h / Compiler.cpp`)

### Pass 1 — `collect_signatures`

A shallow walk of the top-level statement list. Every `LocalFuncStmt` is registered in `func_sigs_` with its name, parameter count, and a pointer to its body. This allows forward references within the shader body.

### Pass 2 — `type_expr` / `type_block` / `type_stmt`

A recursive bottom-up walk. Each expression is given a `TypeInfo`:

- Literals get a fresh type variable (`new_tvar()`).
- `NameExpr` looks up the symbol table.
- `CallExpr` applies built-in typing rules (e.g., `vec3(float, float, float) → vec3`) or assigns a fresh tvar for user-defined calls (resolved after monomorphization).
- `BinaryExpr` applies operator typing:
  - Same type × same type → same type
  - Scalar × Vector → Vector (broadcast)
  - Scalar × Matrix → Matrix
  - `matN * vecN` → `vecN`
  - `%` on floats maps to `mod(a, b)` at emission time

Results are stored in `expr_types_` (an `unordered_map<const Expr*, TypeInfo>`).

### Pass 3 — `solve`

Iterates `expr_types_` and calls `uf_.resolve()` on every entry. Type variables that remain unconstrained are defaulted to `Float`. Struct field types are resolved at this point.

### Validation

Checks that are easier to verify after type solving:

- No recursive calls (direct or indirect)
- No upvalue capture inside helper functions (closures are forbidden)
- Functions have exactly one `return` statement
- `nil` does not appear
- The middle operand of an `a and b or c` ternary is not `false` (which would break the `? :` translation)

### Pass 4 — `monomorphize`

The most complex pass. Starting from the shader entry function (`shader_main`), it performs a DFS traversal of the call graph. At each `CallExpr` to a user-defined function, it:

1. Resolves the concrete argument types from the caller's `ExprTypeMap`.
2. Constructs a `MonoKey { func_name, lib_name, arg_types[] }`.
3. If this key has not been seen before, creates a new `MonoInstance`:
   - Clones the symbol table.
   - Re-runs type inference (`type_expr`) on the function body with the concrete parameter types bound.
   - Runs `solve` on the instance's local `UnionFind`.
   - Assigns a mangled emission name (e.g., `fade` → `fade_float` or `fade_0`).
4. Records the `(CallExpr*, emitted_name)` pair in the caller's `CallNameMap`.
5. Recursively processes callees before callers, building `mono_order_` in the correct emission sequence.

`shaderlib` functions are handled identically — their source blocks are parsed on demand and compiled as part of the same monomorphization registry.

### Pass 5 — `dedup_structs`

Anonymous struct types (from `TableExpr` nodes) may be structurally identical across different call sites. This pass:

1. Computes a canonical string key for each struct (sorted field names + type strings).
2. Merges structs with identical keys, rewriting all `struct_id` references.
3. Topologically sorts struct definitions so that structs used as field types are emitted before the structs that contain them.

---

## Emitter (`Emitter.h / Emitter.cpp`)

`Emitter::emit()` produces a complete, self-contained GLSL string in this order:

1. `#version 330 core`
2. Built-in uniforms (`u_resolution`, `u_time`, `u_delta`, `u_mouse`)
3. User uniforms (declared from the outer closure parameter list)
4. Struct definitions (topo-sorted; named structs use user-declared names, anonymous structs use `S0`, `S1`, …)
5. Helper function instances in `mono_order_` (callee-before-caller)
6. `out vec4 frag_out;` and `void main()` wrapper (injects `uv = gl_FragCoord.xy / u_resolution`)
7. The body of `shader_main`

### Expression stringification

`expr_str(e)` recursively converts an expression to a GLSL string using the per-instance `ExprTypeMap` and `CallNameMap`. Notable translations:

| Lua | GLSL |
|---|---|
| `~=` | `!=` |
| `not x` | `!x` |
| `a and b or c` | `(a) ? (b) : (c)` |
| `a % b` (float context) | `mod(a, b)` |
| `math.sin(x)` | `sin(x)` |
| `bit.band(a, b)` | `(a & b)` |

Numeric literals are emitted with the correct GLSL suffix: integer type contexts use bare integers (`1`, `3`), float contexts use decimal notation (`1.0`, `3.0`).

`expr_str_broadcast(e, vec_type)` auto-promotes a scalar expression to `vecN(x)` when a genType builtin expects a matching vector (e.g., `mix(color, vec3(1.0), t)` where `t` is a `float`).

---

## Data Flow Summary

```
ShaderFunc AST
  │
  ├─ Pass 1 ──► func_sigs_
  │
  ├─ Pass 2 ──► expr_types_  +  uf_ (type variables)
  │
  ├─ Pass 3 ──► uf_ resolved; expr_types_ updated
  │
  ├─ validate
  │
  ├─ Pass 4 ──► mono_registry_  (MonoKey → MonoInstance)
  │             mono_order_     (emission order)
  │
  ├─ Pass 5 ──► structs_ deduplicated + topo-sorted
  │
  └─ Emitter ──► GLSL string
```

---

## Adding a New Built-in Function

1. Add it to `SymbolTable::SymbolTable()` with kind `Builtin` and the appropriate `TypeInfo`.
2. If the function requires special typing logic (e.g., return type depends on argument types), handle it in `Compiler::type_call_expr()`.
3. If it requires special emission syntax, handle it in `Emitter::expr_str()`.
4. Add a test case in `test/test_compiler.cpp`.
