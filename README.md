# luaGPU

A real-time GPU shader authoring tool that lets you write GLSL fragment shaders using a Lua-like syntax. Write shaders in `.lua` files — luaGPU compiles them to GLSL 3.30 core, links them as OpenGL programs, and renders them in a fullscreen window with **hot-reload on file save**.

## Features

- Write shaders in readable Lua syntax — no raw GLSL required
- Full type inference: numeric literals resolve to `float` or `int` by context
- Monomorphization of polymorphic helper functions (like C++ templates)
- Named and anonymous structs
- In-window error overlay when compilation fails — no console hunting
- Hot-reload: save the file, the shader rebuilds automatically
- Built-in uniforms: `u_time`, `u_delta`, `u_resolution`, `u_mouse`
- Reusable shader libraries via `shaderlib()`

## Requirements

- Windows (hot-reload uses Win32 `ReadDirectoryChangesW`)
- CMake 3.24+
- vcpkg with the following packages: `luajit`, `opengl`, `glfw3`, `glad`
- MSVC (C++17)

## Building

```bat
git clone <repo>
cd luaGPU

cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

If `VCPKG_ROOT` is set in your environment, the toolchain file is picked up automatically and the `-D` flag can be omitted.

### Build targets

| Target | Description |
|---|---|
| `luagpu` | Main application |
| `compiler` | Lua→GLSL compiler (static library, no Lua/GL dependency) |
| `host` | LuaJIT bridge (static library) |
| `test_compiler` | Compiler unit/integration tests |
| `emit_examples` | CLI tool: compile all examples and print the generated GLSL |

## Usage

```bat
luagpu <shader.lua> [width] [height]
```

**Key bindings:**

| Key | Action |
|---|---|
| `Escape` | Quit |
| `R` | Force shader reload |

## Writing a Shader

A shader file must return a `ShaderHandle` created by calling `shader()`.

```lua
local myShader = shader(function(u_time, u_resolution)

    -- Helper functions become GLSL functions
    local function fade(t)
        return t * t * (3.0 - 2.0 * t)
    end

    -- The returned function is the fragment shader entry point.
    -- `uv` is the normalized fragment coordinate in [0,1]^2.
    return function(uv)
        local t = fade(uv.x + sin(u_time) * 0.2)
        return vec4(t, uv.y, 0.5, 1.0)
    end
end)

return myShader
```

### Outer function parameters → uniforms

Parameters of the outer `shader()` closure become GLSL `uniform` declarations. Their GLSL types are determined by the values you pass in the Lua host (via constructor metatables). The following four uniforms are always available without declaration:

| Name | GLSL type | Description |
|---|---|---|
| `uv` | `vec2` | Normalized fragment coordinate `gl_FragCoord.xy / u_resolution` |
| `u_time` | `float` | Elapsed time in seconds |
| `u_delta` | `float` | Time since last frame in seconds |
| `u_resolution` | `vec2` | Window size in pixels |
| `u_mouse` | `vec2` | Mouse position in pixels |

### GLSL built-ins

All standard GLSL math and geometric functions are available unqualified:

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `sqrt`, `abs`, `floor`, `ceil`, `fract`, `mod`, `sign`, `min`, `max`, `clamp`, `mix`, `step`, `smoothstep`, `length`, `distance`, `dot`, `cross`, `normalize`, `reflect`, `refract`, `pow`, `texture`

Standard `math.*` aliases (`math.sin`, `math.cos`, `math.pow`, etc.) also work.

### Types and constructors

```lua
-- Scalar
local f = float(1.5)
local i = int(3)
local b = bool(true)

-- Vectors
local v2 = vec2(1.0, 0.5)
local v3 = vec3(0.0, 1.0, 0.0)
local v4 = vec4(v3, 1.0)        -- can mix scalars and smaller vectors

-- Integer vectors
local iv = ivec2(3, 4)

-- Matrices
local m = mat4(...)

-- Swizzle
local xy = v3.xy    -- vec2
local r  = v4.r     -- float (same as .x)
```

### Structs

```lua
-- Named struct
local Ray = struct({ origin = vec3, dir = vec3 })

local function make_ray(ro, rd)
    return Ray { origin = ro, dir = rd }
end

-- Anonymous (inline table constructor)
local function sphere_hit(p)
    return { dist = length(p) - 1.0, hit = length(p) < 1.0 }
end
```

### Shader libraries

Factor reusable code into a library. Only functions actually called are emitted.

```lua
-- noise_lib.lua
return shaderlib(function()
    local function hash(p)
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453)
    end

    return {
        hash  = hash,
        value = function(p) ... end,
    }
end)
```

```lua
-- my_shader.lua
local noise = require("noise_lib")

local myShader = shader(function(u_time)
    return function(uv)
        return vec4(vec3(noise.hash(uv)), 1.0)
    end
end)

return myShader
```

### Control flow

```lua
-- if / elseif / else
if x > 0.5 then
    ...
elseif x > 0.25 then
    ...
else
    ...
end

-- Ternary (Lua idiom, compiled to GLSL ?: operator)
local val = cond and a or b

-- for loop (numeric)
for i = 0, 10 do
    ...
end

-- while loop
local i = 0
while i < 64 do
    ...
    i = i + 1
end

-- break
while true do
    if done then
        break
    end
end
```

### Operator notes

| Lua | GLSL |
|---|---|
| `~=` | `!=` |
| `not x` | `!x` |
| `a and b or c` | `(a) ? (b) : (c)` |
| `a % b` (float) | `mod(a, b)` |
| `a % b` (int) | `a % b` |

## Examples

| File | Technique |
|---|---|
| `examples/plasma.lua` | Sine-wave interference plasma with cosine palette |
| `examples/mandelbrot.lua` | Smooth-colouring Mandelbrot with animated zoom |
| `examples/raymarcher.lua` | 3D SDF ray marcher: sphere + torus + ground plane, soft shadows |
| `examples/fbm_landscape.lua` | Fractional Brownian Motion terrain with biome colouring |
| `examples/voronoi.lua` | Voronoi/Worley cellular noise |
| `examples/struct_test.lua` | Named struct declaration and usage |

Run any example:

```bat
luagpu examples/plasma.lua
luagpu examples/raymarcher.lua 1280 720
```

## Running Tests

```bat
cmake --build build --target test_compiler
build\Release\test_compiler.exe
```

Or via CTest:

```bat
ctest --test-dir build -C Release
```

## Project Structure

```
src/
  compiler/       Lua→GLSL compiler (pure C++, no external dependencies)
    Lexer          Tokenizer
    Parser         Recursive-descent parser → AST
    AST            AST node definitions
    Types          GLSL type system + union-find constraint solver
    SymbolTable    Scoped symbol table with built-in GLSL functions
    Compiler       Multi-pass type checker and monomorphizer
    Emitter        GLSL code generator
  host/           LuaJIT bridge
    ShaderCompiler Extracts shader source from Lua closures, drives compilation
  main.cpp        GLFW window, render loop, file watcher, error overlay

examples/         Lua shader examples
test/             Compiler unit and integration tests
tools/            emit_examples: compile examples and dump generated GLSL
```

## License

See [LICENSE](LICENSE).
