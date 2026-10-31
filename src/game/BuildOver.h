#pragma once

#include <cstdint>

namespace tsukuyomi::buildover {

void resolve();
bool ready();

int canBuildOver(void* gameMode, const std::int32_t pos[3]);

}
