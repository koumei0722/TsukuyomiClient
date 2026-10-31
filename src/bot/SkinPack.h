#pragma once
#include "Skin.h"
#include <filesystem>
#include <map>
#include <memory>

namespace tsukuyomi::bot::skin {
struct Pack { std::string name; std::filesystem::path dir; std::vector<std::string> skins; };
std::vector<Pack> listPacks(const std::vector<std::filesystem::path>& roots);
struct Loaded { std::shared_ptr<const Skin> skin; std::string label, error; };
Loaded load(const std::string& spec, const std::vector<std::filesystem::path>& roots,
            const std::vector<std::filesystem::path>& geometryRoots = {});
std::string resourcePatchFor(const nlohmann::json& entry);
std::optional<std::string> extractGeometry(std::string_view text, const std::string& identifier, std::string& error);
std::optional<Image> decodeImage(const std::filesystem::path& file, std::string& error);
struct SkinConfig {
    std::string current, main, sub;
    std::map<std::string, std::string> names;
    bool operator==(const SkinConfig&) const = default;
    bool empty() const { return current.empty() && main.empty() && sub.empty() && names.empty(); }
};
std::optional<SkinConfig> loadSettings(const std::filesystem::path& file, std::string& error);
bool saveSettings(const std::filesystem::path& file, const SkinConfig& settings);
}
