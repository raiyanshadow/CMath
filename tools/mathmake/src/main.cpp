#include "BuildSystem.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
    void printHelp() {
        std::cout << 
            "CMath mathmake 0.1\n\n"
            "Usage:\n"
            "  mathmake <file.cmath> [options]       Build file.cmath into file.exe\n"
            "  mathmake build <file.cmath> [options] Build a native executable\n"
            "  mathmake run <file.cmath> [options]   Build and run the executable\n"
            "  mathmake check <file.cmath>           Check syntax, types, and domains\n"
            "  mathmake --version                    Show version\n"
            "  mathmake --help                       Show this help\n\n"
            "Options:\n"
            "  -o <file.exe>     Set executable output path\n"
            "  -O0 | -O1 | -O2 | -O3  Set native code-generation optimization\n"
            "  --clangxx <path>  Set the clang++ linker driver\n\n"
            "You can also set CMATH_CLANGXX to the path of clang++.exe.\n";
    }

    bool parseOptimization(std::string_view argument, cmath::NativeOptimizationLevel& level) {
        if (argument == "-O0") level = cmath::NativeOptimizationLevel::None;
        else if (argument == "-O1") level = cmath::NativeOptimizationLevel::Less;
        else if (argument == "-O3") level = cmath::NativeOptimizationLevel::Default;
        else if (argument == "-O3") level = cmath::NativeOptimizationLevel::Agressive;
        else return false;
        return true;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printHelp();
        return 2;
    }

    const std::string first = argv[1];
    if (first == "--help" || first == "-h") {
        printHelp();
        return 0;
    }

    if (first == "--version") {
        std::cout << "CMath mathmake 0.2 (LLVM native backend)\n";
        return 0;
    }

    mathmake::BuildSystem buildSystem;
    if (first == "clean") {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::current_path() / "build" / "cmath", error);
        if (error) {
            std::cerr << "mathmake: could not remove build directory: " << error.message() << '\n';
            return 1;
        }
        std::cout << "Removed CMath native build artifacts\n";
        return 0;
    }

    if (first == "check") {
        if (argc < 3) {
            std::cerr << "mathmake: check requires a .cmath file\n";
            return 2;
        }
        return buildSystem.check(argv[2]);
    }

    mathmake::BuildOptions options;
    int argumentIndex = 1;
    if (first == "build" || first == "run") {
        options.runAfterBuild = first == "run";
        argumentIndex = 2;
    }
    if (argumentIndex >= argc) {
        std::cerr << "mathmake: no .cmath source file was supplied\n";
        return 2;
    }
    options.sourceFile = argv[argumentIndex++];

    while (argumentIndex < argc) {
        const std::string argument = argv[argumentIndex++];
        if (argument == "-o") {
            if (argumentIndex >= argc) {
                std::cerr << "mathmake: -o requires an output path\n";
                return 2;
            }
            options.outputFile = argv[argumentIndex++];
        } else if (argument == "--clangxx") {
            if (argumentIndex >= argc) {
                std::cerr << "mathmake: --clangxx requires a path\n";
                return 2;
            }
            options.clangxx = argv[argumentIndex++];
        } else if (!parseOptimization(argument, options.optimization)) {
            std::cerr << "mathmake: unknown option '" << argument << "'\n";
            return 2;
        }
    }

    return buildSystem.build(options);
}