#include "SkinSteal.h"
#include "Crypto.h"
#include "SkinPack.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <regex>

namespace tsukuyomi::bot::skin {
namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;
constexpr std::size_t kMaxPlayers = 256;
constexpr const char* kSourceKey = "stolen_from";

std::string lower(std::string value, bool removeSpace = false)
{
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (removeSpace) std::erase_if(value, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; });
    return value;
}
Path fromUtf8(std::string_view value)
{
    return Path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string utf8(const Path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::optional<std::string> readFile(const Path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return text;
}
Path temporaryFor(const Path& file)
{
    static std::atomic<unsigned> serial{0};
    auto temporary = file;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(serial.fetch_add(1)) + L".tmp";
    return temporary;
}
bool replaceWith(const Path& temporary, const Path& file)
{
    if (MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return false;
}
bool writeText(const Path& file, std::string_view text)
{
    const auto temporary = temporaryFor(file);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size())); output.close();
        if (!output) { std::error_code error; std::filesystem::remove(temporary, error); return false; }
    }
    return replaceWith(temporary, file);
}
bool hasImage(const Image& image)
{
    return image.width && image.height && image.rgba.size() == std::size_t{image.width} * image.height * 4;
}
bool geometryDocument(const std::string& text)
{
    const auto value = Json::parse(text, nullptr, false);
    if (!value.is_object()) return false;
    if (value.contains("minecraft:geometry")) return true;
    return std::any_of(value.items().begin(), value.items().end(), [](const auto& item) { return item.key().starts_with("geometry."); });
}
std::optional<Json> engineVersion(const std::string& text)
{
    static const std::regex pattern(R"((\d{1,5})\.(\d{1,5})\.(\d{1,5}))");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) return std::nullopt;
    return Json::array({std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str())});
}
}

std::optional<std::vector<PlayerListRow>> readPlayerList(protocol::Bytes body)
{
    protocol::Reader reader(body);
    const auto count = reader.varUInt32();
    if (!count || *count > reader.remaining() / 18) return std::nullopt;
    std::vector<PlayerListRow> rows;
    rows.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        PlayerListRow row;
        const auto variant = reader.varUInt32();
        if (!variant || *variant > 1 || !reader.byte()) return std::nullopt;
        row.add = *variant == 1;
        for (auto& byte : row.uuid) { const auto value = reader.byte(); if (!value) return std::nullopt; byte = *value; }
        if (row.add) {
            const auto unique = reader.varInt64();
            auto name = reader.string(), xuid = reader.string();
            if (!unique || !name || !xuid || !reader.string() || !reader.string() || !reader.fixedBytes(4)) return std::nullopt;
            auto skin = read(reader);
            if (!skin || !reader.boolean() || !reader.boolean() || !reader.boolean() || !reader.fixedBytes(4)) return std::nullopt;
            row.uniqueId = *unique; row.name = std::move(*name); row.xuid = std::move(*xuid); row.skin = std::move(*skin);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
bool SkinCatalog::observe(std::uint16_t id, protocol::Bytes body)
{
    if (id == 0x3f) {
        auto rows = readPlayerList(body);
        if (!rows) return false;
        for (auto& row : *rows) {
            if (!row.add) {
                if (const auto it = players_.find(row.uuid); it != players_.end()) it->second.player.present = false;
                continue;
            }
            auto& entry = players_[row.uuid];
            entry.player = {row.uuid, std::move(row.name), std::move(row.xuid), std::make_shared<const Skin>(std::move(row.skin)), true};
            entry.serial = ++serial_;
        }
        evict();
        return true;
    }
    if (id == 0x5d) {
        auto changed = readPlayerSkin(body);
        if (!changed) return false;
        if (const auto it = players_.find(changed->uuid); it != players_.end()) {
            it->second.player.skin = std::make_shared<const Skin>(std::move(changed->skin));
            it->second.serial = ++serial_;
        }
        return true;
    }
    return true;
}
void SkinCatalog::evict()
{
    while (players_.size() > kMaxPlayers) {
        auto victim = players_.end();
        for (auto it = players_.begin(); it != players_.end(); ++it) {
            if (victim == players_.end()) { victim = it; continue; }
            const auto key = [](const auto& entry) { return std::pair{entry.second.player.present, entry.second.serial}; };
            if (key(*it) < key(*victim)) victim = it;
        }
        players_.erase(victim);
    }
}
std::optional<ListedPlayer> SkinCatalog::find(const std::string& name) const
{
    const auto wanted = lower(name);
    const Entry* best = nullptr;
    for (const auto& [uuid, entry] : players_) {
        if (lower(entry.player.name) != wanted) continue;
        if (!best || std::pair{entry.player.present, entry.serial} > std::pair{best->player.present, best->serial}) best = &entry;
    }
    if (!best) return std::nullopt;
    return best->player;
}
std::vector<std::string> SkinCatalog::names() const
{
    std::vector<std::string> result;
    for (const auto& [uuid, entry] : players_) if (entry.player.present) result.push_back(entry.player.name);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return lower(a) < lower(b); });
    return result;
}

