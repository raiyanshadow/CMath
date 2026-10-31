# CMath Language 0.1

A small, statically typed mathematical DSL implementation for CMath.

This is a complete first language slice, not a finished general-purpose
programming language. Its purpose is to establish the front end, type system,
IR boundary, interpreter, diagnostics, and public `LanguageEngine` API.

## Build

```sh
cmake -S . -B build
cmake --build build
```

The library target is `cmath-language` (also available as `CMath::Language`). Public headers are under
`include/cmath/language`; private compiler implementation is under
`src/language`.

### Add it to an existing CMake project

Add the five implementation files to your existing `cmath-core` target, or
build this directory as a subdirectory. If building it as a subdirectory:

```cmake
add_subdirectory(path/to/CMathLanguage01)
target_link_libraries(your-cmath-target PRIVATE cmath-language)
```

If you already have your own root `CMakeLists.txt`, do not replace it with this
package's standalone one. Copy the sources/headers into your existing tree and
add these source files to the appropriate target:

```text
src/language/Lexer.cpp
src/language/Parser.cpp
src/language/SemanticAnalyzer.cpp
src/language/Interpreter.cpp
src/language/LanguageEngine.cpp
```

The target needs C++20 and include paths `include/` (public) and
`src/language/` (private).

### Add it to an existing qmake project

```qmake
CONFIG += c++20
INCLUDEPATH += include src/language

SOURCES += \
    src/language/Lexer.cpp \
    src/language/Parser.cpp \
    src/language/SemanticAnalyzer.cpp \
    src/language/Interpreter.cpp \
    src/language/LanguageEngine.cpp

HEADERS += \
    include/cmath/language/Diagnostic.h \
    include/cmath/language/CompileOptions.h \
    include/cmath/language/ExecutionOptions.h \
    include/cmath/language/CompiledProgram.h \
    include/cmath/language/CompileResult.h \
    include/cmath/language/ExecutionResult.h \
    include/cmath/language/LanguageEngine.h \
    src/language/LanguageInternal.h
```

## C++ API

```cpp
#include <iostream>
#include <string>
#include "cmath/language/LanguageEngine.h"

int main() {
    const std::string source = R"CMATH(
domain Real
let x: Real = sqrt(2)
print(x)
)CMATH";

    cmath::LanguageEngine engine;
    const auto compiled = engine.compile(source);

    if (!compiled.succeeded()) {
        for (const auto& diagnostic : compiled.diagnostics) {
            std::cerr << diagnostic.code << " at "
                      << diagnostic.location.line << ':'
                      << diagnostic.location.column << ": "
                      << diagnostic.message << '\n';
        }
        return 1;
    }

    const auto result = engine.execute(*compiled.program);
    if (!result.succeeded) {
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 2;
    }

    std::cout << result.output;
}
```

## Language 0.1 syntax

- Every source file begins with `domain Integer`, `domain Rational`,
  `domain Real`, or `domain Complex` (comments and blank lines may precede it).
- Variables use `let name: Type = expression` or
  `var name: Type = expression`. `let` is immutable; `var` is mutable.
- A type annotation may be omitted and inferred, for example `let n = 12`.
- Function parameters and return types are explicit:
  `def f(x: Real) -> Real:` followed by an indented body.
- Function bodies must end in a `return` statement. Nested functions and
  control-flow statements are not supported yet.
- Expressions support numbers, strings, booleans, names, member access,
  function calls, unary `+`/`-`, arithmetic `+ - * / % ^`, and comparisons
  `== != < > <= >=`.
- `^` means exponentiation. Its exponent must have type `Integer`. A statically
  known nonnegative integer power of an Integer returns Integer; other integer
  powers return Rational so negative exponents can be represented exactly.
- `#` begins a comment. Indentation uses spaces, not tabs.

## Types and conversion rules

Built-in types are `Integer`, `Rational`, `Real`, `Complex`, `Boolean`,
`String`, and `Unit` (the last is used for functions that return no value).

Numeric literals are typed as follows:

- `5` is `Integer`.
- `4.5` is an exact `Rational` literal (`9/2`), within the current 64-bit
  numerator/denominator implementation limits.
- Scientific notation such as `1e-3` is `Real`.

Implicit numeric conversions widen only:

```text
Integer -> Rational -> Real -> Complex
```

Narrowing conversions are rejected. Thus `let z: Integer = 4.5` is a compile
error. Use `to_integer_exact(value)` for an explicit checked conversion; it
fails at runtime unless the value is exactly an integer. `floor(value)` is a
separate, non-exact rounding operation.

`domain` declares the file's default mathematical context; it does not force
all variables to have that type. Explicitly typed Complex values are permitted
inside a `domain Real` file. In Language 0.1, `domain Complex` also makes
transcendental functions of non-complex numeric inputs (`sqrt`, `sin`, `cos`,
`tan`, `exp`, `log`) return Complex values. Otherwise they return Real values
for non-complex inputs. This policy can be refined when user-defined domains
and algebraic extensions are implemented.

## Built-ins

- `print(...values)`
- `sqrt(x)`, `sin(x)`, `cos(x)`, `tan(x)`, `exp(x)`, `log(x)`
- `abs(x)`, `floor(x)`, `approximate(x)`, `to_integer_exact(x)`
- Constants `pi`, `e`, `Complex.i`, and `Complex.Imaginary`

Trigonometric functions use radians.

## Current boundaries

This release deliberately does not yet include loops, conditionals, arrays,
classes, lambdas, imports, custom type declarations, custom algebraic domains,
operator declarations, extension loading, or C++ code generation. Those should
be introduced as later language features after the core semantics are tested.

`Integer` and `Rational` currently use signed 64-bit storage; `Real` and
`Complex` use `double` and `std::complex<double>`. The implementation checks
common integer/rational overflows, but it is not an arbitrary-precision number
system. The domain/type system is intentionally separate: static checking
handles type compatibility, while value-dependent mathematical preconditions
are checked at compile time when a simple constant expression is recognized,
and otherwise at runtime.

The interpreter has instruction and call-depth budgets and exposes no direct
filesystem, process, or network operations. This is a useful baseline, not a
complete security sandbox for future native extensions.

## Example type/domain diagnostics

```python
domain Real
let imaginary = Complex.Imaginary
let z: Complex = 1 + imaginary^2  # valid: explicit Complex values are allowed
let n: Integer = 4.5             # compile-time type error
let bad: Real = sqrt(-1)         # compile-time domain error
```

A variable-dependent precondition is checked when the expression executes:

```python
domain Real
var x: Real = -1
let y: Real = sqrt(x)            # runtime domain error
```

Set `domain Complex` when real-input transcendental operations should use the
complex extension by default.
