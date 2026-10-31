#pragma once

#include <cstddef>

namespace cmath {
    struct CompileOptions {
        std::size_t maximumDiagnostics = 1000;
        std::size_t maximumSourceBytes = 4096*4096;
        bool allowShadowing = false;
    };
}