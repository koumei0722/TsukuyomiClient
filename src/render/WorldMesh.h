#pragma once

#include "render/DebugLine.h"

#include <vector>

namespace tsukuyomi::worldmesh {

void setDebugLines(std::vector<DebugLine> lines);

bool installHooks();

bool active(bool xray);

void report();

void onPresent();

void shutdown();

}
