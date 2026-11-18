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
            "Mutually recursive functions",
            [] {
                expectOutput(
                    R"(
domain Integer

def is_even(n: Integer) -> Boolean:
    if n == 0:
        return true
    else:
        return is_odd(n - 1)

def is_odd(n: Integer) -> Boolean:
    if n == 0:
        return false
    else:
        return is_even(n - 1)

print(is_even(10))
print(is_odd(10))
print(is_even(9))
print(is_odd(9))
)",
                    "true\nfalse\nfalse\ntrue\n"
                );
            }
        },
        {
            "Lexical block scopes",
            [] {
                expectOutput(
                    R"(
domain Integer

def scoped(x: Integer) -> Integer:
    let base: Integer = x

    if true:
        let inner: Integer = x + 2
        print(inner)

    return base

print(scoped(5))
)",
                    "7\n5\n"
                );
            }
        },
        {
            "Block variables do not escape their scope",
            [] {
                expectCompileFailure(
                    R"(
domain Integer

def invalid() -> Integer:
    if true:
        let hidden: Integer = 10

    return hidden
)"
                );
            }
        },
        {
            "Complete return-path analysis",
            [] {
                expectOutput(
                    R"(
domain Integer

def classify(n: Integer) -> Integer:
    if n > 0:
        return 1
    elif n < 0:
        return -1
    else:
        return 0

print(classify(10))
print(classify(-10))
print(classify(0))
)",
                    "1\n-1\n0\n"
                );
            }
        },
        {
            "Incomplete return path rejected",
            [] {
                expectCompileFailure(
                    R"(
domain Integer

def invalid(n: Integer) -> Integer:
    if n > 0:
        return 1
)"
                );
            }
        },
        {
    "Nested function type signatures",
    [] {
        expectOutput(
            R"(
domain Integer

def preserve(callback: fn(x: Integer) -> Real) -> fn(Integer) -> Real:
    return callback

print(1)
)",
            "1\n"
        );
    }
},
{
    "Nested function type mismatch",
    [] {
        expectCompileFailure(
            R"(
domain Integer

def invalid(callback: fn(Integer) -> Real) -> fn(Integer) -> Integer:
    return callback
)"
        );
    }
},
{
    "Unknown type inside function signature",
    [] {
        expectCompileFailure(
            R"(
domain Integer

def invalid(callback: fn(Integer) -> Missing) -> Integer:
    return 0
)"
        );
    }
},
{
    "Lambda declaration and invocation",
    [] {
        expectOutput(
            R"(
domain Integer

lambda double = fn(x: Integer) -> Integer:
    return x * 2

print(double(5))
)",
            "10\n"
        );
    }
},
{
    "Higher-order function with lambda argument",
    [] {
        expectOutput(
            R"(
domain Integer

def apply(callback: fn(Integer) -> Integer, value: Integer) -> Integer:
    return callback(value)

lambda triple = fn(x: Integer) -> Integer:
    return x * 3

print(apply(triple, 5))
)",
            "15\n"
        );
    }
},
{
    "Returned closure captures local variable",
    [] {
        expectOutput(
            R"(
domain Integer

def make_adder(x: Integer) -> fn(Integer) -> Integer:
    lambda add = fn(y: Integer) -> Integer:
        return x + y

    return add

let add_five = make_adder(5)

print(add_five(10))
print(add_five(20))
)",
            "15\n25\n"
        );
    }
},
{
    "Mutable captured variable persists between calls",
    [] {
        expectOutput(
            R"(
domain Integer

def make_counter() -> fn() -> Integer:
    var count: Integer = 0

    lambda next = fn() -> Integer:
        count += 1
        return count

    return next

let counter = make_counter()

print(counter())
print(counter())
print(counter())
)",
            "1\n2\n3\n"
        );
    }
},
{
    "Function argument signature mismatch",
    [] {
        expectCompileFailure(
            R"(
domain Integer

def apply(callback: fn(Integer) -> Integer) -> Integer:
    return callback(5)

lambda wrong = fn(x: Real) -> Real:
    return x

print(apply(wrong))
)"
        );
    }
},
{
    "Function value rejected as scalar argument",
    [] {
        expectCompileFailure(
            R"(
domain Integer

def expects_integer(x: Integer) -> Integer:
    return x

lambda callback = fn(x: Integer) -> Integer:
    return x * 2

print(expects_integer(callback))
)"
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
