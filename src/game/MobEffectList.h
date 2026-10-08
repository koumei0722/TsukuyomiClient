#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi::mobeffects {

struct Effect {
    int index = 0;
    int id = 0;
    int duration = 0;
    int amplifier = 0;
    bool ambient = false;
    const void* effect = nullptr;
    std::uint32_t color = 0;
};

inline constexpr int kMaxId = 0x25;

void resolve();
bool ready();

bool read(std::vector<Effect>& out);

bool listedByIndex(std::vector<std::uint8_t>& out);

bool displayName(int id, int amplifier, std::string& out);
bool durationText(int ticks, std::string& out);

}
