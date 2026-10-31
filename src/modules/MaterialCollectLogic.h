#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi::materialcollect {

long long requiredAmount(int multiplier, long long missing, long long totalTimesMultiplier, long long available);

long long collectionTarget(long long required, int extra, bool roundUp, int maxStack);

int movableAmount(int count, bool retain, int retainAmount);

struct BoxItem {
    std::string name;
    int count = 0;
};
int singleMaterialBoxAmount(const std::vector<BoxItem>& contents, const std::string& want, int maxStack,
                            double threshold);

int pickBox(const std::vector<int>& amounts, long long remaining, long long looseAmount);

struct PlayerSlot {
    bool empty = true;
    bool same = false;
    int count = 0;
    int maxStack = 0;
};
std::vector<std::pair<int, int>> distributeRightClicks(const std::vector<PlayerSlot>& slots, int amount);

}
