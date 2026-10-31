#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Diagnostic.h"

namespace cmath {
    struct ExecutionResult {
        bool succeeded = false;
        std::string output;
        std::optional<std::string> lastValue;
        std::vector<Diagnostic>
    };
}