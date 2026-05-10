# Lua→GLSL Shader Compiler: Implementation Spec

## Overview

A compiler that accepts a restricted subset of Lua syntax and translates it into valid GLSL 3.3 core fragment shaders. The system is embedded inside a LuaJIT host application with an OpenGL rendering backend. Shader definitions are written in Lua using a `shader()` C++ function that extracts source text at runtime, compiles it to GLSL, and returns a handle the caller uses to manage uniforms and draw calls.

---

## Host Environment

- **Implementation language**: C++
- **Lua runtime**: LuaJIT (Lua 5.1 compatible debug API)
- **Rendering backend**: OpenGL
- **GLSL target**: `#version 330 core`
- **Source extraction mechanism**: `debug.getinfo(fn, "Sl")` — see Source Extraction

---

## Authoring Interface

### `shader(fn)`

The primary entry point. `fn` is a Lua closure whose structure encodes the entire shader:

```lua
local myShader = shader(function(u_time, u_resolution)
    -- inner local functions become GLSL helper functions
    local function fade(t)
        return t * t * (3.0 - 2.0 * t)
    end

    -- the returned function is the fragment shader entry point
    return function main(uv)
        return vec4(fade(uv.x), 0.0, 0.0, 1.0)
    end
end)
```

- **Outer function parameters** → GLSL `uniform` declarations
- **Inner `local function` declarations** → GLSL helper functions, emitted before `main`
- **Returned function** → fragment shader entry point; always returns `vec4`; identified by being the return value, not by name
- `fn` is **never executed** by Lua. It is a syntax vehicle for delivering structured source text with correct scoping.

### `shaderlib(fn)`

Declares a reusable library of GLSL helper functions, usable across multiple shaders.

```lua
-- noise.lua
return shaderlib(function()
    local function hash(p)
        -- private helper, not exported
    end

    return {
        fbm = function(uv)
            hash(uv)  -- can call private helpers
        end,
        worley = function(uv)
        end,
    }
end)
```

- The outer closure's `local function` declarations are **private** — available within the lib but not exported
- The **returned table** defines the public API
- Only functions reachable from the consuming shader's call graph are emitted (tree-shaking)
- `shaderlib` values may be used as upvalues of a `shader()` call:

```lua
local noise = require("shaders.noise")  -- normal LuaJIT require

local x = shader(function(u_time)
    return function main(uv)
        local n = noise.fbm(uv)  -- compiler detects noise is a shaderlib upvalue
        return vec4(n, n, n, 1.0)
    end
end)
```

---

## Source Extraction

The `shader()` C function extracts Lua source using `debug.getinfo`:

```cpp
lua_Debug ar;
lua_pushvalue(L, 1);          // push fn argument
lua_getinfo(L, ">Sl", &ar);  // pops fn, fills ar

const char *src       = ar.source;
int         line_start = ar.linedefined;
int         line_end   = ar.lastlinedefined;
```

Source prefix conventions (standard Lua):

| Prefix | Meaning |
|--------|---------|
| `@`    | Source is a file path — read file, slice `[line_start, line_end]` |
| `=`    | Source is a label (e.g. `=[C]`) — cannot recover source, error |
| none   | Source is the chunk text verbatim — slice directly |

Lines are **1-indexed** and relative to the chunk start.

---

## Uniform System

Uniforms are declared as parameters to the outer shader function and supplied as **upvalues** of that function at the call site.

### Type Declaration

Uniform types are determined by the **constructor function** used at the declaration site:

```lua
local u_intensity  = float(0.5)
local u_color      = vec3(1.0, 0.0, 0.0)
local u_transform  = mat4(...)
local u_tex        = myEngine:getTexture("noise")  -- userdata, inferred as sampler2D
```

Supported constructors and their GLSL types:

| Lua constructor | GLSL type   |
|-----------------|-------------|
| `float(x)`      | `float`     |
| `int(x)`        | `int`       |
| `bool(x)`       | `bool`      |
| `vec2(x,y)`     | `vec2`      |
| `vec3(x,y,z)`   | `vec3`      |
| `vec4(x,y,z,w)` | `vec4`      |
| `mat2(...)`     | `mat2`      |
| `mat3(...)`     | `mat3`      |
| `mat4(...)`     | `mat4`      |
| userdata (texture metatable) | `sampler2D` |

