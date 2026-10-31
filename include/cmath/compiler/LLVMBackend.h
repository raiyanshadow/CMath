#pragma once

#include <filesystem>
#include <string>

#include "NativeCompileResult.h"
#include "cmath/language/CompiledProgram.h"

namespace cmath {
    enum class NativeOptimizationLevel {
        None,
        Less,
        Default,
        Agressive
    };

    struct LLVMBackendOptions {
        std::string targetTriple;
        std::string cpu = "generic";
        NativeOptimizationLevel optimization = NativeOptimizationLevel::Default;
    };

    class LLVMBackend {
        public:
            [[nodiscard]]
            NativeCompileResult emitObjectFile(
                const CompiledProgram& program,
                const std::filesystem::path& output,
                const LLVMBackendOptions& options = {}) const;
    };
}