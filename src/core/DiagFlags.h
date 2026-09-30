#pragma once

#include <filesystem>

namespace tsukuyomi::diagflags {

enum class Flag { HooksAll, FrameTrace, HotCount, Perf, Count };

void poll();
void poll(const std::filesystem::path& dir);
bool get(Flag flag);

}
