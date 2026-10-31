#pragma once

#include <cstdint>

namespace tsukuyomi::chunkresend {

bool resolve();
bool ready();

bool start();
void pump();
bool running();

}
