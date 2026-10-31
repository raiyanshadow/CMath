#pragma once

#include <memory>
#include <string_view>

#include "CompiledProgram.h"
#include "CompileOptions.h"
#include "CompileResult.h"
#include "ExecutionOptions.h"
#include "ExecutionResult.h"

namespace cmath {
    class LanguageEngine {
        public:
            LanguageEngine() = default;

            [[nodiscard]]
            CompileResult compile(
                std::string_view source,
                const CompileOptions& options = {}
            ) const;

            [[nodiscard]]
            ExecutionResult execute(
                const CompiledProgram& program,
                const ExecutionOptions& options = {}
            ) const;
    };
}