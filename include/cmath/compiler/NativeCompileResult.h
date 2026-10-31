#pragma once

#include <filesystem>
#include <vector>

#include "cmath/language/Diagnostic.h"

namespace cmath {
    struct NativeCompileResult {
        bool succeeded = false;
        std::filesystem::path objectFile;
        std::vector<Diagnostic> diagnostics;
    };
}