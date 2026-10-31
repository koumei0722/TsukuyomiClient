#pragma once
#include "Skin.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot::skin {

struct PlayerListRow {
    bool add = false;
    possess::Uuid uuid{};
    std::int64_t uniqueId = 0;
    std::string name, xuid;
    Skin skin;
};
std::optional<std::vector<PlayerListRow>> readPlayerList(protocol::Bytes body);
struct ListedPlayer {
    possess::Uuid uuid{};
    std::string name, xuid;
    std::shared_ptr<const Skin> skin;
    bool present = true;
};

class SkinCatalog {
public:
    bool observe(std::uint16_t id, protocol::Bytes body);
    std::optional<ListedPlayer> find(const std::string& name) const;
    std::vector<std::string> names() const;
    std::size_t size() const { return players_.size(); }
    void clear() { players_.clear(); serial_ = 0; }
private:
    struct Entry { ListedPlayer player; std::uint64_t serial = 0; };
    void evict();
    std::map<possess::Uuid, Entry> players_;
    std::uint64_t serial_ = 0;
};

bool validSkinName(const std::string& name);

struct SaveResult {
    std::filesystem::path dir;
    std::string geometryName;
    bool geometryFile = false;
    bool cape = false;
    bool fullSkin = false;
    bool replaced = false;
    std::string error;
};
SaveResult saveSkinPack(const std::filesystem::path& root, const std::string& name, const Skin& skin, const std::string& source);

bool writePng(const std::filesystem::path& file, const Image& image, std::string& error);
}
