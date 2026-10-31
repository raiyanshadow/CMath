#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cmath/compiler/LLVMBackend.h"

namespace mathmake {

struct BuildOptions {
    std::filesystem::path sourceFile;
    std::filesystem::path outputFile;
    std::string clangxx;
    cmath::NativeOptimizationLevel optimization = cmath::NativeOptimizationLevel::Default;
    bool runAfterBuild = false;
};

class BuildSystem {
public:
    int check(const std::filesystem::path& sourceFile) const;
    int build(const BuildOptions& options) const;

private:
    static void printDiagnostics(
        const std::filesystem::path& sourceFile,
        const std::vector<cmath::Diagnostic>& diagnostics);
};

} // namespace mathmake
