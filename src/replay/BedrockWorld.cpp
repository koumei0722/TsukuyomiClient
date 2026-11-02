#include "replay/BedrockWorld.h"
#include "replay/JeWorldTables.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tsukuyomi::replay {
namespace {
template<class Map> void merge(Map& target, const Map& values) { for (const auto& [key, value] : values) target[key] = value; }
int weatherData(float strength) {
    return static_cast<int>(std::clamp(std::round(strength * 65535.0), 0.0, static_cast<double>((std::numeric_limits<int>::max)())));
}
}
void BedrockWorld::setLocal(std::uint64_t runtime, std::int64_t unique) { localRuntime_ = runtime; localUnique_ = unique; entities_.erase(runtime); }
void BedrockWorld::reset() { *this = BedrockWorld{}; }
void BedrockWorld::apply(const bedrock::Received& received) {
    using namespace bedrock;
    using namespace jeworld;
    auto local = [&](std::uint64_t id) { return localRuntime_ && *localRuntime_ == id; };
    auto removeLinks = [&](std::int64_t unique) { std::erase_if(links_, [&](const auto& l) { return l.ridden == unique || l.rider == unique; }); };
    auto link = [&](const EntityLink& l) {
        std::erase_if(links_, [&](const auto& old) { return old.rider == l.rider; });
        if (l.type == 1 || l.type == 2) links_.push_back(l);
    };
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, StartGameHead>) { setLocal(p.runtimeId, p.uniqueId); gameMode_ = p.gameMode; }
        else if constexpr (std::is_same_v<T, ItemRegistry>) { registry_ = p; jeworld::setBedrockItemNames(p.items); for (const auto& [id, name] : p.items) if (name == "minecraft:shield") setShieldNetworkId(id); }
        else if constexpr (std::is_same_v<T, AddActor> || std::is_same_v<T, AddPlayer> || std::is_same_v<T, AddItemActor>) {
            if (local(p.runtimeId) || (!entities_.contains(p.runtimeId) && entities_.size() >= 4096)) return;
            Entity e; e.spawn = p; e.uniqueId = p.uniqueId; e.position = p.position; e.metadata = p.metadata;
            if constexpr (!std::is_same_v<T, AddItemActor>) { e.pitch = p.pitch; e.yaw = p.yaw; e.headYaw = p.headYaw; }
            if constexpr (std::is_same_v<T, AddActor>) for (const auto& attr : p.attributes) e.attributes[attr.name] = attr;
            if constexpr (std::is_same_v<T, AddPlayer>) {
                e.equipment[0] = p.heldItem;
                if (auto it = players_.find(p.uuid); it != players_.end()) it->second.gameMode = p.gameMode;
            }
            if (auto old = entities_.find(p.runtimeId); old != entities_.end()) removeLinks(old->second.uniqueId);
            entities_[p.runtimeId] = std::move(e);
            if constexpr (!std::is_same_v<T, AddItemActor>) for (const auto& l : p.links) link(l);
        } else if constexpr (std::is_same_v<T, RemoveActor>) {
            std::erase_if(entities_, [&](const auto& e) { return e.second.uniqueId == p.uniqueId; }); removeLinks(p.uniqueId);
        } else if constexpr (std::is_same_v<T, ChangeDimension>) { entities_.clear(); links_.clear(); }
        else if constexpr (std::is_same_v<T, MoveActorAbsolute> || std::is_same_v<T, MovePlayer> || std::is_same_v<T, MoveActorDelta>) {
            auto it = entities_.find(p.runtimeId); if (it == entities_.end()) return; auto& e = it->second;
            if constexpr (std::is_same_v<T, MoveActorDelta>) {
                if (p.x) e.position.x = *p.x; if (p.y) e.position.y = *p.y; if (p.z) e.position.z = *p.z;
                if (p.pitch) e.pitch = *p.pitch; if (p.yaw) e.yaw = *p.yaw; if (p.headYaw) e.headYaw = *p.headYaw;
            } else { e.position = p.position; e.pitch = p.pitch; e.yaw = p.yaw; e.headYaw = p.headYaw; }
            if constexpr (std::is_same_v<T, MoveActorAbsolute>) e.onGround = (p.flags & 1) != 0; else e.onGround = p.onGround;
        } else if constexpr (std::is_same_v<T, SetActorData>) {
            if (local(p.runtimeId)) merge(localMetadata_, p.metadata);
            else if (auto it = entities_.find(p.runtimeId); it != entities_.end()) merge(it->second.metadata, p.metadata);
        } else if constexpr (std::is_same_v<T, UpdateAttributes>) {
            if (local(p.runtimeId)) for (const auto& attr : p.attributes) localAttributes_[attr.name] = attr;
            else if (auto it = entities_.find(p.runtimeId); it != entities_.end()) for (const auto& attr : p.attributes) it->second.attributes[attr.name] = attr;
        } else if constexpr (std::is_same_v<T, MobEquipment>) {
            if (received.sent) { selected_ = p.hotBarSlot; return; }
            if (local(p.runtimeId)) return;
            if (auto it = entities_.find(p.runtimeId); it != entities_.end() && (p.window == 0 || p.window == 119)) it->second.equipment[p.window == 0 ? 0 : 1] = p.item;
        } else if constexpr (std::is_same_v<T, MobArmourEquipment>) {
            if (local(p.runtimeId)) return;
            if (auto it = entities_.find(p.runtimeId); it != entities_.end()) {
                auto& eq = it->second.equipment; eq[5] = p.helmet; eq[4] = p.chestplate; eq[3] = p.leggings; eq[2] = p.boots;
                if (!p.body.empty() || eq.contains(6)) eq[6] = p.body;
            }
        } else if constexpr (std::is_same_v<T, SetActorLink>) link(p.link);
        else if constexpr (std::is_same_v<T, SetHealth>) { Attribute a; a.name = "minecraft:health"; a.value = static_cast<float>(p.health); localAttributes_[a.name] = a; }
        else if constexpr (std::is_same_v<T, MobEffect>) {
            if (!local(p.runtimeId)) return;
            if (p.operation == kMobEffectRemove) effects_.erase(p.effect); else if (p.operation == kMobEffectAdd || p.operation == kMobEffectModify) effects_[p.effect] = p;
        } else if constexpr (std::is_same_v<T, SetPlayerGameType>) gameMode_ = p.gameMode;
        else if constexpr (std::is_same_v<T, UpdatePlayerGameType>) {
            if (localUnique_ && *localUnique_ == p.uniqueId) gameMode_ = p.gameMode;
            for (auto& [uuid, player] : players_) { (void)uuid; if (player.entry.uniqueId == p.uniqueId) player.gameMode = p.gameMode; }
            for (auto& [id, e] : entities_) { (void)id; if (e.uniqueId == p.uniqueId) if (auto* a = std::get_if<AddPlayer>(&e.spawn)) a->gameMode = p.gameMode; }
        } else if constexpr (std::is_same_v<T, PlayerHotBar>) { if (p.select && p.window == 0) selected_ = static_cast<int>(p.selected); }
        else if constexpr (std::is_same_v<T, PlayerList>) {
            for (const auto& e : p.entries) {
                if (!e.add) players_.erase(e.uuid);
                else if (players_.contains(e.uuid) || players_.size() < 1024) {
                    int mode = players_.contains(e.uuid) ? players_.at(e.uuid).gameMode : 0;
                    for (const auto& [id, entity] : entities_) { (void)id; if (auto* a = std::get_if<AddPlayer>(&entity.spawn); a && a->uuid == e.uuid) mode = a->gameMode; }
                    players_[e.uuid] = {e, mode};
                }
            }
        } else if constexpr (std::is_same_v<T, LevelEvent>) {
            const float strength = p.data > 0 ? p.data / 65535.0f : 1.0f;
            if (p.type == kLevelEventStartRaining) rain_ = strength; else if (p.type == kLevelEventStopRaining) rain_ = 0;
            else if (p.type == kLevelEventStartThunderstorm) thunder_ = strength; else if (p.type == kLevelEventStopThunderstorm) thunder_ = 0;
        } else if constexpr (std::is_same_v<T, BossEvent>) {
            if (p.type == kBossEventHide) bosses_.erase(p.uniqueId);
            else if (p.type == kBossEventShow) { if (bosses_.contains(p.uniqueId) || bosses_.size() < 64) bosses_[p.uniqueId] = p; }
            else if (auto it = bosses_.find(p.uniqueId); it != bosses_.end()) {
                auto& b = it->second;
                if (p.type == kBossEventHealthPercentage) b.health = p.health;
                else if (p.type == kBossEventTitle) { b.title = p.title; b.filteredTitle = p.filteredTitle; }
                else if (p.type == kBossEventAppearanceProperties || p.type == kBossEventTexture) {
                    b.colour = p.colour; b.overlay = p.overlay;
                    if (p.type == kBossEventAppearanceProperties) b.screenDarkening = p.screenDarkening;
                }
            }
        } else if constexpr (std::is_same_v<T, PlayerAuthInputHead>) {
            auto has = [&](int flag) { return std::find(p.flags.begin(), p.flags.end(), flag) != p.flags.end(); };
            for (int f : {kInputFlagSneaking, kInputFlagSprinting}) { if (has(f)) localInput_.insert(f); else localInput_.erase(f); }
            const std::pair<int, int> pairs[] = {{kInputFlagStartSwimming, kInputFlagStopSwimming}, {kInputFlagStartGliding, kInputFlagStopGliding}, {kInputFlagStartCrawling, kInputFlagStopCrawling}};
            for (const auto& [start, stop] : pairs) { if (has(start)) localInput_.insert(start); if (has(stop)) localInput_.erase(start); }
        }
    }, received.packet);
}
std::vector<bedrock::Received> BedrockWorld::snapshot() const {
    using namespace bedrock; using namespace jeworld;
    std::vector<Received> out; auto add = [&](Packet p) { out.push_back({false, std::move(p)}); };
    if (registry_) add(*registry_);
    PlayerList list; for (const auto& [uuid, p] : players_) { (void)uuid; list.entries.push_back(p.entry); } if (!list.entries.empty()) add(list);
    for (const auto& [id, e] : entities_) {
        std::visit([&](auto p) {
            using T = std::decay_t<decltype(p)>; p.position = e.position; p.metadata = e.metadata;
            if constexpr (!std::is_same_v<T, AddItemActor>) { p.pitch = e.pitch; p.yaw = e.yaw; p.headYaw = e.headYaw; p.links.clear(); }
            if constexpr (std::is_same_v<T, AddActor>) { p.attributes.clear(); for (const auto& [key, a] : e.attributes) { (void)key; p.attributes.push_back(a); } }
            if constexpr (std::is_same_v<T, AddPlayer>) if (auto it = e.equipment.find(0); it != e.equipment.end()) p.heldItem = it->second;
            add(p);
        }, e.spawn);
        add(MoveActorAbsolute{id, static_cast<std::uint8_t>(e.onGround ? 1 : 0), e.position, e.pitch, e.yaw, e.headYaw});
        if (!e.attributes.empty() && !std::holds_alternative<AddActor>(e.spawn)) { UpdateAttributes attrs; attrs.runtimeId = id; for (const auto& [key, a] : e.attributes) { (void)key; attrs.attributes.push_back(a); } add(attrs); }
        for (int hand : {0, 1}) if (auto it = e.equipment.find(hand); it != e.equipment.end()) add(MobEquipment{id, it->second, 0, 0, static_cast<std::uint8_t>(hand ? 119 : 0)});
        if (e.equipment.contains(2) || e.equipment.contains(3) || e.equipment.contains(4) || e.equipment.contains(5) || e.equipment.contains(6)) {
            auto item = [&](int slot) { auto it = e.equipment.find(slot); return it == e.equipment.end() ? ItemInstance{} : it->second; };
            add(MobArmourEquipment{id, item(5), item(4), item(3), item(2), item(6)});
        }
    }
    for (const auto& l : links_) add(SetActorLink{l});
    if (rain_ > 0) add(LevelEvent{kLevelEventStartRaining, {}, weatherData(rain_)});
    if (thunder_ > 0) add(LevelEvent{kLevelEventStartThunderstorm, {}, weatherData(thunder_)});
    for (const auto& [id, b] : bosses_) { (void)id; add(b); }
    for (const auto& [uuid, p] : players_) { (void)uuid; if (!localUnique_ || p.entry.uniqueId != *localUnique_) add(UpdatePlayerGameType{p.gameMode, p.entry.uniqueId, 0}); }
    add(SetPlayerGameType{gameMode_});
    if (localRuntime_) {
        add(SetActorData{*localRuntime_, localMetadata_, 0});
        UpdateAttributes a; a.runtimeId = *localRuntime_; for (const auto& [name, value] : localAttributes_) { (void)name; a.attributes.push_back(value); } add(a);
        for (const auto& [id, value] : effects_) { (void)id; auto e = value; e.operation = kMobEffectAdd; add(e); }
    }
    PlayerAuthInputHead input; input.flags.assign(localInput_.begin(), localInput_.end()); add(input);
    add(PlayerHotBar{static_cast<std::uint32_t>(selected_), 0, true}); return out;
}
}
