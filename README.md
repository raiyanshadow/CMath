# CMath Language 0.2 — LLVM Native Compiler

CMath is a C++20 implementation of a mathematical programming language with a lexer, parser, semantic analyzer, interpreter, and LLVM native-code compiler backend.

Version 0.2 introduces the `mathmake` command-line tool and an initial native compilation pipeline.

The native backend emits an object file using LLVM's C++ API. `mathmake` then invokes `clang++` as the platform linker driver to produce a native executable.

On Windows, the default output is an `.exe` file.

---

## Using the prebuilt Windows release

Download the latest Windows x64 release:

https://github.com/raiyanshadow/CMath/releases

Extract the ZIP archive. Its contents include:

```text
CMath/
    mathmake.exe
    LLVM.dll
    ...
    README.md
    LICENSE

    examples/
        native_smoke.cmath
        native_functions.cmath
```

Additional LLVM runtime DLLs are included when provided by the LLVM distribution.

### Requirements

To run the prebuilt `mathmake.exe`:

- Windows x64.
- Required Windows system runtime components.

The release archive includes LLVM runtime DLLs from the LLVM distribution used to build CMath.

You do not need CMake, a C++ compiler, or the LLVM development headers to use the prebuilt `mathmake.exe`.

### Requirements for native compilation

To compile `.cmath` programs into native executables, you additionally need:

- `clang++` compatible with the LLVM 23.1.4 toolchain used by this release.
- Visual Studio 2022 Build Tools or a compatible MSVC toolchain.
- A Windows SDK.
- The MSVC linker and associated libraries.

CMath does not package `clang++.exe` or the complete Visual Studio toolchain.

Make sure `clang++` is available through PATH:

```powershell
clang++ --version
```

Alternatively, explicitly configure its location:

```powershell
$env:CMATH_CLANGXX = "C:\Program Files\LLVM\bin\clang++.exe"
```

You can also specify it directly using `--clangxx`.

---

## Building CMath from source

### Requirements

- CMake 3.20 or newer.
- C++20-compatible compiler.
- LLVM 23.1.4 development SDK.
- LLVM CMake configuration (`LLVMConfig.cmake`).
- zstd development package.
- zlib development package.
- `clang++`.
- On Windows, Visual Studio 2022 Build Tools and the Windows SDK.

The ordinary LLVM Windows installer may provide `clang.exe` and `clang++.exe` without providing the LLVM C++ development headers and CMake package.

The native compiler requires the development SDK, not just the Clang executable.

### Configure and build

For a Windows installation using vcpkg:

```powershell
$llvmDir = "C:/LLVM-23.1.4/lib/cmake/llvm"
$vcpkg = "C:/vcpkg"
$installed = "$vcpkg/installed/x64-windows-static"

cmake -S . -B build `
    -G "Visual Studio 17 2022" `
    -A x64 `
    "-DLLVM_DIR=$llvmDir" `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static" `
    "-Dzstd_DIR=$installed/share/zstd" `
    "-DZLIB_ROOT=$installed"
```

When required by LLVM's Windows CMake configuration, also supply `MSVC_DIA_SDK_DIR` pointing to the Visual Studio DIA SDK.

Build:

```powershell
cmake --build build --config Release --target mathmake --parallel
```

This builds:

- `cmath-language`: language frontend and interpreter.
- `cmath-compiler`: LLVM IR generation and native object-file emission.
- `mathmake`: command-line build tool.

The LLVM development installation is only required when building CMath itself. It is not required when using the downloaded release.

---

## Using mathmake

### Build a program

```powershell
mathmake main.cmath
```

Equivalent explicit command:

```powershell
mathmake build main.cmath
```

By default, this creates:

```text
main.cmath
main.exe
build/cmath/main.obj
```

### Run a program

Compile and execute:

```powershell
mathmake run main.cmath
```

Or execute the generated executable independently:

```powershell
.\main.exe
```

### Check a program

Check syntax, types, and mathematical domain rules without native compilation:

```powershell
mathmake check main.cmath
```

This command does not require `clang++`.

### Choose an output path

```powershell
mathmake build main.cmath -o out\geometry.exe
```

### Choose an optimization level

