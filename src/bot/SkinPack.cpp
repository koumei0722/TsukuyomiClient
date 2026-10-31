#include "SkinPack.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace tsukuyomi::bot::skin {
namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;
std::string utf8(const Path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
Path fromUtf8(std::string_view value)
{
    return Path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string lower(std::string value, bool removeSpace = false)
{
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (removeSpace) std::erase_if(value, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; });
    return value;
}
bool regular(const Path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}
std::vector<Path> directories(const Path& root)
{
    std::vector<Path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error)) result.push_back(it->path());
    std::sort(result.begin(), result.end());
    return result;
}
std::optional<std::string> readFile(const Path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return text;
}
struct InvalidJson {};
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

class Cursor {
public:
    std::string_view text;
    std::size_t pos = 0;
    explicit Cursor(std::string_view value) : text(value)
    {
        if (text.starts_with("\xef\xbb\xbf")) pos = 3;
    }
    void skip()
    {
        while (pos < text.size()) {
            if (space(text[pos])) { ++pos; continue; }
            if (text.substr(pos, 2) == "//") {
                pos += 2;
                while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') ++pos;
            } else if (text.substr(pos, 2) == "/*") {
                const auto end = text.find("*/", pos + 2);
                if (end == std::string_view::npos) throw InvalidJson{};
                pos = end + 2;
            } else break;
        }
    }
    bool take(char c)
    {
        skip();
        if (pos < text.size() && text[pos] == c) { ++pos; return true; }
        return false;
    }
    std::string_view string()
    {
        skip();
        const auto start = pos;
        if (pos >= text.size() || text[pos++] != '"') throw InvalidJson{};
        while (pos < text.size()) {
            const auto c = text[pos++];
            if (c == '"') return text.substr(start, pos - start);
            if (static_cast<unsigned char>(c) < 32) throw InvalidJson{};
            if (c == '\\') {
                if (pos == text.size()) throw InvalidJson{};
                ++pos;
            }
        }
        throw InvalidJson{};
    }
    std::string_view value()
    {
        skip();
        const auto start = pos;
        if (pos == text.size()) throw InvalidJson{};
        if (text[pos] == '"') return string();
        if (text[pos] == '{' || text[pos] == '[') {
            std::vector<char> stack;
            do {
                skip();
                if (pos == text.size()) throw InvalidJson{};
                const char c = text[pos];
                if (c == '"') { string(); continue; }
                ++pos;
                if (c == '{' || c == '[') stack.push_back(c);
                else if (c == '}' || c == ']') {
                    if (stack.empty() || stack.back() != (c == '}' ? '{' : '[')) throw InvalidJson{};
                    stack.pop_back();
                }
            } while (!stack.empty());
        } else {
            while (pos < text.size() && !space(text[pos]) && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' && text[pos] != '/') ++pos;
            if (pos == start) throw InvalidJson{};
        }
        return text.substr(start, pos - start);
    }
    void finish()
    {
        take(','); skip();
        if (pos != text.size()) throw InvalidJson{};
    }
};
Json parseValue(std::string_view value)
{
    Cursor cursor(value);
    std::string clean;
    clean.reserve(value.size());
    while (cursor.pos < value.size()) {
        const auto before = cursor.pos;
        cursor.skip();
        if (cursor.pos != before) clean += ' ';
        if (cursor.pos == value.size()) break;
        if (value[cursor.pos] == '"') clean += cursor.string();
        else if (value[cursor.pos] == ',') {
            ++cursor.pos; cursor.skip();
            if (cursor.pos < value.size() && value[cursor.pos] != '}' && value[cursor.pos] != ']') clean += ',';
        } else clean += value[cursor.pos++];
    }
    return Json::parse(clean);
}
template<class F> void objectFields(std::string_view value, F field, bool documentTrailingComma = false)
{
    Cursor cursor(value);
    if (!cursor.take('{')) throw InvalidJson{};
    if (!cursor.take('}')) {
        for (;;) {
            auto key = Json::parse(cursor.string()).get<std::string>();
            if (!cursor.take(':')) throw InvalidJson{};
            field(std::move(key), cursor.value());
            if (cursor.take('}')) break;
            if (!cursor.take(',')) throw InvalidJson{};
            if (cursor.take('}')) break;
            if (documentTrailingComma) {
                cursor.skip();
                if (cursor.pos == value.size()) break;
            }
        }
    }
    cursor.finish();
}
template<class F> void arrayValues(std::string_view value, F element)
{
    Cursor cursor(value);
    if (!cursor.take('[')) throw InvalidJson{};
    if (!cursor.take(']')) {
        for (;;) {
            element(cursor.value());
            if (cursor.take(']')) break;
            if (!cursor.take(',')) throw InvalidJson{};
            if (cursor.take(']')) break;
        }
    }
    cursor.finish();
}
Json readJson(const Path& path)
{
    const auto value = readFile(path);
    if (!value) return Json::object();
    try { return parseValue(*value); }
    catch (const std::exception&) { return Json::object(); }
    catch (const InvalidJson&) { return Json::object(); }
}
std::string stringField(const Json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
struct PackInfo { Pack pack; Json entries, manifest; std::vector<std::string> aliases; };
std::vector<PackInfo> scanPacks(const std::vector<Path>& roots)
{
    std::vector<PackInfo> result;
    std::set<std::string> seenNames;
    for (const auto& root : roots) {
        std::set<std::string> rootNames;
        auto dirs = directories(root);
        if (regular(root / "skins.json")) dirs.insert(dirs.begin(), root);
        for (const auto& dir : dirs) {
            if (!regular(dir / "skins.json")) continue;
            const auto data = readJson(dir / "skins.json");
            if (!data.is_object() || !data.contains("skins") || !data["skins"].is_array()) continue;
            PackInfo info;
            info.manifest = readJson(dir / "manifest.json");
            if (!info.manifest.is_object()) info.manifest = Json::object();
            info.pack.dir = dir; info.entries = data["skins"];
            const auto header = info.manifest.value("header", Json::object());
            info.pack.name = stringField(header, "name");
            if (info.pack.name.empty()) info.pack.name = stringField(data, "localization_name");
            if (info.pack.name.empty()) info.pack.name = utf8(dir.filename());
            const auto name = lower(info.pack.name, true);
            if (seenNames.contains(name)) continue;
            rootNames.insert(name);
            info.aliases = {utf8(dir.filename()), info.pack.name, stringField(data, "localization_name"), stringField(data, "serialize_name")};
            for (const auto& row : info.entries) info.pack.skins.push_back(stringField(row, "localization_name"));
            result.push_back(std::move(info));
        }
        seenNames.insert(rootNames.begin(), rootNames.end());
    }
    return result;
}
bool skinDimensions(const Image& image)
{
    return (image.width == 64 && (image.height == 32 || image.height == 64))
        || (image.width == 128 && image.height == 128) || (image.width == 256 && image.height == 256);
}
Path texturePath(Path path)
{
    if (regular(path)) return path;
    const auto extension = lower(utf8(path.extension()));
    if (extension == ".png") path.replace_extension(".tga");
    else if (extension == ".tga") path.replace_extension(".png");
    return path;
}
std::optional<std::string> vanillaGeometry(const std::vector<Path>& roots, const std::string& name)
{
    for (const auto& root : roots) {
        const auto file = (lower(utf8(root.filename()), true) == "vanilla" ? root : root / "vanilla") / "geometry.json";
        if (const auto value = readFile(file)) {
            std::string error;
            if (auto geometry = extractGeometry(*value, name, error)) return geometry;
        }
    }
    return std::nullopt;
}
std::string engineVersion(const Json& manifest)
{
    const auto header = manifest.value("header", Json::object());
    const auto it = header.find("min_engine_version");
    if (it == header.end() || !it->is_array() || it->size() != 3) return "1.14.0";
    std::string result;
    for (const auto& part : *it) {
        if (!part.is_number_integer() || part.get<std::int64_t>() < 0) return "1.14.0";
        if (!result.empty()) result += '.';
        result += part.dump();
    }
    return result;
}
struct ComApartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(status)) CoUninitialize(); }
};
std::optional<Image> decodePng(const Path& file, std::string& error)
{
    ComApartment apartment;
    if (FAILED(apartment.status) && apartment.status != RPC_E_CHANGED_MODE) { error = "Cannot initialize WIC"; return std::nullopt; }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Image image;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        || FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&image.width, &image.height))
        || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        error = "Cannot decode PNG"; return std::nullopt;
    }
    const auto pixels = std::uint64_t{image.width} * image.height;
    if (!image.width || !image.height || pixels > UINT32_MAX / 4) { error = "PNG dimensions are too large"; return std::nullopt; }
    image.rgba.resize(static_cast<std::size_t>(pixels) * 4);
    if (FAILED(converter->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()))) {
        error = "Cannot read PNG pixels"; return std::nullopt;
    }
    return image;
}
std::optional<Image> decodeTga(const Path& file, std::string& error)
{
    const auto input = readFile(file);
    if (!input || input->size() < 18) { error = "Invalid TGA header"; return std::nullopt; }
    const auto byte = [&](std::size_t at) { return static_cast<std::uint8_t>((*input)[at]); };
    const auto width = std::uint32_t{byte(12)} | (std::uint32_t{byte(13)} << 8);
    const auto height = std::uint32_t{byte(14)} | (std::uint32_t{byte(15)} << 8);
    const auto type = byte(2), depth = byte(16), descriptor = byte(17);
    if (byte(1) != 0 || (type != 2 && type != 10) || (depth != 24 && depth != 32) || (descriptor & 0xc0) || !width || !height) {
        error = "Unsupported TGA format"; return std::nullopt;
    }
    std::size_t at = 18u + byte(0);
    const auto pixels = static_cast<std::size_t>(width) * height, bytesPerPixel = static_cast<std::size_t>(depth / 8);
    const auto minimum = type == 2 ? pixels * bytesPerPixel : ((pixels + 127) / 128) * (1 + bytesPerPixel);
    if (at > input->size() || minimum > input->size() - at) { error = "Truncated TGA data"; return std::nullopt; }
    Image image{width, height, std::vector<std::uint8_t>(pixels * 4)};
    const auto pixel = [&]() {
        if (bytesPerPixel > input->size() - at) throw InvalidJson{};
        std::array<std::uint8_t, 4> rgba{byte(at + 2), byte(at + 1), byte(at), depth == 32 ? byte(at + 3) : std::uint8_t{255}};
        at += bytesPerPixel;
        return rgba;
    };
    const auto store = [&](std::size_t index, const std::array<std::uint8_t, 4>& rgba) {
        const auto x = (descriptor & 0x10) ? width - 1 - index % width : index % width;
        const auto y = (descriptor & 0x20) ? index / width : height - 1 - index / width;
        std::copy(rgba.begin(), rgba.end(), image.rgba.begin() + (y * width + x) * 4);
    };
    try {
        for (std::size_t index = 0; index < pixels;) {
            std::size_t run = 1;
            bool repeat = false;
            if (type == 10) {
                if (at == input->size()) throw InvalidJson{};
                const auto packet = byte(at++);
                run = (packet & 127u) + 1u; repeat = (packet & 128u) != 0;
            }
            if (run > pixels - index) throw InvalidJson{};
            const auto first = pixel();
            store(index++, first);
            for (std::size_t i = 1; i < run; ++i) store(index++, repeat ? first : pixel());
        }
    } catch (const InvalidJson&) { error = "Invalid TGA pixel data"; return std::nullopt; }
    return image;
}
}
std::vector<Pack> listPacks(const std::vector<Path>& roots)
{
    std::vector<Pack> result;
    try { for (auto& info : scanPacks(roots)) result.push_back(std::move(info.pack)); }
    catch (const std::exception&) {}
    return result;
}
std::string resourcePatchFor(const Json& entry)
{
    Json patch = {{"geometry", {{"default", entry.at("geometry")}}}};
    for (const auto* key : {"animations", "enable_attachables", "held_item_ignores_lighting", "hide_armor"})
        if (entry.contains(key)) patch[key] = entry[key];
    return patch.dump();
}
std::optional<std::string> extractGeometry(std::string_view text, const std::string& identifier, std::string& error)
{
    error.clear();
    try {
        struct Definition { std::string key; std::string_view value; };
        std::unordered_map<std::string, Definition> definitions;
        Json version;
        std::string_view modern;
        objectFields(text, [&](std::string key, std::string_view value) {
            if (key == "format_version") version = parseValue(value);
            else if (key == "minecraft:geometry") modern = value;
            else if (key.starts_with("geometry.")) {
                auto name = key.substr(0, key.find(':'));
                definitions.emplace(std::move(name), Definition{std::move(key), value});
            }
        }, true);
        if (!version.is_string()) throw InvalidJson{};
        if (!modern.empty()) {
            definitions.clear();
            arrayValues(modern, [&](std::string_view value) {
                std::string key;
                objectFields(value, [&](std::string field, std::string_view part) {
                    if (field == "description") key = stringField(parseValue(part), "identifier");
                });
                if (!key.empty()) {
                    auto name = key.substr(0, key.find(':'));
                    definitions.emplace(std::move(name), Definition{std::move(key), value});
                }
            });
        }
        Json output = {{"format_version", version}};
        if (!modern.empty()) output["minecraft:geometry"] = Json::array();
        std::set<std::string> visited;
        auto name = identifier;
        for (;;) {
            const auto it = definitions.find(name);
            if (it == definitions.end()) { error = "Geometry " + name + " was not found"; return std::nullopt; }
            if (!visited.insert(name).second) { error = "Geometry inheritance cycle"; return std::nullopt; }
            auto value = parseValue(it->second.value);
            if (!value.is_object()) throw InvalidJson{};
            if (modern.empty()) output[it->second.key] = std::move(value);
            else output["minecraft:geometry"].push_back(std::move(value));
            const auto colon = it->second.key.find(':');
            if (colon == std::string::npos) break;
            name = it->second.key.substr(colon + 1);
        }
        return output.dump();
    } catch (const InvalidJson&) { error = "Invalid geometry JSON"; }
      catch (const std::exception&) { error = "Invalid geometry JSON"; }
    return std::nullopt;
}
std::optional<Image> decodeImage(const Path& file, std::string& error)
{
    error.clear();
    try {
        const auto extension = lower(utf8(file.extension()));
        if (extension == ".png") return decodePng(file, error);
        if (extension == ".tga") return decodeTga(file, error);
        error = "Unsupported image extension";
    } catch (const std::exception&) { error = "Cannot read image"; }
    return std::nullopt;
}
Loaded load(const std::string& spec, const std::vector<Path>& roots, const std::vector<Path>& geometryRoots)
{
    try {
        auto vanillaRoots = roots;
        vanillaRoots.insert(vanillaRoots.end(), geometryRoots.begin(), geometryRoots.end());
        auto fileSpec = spec;
        const bool slimFile = fileSpec.starts_with("slim:");
        if (slimFile) fileSpec.erase(0, 5);
        auto file = fromUtf8(fileSpec);
        const auto extension = lower(utf8(file.extension()));
        if (extension == ".png" || extension == ".tga") {
            if (file.is_relative()) {
                if (roots.empty()) return {{}, {}, "Skin image root is missing"};
                file = roots.front() / file;
            }
            std::string error;
            auto image = decodeImage(file, error);
            if (!image) return {{}, {}, error};
            if (!skinDimensions(*image)) return {{}, {}, "Unsupported skin dimensions"};
            Skin skin;
            skin.image = std::move(*image); skin.slim = slimFile;
            const std::string name = slimFile ? "geometry.humanoid.customSlim" : "geometry.humanoid.custom";
            skin.resourcePatch = resourcePatchFor({{"geometry", name}});
            skin.geometry = vanillaGeometry(vanillaRoots, name).value_or(""); skin.geometryEngineVersion = "1.14.0";
            skin.skinId = "Tsukuyomi_" + utf8(file.filename()); skin.fullId = skin.skinId;
            return {std::make_shared<const Skin>(std::move(skin)), utf8(file.filename()), {}};
        }
        auto packs = scanPacks(roots);
        const auto slash = spec.find_first_of("/:");
        const auto packName = slash == std::string::npos ? std::string{} : lower(spec.substr(0, slash), true);
        const auto skinName = lower(slash == std::string::npos ? spec : spec.substr(slash + 1), true);
        struct Match { std::size_t pack, row; std::string label; };
        std::vector<Match> matches;
        for (std::size_t p = 0; p < packs.size(); ++p) {
            const auto& pack = packs[p];
            if (slash != std::string::npos && (packName.empty() || std::none_of(pack.aliases.begin(), pack.aliases.end(),
                [&](const auto& alias) { return !alias.empty() && lower(alias, true) == packName; }))) continue;
            for (std::size_t r = 0; r < pack.pack.skins.size(); ++r)
                if ((!skinName.empty() && skinName == lower(pack.pack.skins[r], true))
                    || (slash != std::string::npos && skinName == std::to_string(r + 1)))
                    matches.push_back({p, r, pack.pack.name + ":" + pack.pack.skins[r]});
        }
        if (matches.empty()) return {{}, {}, "Skin was not found"};
        if (matches.size() != 1) {
            std::string error = "Ambiguous skin: ";
            for (std::size_t i = 0; i < std::min<std::size_t>(5, matches.size()); ++i) { if (i) error += ", "; error += matches[i].label; }
            return {{}, {}, error};
        }
        const auto& match = matches.front();
        const auto& pack = packs[match.pack];
        const auto& entry = pack.entries[match.row];
        if (const auto full = stringField(entry, "tsukuyomi_full_skin"); !full.empty()) {
            const auto data = readFile(pack.pack.dir / fromUtf8(full));
            if (!data) return {{}, {}, "cannot read " + full};
            protocol::Reader reader(protocol::bytes(*data));
            auto skin = read(reader);
            if (!skin || reader.remaining()) {
                protocol::Reader legacy(protocol::bytes(*data));
                skin = readLegacy(legacy);
                if (!skin || legacy.remaining()) return {{}, {}, "invalid " + full};
            }
            return {std::make_shared<const Skin>(std::move(*skin)), match.label, {}};
        }
        Skin skin;
        std::string error;
        const auto texture = stringField(entry, "texture");
        if (texture.empty()) return {{}, {}, "Skin texture is missing"};
        auto image = decodeImage(texturePath(pack.pack.dir / fromUtf8(texture)), error);
        if (!image) return {{}, {}, error};
        if (!skinDimensions(*image)) return {{}, {}, "Unsupported skin dimensions"};
        skin.image = std::move(*image);
        const auto cape = stringField(entry, "cape");
        if (!cape.empty()) if (auto imageCape = decodeImage(texturePath(pack.pack.dir / fromUtf8(cape)), error)) skin.cape = std::move(*imageCape);
        const auto name = stringField(entry, "geometry");
        if (name.empty()) return {{}, {}, "Skin geometry name is missing"};
        std::optional<std::string> geometry;
        if (const auto data = readFile(pack.pack.dir / "geometry.json")) geometry = extractGeometry(*data, name, error);
        if (!geometry) geometry = vanillaGeometry(vanillaRoots, name);
        if (!geometry && name != "geometry.humanoid.custom" && name != "geometry.humanoid.customSlim")
            return {{}, {}, "geometry " + name + " is not defined in the pack"};
        skin.geometry = geometry.value_or(""); skin.resourcePatch = resourcePatchFor(entry);
        skin.slim = name == "geometry.humanoid.customSlim"; skin.geometryEngineVersion = engineVersion(pack.manifest);
        auto uuid = stringField(pack.manifest.value("header", Json::object()), "uuid");
        if (uuid.empty()) uuid = utf8(pack.pack.dir.filename());
        skin.skinId = uuid + "_" + pack.pack.skins[match.row]; skin.fullId = skin.skinId;
        return {std::make_shared<const Skin>(std::move(skin)), match.label, {}};
    } catch (const std::exception&) { return {{}, {}, "Cannot load skin pack"}; }
}
std::optional<SkinConfig> loadSettings(const Path& file, std::string& error)
{
    error.clear();
    std::error_code missing;
    if (!std::filesystem::exists(file, missing) && !missing) return SkinConfig{};
    const auto text = readFile(file);
    if (!text) { error = "cannot read the file"; return std::nullopt; }
    try {
        const auto data = parseValue(*text);
        if (!data.is_object()) { error = "the top is not an object"; return std::nullopt; }
        SkinConfig result;
        const auto field = [&](const char* key, std::string& out) {
            const auto it = data.find(key);
            if (it == data.end() || it->is_null()) return true;
            if (!it->is_string()) { error = std::string("\"") + key + "\" is not a string"; return false; }
            out = it->get<std::string>();
            return true;
        };
        if (!field("current", result.current) || !field("main", result.main) || !field("sub", result.sub)) return std::nullopt;
        if (const auto it = data.find("names"); it != data.end() && !it->is_null()) {
            if (!it->is_object()) { error = "\"names\" is not an object"; return std::nullopt; }
            for (const auto& [name, spec] : it->items()) {
                if (spec.is_null()) continue;
                if (!spec.is_string()) { error = "names \"" + name + "\" is not a string"; return std::nullopt; }
                if (!spec.get<std::string>().empty()) result.names[lower(name)] = spec.get<std::string>();
            }
        }
        return result;
    } catch (const InvalidJson&) { error = "invalid JSON"; }
      catch (const std::exception&) { error = "invalid JSON"; }
    return std::nullopt;
}
bool saveSettings(const Path& file, const SkinConfig& settings)
{
    Path temporary;
    try {
        Json names = Json::object();
        for (const auto& [name, spec] : settings.names) names[lower(name)] = spec;
        const auto line = [](const char* key, const Json& value) { return std::string("  \"") + key + "\": " + value.dump(); };
        const std::string text = "{\n" + line("current", settings.current) + ",\n" + line("main", settings.main) + ",\n"
            + line("sub", settings.sub) + ",\n" + line("names", names) + "\n}\n";
        std::error_code error;
        if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), error);
        if (error) return false;
        static std::atomic<unsigned> serial{0};
        temporary = file;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(serial.fetch_add(1)) + L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << text; output.close();
            if (!output) throw std::runtime_error("Cannot write settings");
        }
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace settings");
        return true;
    } catch (const std::exception&) {
        std::error_code error;
        if (!temporary.empty()) std::filesystem::remove(temporary, error);
        return false;
    }
}
}