- Upvalues with no recognized constructor are **ignored** — not treated as uniforms
- `_ENV` is explicitly skipped during upvalue iteration
- If a uniform upvalue is `nil` at compile time, emit a compile error with the variable name

### Frame Sync

Before each draw call, the host iterates all tracked `(name, upvalue_index)` pairs and calls the appropriate `glUniform*` based on the stored type:

```cpp
for (auto &uniform : tracked_upvalues) {
    lua_getupvalue(L, shader_fn_idx, uniform.upvalue_index);
    // dispatch to glUniform1f / glUniform3fv / glUniformMatrix4fv / etc.
    lua_pop(L, 1);
}
```

Type is **locked at compile time**. If an upvalue's Lua type changes between frames (e.g. reassigned to a different constructor result), behavior is undefined — the implementation may error or silently misbehave. A runtime type check on each sync is recommended.

---

## Built-in Variables

The following are injected into `main`'s scope automatically. They do not need to be declared as uniforms.

| Lua name       | GLSL source                          | Type   |
|----------------|--------------------------------------|--------|
| `uv`           | `gl_FragCoord.xy / u_resolution`     | `vec2` |
| `frag_coord`   | `gl_FragCoord`                       | `vec4` |
| `u_resolution` | uniform `vec2 u_resolution`          | `vec2` |
| `u_time`       | uniform `float u_time`               | `float`|
| `u_delta`      | uniform `float u_delta`              | `float`|
| `u_mouse`      | uniform `vec2 u_mouse` (normalized)  | `vec2` |

`u_resolution`, `u_time`, `u_delta`, and `u_mouse` are always emitted as uniforms and synced by the host automatically, regardless of whether the shader references them.

---

## Supported Lua Subset

### Types

Only the following types exist in the compiler's type system. All other Lua values are rejected at the boundary.

```
float | int | bool
vec2 | vec3 | vec4
ivec2 | ivec3 | ivec4
mat2 | mat3 | mat4
sampler2D
void
struct (anonymous, generated — see Structs)
```

### Statements

| Lua construct | Supported | Notes |
|---|---|---|
| `local x = expr` | ✓ | Type inferred from expr |
| `local function f(...)` | ✓ | Helper functions |
| `if / elseif / else / end` | ✓ | |
| `for i = start, stop, step do` | ✓ | Numeric for only |
| `while cond do` | ✓ | |
| `break` | ✓ | |
| `return expr` | ✓ | Single value only |
| `repeat / until` | ✗ | |
| `goto` | ✗ | |
| `for k,v in pairs(...)` | ✗ | Generic for excluded |

### Expressions

| Lua construct | Supported | Notes |
|---|---|---|
| Arithmetic `+ - * / %` | ✓ | |
| Comparison `== ~= < > <= >=` | ✓ | |
| Logic `and or not` | ✓ | Boolean context and ternary pattern only — see below |
| `//` floor division | ✗ | Not available in LuaJIT 5.1 |
| Bitwise ops | ✗ | Not available in LuaJIT 5.1 — use `bit.*` / globals instead |
| `a and b or c` ternary | ✓ | Special-cased — see below |
| String concat `..` | ✗ | Hard error |
| Swizzle `.xyz` `.rgb` etc. | ✓ | Validated against base type |
| Field access `.field` | ✓ | Structs only |
| Function calls | ✓ | No first-class functions |
| Closures / upvalue capture | ✗ | Hard error inside shader body |
| Recursion | ✗ | Hard error |
| Multiple return values | ✗ | Hard error |
| `nil` | ✗ | Hard error |

### `and` / `or` Usage

**Boolean context** (`if`, `while`, `elseif` conditions): `and`/`or` map directly to `&&`/`||`.

**Ternary pattern** — `a and b or c` is recognized as a special case and emitted as a GLSL ternary:

```lua
local x = (val > 0.5) and vec3(1.0) or vec3(0.0)
-- emits: vec3 x = (val > 0.5) ? vec3(1.0) : vec3(0.0);
```

