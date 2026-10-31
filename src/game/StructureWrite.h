#pragma once

#include "game/Structure.h"
#include <ostream>

namespace tsukuyomi::structure {
bool write(std::ostream& out, const Structure& value, const std::int32_t origin[3], std::string& why);
}
