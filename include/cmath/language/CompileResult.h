#pragma once

#include <optional>
#include <vector>

#include "CompiledProgram.h"
#include "Diagnostic.h"

namespace cmath {
    struct CompileResult {
        std::optional<CompiledProgram> program;
        std::vector<Diagnostic> diagnostics;

        [[nodiscard]]
        bool succeeded() const noexcept {
            return program.has_value();
        }
    };
}