Recognition rules:
- The pattern must be exactly `<bool_expr> and <value_expr> or <value_expr>`
- `b` and `c` must have the same resolved type — type error otherwise
- `a` must resolve to `bool` — type error otherwise
- The result type is the type of `b`/`c`

**Caveat**: `a and b or c` only behaves as a ternary when `b` is never `false`. Since `nil` is excluded from the type system and `bool` is the only scalar that can be `false`, this failure case only occurs if `b` is the literal `false` — which the compiler should detect and reject with an error: `'and/or' ternary: middle operand must not be a bool literal false`.

**All other value-returning uses of `and`/`or`** are a hard error. Only the exact ternary pattern and boolean-context uses are permitted.

### Bitwise Operations

LuaJIT (5.1) does not have native bitwise syntax. Bitwise operations are exposed as:

- `bit.band(a, b)` → `(a & b)`
- `bit.bor(a, b)` → `(a | b)`
- `bit.bxor(a, b)` → `(a ^ b)`
- `bit.bnot(a)` → `(~a)`
- `bit.lshift(a, b)` / `shl(a, b)` → `(a << b)`
- `bit.rshift(a, b)` / `shr(a, b)` → `(a >> b)`

Both `bit.*` namespace and bare globals (`shl`, `shr`) are recognized by the compiler and emitted as GLSL bitwise operators. All operands must be `int` or `ivec*` — type error otherwise.

### Math Builtins

The following `math.*` functions are mapped to GLSL equivalents and available as globals:

| Lua | GLSL |
|-----|------|
| `math.sin(x)` | `sin(x)` |
| `math.cos(x)` | `cos(x)` |
| `math.tan(x)` | `tan(x)` |
| `math.asin(x)` | `asin(x)` |
| `math.acos(x)` | `acos(x)` |
| `math.atan(y,x)` | `atan(y,x)` |
| `math.sqrt(x)` | `sqrt(x)` |
| `math.abs(x)` | `abs(x)` |
| `math.floor(x)` | `floor(x)` |
| `math.ceil(x)` | `ceil(x)` |
| `math.max(a,b)` | `max(a,b)` |
| `math.min(a,b)` | `min(a,b)` |
| `math.pow(a,b)` | `pow(a,b)` |
| `math.exp(x)` | `exp(x)` |
| `math.log(x)` | `log(x)` |

The following GLSL builtins are also available as globals (no `math.` prefix):

`mix`, `clamp`, `smoothstep`, `step`, `fract`, `mod`, `sign`, `length`, `distance`, `dot`, `cross`, `normalize`, `reflect`, `refract`

### Texture Sampling

```lua
local c = texture(u_tex, uv)  -- returns vec4
```

`texture(sampler2D, vec2) → vec4`. This is the only supported sampler operation. The compiler recognizes `texture` as a builtin and types the result as `vec4`.

---

## Type System

### Type Representation

Each AST node is annotated with a `TypeInfo` during the type-checking pass:

```cpp
enum class GlslType {
    Float, Int, Bool,
    Vec2, Vec3, Vec4,
    IVec2, IVec3, IVec4,
    Mat2, Mat3, Mat4,
    Sampler2D,
    Void,
    Struct,   // anonymous; identity determined by struct_id
    Unknown,  // unresolved type variable / error sentinel
};

struct TypeInfo {
    GlslType tag       = GlslType::Unknown;
    int      struct_id = -1;  // only valid when tag == Struct
    int      tvar_id   = -1;  // index into union-find table; -1 if concrete
};
```

Two `TypeInfo` values are equal when:
- Both tags match, and
- If tag is `Struct`, both `struct_id` values match (after deduplication)

### Numeric Literal Inference (Constraint-Based)

Bare numeric literals are assigned a **type variable** rather than a concrete type. Constraints are collected during the expression walk and solved afterward.

```lua
local x = someFloat + 1   -- 1 gets TVar(n)
-- constraint: TVar(n) must satisfy float + TVar(n) → float
-- resolved: TVar(n) = float
```

**Resolution rules:**

- Constraints are solved via union-find over type variables
- Conflicting constraints (same variable must be both `float` and `int`) → type error, report both usage sites
- Unresolved type variables (no constraints) → default to `float`

