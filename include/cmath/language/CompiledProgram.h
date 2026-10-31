#pragma once

#include <memory>
#include <utility>

namespace cmath::language::detail {
    struct ProgramIR;
}

namespace cmath {
    class LanguageEngine;
    class LLVMBackend;

    class CompiledProgram {
        public:
            CompiledProgram() = default;
            [[nodiscard]]
            bool valid() const noexcept {
                return static_cast<bool>(program_);
            }

        private:
            explicit CompiledProgram(
                std::shared_ptr<const language::detail::ProgramIR> program)
                : program_(std::move(program)) {}

                std::shared_ptr<const language::detail::ProgramIR> program_;
                friend class LanguageEngine; 
                friend class LLVMBackend;
    };
}