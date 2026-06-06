# include/runtime/CMakeLists.txt
# C++20 module infrastructure (trust.runtime module)

# ── PCM build configuration ──
set(RUNTIME_PCM_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(RUNTIME_PCM "${RUNTIME_PCM_DIR}/trust.runtime.pcm")
set(RUNTIME_PCM_OBJ "${RUNTIME_PCM_DIR}/trust.runtime_module.o")
set(RUNTIME_CPPM "${CMAKE_CURRENT_SOURCE_DIR}/../runtime.cppm")

set(RUNTIME_MODULE_HEADERS
    types/rational.hpp
)
list(TRANSFORM RUNTIME_MODULE_HEADERS PREPEND "${PROJECT_INCLUDE_DIR}/")

# Resolve GMP include dirs from gmp_interface target
get_target_property(_gmp_includes gmp_interface INTERFACE_INCLUDE_DIRECTORIES)

set(RUNTIME_COMPILE_OPTIONS
    -std=c++${CMAKE_CXX_STANDARD}
    -I${PROJECT_INCLUDE_DIR}
    -I${RUNTIME_PCM_DIR}
    -I${_gmp_includes}
)

# Detect compiler family (set by trust module build)
if(NOT DEFINED TRUST_IS_CLANG)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(TRUST_IS_CLANG TRUE)
    else()
        set(TRUST_IS_CLANG FALSE)
    endif()
endif()

if(TRUST_IS_CLANG)
    # Step 1: Precompile module to PCM (Clang)
    add_custom_command(
        OUTPUT "${RUNTIME_PCM}"
        COMMAND ${CMAKE_CXX_COMPILER} ${RUNTIME_COMPILE_OPTIONS}
                -x c++-module --precompile
                ${RUNTIME_CPPM}
                -o ${RUNTIME_PCM}
        DEPENDS ${RUNTIME_CPPM} ${RUNTIME_MODULE_HEADERS}
        COMMENT "[Clang] Precompiling runtime module -> ${RUNTIME_PCM}"
        VERBATIM
    )

    # Step 2: Compile PCM to object file for linking (Clang)
    add_custom_command(
        OUTPUT "${RUNTIME_PCM_OBJ}"
        COMMAND ${CMAKE_CXX_COMPILER} ${RUNTIME_COMPILE_OPTIONS}
                -fmodule-file=trust.runtime=${RUNTIME_PCM}
                -x c++-module
                ${RUNTIME_CPPM}
                -c -o ${RUNTIME_PCM_OBJ}
        DEPENDS ${RUNTIME_PCM} ${RUNTIME_CPPM}
        COMMENT "[Clang] Compiling runtime module PCM -> ${RUNTIME_PCM_OBJ}"
        VERBATIM
    )
else()
    # Single-step compilation with -fmodules-ts (GCC)
    add_custom_command(
        OUTPUT "${RUNTIME_PCM_OBJ}"
        COMMAND ${CMAKE_CXX_COMPILER} ${RUNTIME_COMPILE_OPTIONS}
                -fmodules-ts
                -c ${RUNTIME_CPPM}
                -o ${RUNTIME_PCM_OBJ}
        DEPENDS ${RUNTIME_CPPM} ${RUNTIME_MODULE_HEADERS}
        COMMENT "[GCC] Compiling runtime module -> ${RUNTIME_PCM_OBJ}"
        VERBATIM
    )
endif()

add_custom_target(runtime_pcm_target_all ALL DEPENDS ${RUNTIME_PCM_OBJ})

# ── Library: runtime_lib (torch-independent, static) ──
add_library(runtime_lib STATIC
    ${PROJECT_SOURCE_DIR}/runtime/rational.cpp
)
set_target_properties(runtime_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(runtime_lib PUBLIC types_lib gmp_interface trust_include_dirs)

# ── Library: tensor_cpu (torch-dependent, shared plugin) ──
if(WITH_TORCH)
    add_library(tensor_cpu SHARED
        ${PROJECT_SOURCE_DIR}/runtime/tensor.cpp
    )

    target_include_directories(tensor_cpu PUBLIC
        ${PROJECT_INCLUDE_DIR}
    )
    target_include_directories(tensor_cpu SYSTEM PUBLIC
        ${LIBTORCH_INCLUDE_DIRS}
    )

    target_link_directories(tensor_cpu PUBLIC
        ${LIBTORCH_LIB_DIRS}
    )

    target_link_libraries(tensor_cpu PUBLIC
        runtime_lib
        torch
        c10
        torch_cpu
    )

    set_target_properties(tensor_cpu PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    target_compile_definitions(tensor_cpu PRIVATE
        TRUST_PLUGIN_DIR="${CMAKE_BINARY_DIR}"
    )

    add_dependencies(tensor_cpu runtime_pcm_target_all)
endif()

# ── Function to attach runtime module to a target ──
function(target_runtime_module target)
    set(_runtime_pcm_obj "${CMAKE_CURRENT_BINARY_DIR}/trust.runtime_module.o")
    add_dependencies(${target} runtime_pcm_target_all)
    target_sources(${target} PRIVATE ${_runtime_pcm_obj})
    set_source_files_properties(${_runtime_pcm_obj} PROPERTIES
        EXTERNAL_OBJECT true
        GENERATED true
    )
    if(TRUST_IS_CLANG)
        set(_runtime_pcm_path "${CMAKE_CURRENT_BINARY_DIR}/trust.runtime.pcm")
        target_compile_options(${target} PRIVATE
            -fmodule-file=trust.runtime=${_runtime_pcm_path}
        )
    else()
        target_compile_options(${target} PRIVATE
            -fmodules-ts
        )
    endif()
endfunction()
