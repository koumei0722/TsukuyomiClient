#pragma once

#include <cstddef>

namespace tsukuyomi::memory {

bool isReadable(const void* address, size_t size);

bool isWritable(const void* address, size_t size);

bool isExecutable(const void* address, size_t size);

bool inGameModule(const void* address);

void* ripTarget(const std::byte* at, std::size_t dispOffset);

std::size_t functionSize(const void* address);

}