**Default: `float`** — bare integer literals like `3` in `x * 3` default to `float` when unconstrained. This matches GLSL conventions and user intent in shader code.

### Operator Typing Rules

```
same_type OP same_type          → same_type
scalar OP vector                → vector  (broadcast)
vector OP scalar                → vector
scalar OP matrix                → matrix
matrix OP scalar                → matrix
mat4 * vec4                     → vec4
mat3 * vec3                     → vec3
mat2 * vec2                     → vec2
anything involving TY_UNKNOWN   → TY_UNKNOWN  (poison)
```

Mismatched types with no applicable rule → type error.

### Swizzle Typing

```
base: vecN, swizzle length 1    → float
base: vecN, swizzle length 2    → vec2
base: vecN, swizzle length 3    → vec3
base: vecN, swizzle length 4    → vec4
```

Validation:
- Base must be a vector type
- Each swizzle component (`x/y/z/w` or `r/g/b/a` or `s/t/p/q`) must be within the base vector's dimension
- Sets may not be mixed (`.xg` is invalid)

### Structs

Anonymous structs are generated from table constructors used in value position:

```lua
local p = { x = 1.0, y = 2.0 }  -- generates: struct { float x; float y; }
```

- Every table constructor is **eagerly assigned a fresh `struct_id`** during the expression walk — no deduplication at this stage
- Field types are stored as `TypeInfo` (possibly still type variables) pointing into the union-find table
- Struct identity is **structural** — determined after constraint solving, not at instantiation time
- Structs may be used as function return types and parameter types
- Nested structs are supported (field whose type is itself a struct)
- Dynamic field access (computed keys) is not supported

**Struct unification during constraint solving:**

When two struct-typed expressions are unified (e.g. a polymorphic function called with two different struct arguments that must match), their `struct_id`s may differ. Structural comparison at that moment may not be possible because field types are still type variables. Instead, a **deferred struct unification constraint** is emitted:

```cpp
struct StructUnifyConstraint {
    int struct_id_a;
    int struct_id_b;
    SourceLocation loc;  // for error reporting
};
```

Solving order:
1. Solve all scalar type variable constraints (union-find over `tvar_id`s)
2. All field types are now concrete
3. Solve struct unification constraints — process in leaf-first order (inner structs before outer) to handle nesting
4. For each constraint: compare fields structurally; if equal, merge the losing `struct_id` into the winning one and rewrite all AST references; if not equal, type error
5. Topo-sort surviving struct definitions by dependency for emission

### Symbol Table

Lexically scoped, implemented as a linked list of scope frames:

```
global scope: builtin functions, math.* aliases, vec/mat constructors
  └─ shader scope: uniforms (outer fn params), shaderlib upvalues
       └─ function scope: params, return type
            └─ block scope: local vars
                 └─ nested block scope: ...
```

Lookup walks up the parent chain. The compiler tracks whether each binding is:
- A uniform (upvalue)
- A shaderlib reference
- A local variable
- A function

### Compilation Passes

**Pass 1 — Scope & signature collection**

Walk all function declarations reachable from the shader entry point (including shaderlibs). For each function record its name, parameter count, and source location. Parameter types are left unresolved at this stage — shaderlib functions are implicitly polymorphic (see Monomorphization below). Return types are inferred from `return` expressions in pass 2.

**Pass 2 — Expression typing and constraint collection**

Bottom-up AST walk. Assign `TypeInfo` to each node. Collect:
- Scalar type variable constraints into the union-find table
- Deferred struct unification constraints into a separate list

**Pass 3 — Constraint solving**

1. Solve scalar constraints via union-find. Conflicts → type error with both usage sites.
2. Unresolved scalars default to `float`.
3. Resolve struct field types through union-find results.
4. Solve struct unification constraints leaf-first. Conflicts → type error.
5. Topo-sort struct definitions by dependency.

**Pass 4 — Monomorphization**

Walk the call graph from the entry point. For each call to a shaderlib or local function, record the concrete argument types. Group by unique argument type signature. Type-check and emit one GLSL function per unique signature. Recurse into callees. (See Monomorphization.)

### Monomorphization

