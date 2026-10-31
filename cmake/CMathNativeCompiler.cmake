# Include this file after defining the CMath language/core target, then call:
#   cmath_add_native_compiler(cmath-language)
# or, in a larger CMath tree:
#   cmath_add_native_compiler(cmath-core)

set(_CMATH_NATIVE_COMPILER_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
option(CMATH_BUILD_MATHMAKE "Build the mathmake command-line tool" ON)

function(cmath_add_native_compiler language_target)
    if(NOT TARGET ${language_target})
        message(FATAL_ERROR
            "cmath_add_native_compiler(): '${language_target}' is not a CMake target. "
            "Create the language/core target before including this module.")
    endif()
    if(TARGET cmath-compiler)
        message(FATAL_ERROR "Target 'cmath-compiler' already exists.")
    endif()

    find_package(zstd CONFIG REQUIRED)
    find_package(LLVM REQUIRED CONFIG)
    if(MSVC)
        set(_CMATH_MSVC_RUNTIME
            "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )

        set_property(TARGET ${language_target}
            PROPERTY MSVC_RUNTIME_LIBRARY "${_CMATH_MSVC_RUNTIME}"
        )
    endif()
    message(STATUS "CMath native compiler: LLVM ${LLVM_PACKAGE_VERSION}")

    add_library(cmath-compiler STATIC
        "${_CMATH_NATIVE_COMPILER_ROOT}/src/compiler/LLVMBackend.cpp"
    )
    if(MSVC)
        set_property(TARGET cmath-compiler
            PROPERTY MSVC_RUNTIME_LIBRARY "${_CMATH_MSVC_RUNTIME}"
        )
    endif()
    add_library(CMath::Compiler ALIAS cmath-compiler)
    target_compile_features(cmath-compiler PUBLIC cxx_std_20)
    target_include_directories(cmath-compiler
        PUBLIC "${_CMATH_NATIVE_COMPILER_ROOT}/include"
        PRIVATE "${_CMATH_NATIVE_COMPILER_ROOT}/src/language"
    )
    target_include_directories(cmath-compiler SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(cmath-compiler PUBLIC ${language_target})

    if(TARGET LLVM)
        target_link_libraries(cmath-compiler PRIVATE LLVM)
    else()
        llvm_map_components_to_libnames(CMATH_LLVM_LIBS
            Core Support Target MC MCParser CodeGen AsmPrinter native
        )
        target_link_libraries(cmath-compiler PRIVATE ${CMATH_LLVM_LIBS})
    endif()

    if(LLVM_DEFINITIONS)
        separate_arguments(CMATH_LLVM_DEFINITION_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
        foreach(definition IN LISTS CMATH_LLVM_DEFINITION_LIST)
            if(definition MATCHES "^-D(.+)$")
                target_compile_definitions(cmath-compiler PRIVATE "${CMAKE_MATCH_1}")
            else()
                target_compile_options(cmath-compiler PRIVATE "${definition}")
            endif()
        endforeach()
    endif()

    if(MSVC)
        target_compile_options(cmath-compiler PRIVATE /W4 /permissive-)
    else()
        target_compile_options(cmath-compiler PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(CMATH_BUILD_MATHMAKE)
        find_program(CMATH_CLANGXX_EXECUTABLE
            NAMES clang++ clang++.exe
            HINTS "${LLVM_TOOLS_BINARY_DIR}"
        )

        add_executable(mathmake
            "${_CMATH_NATIVE_COMPILER_ROOT}/tools/mathmake/src/main.cpp"
            "${_CMATH_NATIVE_COMPILER_ROOT}/tools/mathmake/src/BuildSystem.cpp"
        )
        if(MSVC)
            set_property(TARGET mathmake
                PROPERTY MSVC_RUNTIME_LIBRARY "${_CMATH_MSVC_RUNTIME}"
            )
        endif()
        target_compile_features(mathmake PRIVATE cxx_std_20)
        target_include_directories(mathmake PRIVATE
            "${_CMATH_NATIVE_COMPILER_ROOT}/include"
            "${_CMATH_NATIVE_COMPILER_ROOT}/tools/mathmake/src"
        )
        target_link_libraries(mathmake PRIVATE cmath-compiler ${language_target})

        if(CMATH_CLANGXX_EXECUTABLE)
            file(TO_CMAKE_PATH "${CMATH_CLANGXX_EXECUTABLE}" CMATH_CLANGXX_CMAKE_PATH)
            target_compile_definitions(mathmake PRIVATE
                CMATH_DEFAULT_CLANGXX_PATH="${CMATH_CLANGXX_CMAKE_PATH}"
            )
        else()
            message(WARNING
                "clang++ was not found by CMake. mathmake will search PATH at runtime "
                "or use the CMATH_CLANGXX environment variable.")
        endif()

        if(MSVC)
            target_compile_options(mathmake PRIVATE /W4 /permissive-)
        else()
            target_compile_options(mathmake PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endfunction()
