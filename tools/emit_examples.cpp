// tools/emit_examples.cpp
// Reads each example .lua file, compiles it, and prints the resulting GLSL.
// Run from the repo root: build\Debug\emit_examples.exe

#include "../src/compiler/Parser.h"
#include "../src/compiler/Compiler.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open: " << path << "\n"; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    struct Example { const char *name; const char *path; };
    std::vector<Example> examples = {
        { "plasma",        "examples/plasma.lua"        },
        { "mandelbrot",    "examples/mandelbrot.lua"    },
        { "raymarcher",    "examples/raymarcher.lua"    },
        { "voronoi",       "examples/voronoi.lua"       },
        { "fbm_landscape", "examples/fbm_landscape.lua" },
    };

    int failures = 0;
    for (auto &ex : examples) {
        std::string src = read_file(ex.path);
        if (src.empty()) { ++failures; continue; }

        CompileResult r;
        try {
            ShaderFunc sf = Parser::parse(src, ex.name);
            Compiler c;
            r = c.compile(sf, {});
        } catch (const std::exception &e) {
            std::cerr << "=== " << ex.name << " EXCEPTION: " << e.what() << "\n";
            ++failures;
            continue;
        }

        std::cout << "// ════════════════════════════════════════════════════\n";
        std::cout << "// " << ex.name << "\n";
        std::cout << "// ════════════════════════════════════════════════════\n";
        if (!r.ok) {
            std::cerr << "ERRORS in " << ex.name << ":\n";
            for (auto &d : r.errors)
                std::cerr << "  " << d.loc.file << ":" << d.loc.line << ": " << d.message << "\n";
            ++failures;
        } else {
            std::cout << r.glsl << "\n";
        }
    }

    return failures ? 1 : 0;
}
