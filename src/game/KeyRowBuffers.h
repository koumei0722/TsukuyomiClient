#pragma once

#include <cstddef>

namespace tsukuyomi::keyrowbuffers {

inline int pickFree(const int* used, std::size_t usedCount, std::size_t bufferCount)
{
    for (std::size_t candidate = 0; candidate < bufferCount; ++candidate) {
        bool taken = false;
        for (std::size_t i = 0; i < usedCount && !taken; ++i) {
            taken = used[i] == static_cast<int>(candidate);
        }
        if (!taken) {
            return static_cast<int>(candidate);
        }
    }
    return -1;
}

}
