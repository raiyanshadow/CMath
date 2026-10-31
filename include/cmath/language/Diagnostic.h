#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cmath {
    struct SourceLocation {
        std::size_t line = 1;
        std::size_t column = 1;
        std::size_t offset = 0;
    };

    enum class DiagnosticSeverity {
        Error,
        Warning,
        Note
    };

    struct Diagnostic {
        DiagnosticSeverity severity = DiagnosticSeverity::Error;
        std::string code;
        std::string message;
        SourceLocation location();
        std::vector<std::string> notes;
    };
}