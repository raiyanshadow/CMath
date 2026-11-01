cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(CMathLanguage VERSION 0.2.0 LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

option(CMATH_BUILD_MATHMAKE "Build the mathmake command-line tool" ON)
option(CMATH_BUILD_NATIVE_COMPILER "Build the LLVM native compiler backend" ON)

add_library(cmath-language STATIC
    src/language/Lexer.cpp
    src/language/Parser.cpp
    src/language/SemanticAnalyzer.cpp
    src/language/Interpreter.cpp
    src/language/LanguageEngine.cpp
)
add_library(CMath::Language ALIAS cmath-language)

target_compile_features(cmath-language PUBLIC cxx_std_20)
target_include_directories(cmath-language
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/language
)
set_target_properties(cmath-language PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(cmath-language PRIVATE /W4 /permissive-)
else()
    target_compile_options(cmath-language PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMATH_BUILD_NATIVE_COMPILER)
    include(cmake/CMathNativeCompiler.cmake)
    cmath_add_native_compiler(cmath-language)
endif()

include(GNUInstallDirs)
install(TARGETS cmath-language
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(TARGET cmath-compiler)
    install(TARGETS cmath-compiler
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
if(TARGET mathmake)
    install(TARGETS mathmake
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(cmath-language-tests
        tests/LanguageTests.cpp
    )

    target_compile_features(cmath-language-tests
        PRIVATE cxx_std_20
    )

    target_link_libraries(cmath-language-tests
        PRIVATE CMath::Language
    )

    if(MSVC)
        target_compile_options(cmath-language-tests
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(cmath-language-tests
            PRIVATE -Wall -Wextra -Wpedantic
        )
    endif()

    add_test(
        NAME cmath-language-tests
        COMMAND cmath-language-tests
    )
endif()