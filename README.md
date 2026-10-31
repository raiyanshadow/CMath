# CMath Language 0.2 — LLVM native compiler preview

This package extends the CMath Language 0.1 frontend with an LLVM native-code
backend and the `mathmake` command-line tool. The lexer, parser, semantic
analyzer, interpreter, and private `ProgramIR` remain the same as Language 0.1.

The native compiler is an early vertical slice. It emits a native object file
with LLVM's C++ API, then `mathmake` invokes `clang++` as the platform linker
driver to produce an executable. On Windows the default output is a `.exe`.

## Requirements

- CMake 3.20 or newer.
- A C++20 compiler compatible with the LLVM SDK.
- LLVM development SDK (headers, libraries, and `LLVMConfig.cmake`).
- `clang++` available on `PATH`, discoverable by CMake, or specified through
  `CMATH_CLANGXX` / `--clangxx`.
- On Windows, a working Windows SDK and MSVC-compatible linker/runtime must be
  available to `clang++`.

The ordinary LLVM Windows installer can provide `clang.exe` and `clang++.exe`
without providing the LLVM C++ development headers and CMake package. The native
compiler needs the development SDK, not just the Clang executable.

## Configure and build

If `LLVMConfig.cmake` is at
`C:/LLVM-23.1.4/lib/cmake/llvm/LLVMConfig.cmake`, configure with:

```powershell
cmake -S . -B build -DLLVM_DIR="C:/LLVM-23.1.4/lib/cmake/llvm"
cmake --build build --config Release
```

This builds:

- `cmath-language`: the original CMath Language 0.1 frontend and interpreter.
- `cmath-compiler`: LLVM IR generation and native object-file emission.
- `mathmake`: the command-line build tool.

`LLVM_DIR` must point to the directory containing `LLVMConfig.cmake`, not to the
LLVM `bin` directory. The CMake configuration searches for `clang++` in LLVM's
tools directory. You can override the linker driver at runtime:

```powershell
$env:CMATH_CLANGXX = "C:\Program Files\LLVM\bin\clang++.exe"
```

Or pass `--clangxx "C:\Program Files\LLVM\bin\clang++.exe"` to `mathmake`.

## Using mathmake

Compile a source file into an executable:

```powershell
mathmake main.cmath
```

Equivalent explicit command:

```powershell
mathmake build main.cmath
```

By default this creates:

```text
main.cmath
main.exe
build/cmath/main.obj
```

Run it immediately after building:

```powershell
mathmake run main.cmath
```

Or execute it independently:

```powershell
.\main.exe
```

Check syntax, static types, and domain rules without generating native code:

```powershell
mathmake check main.cmath
```

Choose an output name and code-generation optimization level:

```powershell
mathmake build main.cmath -o out\geometry.exe -O2
```

Remove the generated `build/cmath` native-object directory from the current working directory (the CMake build directory is preserved):

```powershell
mathmake clean
```

## First native test

`examples/native_smoke.cmath`:

```python
domain Real

let x: Real = 10
let y: Real = 20
print(x + y)
```

Build and execute:

```powershell
mathmake examples\native_smoke.cmath
.\examples\native_smoke.exe
```

Expected output:

```text
30
```

A second example exercises a user-defined function and real-valued math:

```powershell
mathmake examples\native_functions.cmath
.\examples\native_functions.exe
```

Expected output:

```text
5
```

## Native backend support in this preview

The initial native backend supports:

- `Integer` and `Real` local variables, function parameters, and return values.
- `Boolean` and `String` local values, function parameters, and return values.
- Widening `Integer` to `Real`.
- Rational decimal literals when the surrounding context explicitly widens
  them to `Real` (for example, `let x: Real = 4.5`). This is an approximation
  because a native `double` is not an exact Rational representation.
- Integer `+`, `-`, `*`, `%`; Real `+`, `-`, `*`, `/`, `%`, `^`.
- Numeric comparisons, Boolean equality, and String comparisons.
- User-defined functions, local declarations, assignments, and returns.
- `print()` for Integer, Real, Boolean, and String values.
- Real `sqrt`, `sin`, `cos`, `tan`, `exp`, `log`, `abs`, `floor`, and
  `approximate`; Integer `abs` and `floor`.

The backend deliberately reports a native-code diagnostic rather than silently
miscompiling unsupported features. Not yet supported natively:

- Exact Rational variables, function parameters/returns, and printing.
- Complex values and complex-valued built-ins.
- `to_integer_exact()`.
- Integer exponentiation.
- String concatenation.
- Full interpreter-equivalent overflow diagnostics and runtime error messages.

These are limitations of the first native backend, not changes to the language
frontend or interpreter. Programs using unsupported features can still be
checked and run with the existing `LanguageEngine::execute()` interpreter API.

## How the pipeline works

```text
.cmath source
    -> LanguageEngine (lexer, parser, semantic analysis)
    -> opaque CompiledProgram / private ProgramIR
    -> LLVMBackend (LLVM IR and native object file)
    -> clang++ linker driver
    -> native executable
```

`CompiledProgram` keeps its IR private. `LLVMBackend` is granted internal
friend access so the public language API does not expose frontend internals.
The desktop Qt application and `mathmake` can both use the same
`LanguageEngine` and compiler library.

## Integrating into an existing CMath root project

The easiest integration is to keep this directory as a subdirectory and add
this to your existing root `CMakeLists.txt`:

```cmake
add_subdirectory(path/to/CMathLanguage01)
```

Then link your CLI or application target as needed:

```cmake
target_link_libraries(your-cmath-target PRIVATE CMath::Language CMath::Compiler)
```

Configure your root project with the same `-DLLVM_DIR=...` argument. If your
root project already contains copies of the Language 0.1 sources, do not build
two copies of them into the same executable; either use this subdirectory's
`cmath-language` target or keep your own target and copy the additions listed
below into it.

## File change list

Modified:

- `CMakeLists.txt`
- `README.md`
- `include/cmath/language/CompiledProgram.h` (adds internal LLVM backend friendship)

New:

- `include/cmath/compiler/LLVMBackend.h`
- `include/cmath/compiler/NativeCompileResult.h`
- `src/compiler/LLVMBackend.cpp`
- `cmake/CMathNativeCompiler.cmake`
- `tools/mathmake/src/BuildSystem.h`
- `tools/mathmake/src/BuildSystem.cpp`
- `tools/mathmake/src/main.cpp`
- `examples/native_smoke.cmath`
- `examples/native_functions.cmath`
