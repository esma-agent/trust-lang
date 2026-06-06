# include/trust/CMakeLists.txt
# C++20 module infrastructure (trust module)

# ── PCM build configuration ──
set(TRUST_PCM "${CMAKE_CURRENT_BINARY_DIR}/trust.pcm")
set(TRUST_PCM_OBJ "${CMAKE_CURRENT_BINARY_DIR}/trust_module.o")
set(TRUST_CPPM "${PROJECT_INCLUDE_DIR}/trust.cppm")

set(TRUST_MODULE_HEADERS
    types/types.hpp
    types/type_info.hpp
    types/forward.hpp
    types/value.hpp
    types/numbers.hpp
    types/integers.hpp
    types/strings.hpp
    types/templates.hpp

    types/rational.hpp
    types/tensors.hpp

    types/buildin.hpp

)
list(TRANSFORM TRUST_MODULE_HEADERS PREPEND "${PROJECT_INCLUDE_DIR}/")

set(TRUST_MODULE_GENERATED_HEADERS
    ${CMAKE_BINARY_DIR}/include/trust/version.h
)

# Generated parser output files (flex only; ast-generated files come via parser_generated)
set(TRUST_PARSER_GEN_DIR "${CMAKE_BINARY_DIR}/include/parser")
set(TRUST_PARSER_GEN_OUTPUTS
    "${TRUST_PARSER_GEN_DIR}/flex.gen.h"
)

set(TRUST_COMPILE_OPTIONS
    -std=c++${CMAKE_CXX_STANDARD}
    -I${PROJECT_INCLUDE_DIR}
    -I${TRUST_PARSER_GEN_DIR}
    -I${CMAKE_CURRENT_BINARY_DIR}
    -I${CMAKE_BINARY_DIR}/include
)

# Detect compiler family
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(TRUST_IS_CLANG TRUE)
else()
    set(TRUST_IS_CLANG FALSE)
endif()

if(TRUST_IS_CLANG)
    # ── Clang: two-step (PCM → .o) ──
    # Step 1: Precompile module to PCM
    add_custom_command(
        OUTPUT "${TRUST_PCM}"
        COMMAND ${CMAKE_CXX_COMPILER} ${TRUST_COMPILE_OPTIONS}
                -x c++-module --precompile
                ${TRUST_CPPM}
                -o ${TRUST_PCM}
        DEPENDS ${TRUST_CPPM} ${TRUST_MODULE_HEADERS} ${TRUST_MODULE_GENERATED_HEADERS} ${TRUST_PARSER_GEN_OUTPUTS} parser_generated
        COMMENT "[Clang] Precompiling module -> ${TRUST_PCM}"
        VERBATIM
    )

    # Step 2: Compile PCM to object file for linking
    add_custom_command(
        OUTPUT "${TRUST_PCM_OBJ}"
        COMMAND ${CMAKE_CXX_COMPILER} ${TRUST_COMPILE_OPTIONS}
                -fmodule-file=trust=${TRUST_PCM}
                -x c++-module
                ${TRUST_CPPM}
                -c -o ${TRUST_PCM_OBJ}
        DEPENDS ${TRUST_PCM} ${TRUST_CPPM}
        COMMENT "[Clang] Compiling module PCM -> ${TRUST_PCM_OBJ}"
        VERBATIM
    )

    # ── Function to attach trust module to a target (Clang) ──
    function(target_trust_module target)
        add_dependencies(${target} trust_pcm_target_all)
        target_sources(${target} PRIVATE ${TRUST_PCM_OBJ})
        set_source_files_properties(${TRUST_PCM_OBJ} PROPERTIES
            EXTERNAL_OBJECT true
            GENERATED true
        )
        target_compile_options(${target} PRIVATE
            -fmodule-file=trust=${TRUST_PCM}
        )
    endfunction()
else()
    # ── GCC: single-step compilation with -fmodules-ts ──
    # GCC generates both the .o file and CMI (gcm.cache/) in a single step.
    set(TRUST_MODULE_FLAGS "-fmodules-ts")
    add_custom_command(
        OUTPUT "${TRUST_PCM_OBJ}"
        COMMAND ${CMAKE_CXX_COMPILER} ${TRUST_COMPILE_OPTIONS}
                ${TRUST_MODULE_FLAGS}
                -c ${TRUST_CPPM}
                -o ${TRUST_PCM_OBJ}
        DEPENDS ${TRUST_CPPM} ${TRUST_MODULE_HEADERS} ${TRUST_MODULE_GENERATED_HEADERS} ${TRUST_PARSER_GEN_OUTPUTS} parser_generated
        COMMENT "[GCC] Compiling module -> ${TRUST_PCM_OBJ}"
        VERBATIM
    )

    # ── Function to attach trust module to a target (GCC) ──
    function(target_trust_module target)
        add_dependencies(${target} trust_pcm_target_all)
        target_sources(${target} PRIVATE ${TRUST_PCM_OBJ})
        set_source_files_properties(${TRUST_PCM_OBJ} PROPERTIES
            EXTERNAL_OBJECT true
            GENERATED true
        )
        target_compile_options(${target} PRIVATE
            ${TRUST_MODULE_FLAGS}
        )
    endfunction()
endif()

add_custom_target(trust_pcm_target_all ALL DEPENDS ${TRUST_PCM_OBJ})
add_dependencies(trust_pcm_target_all parser_lib)
