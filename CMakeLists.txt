cmake_minimum_required(VERSION 3.24)
project(luaGPU LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── vcpkg toolchain ────────────────────────────────────────────────────────────
# Pass -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake at
# configure time, or set the VCPKG_ROOT environment variable and let the block
# below pick it up automatically.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

# ── Dependencies ───────────────────────────────────────────────────────────────
# LuaJIT: vcpkg provides pkg-config only, which requires pkg-config.exe on
# Windows. Use find_path / find_library instead to locate the installed files
# directly from the vcpkg install tree.
find_path(LUAJIT_INCLUDE_DIR luajit.h
    PATH_SUFFIXES luajit-2.1 luajit-2.0 luajit)
find_library(LUAJIT_LIBRARY
    NAMES luajit-5.1 lua51
    PATH_SUFFIXES lib)
if(NOT LUAJIT_INCLUDE_DIR OR NOT LUAJIT_LIBRARY)
    message(FATAL_ERROR "LuaJIT not found. Run: vcpkg install luajit")
endif()
add_library(LuaJIT::LuaJIT UNKNOWN IMPORTED)
set_target_properties(LuaJIT::LuaJIT PROPERTIES
    IMPORTED_LOCATION             "${LUAJIT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LUAJIT_INCLUDE_DIR}")

find_package(OpenGL REQUIRED)
find_package(glfw3  REQUIRED CONFIG)
find_package(glad   REQUIRED CONFIG)

# ── Compiler static library ────────────────────────────────────────────────────
file(GLOB COMPILER_SOURCES src/compiler/*.cpp)
add_library(compiler STATIC ${COMPILER_SOURCES})
target_include_directories(compiler PUBLIC src/compiler)
target_compile_options(compiler PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>
)

# ── Host static library ────────────────────────────────────────────────────────
file(GLOB HOST_SOURCES src/host/*.cpp)
add_library(host STATIC ${HOST_SOURCES})
target_include_directories(host PUBLIC src/host)
target_link_libraries(host PUBLIC
    compiler
    LuaJIT::LuaJIT
)

# ── Application executable ─────────────────────────────────────────────────────
add_executable(luagpu src/main.cpp)
target_include_directories(luagpu PRIVATE src/host src/compiler)
target_link_libraries(luagpu PRIVATE
    host
    OpenGL::GL
    glfw
    glad::glad
)
target_compile_options(luagpu PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>
)

# ── Tests ──────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_compiler test/test_compiler.cpp)
target_link_libraries(test_compiler PRIVATE compiler)
target_compile_options(test_compiler PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>
)
add_test(NAME compiler_tests COMMAND test_compiler)

# ── Tools ──────────────────────────────────────────────────────────────────────
add_executable(emit_examples tools/emit_examples.cpp)
target_link_libraries(emit_examples PRIVATE compiler)
target_compile_options(emit_examples PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>
)

# ── Snapshot tool ──────────────────────────────────────────────────────────────
# Headless one-frame render → 16-colour quantized PPM + palette on stdout.
add_executable(snapshot tools/snapshot.cpp)
target_include_directories(snapshot PRIVATE src/host src/compiler)
target_link_libraries(snapshot PRIVATE
    host
    OpenGL::GL
    glfw
    glad::glad
)
target_compile_options(snapshot PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>
)
