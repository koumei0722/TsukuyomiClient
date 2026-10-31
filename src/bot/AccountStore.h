#pragma once
#include "Crypto.h"
#include <filesystem>
#include <string_view>

namespace tsukuyomi::bot {
struct StoredAccount {
    std::string name, xuid, deviceId, mcDeviceId;
    crypto::EcKey proofKey;
    std::string refreshToken;
    std::int64_t savedAt = 0;
};
namespace accountStore {
struct LoadResult { std::vector<StoredAccount> accounts; std::vector<std::string> warnings; };
LoadResult load(const std::filesystem::path&);
std::vector<std::string> names(const std::filesystem::path&);
bool save(const std::filesystem::path&, const std::vector<StoredAccount>&);
void upsert(std::vector<StoredAccount>&, StoredAccount account);
bool remove(std::vector<StoredAccount>&, std::string_view name);
}
}
