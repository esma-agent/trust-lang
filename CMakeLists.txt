# ═══════════════════════════════════════════════════════════════════════
#  Build System Rules
# ═══════════════════════════════════════════════════════════════════════
#
#  1. Source file paths
#     ┌ src/ files   →  ${PROJECT_SOURCE_DIR}/<component>/<file>.cpp
#     │                  (PROJECT_SOURCE_DIR = ${CMAKE_SOURCE_DIR}/src)
#     ├ non-src files →  ${CMAKE_SOURCE_DIR}/<path>/<file>.cpp
#     │                  (tests/, contrib/, etc.)
#     └ Generated     →  ${CMAKE_BINARY_DIR}/<path>/<file>.cpp
#     NO relative paths with ".." in source file lists.
#
#  2. Header directories
#     ┌ Project headers →  trust_include_dirs INTERFACE target
#     │                    (adds ${PROJECT_INCLUDE_DIR} = include/)
#     └ Component-local →  target_include_directories(<lib> PUBLIC/PRIVATE ...)
#
#  3. Libraries (static)
#     name: <component>_lib
#     type: STATIC
#     visibility: PUBLIC propagates upstream, PRIVATE for impl details
#
#  4. Executables (tools)
#     name: prefixed by purpose (trust_cli, stdlib, gencpp_from_ast, ...)
#     CMAKE_RUNTIME_OUTPUT_DIRECTORY = ${CMAKE_BINARY_DIR} (all binaries flat).
#
#  5. Test categories
#     ┌ unit/  — C++ unit tests (single executable: unit_tests)
#     │           Sources under test/unit/, registered in test/unit/CMakeLists.txt.
#     ├ lit/   — LIT integration tests (lit.cfg.py.in → lit.cfg.py)
#     │           Sources under test/lit/, registered in test/lit/CMakeLists.txt.
#     └ vscode/ — VSCode extension unit & integration tests
#                 (run by jest/node, registered in test/vscode/CMakeLists.txt).
#
#  6. Debug executables (simple_integration / lldb_debuggee)
#     ─ Declared locally in test/unit/debug/CMakeLists.txt.
#     ─ Each gets a custom target `run_<name>` to invoke with correct args.
#     ─ All `run_*` targets collected into `run_debug_tests` / `run_tests`.
#     To run a single utility:  cmake --build . --target run_<name>
#
#  7. Generated files
#     custom_command + custom_target + add_dependencies for ordering.
#
#  8. C++20 modules
#     Separate PCM build via custom_command.
#     target_trust_module() / target_runtime_module() to attach.
#
#  9. Component CMakeLists.txt
#     Each component has exactly one CMakeLists.txt file, located
#     at include/<component>/CMakeLists.txt. This file defines the static
#     library and/or executables for the component. The src/<component>/
#     directory contains only .cpp implementation files, but does not
#     contain its own CMakeLists.txt file.
#     This rule applies only to project COMPONENTS (include/*/). Test
#     directories (test/*) are not components and may have their own
#     CMakeLists.txt layout.
#
# 10. VSCode tasks.json
#     The project root must always contain an up-to-date .vscode/tasks.json.
#     Required tasks:
#     - Project build (cmake: configure + build)
#       Combine configure and build in a single command (generator: Unix Makefiles,
#       build dir: _build/).
#     - All tests (test: all) — unit + lit + debug + vscode
#     - Direct execution of unit_tests with shuffle and filter
#     There may be more tasks, but these three tasks must be present.
#
# 11. Common build commands
#     ──────────────────────────────────────────────────────────
#     # Конфигурация (из корня проекта)
#     cmake -B _build -G "Unix Makefiles"
#     cmake -B _build -G "Unix Makefiles" -DWITH_TORCH=ON   # с Torch
#
#     # Сборка всего проекта
#     cmake --build _build
#
#     # Сборка отдельных библиотек
#     cmake --build _build --target <"component name" + _lib>
#
#     # Сборка исполняемых файлов
#     cmake --build _build --target <name>
#
#     # Сборка и запуск тестов
#     cmake --build _build --target unit_tests             # сборка unit-тестов (без запуска)
#     cmake --build _build --target run_unit_tests         # сборка + запуск unit-тестов
#     cmake --build _build --target run_lit_tests          # LIT-тесты
#     cmake --build _build --target run_debug_tests        # debug-тесты
#     cmake --build _build --target run_vscode_tests       # VSCode-тесты
#     cmake --build _build --target run_tests              # все тесты сразу
#
#     # Отдельные debug-тесты
#     cmake --build _build --target run_simple_integration # debug integration
#     cmake --build _build --target run_lldb_main          # LLDB без DAP
#
#     # Очистка
#     cmake --build _build --target clean
#
# ═══════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.28)

# ── Compilers ──
# Use GCC 14 (libstdc++ from GCC 14 has compatibility issues with clang 17)
set(CMAKE_C_COMPILER gcc CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER g++ CACHE FILEPATH "" FORCE)

