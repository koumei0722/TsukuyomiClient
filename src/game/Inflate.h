#pragma once

#include <cstddef>
#include <vector>

namespace tsukuyomi::inflate {

bool isGzip(const char* bytes, std::size_t size);

const char* gunzip(const char* bytes, std::size_t size, std::vector<char>& out, std::size_t maxOutput);

const char* inflateRaw(const char* bytes, std::size_t size, std::vector<char>& out, std::size_t maxOutput,
                       std::size_t* used = nullptr);

}
