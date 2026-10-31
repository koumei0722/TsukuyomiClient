#pragma once

#include "game/Structure.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi::paste {

enum class Replace : int { None, All, NonAir };
bool shouldSend(Replace rule, bool wantAir, bool realAir, bool same);
bool blockArgument(const std::string& name, const std::vector<structure::StateValue>& states, std::string& out);

struct Cell {
    std::int32_t x = 0, y = 0, z = 0;
    std::string block;
    std::size_t count = 1;
};
struct Command {
    std::string text;
    std::size_t blocks = 0;
    std::int32_t x = 0, y = 0, z = 0;
};
std::vector<Cell> runs(std::vector<Cell> cells);
std::vector<Command> commands(std::vector<Cell> cells);

}