project(trust-lang LANGUAGES C CXX)

# ── Common paths ──
set(PROJECT_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
set(PROJECT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src")

# ── Global settings ──
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Symlink compile_commands.json into project root for clang-tidy ──
file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
     "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Strict compile options ──
# NOTE: These flags apply to ALL project code via add_compile_options.
# Exceptions are made for:
#   - System/external headers: marked as SYSTEM include directories
#     (see cmake/dependencies.cmake and component CMakeLists.txt)
#   - Generated files (lex.yy.cpp, parser.tab.cpp): use
#     set_source_files_properties(... COMPILE_OPTIONS "-Wno-old-style-cast")
#     (see include/parser/CMakeLists.txt, test/unit/CMakeLists.txt)
#   - test/unit/unit_tests: additional -Wno-unused-result
add_compile_options(
    -Werror
    -Wswitch
    -Werror=switch
    -Wold-style-cast
    -Wreorder
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wimplicit-fallthrough
)

# ── Linker options ──
# -rdynamic нужен для backtrace_symbols() — чтобы видеть имена функций
# в стеке вызовов. Применяем ко всем исполняемым файлам.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")

# ── Options ──
option(WITH_TORCH "Enable libtorch support (builds libtensor_cpu.so)" OFF)
option(WITH_SOLVER "Enable SMT solver (Z3) for formal verification" OFF)

if(WITH_TORCH)
    set(LIBTORCH_ROOT "${CMAKE_SOURCE_DIR}/contrib/libtorch")
    set(LIBTORCH_INCLUDE_DIRS
        ${LIBTORCH_ROOT}/include
        ${LIBTORCH_ROOT}/include/torch/csrc/api/include
    )
    set(LIBTORCH_LIB_DIRS ${LIBTORCH_ROOT}/lib)
endif()

# ── Module path ──
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Modules ──
include(version)
include(dependencies)
include(generate_files)

# ── Configuration display ──
message(STATUS "═══════════════════════════════════════════════════")
message(STATUS "Project Configuration:")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C Compiler:   ${CMAKE_C_COMPILER}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Dir:    ${CMAKE_BINARY_DIR}")
message(STATUS "  Python3:      ${Python3_EXECUTABLE} (${Python3_VERSION})")
message(STATUS "  lit:          ${LIT_EXECUTABLE}")
message(STATUS "  Flex:         ${FLEX_EXECUTABLE}")
message(STATUS "  Bison:        ${BISON_EXECUTABLE}")
message(STATUS "  nlohmann_json: ${nlohmann_json_VERSION}")
message(STATUS "  LLVM:         ${LLVM_VERSION}")
message(STATUS "  Version:      ${TRUST_VERSION_FULL}")
message(STATUS "═══════════════════════════════════════════════════")

# ── Interface targets ──
add_library(trust_include_dirs INTERFACE)
target_include_directories(trust_include_dirs INTERFACE ${PROJECT_INCLUDE_DIR})

# ── Subdirectories ──
add_subdirectory(include/trust)

if(WITH_TORCH)
    add_subdirectory(include/precompiled)
endif()

add_subdirectory(include/diag)
add_subdirectory(include/ast)
add_subdirectory(include/parser)
add_subdirectory(include/stdlib)
add_subdirectory(include/types)
add_subdirectory(include/pipeline)
add_subdirectory(include/solver)
add_subdirectory(include/runtime)
add_subdirectory(include/debug)
add_subdirectory(include/lsp)
add_subdirectory(include/vscode)

# ── Unit tests ──
add_subdirectory(test/unit)

# ── LIT tests ──
add_subdirectory(test/lit)

# ── Общие пути для тестовых данных ──
# Определены здесь, чтобы быть доступными во всех test-поддиректориях
set(TEST_DATA_DIR "${CMAKE_BINARY_DIR}/test_data" CACHE INTERNAL "Test data directory")
set(TEST_SRC      "${CMAKE_SOURCE_DIR}/test/unit/debug/simple_example.src" CACHE INTERNAL "Trust source file")
set(TEST_CPP      "${TEST_DATA_DIR}/.trust/simple_example.cpp" CACHE INTERNAL "Transpiled C++ file")
set(TEST_MAP      "${TEST_DATA_DIR}/.trust/simple_example.map" CACHE INTERNAL "Source map file")
set(TEST_BINARY   "${TEST_DATA_DIR}/simple_example" CACHE INTERNAL "Compiled ELF binary")

# ── Common test data (transpile + compile) ──
add_subdirectory(test)

# ── VSCode extension tests ──
add_subdirectory(test/vscode)

# ── Top-level test targets ──
add_custom_target(run_tests
    DEPENDS run_unit_tests run_lit_tests run_vscode_tests #run_debug_tests
    COMMENT "Run all tests (unit + lit + debug + vscode)"
)