```powershell
mathmake build main.cmath -O2
```

Supported optimization levels:

| Option | Description |
|---|---|
| `-O0` | No optimization |
| `-O1` | Less optimization |
| `-O2` | Default optimization |
| `-O3` | Aggressive optimization |

### Select the linker

Using an environment variable:

```powershell
$env:CMATH_CLANGXX = "C:\Program Files\LLVM\bin\clang++.exe"

mathmake build main.cmath
```

Using a command-line argument:

```powershell
mathmake build main.cmath --clangxx "C:\Program Files\LLVM\bin\clang++.exe"
```

Linker selection priority:

1. `--clangxx`
2. `CMATH_CLANGXX`
3. Valid compiler path discovered during CMake configuration
4. `clang++` through PATH

### Clean generated artifacts

```powershell
mathmake clean
```

This removes the generated `build/cmath` native-object directory in the current working directory. It does not remove the CMake build directory.

---

## First native test

Create `examples/native_smoke.cmath`:

```python
domain Real

let x: Real = 10
let y: Real = 20

print(x + y)
```

Build and execute:

```powershell
mathmake run examples\native_smoke.cmath
```

Expected output:

```text
30
```

### Functions test

A second example exercises user-defined functions and real-valued mathematics.

```powershell
mathmake run examples\native_functions.cmath
```

Expected output:

```text
5
```

---

## Native backend support

The initial LLVM native backend supports:

- `Integer` and `Real` local variables, function parameters, and return values.
- `Boolean` and `String` local values, function parameters, and return values.
- Widening `Integer` to `Real`.
- Rational decimal literals when the surrounding context explicitly widens them to `Real`.
- Integer `+`, `-`, `*`, `%`.
- Real `+`, `-`, `*`, `/`, `%`, `^`.
- Numeric comparisons.
- Boolean equality.
- String comparisons.
- User-defined functions.
- Local declarations, assignments, and returns.
- `print()` for Integer, Real, Boolean, and String.
- Real `sqrt`, `sin`, `cos`, `tan`, `exp`, `log`, `abs`, `floor`, and `approximate`.
- Integer `abs` and `floor`.

Rational decimal literals widened to `Real` use native floating-point representation. They are therefore approximations rather than exact Rational values.

### Unsupported native features

The following features are not yet supported by the native backend:

- Exact Rational variables, function parameters, returns, and printing.
- Complex values and complex-valued built-ins.
- `to_integer_exact()`.
- Integer exponentiation.
- String concatenation.
- Full interpreter-equivalent overflow diagnostics and runtime error messages.

Unsupported features produce native-code diagnostics rather than silently generating incorrect code.

These limitations apply to the initial native backend, not the language frontend or interpreter.

Programs using unsupported native features can still be checked and executed through the existing `LanguageEngine::execute()` interpreter API.

---

## Compiler architecture

The CMath compilation pipeline:

```text
.cmath source
     |
     v
LanguageEngine
     |
     +-- Lexer
     +-- Parser
     +-- Semantic Analyzer
     |
     v
CompiledProgram
     |
     v
LLVMBackend
     |
     +-- LLVM IR generation
     +-- Native object-file emission
     |
     v
clang++ linker driver
     |
     v
Native executable
```

`CompiledProgram` keeps its internal representation private.

`LLVMBackend` receives internal friend access, allowing the public language API to avoid exposing frontend implementation details.

Both command-line and desktop applications can use the same language and compiler libraries.

---

## Integrating into an existing CMath project

The easiest integration is to add CMath as a subdirectory:

```cmake
add_subdirectory(path/to/CMath)
```

Then link the required targets:

```cmake
target_link_libraries(
    your-cmath-target
    PRIVATE
    CMath::Language
    CMath::Compiler
)
```

Configure the root project with the same LLVM SDK and dependency configuration.

If your root project already contains copies of the language frontend sources, avoid compiling two copies into the same executable.

Use either the CMath language target or your own frontend target.

---

## Project

GitHub:

https://github.com/raiyanshadow/CMath

Current version: 0.2.0

Language standard: C++20

Build system: CMake

Native compiler backend: LLVM

Command-line tool: `mathmake`