bool validSkinName(const std::string& name)
{
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.') return false;
    for (const unsigned char c : name)
        if (c < 0x20 || c == 0x7f || std::string_view("\\/:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
    static const std::regex reserved(R"((con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?)", std::regex::icase);
    return !std::regex_match(name, reserved);
}

bool writePng(const Path& file, const Image& image, std::string& error)
{
    if (!hasImage(image)) { error = "the image is empty"; return false; }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) { error = "cannot initialize WIC"; return false; }
    const auto temporary = temporaryFor(file);
    bool ok = false;
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        std::vector<std::uint8_t> bgra(image.rgba);
        for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
            && SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE))
            && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
            && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))
            && SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format))
            && format == GUID_WICPixelFormat32bppBGRA
            && SUCCEEDED(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(bgra.size()), bgra.data()))
            && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!ok) { std::error_code ignored; std::filesystem::remove(temporary, ignored); error = "cannot encode PNG"; return false; }
    if (!replaceWith(temporary, file)) { error = "cannot write " + utf8(file.filename()); return false; }
    return true;
}

SaveResult saveSkinPack(const Path& root, const std::string& name, const Skin& skin, const std::string& source)
{
    SaveResult result;
    if (!validSkinName(name)) { result.error = "invalid skin name"; return result; }
    if (!hasImage(skin.image)) { result.error = "the skin has no image"; return result; }
    result.dir = root / fromUtf8(name);
    std::error_code error;
    const auto key = lower(name, true);
    for (const auto& pack : listPacks({root})) {
        if (std::filesystem::equivalent(pack.dir, result.dir, error)) continue;
        for (const auto& other : pack.skins)
            if (lower(other, true) == key) { result.error = "a skin named " + other + " already exists in " + pack.name; return result; }
    }
    if (std::filesystem::exists(result.dir, error)) {
        const auto old = readFile(result.dir / "skins.json");
        const auto data = old ? Json::parse(*old, nullptr, false) : Json();
        bool leftover = !old;
        for (std::filesystem::directory_iterator it(result.dir, error), end; leftover && !error && it != end; it.increment(error)) {
            const auto file = lower(utf8(it->path().filename()));
            leftover = file == "skin.png" || file == "skin.bin" || file == "cape.png" || file == "geometry.json" || file == "manifest.json" || file.ends_with(".tmp");
        }
        if (error) leftover = false;
        if (!leftover && (!data.is_object() || !data.contains(kSourceKey))) {
            result.error = "Tsukuyomi/skins/" + name + " already exists and was not made by steal";
            return result;
        }
        result.replaced = true;
    }
    std::filesystem::create_directories(result.dir, error);
    if (error) { result.error = "cannot create Tsukuyomi/skins/" + name; return result; }

    Json entry = {{"localization_name", name}, {"texture", "skin.png"}, {"type", "free"}};
    const auto patch = Json::parse(skin.resourcePatch, nullptr, false);
    if (patch.is_object()) {
        if (const auto it = patch.find("geometry"); it != patch.end() && it->is_object())
            if (const auto def = it->find("default"); def != it->end() && def->is_string()) result.geometryName = def->get<std::string>();
        for (const auto* field : {"animations", "enable_attachables", "held_item_ignores_lighting", "hide_armor"})
            if (patch.contains(field)) entry[field] = patch[field];
    }
    if (result.geometryName.empty()) result.geometryName = skin.slim ? "geometry.humanoid.customSlim" : "geometry.humanoid.custom";
    entry["geometry"] = result.geometryName;

    std::string reason;
    if (!writePng(result.dir / "skin.png", skin.image, reason)) { result.error = reason; return result; }
    result.cape = hasImage(skin.cape);
    if (result.cape) {
        if (!writePng(result.dir / "cape.png", skin.cape, reason)) { result.error = reason; return result; }
        entry["cape"] = "cape.png";
    }
    result.fullSkin = skin.persona;
    if (result.fullSkin) {
        if (!writeText(result.dir / "skin.bin", serialize(skin))) { result.error = "cannot write skin.bin"; return result; }
        entry["tsukuyomi_full_skin"] = "skin.bin";
    }
    result.geometryFile = geometryDocument(skin.geometry);
    if (result.geometryFile && !writeText(result.dir / "geometry.json", skin.geometry)) { result.error = "cannot write geometry.json"; return result; }
    const auto version = engineVersion(skin.geometryEngineVersion);
    const bool manifest = version && skin.geometryEngineVersion != "1.14.0";
    if (manifest) {
        const Json data = {
            {"format_version", 2},
            {"header", {{"name", name}, {"uuid", crypto::newUuid()}, {"version", {1, 0, 0}}, {"min_engine_version", *version}}},
            {"modules", Json::array({{{"type", "skin_pack"}, {"uuid", crypto::newUuid()}, {"version", {1, 0, 0}}}})}};
        if (!writeText(result.dir / "manifest.json", data.dump(2) + "\n")) { result.error = "cannot write manifest.json"; return result; }
    }
    if (result.replaced) {
        if (!result.cape) std::filesystem::remove(result.dir / "cape.png", error);
        if (!skin.persona) std::filesystem::remove(result.dir / "skin.bin", error);
        if (!result.geometryFile) std::filesystem::remove(result.dir / "geometry.json", error);
        if (!manifest) std::filesystem::remove(result.dir / "manifest.json", error);
    }
    const Json pack = {{"serialize_name", name}, {"localization_name", name}, {kSourceKey, source}, {"skins", Json::array({entry})}};
    if (!writeText(result.dir / "skins.json", pack.dump(2) + "\n")) { result.error = "cannot write skins.json"; return result; }
    return result;
}
}
