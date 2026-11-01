
#include "cmath/language/LanguageEngine.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Test = std::pair<std::string, std::function<void()>>;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticSummary(const std::vector<cmath::Diagnostic>& diagnostics) {
    std::string result;

    for (const auto& diagnostic : diagnostics) {
        result += diagnostic.code + ": " + diagnostic.message + "\n";
    }

    return result;
}

void expectOutput(
    const std::string& source,
    const std::string& expected
) {
    cmath::LanguageEngine engine;

    const auto compiled = engine.compile(source);

    require(
        compiled.succeeded(),
        "Compilation failed:\n" + diagnosticSummary(compiled.diagnostics)
    );

    const auto execution = engine.execute(*compiled.program);

    require(
        execution.succeeded,
        "Execution failed:\n" + diagnosticSummary(execution.diagnostics)
    );

    require(
        execution.output == expected,
        "Unexpected output.\nExpected:\n" + expected +
        "\nReceived:\n" + execution.output
    );
}

void expectCompileFailure(const std::string& source) {
    cmath::LanguageEngine engine;

    const auto compiled = engine.compile(source);

    require(
        !compiled.succeeded(),
        "Expected compilation to fail, but it succeeded."
    );

    require(
        !compiled.diagnostics.empty(),
        "Compilation failed without producing diagnostics."
    );
}

void expectRuntimeFailure(const std::string& source) {
    cmath::LanguageEngine engine;

    const auto compiled = engine.compile(source);

    require(
        compiled.succeeded(),
        "Expected compilation to succeed:\n" +
        diagnosticSummary(compiled.diagnostics)
    );

    const auto execution = engine.execute(*compiled.program);

    require(
        !execution.succeeded,
        "Expected execution to fail, but it succeeded."
    );

    require(
        !execution.diagnostics.empty(),
        "Runtime failure produced no diagnostics."
    );
}

} // namespace

int main() {
    const std::vector<Test> tests{
        {
            "Integer arithmetic",
            [] {
                expectOutput(
                    R"(
domain Integer

let x: Integer = 10
let y: Integer = 20

print(x + y)
)",
                    "30\n"
                );
            }
        },
        {
            "Rational arithmetic",
            [] {
                expectOutput(
                    R"(
domain Rational

let a: Rational = 1 / 3
let b: Rational = 1 / 7

print(a + b)
)",
                    "10/21\n"
                );
            }
        },
        {
            "Real arithmetic",
            [] {
                expectOutput(
                    R"(
domain Real

let x: Real = 10
let y: Real = 20

print(x * y)
)",
                    "200\n"
                );
            }
        },
        {
            "String literals",
            [] {
                expectOutput(
                    R"(
domain Real

print("Hello, CMath!")
)",
                    "Hello, CMath!\n"
                );
            }
        },
        {
            "Boolean literals",
            [] {
                expectOutput(
                    R"(
domain Real

print(true)
print(false)
)",
                    "true\nfalse\n"
                );
            }
        },
        {
            "User-defined functions",
            [] {
                expectOutput(
                    R"(
domain Real

def double(x: Real) -> Real:
    return x * 2

print(double(15))
)",
                    "30\n"
                );
            }
        },
        {
            "Static division by zero",
            [] {
                expectCompileFailure(
                    R"(
domain Real

print(1 / 0)
)"
                );
            }
        },
        {
            "Runtime division by zero",
            [] {
                expectRuntimeFailure(
                    R"(
domain Real

var denominator: Real = 0
print(1 / denominator)
)"
                );
            }
        },
        {
            "Invalid type assignment",
            [] {
                expectCompileFailure(
                    R"(
domain Real

let x: Integer = 1.5
)"
                );
            }
        },
        {
            "Negative integer exponent",
            [] {
                expectOutput(
                    R"(
domain Rational

print(2 ^ -3)
)",
                    "1/8\n"
                );
            }
        }
    };

    std::size_t passed = 0;
    std::size_t failed = 0;

    std::cout << "\nCMath Language Tests\n";
    std::cout << "====================\n\n";

    for (const auto& [name, test] : tests) {
        try {
            test();

            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failed;

            std::cout
                << "[FAIL] "
                << name
                << "\n       "
                << exception.what()
                << '\n';
        }
    }

    std::cout << "\n====================\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    return failed == 0 ? 0 : 1;
}
