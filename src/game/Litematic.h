#pragma once

#include <cstddef>

#include "game/Structure.h"

namespace tsukuyomi::litematic {

structure::LoadResult loadBytes(const char* bytes, std::size_t size);

structure::LoadResult loadNbt(const char* bytes, std::size_t size);

}
