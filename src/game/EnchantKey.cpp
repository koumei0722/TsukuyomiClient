#include "game/EnchantKey.h"

#include <algorithm>

namespace tsukuyomi::containerui {

std::string formatEnchantKey(std::vector<std::pair<int, int>> enchants)
{
    std::sort(enchants.begin(), enchants.end());
    std::string key;
    for (const auto& [id, level] : enchants) {
        if (!key.empty()) {
            key += ',';
        }
        key += std::to_string(id) + '.' + std::to_string(level);
    }
    return key;
}

}
