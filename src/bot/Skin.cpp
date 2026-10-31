#include "Skin.h"
#include "Base64.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tsukuyomi::bot::skin {
namespace {
struct InvalidSkin {};
template<class T> T required(std::optional<T> value)
{
    if (!value) throw InvalidSkin{};
    return std::move(*value);
}
void fixed32(std::string& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>(value >> shift));
}
void text(std::string& out, std::string_view value)
{
    if (!protocol::writeString(out, value)) throw std::length_error("Skin field too large");
}
void count(std::string& out, std::size_t size)
{
    if (size > UINT32_MAX) throw std::length_error("Skin array too large");
    protocol::writeVarUInt32(out, static_cast<std::uint32_t>(size));
}
void writeImage(std::string& out, const Image& image)
{
    fixed32(out, image.width); fixed32(out, image.height);
    text(out, {reinterpret_cast<const char*>(image.rgba.data()), image.rgba.size()});
}
bool validImage(const Image& image)
{
    const auto pixels = std::uint64_t{image.width} * image.height;
    return pixels <= std::numeric_limits<std::size_t>::max() / 4 && pixels * 4 == image.rgba.size();
}
Image readImage(protocol::Reader& reader)
{
    Image image;
    image.width = static_cast<std::uint32_t>(required(reader.fixedBytes(4)));
    image.height = static_cast<std::uint32_t>(required(reader.fixedBytes(4)));
    const auto data = required(reader.string());
    image.rgba.assign(data.begin(), data.end());
    if (!validImage(image)) throw InvalidSkin{};
    return image;
}
void writeColour(std::string& out, std::uint32_t argb)
{
    fixed32(out, argb);
}
std::uint32_t readColour(protocol::Reader& reader)
{
    return static_cast<std::uint32_t>(required(reader.fixedBytes(4)));
}
std::uint32_t readCount(protocol::Reader& reader, std::size_t minimum)
{
    const auto size = required(reader.varUInt32());
    if (size > reader.remaining() / minimum) throw InvalidSkin{};
    return size;
}
int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::uint32_t colourFrom(const std::string& colour)
{
    if (colour == "#0") return 0;
    if ((colour.size() != 7 && colour.size() != 9) || colour[0] != '#') return 0;
    std::uint32_t result = colour.size() == 7 ? 255u : 0u;
    for (std::size_t i = 1; i < colour.size(); ++i) {
        const auto digit = hexDigit(colour[i]);
        if (digit < 0) return 0;
        result = (result << 4) | static_cast<std::uint32_t>(digit);
    }
    return result;
}
possess::Uuid uuidFrom(const std::string& value)
{
    if (value.size() != 36) throw InvalidSkin{};
    possess::Uuid uuid{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < value.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i++] != '-') throw InvalidSkin{};
            continue;
        }
        const auto high = hexDigit(value[i++]), low = hexDigit(value[i++]);
        if (high < 0 || low < 0) throw InvalidSkin{};
        uuid[at++] = static_cast<std::uint8_t>((high << 4) | low);
    }
    std::reverse(uuid.begin(), uuid.begin() + 8);
    std::reverse(uuid.begin() + 8, uuid.end());
    return uuid;
}
std::uint32_t pieceType(const std::string& value)
{
    static constexpr std::string_view names[] = {
        "unknown", "skeleton", "body", "skin", "bottom", "feet", "dress", "top", "high_pants", "hands",
        "outerwear", "facial_hair", "mouth", "eyes", "hair", "hood", "back", "face_accessory", "head", "legs",
        "left_leg", "right_leg", "arms", "left_arm", "right_arm", "capes", "classic_skin", "emote", "unsupported"
    };
    if (!value.starts_with("persona_")) throw InvalidSkin{};
    for (std::uint32_t i = 0; i < std::size(names); ++i)
        if (std::string_view(value).substr(8) == names[i]) return i;
    throw InvalidSkin{};
}
std::uint32_t jsonUInt(const nlohmann::json& value)
{
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw InvalidSkin{};
    const auto number = value.get<std::uint64_t>();
    if (number > UINT32_MAX) throw InvalidSkin{};
    return static_cast<std::uint32_t>(number);
}
std::string decoded(const nlohmann::json& value)
{
    const auto data = required(base64::decode(value.get<std::string>()));
    return {data.begin(), data.end()};
}
Image clientImage(const nlohmann::json& width, const nlohmann::json& height, const nlohmann::json& data)
{
    Image image{jsonUInt(width), jsonUInt(height), required(base64::decode(data.get<std::string>()))};
    if (!validImage(image)) throw InvalidSkin{};
    return image;
}
}
std::string serialize(const Skin& skin)
{
    std::string out;
    text(out, skin.skinId); text(out, skin.resourcePatch);
    writeImage(out, skin.image);
    count(out, skin.animations.size());
    for (const auto& animation : skin.animations) {
        writeImage(out, animation.image); protocol::writeVarUInt32(out, animation.type);
        fixed32(out, std::bit_cast<std::uint32_t>(animation.frames)); protocol::writeVarUInt32(out, animation.expression);
    }
    writeImage(out, skin.cape);
    text(out, skin.geometry); text(out, skin.geometryEngineVersion); text(out, skin.animationData);
    text(out, skin.capeId); text(out, skin.fullId);
    out.push_back(skin.slim ? 0 : 1); writeColour(out, skin.colour);
    count(out, skin.pieces.size());
    for (const auto& piece : skin.pieces) {
        text(out, piece.pieceId); fixed32(out, piece.pieceType);
        out.append(reinterpret_cast<const char*>(piece.packId.data()), piece.packId.size());
        out.push_back(piece.isDefault ? 1 : 0); text(out, piece.productId);
    }
    count(out, skin.tints.size());
    for (const auto& tint : skin.tints) {
        text(out, tint.pieceType);
        for (const auto colour : tint.colours) writeColour(out, colour);
    }
    for (const bool value : {skin.premium, skin.persona, skin.capeOnClassic, skin.primaryUser, skin.overrideAppearance})
        out.push_back(value ? 1 : 0);
    text(out, skin.trusted ? "true" : "false"); text(out, skin.profileHash);
    return out;
}
namespace {
std::optional<Skin> readSkin(protocol::Reader& reader, bool withPlayFabId)
{
    try {
        Skin skin;
        skin.skinId = required(reader.string());
        if (withPlayFabId) skin.playFabId = required(reader.string());
        skin.resourcePatch = required(reader.string());
        skin.image = readImage(reader);
        const auto animations = readCount(reader, 15);
        for (std::uint32_t i = 0; i < animations; ++i) {
            Animation animation;
            animation.image = readImage(reader); animation.type = required(reader.varUInt32());
            animation.frames = required(reader.float32()); animation.expression = required(reader.varUInt32());
            skin.animations.push_back(std::move(animation));
        }
        skin.cape = readImage(reader);
        skin.geometry = required(reader.string()); skin.geometryEngineVersion = required(reader.string()); skin.animationData = required(reader.string());
        skin.capeId = required(reader.string()); skin.fullId = required(reader.string());
        const auto arm = required(reader.byte());
        if (arm > 1) return std::nullopt;
        skin.slim = arm == 0; skin.colour = readColour(reader);
        const auto pieces = readCount(reader, 23);
        for (std::uint32_t i = 0; i < pieces; ++i) {
            PersonaPiece piece;
            piece.pieceId = required(reader.string()); piece.pieceType = static_cast<std::uint32_t>(required(reader.fixedBytes(4)));
            for (auto& byte : piece.packId) byte = required(reader.byte());
            piece.isDefault = required(reader.boolean()); piece.productId = required(reader.string());
            skin.pieces.push_back(std::move(piece));
        }
        const auto tints = readCount(reader, 17);
        for (std::uint32_t i = 0; i < tints; ++i) {
            PieceTint tint;
            tint.pieceType = required(reader.string());
            for (auto& colour : tint.colours) colour = readColour(reader);
            skin.tints.push_back(std::move(tint));
        }
        skin.premium = required(reader.boolean()); skin.persona = required(reader.boolean()); skin.capeOnClassic = required(reader.boolean());
        skin.primaryUser = required(reader.boolean()); skin.overrideAppearance = required(reader.boolean());
        const auto trusted = required(reader.string());
        if (trusted != "true" && trusted != "false") return std::nullopt;
        skin.trusted = trusted == "true"; skin.profileHash = required(reader.string());
        return skin;
    } catch (const InvalidSkin&) { return std::nullopt; }
}
}
std::optional<Skin> read(protocol::Reader& reader) { return readSkin(reader, false); }
std::optional<Skin> readLegacy(protocol::Reader& reader) { return readSkin(reader, true); }
std::string playerSkinPacket(std::uint8_t sub, const possess::Uuid& uuid, const Skin& skin)
{
    std::string out;
    protocol::writeVarUInt32(out, protocol::header(0x5d, 0, sub));
    out.append(reinterpret_cast<const char*>(uuid.data()), uuid.size());
    out += serialize(skin); text(out, ""); text(out, "");
    return out;
}
std::optional<PlayerSkin> readPlayerSkin(protocol::Bytes body)
{
    try {
        protocol::Reader reader(body);
        PlayerSkin result;
        for (auto& byte : result.uuid) byte = required(reader.byte());
        result.skin = required(read(reader));
        result.newName = required(reader.string()); result.oldName = required(reader.string());
        if (reader.remaining()) return std::nullopt;
        return result;
    } catch (const InvalidSkin&) { return std::nullopt; }
}
std::optional<Skin> fromClientData(const nlohmann::json& data)
{
    try {
        Skin skin;
        skin.skinId = data.at("SkinId").get<std::string>(); skin.playFabId = data.value("PlayFabId", std::string());
        skin.profileHash = data.value("ProfileHash", std::string());
        skin.fullId = skin.skinId;
        skin.resourcePatch = decoded(data.at("SkinResourcePatch"));
        skin.image = clientImage(data.at("SkinImageWidth"), data.at("SkinImageHeight"), data.at("SkinData"));
        const auto& animations = data.at("AnimatedImageData");
        if (!animations.is_array()) return std::nullopt;
        for (const auto& row : animations) {
            Animation animation;
            animation.image = clientImage(row.at("ImageWidth"), row.at("ImageHeight"), row.at("Image"));
            animation.type = jsonUInt(row.at("Type")); animation.expression = jsonUInt(row.at("AnimationExpression"));
            animation.frames = row.at("Frames").get<float>();
            if (!std::isfinite(animation.frames)) return std::nullopt;
            skin.animations.push_back(std::move(animation));
        }
        skin.cape = clientImage(data.at("CapeImageWidth"), data.at("CapeImageHeight"), data.at("CapeData"));
        skin.geometry = decoded(data.at("SkinGeometryData")); skin.geometryEngineVersion = decoded(data.at("SkinGeometryDataEngineVersion"));
        skin.animationData = decoded(data.at("SkinAnimationData")); skin.capeId = data.at("CapeId").get<std::string>();
        skin.slim = data.at("ArmSize").get<std::string>() == "slim"; skin.colour = colourFrom(data.at("SkinColor").get<std::string>());
        const auto& pieces = data.at("PersonaPieces");
        if (!pieces.is_array()) return std::nullopt;
        for (const auto& row : pieces) skin.pieces.push_back({row.at("PieceId").get<std::string>(), pieceType(row.at("PieceType").get<std::string>()),
            uuidFrom(row.at("PackId").get<std::string>()), row.at("IsDefault").get<bool>(), row.at("ProductId").get<std::string>()});
        const auto& tints = data.at("PieceTintColors");
        if (!tints.is_array()) return std::nullopt;
        for (const auto& row : tints) {
            PieceTint tint;
            tint.pieceType = row.at("PieceType").get<std::string>();
            if (!tint.pieceType.starts_with("persona_")) return std::nullopt;
            tint.pieceType = tint.pieceType == "persona_hand" ? "hands" : tint.pieceType.substr(8);
            const auto& colours = row.at("Colors");
            if (!colours.is_array() || colours.size() != 4) return std::nullopt;
            for (std::size_t i = 0; i < 4; ++i) tint.colours[i] = colourFrom(colours[i].get<std::string>());
            skin.tints.push_back(std::move(tint));
        }
        skin.premium = data.at("PremiumSkin").get<bool>(); skin.persona = data.at("PersonaSkin").get<bool>();
        skin.capeOnClassic = data.at("CapeOnClassicSkin").get<bool>(); skin.trusted = data.at("TrustedSkin").get<bool>();
        skin.overrideAppearance = data.value("OverrideSkin", false);
        return skin;
    } catch (const InvalidSkin&) { return std::nullopt; }
      catch (const nlohmann::json::exception&) { return std::nullopt; }
}
Skin defaultSkin()
{
    Skin skin;
    skin.resourcePatch = R"({"geometry":{"default":"geometry.humanoid.custom"}})";
    skin.image = {64, 32, std::vector<std::uint8_t>(64 * 32 * 4, 0)};
    for (std::size_t i = 3; i < skin.image.rgba.size(); i += 4) skin.image.rgba[i] = 255;
    skin.geometry = "{}"; skin.geometryEngineVersion = "0.0.0";
    return skin;
}
}
