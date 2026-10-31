#pragma once
#include "BotProtocol.h"
#include "Possess.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot::skin {
struct Image { std::uint32_t width = 0, height = 0; std::vector<std::uint8_t> rgba; };
struct Animation { Image image; std::uint32_t type = 0; float frames = 0; std::uint32_t expression = 0; };
struct PersonaPiece { std::string pieceId; std::uint32_t pieceType = 0; possess::Uuid packId{}; bool isDefault = false; std::string productId; };
struct PieceTint { std::string pieceType; std::array<std::uint32_t, 4> colours{}; };
struct Skin {
    std::string skinId, playFabId, resourcePatch;
    Image image;
    std::vector<Animation> animations;
    Image cape;
    std::string geometry, geometryEngineVersion, animationData, capeId, fullId;
    bool slim = false;
    std::uint32_t colour = 0;
    std::vector<PersonaPiece> pieces;
    std::vector<PieceTint> tints;
    bool premium = false, persona = false, capeOnClassic = false, primaryUser = true, overrideAppearance = false, trusted = false;
    std::string profileHash;
};
std::string serialize(const Skin&);
std::optional<Skin> read(protocol::Reader&);
std::optional<Skin> readLegacy(protocol::Reader&);
std::string playerSkinPacket(std::uint8_t sub, const possess::Uuid& uuid, const Skin&);
struct PlayerSkin { possess::Uuid uuid{}; Skin skin; std::string newName, oldName; };
std::optional<PlayerSkin> readPlayerSkin(protocol::Bytes body);
std::optional<Skin> fromClientData(const nlohmann::json& clientData);
Skin defaultSkin();
}