Shaderlib functions (and local shader functions with unresolved parameter types) are **implicitly polymorphic**. They are not typed once globally — instead they are type-checked and emitted once per unique call signature encountered during call graph traversal.

```lua
-- shaderlib
process = function(p)
    return p.x + p.y
end

-- call sites
process(myVec2)    -- monomorphized as process_vec2(vec2 p) → float
process(myStruct)  -- monomorphized as process_S0(S0 p)     → float
```

**Naming**: monomorphized instances are named `funcname_N` where N is a zero-based index per unique signature, or `funcname_typename` for single-param functions where the type name is unambiguous.

**Error reporting**: type errors in polymorphic function bodies are reported at the **call site**, not the definition site, since the error only manifests for a specific instantiation:

```
shader.lua:31: in call to 'process' with argument type float:
  noise.lua:8: type error: float has no field 'x'
```

**Recursion**: polymorphic functions may not call themselves (recursion is still prohibited). A function calling another polymorphic function causes that callee to also be monomorphized for the relevant argument types.

### Error Handling

- **Poison propagation**: `TY_UNKNOWN` propagates through expressions, allowing multiple errors to be collected before aborting
- A `bool had_errors` flag is set on any type error
- The emitter is not invoked if `had_errors` is set
- Errors include: source file, line number, variable name, and a description

---

## Compilation Pipeline

```
Lua source text  (extracted via debug.getinfo)
        │
        ▼
    [Lexer]
    Tokenize Lua subset
        │
        ▼
    [Parser]
    Produce AST
        │
        ▼
    [Pass 1 — Scope & Signature Collection]
    Build symbol table
    Record function names, param counts, source locations
    Resolve shaderlib upvalue references
    Parameter types left unresolved (polymorphic)
        │
        ▼
    [Pass 2 — Expression Typing]
    Bottom-up AST walk
    Assign TypeInfo to each node
    Collect scalar type variable constraints
    Collect deferred struct unification constraints
    Eagerly assign fresh struct_id to each table constructor
        │
        ▼
    [Pass 3 — Constraint Solving]
    1. Solve scalar constraints (union-find)
    2. Default unresolved scalars to float
    3. Resolve struct field types
    4. Solve struct unification constraints (leaf-first)
    5. Topo-sort struct definitions
        │
        ▼
    [Semantic Validator]
    Check: no recursion
    Check: no closures / upvalue capture inside shader body
    Check: no multiple returns
    Check: no nil
    Check: and/or ternary middle operand is not literal false
        │
        ▼
    [Pass 4 — Monomorphization & Tree-shaking]
    Walk call graph from entry point
    For each call: record concrete argument types
    Type-check each unique instantiation
    Collect only reachable functions (tree-shaking)
    Assign monomorphized names
        │
        ▼
    [Emitter]
    Emit in order:
      1. #version 330 core
      2. Built-in uniforms (always)
      3. User uniform declarations
      4. Struct definitions (topo-sorted)
      5. Shaderlib helpers (monomorphized, reachable only)
      6. Shader-local helpers (monomorphized)
      7. main() wrapper + entry point
```

---

## GLSL Emit Format

### Output structure

```glsl
#version 330 core

// built-in uniforms (always emitted)
uniform vec2  u_resolution;
uniform float u_time;
uniform float u_delta;
uniform vec2  u_mouse;

// user uniforms (from outer fn params)
uniform float u_intensity;
uniform vec3  u_color;
uniform sampler2D u_tex;

// anonymous structs
struct S0 { float x; float y; };

// shaderlib helpers (reachable only)
float noise_hash(vec2 p) { ... }
float noise_fbm(vec2 uv) { ... }

// shader-local helpers
float fade(float t) { ... }

// entry point
out vec4 frag_out;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    frag_out = shader_main(uv);
}

vec4 shader_main(vec2 uv) {
    ...
}
```

### Local variable scoping

Local variables are emitted **inline** at their declaration site, not hoisted. This avoids name collision issues with same-named locals in different scopes:

```glsl
// correct
for (int i = 0; i < 10; i++) {
    float x = float(i) * 2.0;
}

// wrong — do not do this
float x;
for (int i = 0; i < 10; i++) {
    x = float(i) * 2.0;
}
```

