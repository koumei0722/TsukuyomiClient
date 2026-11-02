#pragma once
#include "replay/JeWorldTables.inc"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::replay::jeworld {
int blockStateOfHash(std::uint32_t hash);
int blockOfState(int javaState);
std::string_view bedrockItemName(int networkId);
void setBedrockItemNames(std::vector<std::pair<int, std::string>> table);
int javaItem(std::string_view bedrockName, std::uint32_t meta);
int javaEntityType(std::string_view bedrockIdentifier);
const JavaEntityInfo* javaEntityInfo(int javaType);
int levelSound(std::string_view sound, std::string_view entity);
int levelSoundExtra(std::string_view sound, int extraData);
int playSound(std::string_view name);
int levelEventSound(int bedrockLevelEvent);
const BlockSound* blockSound(int javaBlock);
int soundSource(int javaSound);
int particle(std::string_view bedrockName);
int effect(int bedrockEffect);
}
