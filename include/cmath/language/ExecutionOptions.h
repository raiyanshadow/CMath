#pragma once

#include <cstddef>

namespace cmath {
    struct ExecutionOptions {
        std::size_t maximumInstructions = 10000000;
        std::size_t maximumCallDepth = 1024;
    };
}