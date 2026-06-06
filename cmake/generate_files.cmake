# cmake/generate_files.cmake
# Configure_file and custom targets for generated files

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/trust/version.h.in.cmake
    ${CMAKE_BINARY_DIR}/include/trust/version.h
    @ONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/pipeline/options.h.in
    ${CMAKE_BINARY_DIR}/include/pipeline/options.h
    @ONLY
)
add_custom_target(generate_options_h
    DEPENDS "${CMAKE_BINARY_DIR}/include/pipeline/options.h"
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/test/lit/lit.cfg.py.in
    ${CMAKE_BINARY_DIR}/test/lit/lit.cfg.py
    @ONLY
)
add_custom_target(generate_lit_cfg_py
    DEPENDS "${CMAKE_BINARY_DIR}/test/lit/lit.cfg.py"
)

# ── Generate embed data from dsl.src (#embed fallback for compilers without #embed support) ──
set(DSL_EMBED_OUTPUT "${CMAKE_BINARY_DIR}/include/parser/dsl_embed.h")
set(DSL_EMBED_SCRIPT "${CMAKE_BINARY_DIR}/include/parser/gen_dsl_embed.py")
file(WRITE "${DSL_EMBED_SCRIPT}"
"import sys, pathlib
src = pathlib.Path(sys.argv[1]).read_bytes()
out = sys.argv[2]
with open(out, 'w') as f:
    f.write('// Generated from dsl.src - do not edit\\n')
    f.write('#pragma once\\n')
    f.write('#include <cstddef>\\n')
    f.write('namespace detail {\\n')
    f.write('inline constexpr unsigned char kDslData[] = {\\n')
    for i in range(0, len(src), 16):
        chunk = ', '.join(f'0x{b:02x}' for b in src[i:i+16])
        f.write(f'  {chunk},\\n')
    f.write('};\\n')
    f.write(f'inline constexpr std::size_t kDslSize = {len(src)};\\n')
    f.write('} // namespace detail\\n')
    print(f'Generated {len(src)} bytes -> {out}')
")
add_custom_command(
    OUTPUT "${DSL_EMBED_OUTPUT}"
    COMMAND Python3::Interpreter "${DSL_EMBED_SCRIPT}"
        "${CMAKE_SOURCE_DIR}/src/parser/dsl.src"
        "${DSL_EMBED_OUTPUT}"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/parser/dsl.src"
    COMMENT "[Embed] Generating dsl_embed.h from dsl.src..."
    VERBATIM
)
add_custom_target(generate_dsl_embed
    DEPENDS "${DSL_EMBED_OUTPUT}"
)