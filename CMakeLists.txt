cmake_minimum_required(VERSION 3.20)
project(CMathLanguage LANGUAGES CXX)

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