### Struct naming

Anonymous structs are assigned generated names during the type-checking pass: `S0`, `S1`, `S2`, etc., ordered by first appearance. The same struct shape always gets the same generated name within a compilation unit.

### Operator mapping

| Lua | GLSL |
|-----|------|
| `and` (boolean context) | `&&` |
| `or` (boolean context) | `\|\|` |
| `a and b or c` (ternary) | `(a) ? (b) : (c)` |
| `not` | `!` |
| `~=` | `!=` |
| `%` | `mod(a, b)` (float) or `a % b` (int) |
| `bit.band(a,b)` / | `(a & b)` |
| `bit.bor(a,b)` | `(a \| b)` |
| `bit.bxor(a,b)` | `(a ^ b)` |
| `bit.bnot(a)` | `(~a)` |
| `bit.lshift(a,b)` / `shl(a,b)` | `(a << b)` |
| `bit.rshift(a,b)` / `shr(a,b)` | `(a >> b)` |

---

## Error Messages

All errors must include:

- Source file and line number (Lua-side)
- The offending variable or expression
- A clear description

Examples:

```
shader.lua:14: type error: cannot add vec3 and mat4
shader.lua:22: uniform 'u_color' is nil at compile time — initialize before calling shader()
shader.lua:31: 'and/or' ternary: middle operand must not be literal false
shader.lua:9: recursion detected in function 'fbm' — not supported in GLSL
shader.lua:17: upvalue capture inside shader body — 'myColor' is not a uniform parameter
shader.lua:31: in call to 'process' with argument type float:
  noise.lua:8: type error: float has no field 'x'
```

---

## Implementation Notes

### Detecting shaderlib upvalues

When iterating the shader function's upvalues in C++, tag each value:

```cpp
// check if upvalue is a shaderlib
if (lua_type(L, -1) == LUA_TTABLE) {
    lua_getfield(L, -1, "__is_shaderlib");
    bool is_lib = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (is_lib) { /* it's a shaderlib */ }
}

// check if upvalue is a texture (engine userdata)
if (lua_type(L, -1) == LUA_TUSERDATA) {
    if (luaL_testudata(L, -1, "engine.Texture")) { /* sampler2D */ }
}
```

### Tree-shaking shaderlib functions

Starting from the shader's entry point, walk the call graph. Any call of the form `libname.fnname(...)` where `libname` is a known shaderlib upvalue → add `(libname, fnname, arg_types)` to the monomorphization queue. Recursively walk the instantiated body for further dependencies (including private helpers). Emit only what is reachable.

### Struct interning and deduplication

Each table constructor gets a fresh `struct_id` immediately. After constraint solving, struct unification constraints are solved leaf-first. When two structs are found structurally equal, the higher-numbered id is rewritten to the lower throughout the AST. The surviving set is topo-sorted for emission.

Field ordering for structural comparison: **alphabetical by field name**. Field ordering for emission: **declaration order** (as written in the table constructor).

### Union-find for type variables

```cpp
struct TNode {
    int      parent;    // -1 = root
    TypeInfo concrete;  // valid when resolved
    bool     is_resolved = false;
};

int  find(std::vector<TNode> &nodes, int i);
void unify(std::vector<TNode> &nodes, int a, int b);
void constrain(std::vector<TNode> &nodes, int var, TypeInfo ty);
```

Conflict detection: if `constrain` is called on a node already resolved to a different concrete type → type error with both source locations.

### Monomorphization registry

```cpp
struct MonoKey {
    std::string          func_name;
    std::string          lib_name;   // empty for shader-local
    std::vector<TypeInfo> arg_types;
    bool operator==(const MonoKey &) const;
};

struct MonoInstance {
    MonoKey     key;
    std::string emitted_name;  // e.g. "process_0", "process_vec2"
    TypeInfo    return_type;
    ASTNode    *body;
};

std::unordered_map<MonoKey, MonoInstance> mono_registry;
```

Before emitting a call, look up `(func, arg_types)` in the registry. If not present, instantiate and type-check the body with parameters bound to the concrete argument types, add to the registry, and enqueue for emission.
