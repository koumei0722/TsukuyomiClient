#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tsukuyomi::worldidentity {

struct Snapshot {
    std::uint64_t entry = 0;
    bool received = false;
    std::string levelId, name;
};

void onStartGame(std::string_view levelId, std::string_view name);
Snapshot snapshot();
void clear(std::uint64_t entry);
std::wstring sanitizeKeyPart(std::string_view text);
std::string worldKey(bool received, bool integrated, std::string_view levelId,
                     std::string_view name, std::string_view folder = {}, std::string_view connectionUrl = {});

}
