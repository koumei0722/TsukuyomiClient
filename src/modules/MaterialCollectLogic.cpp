#include "modules/MaterialCollectLogic.h"

#include <algorithm>

namespace tsukuyomi::materialcollect {

namespace {
constexpr int kBoxSlots = 27;
}

long long requiredAmount(int multiplier, long long missing, long long totalTimesMultiplier, long long available)
{
    const long long wanted = (multiplier == 1) ? missing : totalTimesMultiplier;
    return std::max(0LL, wanted - available);
}

long long collectionTarget(long long required, int extra, bool roundUp, int maxStack)
{
    long long target = required + std::max(0, extra);
    if (roundUp && maxStack > 0) {
        const long long rounded = (required + maxStack - 1) / maxStack * maxStack;
        target = std::max(target, rounded);
    }
    return target;
}

int movableAmount(int count, bool retain, int retainAmount)
{
    return std::max(0, count - (retain ? retainAmount : 0));
}

int singleMaterialBoxAmount(const std::vector<BoxItem>& contents, const std::string& want, int maxStack,
                            double threshold)
{
    if (maxStack <= 0) {
        return 0;
    }
    long long amount = 0;
    for (const BoxItem& item : contents) {
        if (item.count <= 0) {
            continue;
        }
        if (item.name != want) {
            return 0;
        }
        amount += item.count;
    }
    if (amount <= 0) {
        return 0;
    }
    const double fill = static_cast<double>(amount) / static_cast<double>(kBoxSlots * maxStack);
    if (fill < threshold) {
        return 0;
    }
    return static_cast<int>(std::min<long long>(amount, kBoxSlots * maxStack));
}

int pickBox(const std::vector<int>& amounts, long long remaining, long long looseAmount)
{
    for (std::size_t i = 0; i < amounts.size(); ++i) {
        if (amounts[i] <= remaining) {
            return static_cast<int>(i);
        }
    }
    return (remaining > looseAmount && !amounts.empty()) ? static_cast<int>(amounts.size()) - 1 : -1;
}

std::vector<std::pair<int, int>> distributeRightClicks(const std::vector<PlayerSlot>& slots, int amount)
{
    std::vector<std::pair<int, int>> out;
    for (int i = static_cast<int>(slots.size()) - 1; i >= 0 && amount > 0; --i) {
        const PlayerSlot& s = slots[static_cast<std::size_t>(i)];
        int clicks = 0;
        if (s.empty) {
            clicks = amount;
        } else if (s.same && s.maxStack > s.count) {
            clicks = std::min(s.maxStack - s.count, amount);
        }
        if (clicks > 0) {
            out.emplace_back(i, clicks);
            amount -= clicks;
        }
    }
    return out;
}

}
