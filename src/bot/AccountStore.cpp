#include "AccountStore.h"
#include "Base64.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace tsukuyomi::bot::accountStore {
LoadResult load(const std::filesystem::path& path)
{
    LoadResult result;
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error) && !error) return result;
        std::ifstream file(path, std::ios::binary);
        if (!file) { result.warnings.push_back("Cannot read BotAccounts file"); return result; }
        const auto root = nlohmann::json::parse(file, nullptr, false);
        if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("accounts") || !root["accounts"].is_array()) {
            result.warnings.push_back("Invalid BotAccounts format or version"); return result;
        }
        std::size_t index = 0;
        for (const auto& row : root["accounts"]) {
            ++index;
            try {
                StoredAccount account;
                account.name = row.at("name").get<std::string>(); account.xuid = row.at("xuid").get<std::string>();
                account.deviceId = row.at("deviceId").get<std::string>(); account.mcDeviceId = row.at("mcDeviceId").get<std::string>();
                account.savedAt = row.at("savedAt").get<std::int64_t>();
                const auto encryptedKey = base64::decode(row.at("proofKey").get<std::string>());
                const auto encryptedToken = base64::decode(row.at("refreshToken").get<std::string>());
                const auto key = encryptedKey ? crypto::unprotect(*encryptedKey) : std::nullopt;
                const auto token = encryptedToken ? crypto::unprotect(*encryptedToken) : std::nullopt;
                auto imported = key ? crypto::EcKey::importPrivate(crypto::Curve::P256, *key) : std::nullopt;
                if (account.name.empty() || account.xuid.empty() || account.deviceId.empty() || account.mcDeviceId.empty()
                    || !imported || !token || token->empty()) {
                    result.warnings.push_back("Skipped invalid or undecryptable account row " + std::to_string(index)); continue;
                }
                account.proofKey = std::move(*imported);
                account.refreshToken.assign(token->begin(), token->end());
                result.accounts.push_back(std::move(account));
            } catch (const nlohmann::json::exception&) {
                result.warnings.push_back("Skipped malformed account row " + std::to_string(index));
            }
        }
    } catch (const std::exception&) { result.warnings.push_back("Cannot parse BotAccounts file"); }
    return result;
}
std::vector<std::string> names(const std::filesystem::path& path)
{
    std::vector<std::string> result;
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error) || error) return result;
        std::ifstream file(path, std::ios::binary);
        const auto root = nlohmann::json::parse(file, nullptr, false);
        if (!root.is_object() || !root.contains("accounts") || !root["accounts"].is_array()) return result;
        for (const auto& row : root["accounts"])
            if (row.is_object() && row.contains("name") && row["name"].is_string()) result.push_back(row["name"].get<std::string>());
    } catch (const std::exception&) { result.clear(); }
    return result;
}
bool save(const std::filesystem::path& path, const std::vector<StoredAccount>& accounts)
{
    std::filesystem::path temporary;
    try {
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& account : accounts) {
            if (account.name.empty() || account.xuid.empty() || account.deviceId.empty() || account.mcDeviceId.empty()
                || account.proofKey.publicX().size() != 32) return false;
            const auto blob = account.proofKey.exportPrivate();
            if (blob.empty() || account.refreshToken.empty()) return false;
            const auto key = crypto::protect(blob);
            const auto token = crypto::protect({reinterpret_cast<const std::uint8_t*>(account.refreshToken.data()), account.refreshToken.size()});
            if (!key || !token) return false;
            rows.push_back({{"name", account.name}, {"xuid", account.xuid}, {"deviceId", account.deviceId},
                {"mcDeviceId", account.mcDeviceId}, {"proofKey", base64::encode(*key)},
                {"refreshToken", base64::encode(*token)}, {"savedAt", account.savedAt}});
        }
        const auto suffix = crypto::newUuid();
        if (suffix.empty()) return false;
        temporary = path; temporary += "." + suffix + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file << nlohmann::json{{"version", 1}, {"accounts", std::move(rows)}}.dump(2);
            file.flush();
            const bool ok = static_cast<bool>(file);
            file.close();
            if (!ok || file.fail()) {
                std::error_code error; std::filesystem::remove(temporary, error); return false;
            }
        }
        if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    } catch (const std::exception&) { }
    if (!temporary.empty()) { std::error_code error; std::filesystem::remove(temporary, error); }
    return false;
}
void upsert(std::vector<StoredAccount>& accounts, StoredAccount account)
{
    const auto found = std::find_if(accounts.begin(), accounts.end(), [&](const auto& item) { return item.xuid == account.xuid; });
    if (found == accounts.end()) accounts.push_back(std::move(account));
    else *found = std::move(account);
}
bool remove(std::vector<StoredAccount>& accounts, std::string_view name)
{
    return std::erase_if(accounts, [&](const auto& item) { return item.name == name; }) != 0;
}
}
