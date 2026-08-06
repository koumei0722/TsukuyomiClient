#pragma once

#include <cstddef>

namespace tsukuyomi::memory {

bool isReadable(const void* address, size_t size);

bool isWritable(const void* address, size_t size);

}
