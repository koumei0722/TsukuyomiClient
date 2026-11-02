#include "replay/JavaWorld.h"
#include "replay/JeWorldTables.h"
#include "replay/JeTables.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace tsukuyomi::replay {
namespace {
using namespace jeworld;
void emit(std::vector<Action>& out, Bytes bytes) { out.push_back({1, std::move(bytes)}); }
int gameMode(int mode) { return mode == 6 ? 3 : mode >= 0 && mode <= 2 ? mode : 0; }
std::int64_t number(const bedrock::Metadata& m, int key) {
    const auto it = m.find(key); if (it == m.end()) return 0;
    return std::visit([](const auto& v) -> std::int64_t { if constexpr (std::is_integral_v<std::decay_t<decltype(v)>>) return v; else return 0; }, it->second);
}
bool flag(const bedrock::Metadata& m, int bit) {
    const auto bits = static_cast<std::uint64_t>(number(m, bit < 64 ? kEntityDataKeyFlags : kEntityDataKeyFlagsTwo));
    return (bits & (std::uint64_t{1} << (bit % 64))) != 0;
}
std::string metaString(const bedrock::Metadata& m, int key) {
    const auto it = m.find(key); if (it != m.end()) if (auto* s = std::get_if<std::string>(&it->second)) return *s; return {};
}
std::vector<java::Equipment> equipment(const std::map<int, java::ItemStack>& table) {
    std::vector<java::Equipment> out; for (const auto& [slot, item] : table) out.push_back({static_cast<std::uint8_t>(slot), item}); return out;
}
std::vector<java::EntityData> dataValues(const std::map<int, java::EntityData>& table) {
    std::vector<java::EntityData> out; for (const auto& [index, value] : table) { (void)index; out.push_back(value); } return out;
}
float attribute(const std::map<std::string, bedrock::Attribute>& table, const char* name, float fallback) {
    const auto it = table.find(name); return it != table.end() && std::isfinite(it->second.value) ? it->second.value : fallback;
}
int boundedInt(float value) { return static_cast<int>(std::clamp(static_cast<double>(value), 0.0, static_cast<double>((std::numeric_limits<int>::max)()))); }
int containerSlot(int slot) { return slot >= 36 && slot < 40 ? 75 - slot : slot; }
constexpr int kHotbarSlots = 9;
bool validPosition(bedrock::Vec3 p, double scale = 0) {
    for (double value : {p.x, p.y, p.z}) {
        if (!std::isfinite(value)) return false;
        if (scale && (value * scale < (std::numeric_limits<std::int32_t>::min)() || value * scale > (std::numeric_limits<std::int32_t>::max)())) return false;
    } return true;
}
}
JavaWorld::JavaWorld(Options options) : options_(std::move(options)), random_(options_.seed) {
    if (options_.localName.empty()) options_.localName = "Player";
    java::PlayerInfo local; local.uuid = options_.localUuid; local.name = options_.localName; local.displayName = java::textComponent(options_.localName); profiles_[local.uuid] = local;
    for (int slot = 0; slot < 6; ++slot) localEquipment_[slot] = {};
    std::vector<Action> ignored; updateLocal(ignored);
}
void JavaWorld::setLocalIds(std::optional<std::uint64_t> runtime, std::optional<std::int64_t> unique) {
    world_.localRuntime_ = runtime; world_.localUnique_ = unique;
    if (runtime) { world_.entities_.erase(*runtime); entities_.erase(*runtime); }
}
void JavaWorld::setDimension(Dimension dim) { options_.dim = dim; world_.entities_.clear(); world_.links_.clear(); entities_.clear(); passengers_.clear(); }
int JavaWorld::localGameMode() const { return gameMode(world_.gameMode_); }
Uuid JavaWorld::uuidFor(std::int64_t unique) const {
    std::mt19937_64 generator(options_.seed ^ static_cast<std::uint64_t>(unique)); Uuid result{};
    for (int half = 0; half < 2; ++half) { auto bits = generator(); for (int i = 0; i < 8; ++i) result[half * 8 + i] = static_cast<std::uint8_t>(bits >> (56 - i * 8)); }
    result[6] = (result[6] & 15) | 64; result[8] = (result[8] & 63) | 128; return result;
}
int JavaWorld::idFor(std::uint64_t runtime) const {
    if (world_.localRuntime_ && runtime == *world_.localRuntime_) return 1;
    const auto it = entities_.find(runtime); return it == entities_.end() ? -1 : it->second.id;
}
int JavaWorld::idForUnique(std::int64_t unique) const {
    if (world_.localUnique_ && unique == *world_.localUnique_) return 1;
    for (const auto& [runtime, e] : world_.entities_) if (e.uniqueId == unique) return idFor(runtime); return -1;
}
java::ItemStack JavaWorld::item(const bedrock::ItemInstance& source) const {
    java::ItemStack out; if (source.empty()) return out;
    const int id = jeworld::javaItem(jeworld::bedrockItemName(source.networkId), source.metadata); if (id <= 0) return out;
    out.id = id; out.count = std::clamp<int>(source.count, 1, 99);
    if (source.userData) {
        const auto& n = *source.userData;
        if (auto* ench = n.find("ench"); ench && ench->type == 9 && !ench->list.empty()) out.glint = true;
        if (auto* damage = n.find("Damage"); damage && damage->type == 3 && damage->integer) out.damage = static_cast<int>(damage->integer);
        if (auto* display = n.find("display")) if (auto* name = display->find("Name"); name && name->type == 8) out.name = java::textComponent(name->text);
        if (auto* colour = n.find("customColor"); colour && colour->type == 3) out.colour = static_cast<std::uint32_t>(colour->integer) & 0xffffff;
    } return out;
}
std::vector<java::EntityData> JavaWorld::metadata(int type, const bedrock::Metadata& m, const std::map<std::string, bedrock::Attribute>& attrs, const bedrock::ItemInstance* stack) const {
    using namespace java; const auto* info = javaEntityInfo(type); if (!info) return {};
    const bool player = (info->flags & 2) != 0;
    std::uint8_t flags = 0;
    if (flag(m, kEntityDataFlagOnFire)) flags |= 1;
    if (flag(m, kEntityDataFlagSneaking)) flags |= 2;
    if (flag(m, kEntityDataFlagSprinting)) flags |= 8;
    if (player && flag(m, kEntityDataFlagSwimming)) flags |= 16;
    if (flag(m, kEntityDataFlagInvisible)) flags |= 32;
    if (flag(m, kEntityDataFlagGliding)) flags |= 128;
    std::vector<EntityData> out{dataByte(0, flags)};
    const bool sleeping = flag(m, kEntityDataFlagSleeping);
    if (player || sleeping) {
        const int pose = sleeping ? 2 : flag(m, kEntityDataFlagGliding) ? 1 : (flag(m, kEntityDataFlagSwimming) || flag(m, kEntityDataFlagCrawling)) ? 3 : flag(m, kEntityDataFlagSneaking) ? 5 : 0;
        out.push_back(dataInt(6, pose, 20));
    }
    if (!player) {
        auto name = metaString(m, kEntityDataKeyName);
        out.push_back(dataComponent(2, name.empty() ? std::nullopt : std::optional<Component>(textComponent(name))));
        out.push_back(dataBoolean(3, number(m, kEntityDataKeyAlwaysShowNameTag) == 1));
    }
    if (info->baby >= 0) out.push_back(dataBoolean(info->baby, flag(m, kEntityDataFlagBaby)));
    if (info->health >= 0) if (auto it = attrs.find("minecraft:health"); it != attrs.end()) out.push_back(dataFloat(info->health, it->second.value));
    if (info->item >= 0 && stack) out.push_back(dataItem(info->item, item(*stack)));
    if (info->fuse >= 0 && m.contains(kEntityDataKeyFuseTime)) out.push_back(dataInt(info->fuse, static_cast<int>(number(m, kEntityDataKeyFuseTime))));
    if (info->wool >= 0) out.push_back(dataByte(info->wool, static_cast<std::uint8_t>(number(m, kEntityDataKeyColorIndex) | (flag(m, kEntityDataFlagSheared) ? 16 : 0))));
    if (info->armorStand >= 0) out.push_back(dataByte(info->armorStand, 4));
    if (info->customisation >= 0) out.push_back(dataByte(info->customisation, 127));
    return out;
}
EntityPose JavaWorld::pose(std::uint64_t runtime, const Entity& e) const {
    const auto& s = world_.entities_.at(runtime); const auto* info = javaEntityInfo(e.type);
    return {e.id, s.position.x, s.position.y - info->offset / 100000.0, s.position.z, s.yaw, s.pitch, s.headYaw, s.onGround};
}
void JavaWorld::spawn(std::uint64_t runtime, Entity& e, std::vector<Action>& out) const {
    auto p = pose(runtime, e); emit(out, java::addEntity(e.id, e.uuid, e.type, p.x, p.y, p.z, p.pitch, p.yaw, p.headYaw, e.data));
    const auto values = dataValues(e.metadata); if (!values.empty()) emit(out, java::setEntityData(e.id, values));
    auto slots = equipment(e.equipment); if (!slots.empty()) emit(out, java::setEquipment(e.id, slots));
}
void JavaWorld::updateEntity(std::uint64_t runtime, std::vector<Action>& out) {
    const auto it = entities_.find(runtime); if (it == entities_.end()) return;
    auto& e = it->second; const auto& source = world_.entities_.at(runtime);
    const auto* drop = std::get_if<bedrock::AddItemActor>(&source.spawn);
    const auto all = metadata(e.type, source.metadata, source.attributes, drop ? &drop->item : nullptr);
    std::vector<java::EntityData> changed;
    for (const auto& value : all) { auto old = e.metadata.find(value.index); if (old == e.metadata.end() || old->second != value) changed.push_back(value); e.metadata.insert_or_assign(value.index, value); }
    if (e.metadata.contains(6) && std::none_of(all.begin(), all.end(), [](const auto& d) { return d.index == 6; })) { auto standing = java::dataInt(6, 0, 20); if (e.metadata.at(6) != standing) changed.push_back(standing); e.metadata.insert_or_assign(6, standing); }
    if (!changed.empty()) emit(out, java::setEntityData(e.id, changed));
    std::vector<java::Equipment> slots;
    for (const auto& [slot, raw] : source.equipment) { auto next = item(raw); auto old = e.equipment.find(slot); if (old == e.equipment.end() || old->second != next) slots.push_back({static_cast<std::uint8_t>(slot), next}); e.equipment[slot] = std::move(next); }
    if (!slots.empty()) emit(out, java::setEquipment(e.id, slots));
}
Bytes JavaWorld::health() const { const auto& a = world_.localAttributes_; return java::setHealth(attribute(a, "minecraft:health", 20), boundedInt(attribute(a, "minecraft:player.hunger", 20)), attribute(a, "minecraft:player.saturation", 5)); }
Bytes JavaWorld::experience() const { const auto& a = world_.localAttributes_; return java::setExperience(attribute(a, "minecraft:player.experience", 0), boundedInt(attribute(a, "minecraft:player.level", 0)), 0); }
void JavaWorld::updateLocal(std::vector<Action>& out) {
    bedrock::Metadata m;
    std::uint64_t bits = 0, bits2 = 0;
    auto set = [&](int bit) { if (bit < 64) bits |= std::uint64_t{1} << bit; else bits2 |= std::uint64_t{1} << (bit - 64); };
    for (int bit : {kEntityDataFlagOnFire, kEntityDataFlagInvisible}) if (flag(world_.localMetadata_, bit)) set(bit);
    const std::pair<int, int> inputs[] = {{kInputFlagSneaking, kEntityDataFlagSneaking}, {kInputFlagSprinting, kEntityDataFlagSprinting}, {kInputFlagStartSwimming, kEntityDataFlagSwimming}, {kInputFlagStartGliding, kEntityDataFlagGliding}, {kInputFlagStartCrawling, kEntityDataFlagCrawling}};
    for (const auto& [input, bit] : inputs) if (world_.localInput_.contains(input)) set(bit);
    m[kEntityDataKeyFlags] = std::bit_cast<std::int64_t>(bits); m[kEntityDataKeyFlagsTwo] = std::bit_cast<std::int64_t>(bits2);
    const auto all = metadata(kPlayerType, m, {}); std::vector<java::EntityData> changed;
    for (const auto& value : all) { auto old = localMetadata_.find(value.index); if (old == localMetadata_.end() || old->second != value) changed.push_back(value); localMetadata_.insert_or_assign(value.index, value); }
    if (!changed.empty()) emit(out, java::setEntityData(1, changed));
    auto hp = health(); if (hp != lastHealth_) { emit(out, hp); lastHealth_ = std::move(hp); }
    auto xp = experience(); if (xp != lastExperience_) { emit(out, xp); lastExperience_ = std::move(xp); }
}
void JavaWorld::updatePassengers(std::vector<Action>& out) {
    std::map<int, std::vector<int>> current;
    for (const auto& l : world_.links_) { const int vehicle = idForUnique(l.ridden), rider = idForUnique(l.rider); if (vehicle >= 0 && rider >= 0) current[vehicle].push_back(rider); }
    for (const auto& [vehicle, riders] : current) { auto old = passengers_.find(vehicle); if (old == passengers_.end() || old->second != riders) emit(out, java::setPassengers(vehicle, riders)); }
    for (const auto& [vehicle, riders] : passengers_) { (void)riders; if (!current.contains(vehicle)) emit(out, java::setPassengers(vehicle, {})); }
    passengers_ = std::move(current);
}
void JavaWorld::addProfile(const bedrock::PlayerListEntry& p, int mode, std::vector<Action>& out) {
    if (!profiles_.contains(p.uuid) && profiles_.size() >= 1025) return;
    java::PlayerInfo info{p.uuid, p.name, gameMode(mode), true, java::textComponent(p.name)};
    if (world_.localUnique_ && p.uniqueId == *world_.localUnique_) { info.uuid = options_.localUuid; info.name = options_.localName; }
    const auto it = profiles_.find(info.uuid);
    if (it == profiles_.end() || it->second.name != info.name || it->second.displayName != info.displayName || it->second.gameMode != info.gameMode) emit(out, java::playerInfoUpdate(45, {&info, 1}));
    profiles_[info.uuid] = std::move(info);
}
std::string JavaWorld::rawText(std::string_view text) const {
    const auto json = nlohmann::json::parse(text, nullptr, false); if (json.is_discarded()) return std::string(text);
    std::function<std::string(const nlohmann::json&, unsigned)> flatten = [&](const nlohmann::json& node, unsigned depth) -> std::string {
        if (depth > 64) return {};
        if (node.is_string()) return node.get<std::string>();
        if (!node.is_object()) return {};
        if (auto it = node.find("rawtext"); it != node.end() && it->is_array()) { std::string result; for (const auto& child : *it) result += flatten(child, depth + 1); return result; }
        if (auto it = node.find("text"); it != node.end() && it->is_string()) return it->get<std::string>();
        if (auto it = node.find("translate"); it != node.end() && it->is_string()) {
            const auto key = it->get<std::string>(); std::vector<std::string> args;
            if (auto with = node.find("with"); with != node.end()) {
                if (with->is_array()) for (const auto& arg : *with) args.push_back(flatten(arg, depth + 1));
                else if (with->is_object()) { auto rt = with->find("rawtext"); if (rt != with->end() && rt->is_array()) for (const auto& arg : *rt) args.push_back(flatten(arg, depth + 1)); }
            }
            if (options_.lang) if (auto translated = options_.lang->translate(key, args)) return *translated; return key;
        } return {};
    }; return flatten(json, 0);
}
void JavaWorld::boss(const bedrock::BossEvent& p, std::vector<Action>& out) const {
    const auto uuid = uuidFor(p.uniqueId); int colour = p.colour <= 5 ? p.colour : p.colour == kBossEventColourWhite ? 6 : 5;
    const int overlay = p.overlay <= 4 ? p.overlay : 0; const std::uint8_t flags = p.screenDarkening ? 1 : 0;
    if (p.type == kBossEventShow) emit(out, java::bossEventAdd(uuid, java::textComponent(p.title), p.health, colour, overlay, flags));
    else if (p.type == kBossEventHide) emit(out, java::bossEventRemove(uuid));
    else if (p.type == kBossEventHealthPercentage) emit(out, java::bossEventProgress(uuid, p.health));
    else if (p.type == kBossEventTitle) emit(out, java::bossEventName(uuid, java::textComponent(p.title)));
    else if (p.type == kBossEventAppearanceProperties || p.type == kBossEventTexture) { emit(out, java::bossEventStyle(uuid, colour, overlay)); if (p.type == kBossEventAppearanceProperties) emit(out, java::bossEventProperties(uuid, flags)); }
}
void JavaWorld::apply(const bedrock::Received& received, std::vector<Action>& out) {
    using namespace bedrock;
    if (std::holds_alternative<ChangeDimension>(received.packet)) return;
    world_.apply(received);
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, AddActor> || std::is_same_v<T, AddPlayer> || std::is_same_v<T, AddItemActor>) {
            const auto source = world_.entities_.find(p.runtimeId); if (source == world_.entities_.end()) return;
            if (const auto old = entities_.find(p.runtimeId); old != entities_.end()) { const int id = old->second.id; emit(out, java::removeEntities({&id, 1})); entities_.erase(old); }
            Entity entity;
            if constexpr (std::is_same_v<T, AddActor>) {
                entity.type = javaEntityType(p.type);
                if (p.type == "minecraft:boat" || p.type == "minecraft:chest_boat") {
                    const auto variant = number(p.metadata, kEntityDataKeyVariant);
                    if (variant < 0 || variant >= static_cast<std::int64_t>(std::size(kBoatTypes))) return;
                    entity.type = p.type == "minecraft:boat" ? kBoatTypes[variant] : kChestBoatTypes[variant];
                }
                if (entity.type == kFallingBlockType) { entity.data = blockStateOfHash(static_cast<std::uint32_t>(number(p.metadata, kEntityDataKeyVariant))); if (entity.data < 0) return; }
                entity.uuid = uuidFor(p.uniqueId);
            } else if constexpr (std::is_same_v<T, AddPlayer>) {
                entity.type = kPlayerType; entity.uuid = p.uuid;
                if (!profiles_.contains(p.uuid)) { PlayerListEntry e; e.uuid = p.uuid; e.uniqueId = p.uniqueId; e.name = p.name; addProfile(e, p.gameMode, out); }
            } else { entity.type = kItemType; entity.uuid = uuidFor(p.uniqueId); }
            if (!javaEntityInfo(entity.type) || !validPosition(p.position)) return;
            entity.id = nextId_++; entity.moved = true; entities_[p.runtimeId] = std::move(entity);
            std::vector<Action> ignored; updateEntity(p.runtimeId, ignored); spawn(p.runtimeId, entities_.at(p.runtimeId), out); updatePassengers(out);
        } else if constexpr (std::is_same_v<T, RemoveActor>) {
            std::vector<int> ids;
            std::erase_if(entities_, [&](const auto& e) { if (!world_.entities_.contains(e.first)) { ids.push_back(e.second.id); return true; } return false; });
            if (!ids.empty()) emit(out, java::removeEntities(ids)); updatePassengers(out);
        } else if constexpr (std::is_same_v<T, MoveActorAbsolute> || std::is_same_v<T, MoveActorDelta> || std::is_same_v<T, MovePlayer>) {
            if (auto it = entities_.find(p.runtimeId); it != entities_.end()) it->second.moved = true;
        } else if constexpr (std::is_same_v<T, SetActorData> || std::is_same_v<T, UpdateAttributes>) {
            if (idFor(p.runtimeId) == 1) updateLocal(out); else updateEntity(p.runtimeId, out);
        } else if constexpr (std::is_same_v<T, MobEquipment> || std::is_same_v<T, MobArmourEquipment>) {
            if (!received.sent && idFor(p.runtimeId) != 1) updateEntity(p.runtimeId, out);
        } else if constexpr (std::is_same_v<T, PlayerAuthInputHead>) updateLocal(out);
        else if constexpr (std::is_same_v<T, PlayerHotBar>) { if (p.select && p.window == 0 && p.selected < 9) applyLocalSlots({}, static_cast<int>(p.selected), out); }
        else if constexpr (std::is_same_v<T, SetHealth>) updateLocal(out);
        else if constexpr (std::is_same_v<T, SetActorLink>) updatePassengers(out);
        else if constexpr (std::is_same_v<T, Animate>) {
            const int id = received.sent ? 1 : idFor(p.runtimeId); if (id < 0) return;
            if (p.action == kAnimateActionSwingArm) emit(out, java::swingAnimation(id, p.hand == 1));
            else if (p.action == kAnimateActionStopSleep) emit(out, java::animate(id, 2));
            else if (p.action == kAnimateActionCriticalHit) emit(out, java::animate(id, 4));
            else if (p.action == kAnimateActionMagicCriticalHit) emit(out, java::animate(id, 5));
        } else if constexpr (std::is_same_v<T, ActorEvent>) {
            const int id = idFor(p.runtimeId); if (id < 0) return;
            if (p.type == kActorEventHurt) emit(out, java::hurtAnimation(id, 0));
            else if (p.type == kActorEventDeath) emit(out, java::entityEvent(id, 3));
            else if (p.type == kActorEventLoveHearts) emit(out, java::entityEvent(id, 18));
            else if (p.type == kActorEventTalismanActivate) emit(out, java::entityEvent(id, 35));
        } else if constexpr (std::is_same_v<T, TakeItemActor>) {
            const int itemId = idFor(p.itemId), taker = idFor(p.takerId); if (itemId < 0 || taker < 0) return;
            int count = 1; if (auto it = world_.entities_.find(p.itemId); it != world_.entities_.end()) if (auto* drop = std::get_if<AddItemActor>(&it->second.spawn)) count = drop->item.count;
            emit(out, java::takeItemEntity(itemId, taker, count));
        } else if constexpr (std::is_same_v<T, MobEffect>) {
            if (idFor(p.runtimeId) != 1) return; const int effectId = jeworld::effect(p.effect); if (effectId < 0) return;
            if (p.operation == kMobEffectRemove) emit(out, java::removeMobEffect(1, effectId));
            else if (p.operation == kMobEffectAdd || p.operation == kMobEffectModify) emit(out, java::updateMobEffect(1, effectId, p.amplifier, p.duration, static_cast<std::uint8_t>(4 | (p.particles ? 2 : 0))));
        } else if constexpr (std::is_same_v<T, SetPlayerGameType> || std::is_same_v<T, UpdatePlayerGameType>) {
            const int mode = gameMode(p.gameMode); Uuid uuid = options_.localUuid; bool local = true;
            if constexpr (std::is_same_v<T, UpdatePlayerGameType>) {
                local = world_.localUnique_ && *world_.localUnique_ == p.uniqueId;
                if (!local) {
                    bool found = false;
                    for (const auto& [key, player] : world_.players_) if (player.entry.uniqueId == p.uniqueId) { uuid = key; found = true; break; }
                    if (!found) for (const auto& [id, e] : world_.entities_) { (void)id; if (e.uniqueId == p.uniqueId) if (auto* player = std::get_if<AddPlayer>(&e.spawn)) { uuid = player->uuid; found = true; break; } }
                    if (!found) return;
                }
            }
            if (local) emit(out, java::gameEvent(3, static_cast<float>(mode)));
            if (auto it = profiles_.find(uuid); it != profiles_.end()) { it->second.gameMode = mode; emit(out, java::playerInfoUpdate(4, {&it->second, 1})); }
        } else if constexpr (std::is_same_v<T, StartGameHead>) {
            if (auto it = profiles_.find(options_.localUuid); it != profiles_.end()) it->second.gameMode = gameMode(p.gameMode);
        } else if constexpr (std::is_same_v<T, PlayerList>) {
            for (const auto& e : p.entries) {
                if (e.add) { auto it = world_.players_.find(e.uuid); if (it != world_.players_.end()) addProfile(e, it->second.gameMode, out); }
                else if (e.uuid != options_.localUuid && profiles_.erase(e.uuid)) emit(out, java::playerInfoRemove({&e.uuid, 1}));
            }
        } else if constexpr (std::is_same_v<T, Text>) {
            std::string message = p.message;
            if (p.type == kTextTypeChat && !p.source.empty()) message = "<" + p.source + "> " + message;
            else if (p.type == kTextTypeWhisper && !p.source.empty()) message = p.source + " whispers: " + message;
            else if (p.type == kTextTypeAnnouncement && !p.source.empty() && !message.starts_with("[" + p.source + "]")) message = "[" + p.source + "] " + message;
            else if (p.type == kTextTypeTranslation && p.needsTranslation && options_.lang) { if (auto translated = options_.lang->translate(message, p.parameters)) message = *translated; }
            else if (p.type == kTextTypeObjectWhisper || p.type == kTextTypeObject || p.type == kTextTypeObjectAnnouncement) message = rawText(message);
            emit(out, java::systemChat(java::textComponent(message), p.type == kTextTypePopup || p.type == kTextTypeJukeboxPopup || p.type == kTextTypeTip));
        } else if constexpr (std::is_same_v<T, SetTitle>) {
            const auto component = java::textComponent(p.action >= kTitleActionTitleTextObject ? rawText(p.text) : p.text);
            if (p.action == kTitleActionClear || p.action == kTitleActionReset) emit(out, java::clearTitles(p.action == kTitleActionReset));
            else if (p.action == kTitleActionSetTitle || p.action == kTitleActionTitleTextObject) emit(out, java::setTitleText(component));
            else if (p.action == kTitleActionSetSubtitle || p.action == kTitleActionSubtitleTextObject) emit(out, java::setSubtitleText(component));
            else if (p.action == kTitleActionSetActionBar || p.action == kTitleActionActionbarTextObject) emit(out, java::setActionBarText(component));
            else if (p.action == kTitleActionSetDurations) emit(out, java::setTitlesAnimation(p.fadeIn, p.stay, p.fadeOut));
        } else if constexpr (std::is_same_v<T, BossEvent>) {
            if (p.type == kBossEventHide || world_.bosses_.contains(p.uniqueId)) boss(p, out);
        } else if constexpr (std::is_same_v<T, LevelEvent>) {
            if (p.type == kLevelEventStartRaining || p.type == kLevelEventStopRaining) { emit(out, java::gameEvent(p.type == kLevelEventStartRaining ? 1 : 2, 0)); emit(out, java::gameEvent(7, world_.rain_)); }
            else if (p.type == kLevelEventStartThunderstorm || p.type == kLevelEventStopThunderstorm) emit(out, java::gameEvent(8, world_.thunder_));
            else if (p.type == kLevelEventParticlesDestroyBlock) {
                const int state = blockStateOfHash(static_cast<std::uint32_t>(p.data));
                if (state >= 0 && validPosition(p.position, 1)) emit(out, java::levelEvent(2001, static_cast<int>(std::floor(p.position.x)), static_cast<int>(std::floor(p.position.y)), static_cast<int>(std::floor(p.position.z)), state, false));
            } else if (p.type == kLevelEventParticlesExplosion) emit(out, java::levelParticles(p.data < 2 ? kExplosionParticle : kExplosionEmitterParticle, p.position.x, p.position.y, p.position.z, 1));
            else { const int soundId = levelEventSound(p.type); if (soundId >= 0 && validPosition(p.position, 8)) emit(out, java::sound(soundId, soundSource(soundId), p.position.x, p.position.y, p.position.z, 1, 1, random_())); }
        } else if constexpr (std::is_same_v<T, LevelSoundEvent>) {
            if (!validPosition(p.position, 8) || p.sound == "break") return;
            int soundId = -1; float volume = 1, pitch = p.baby ? 1.5f : 1;
            if (p.extraData != -1 && (p.sound == "place" || p.sound == "hit" || p.sound == "step" || p.sound == "fall" || p.sound == "item.use.on" || p.sound == "land")) {
                const auto* block = blockSound(blockOfState(blockStateOfHash(static_cast<std::uint32_t>(p.extraData))));
                if (block) {
                    const auto candidate = (p.sound == "place" || p.sound == "item.use.on") ? block->place : p.sound == "hit" ? block->hit : p.sound == "step" ? block->step : block->fall;
                    if (candidate != 0xffff) { soundId = candidate; volume = block->volume; pitch = block->pitch; }
                }
            }
            if (soundId < 0) soundId = levelSoundExtra(p.sound, p.extraData);
            if (soundId < 0) soundId = levelSound(p.sound, p.entity);
            if (soundId >= 0) emit(out, java::sound(soundId, soundSource(soundId), p.position.x, p.position.y, p.position.z, volume, pitch, random_()));
        } else if constexpr (std::is_same_v<T, PlaySound>) {
            const int soundId = jeworld::playSound(p.name); if (soundId >= 0 && validPosition(p.position, 8)) emit(out, java::sound(soundId, soundSource(soundId), p.position.x, p.position.y, p.position.z, p.volume, p.pitch, random_()));
        } else if constexpr (std::is_same_v<T, SpawnParticleEffect>) {
            const int particleId = jeworld::particle(p.name); if (particleId >= 0) emit(out, java::levelParticles(particleId, p.position.x, p.position.y, p.position.z, 1));
        } else if constexpr (std::is_same_v<T, BlockEvent>) {
            if (!blockAt_) return; const auto& b = p.position; const int block = blockOfState(blockAt_(b.x, b.y, b.z));
            if (block >= 0) emit(out, java::blockEvent(b.x, b.y, b.z, static_cast<std::uint8_t>(p.type), static_cast<std::uint8_t>(p.data), block));
        } else if constexpr (std::is_same_v<T, BlockActorData>) {
            const auto* id = p.nbt.find("id"); if (!id || id->type != 8 || (id->text != "Sign" && id->text != "HangingSign") || !blockAt_) return;
            const auto& pos = p.position; const int type = jetables::blockEntityType(blockAt_(pos.x, pos.y, pos.z)); if (type < 0) return;
            NbtWriter n; n.out.u8(10);
            for (const auto& names : {std::pair{"FrontText", "front_text"}, std::pair{"BackText", "back_text"}}) {
                const auto* side = p.nbt.find(names.first); const auto* text = side ? side->find("Text") : nullptr;
                std::string_view lines = text && text->type == 8 ? text->text : std::string_view{};
                n.tag(10, names.second); n.tag(9, "messages"); n.out.u8(8); n.out.i32(4);
                for (int line = 0; line < 4; ++line) { const auto end = lines.find('\n'); auto value = lines.substr(0, end); if (value.ends_with('\r')) value.remove_suffix(1); n.string(value); if (end == lines.npos) lines = {}; else lines.remove_prefix(end + 1); }
                n.tag(8, "color"); n.string("black"); n.tag(1, "has_glowing_text"); const auto* glowing = side ? side->find("GlowingText") : nullptr; n.out.u8(glowing && glowing->integer != 0); n.end();
            }
            n.tag(1, "is_waxed"); const auto* waxed = p.nbt.find("IsWaxed"); n.out.u8(waxed && waxed->integer != 0); n.end(); emit(out, java::blockEntityData(pos.x, pos.y, pos.z, type, n.out.data));
        }
    }, received.packet);
}
void JavaWorld::applyLocalSlots(std::span<const LocalSlot> slots, int selected, std::vector<Action>& out) {
    for (const auto& s : slots) {
        if (s.slot < 0 || s.slot >= static_cast<int>(slots_.size())) continue;
        auto converted = item(s.item); if (converted == slots_[s.slot]) continue;
        slots_[s.slot] = converted; if (s.slot < kHotbarSlots) emit(out, java::containerSetSlot(containerSlot(s.slot), converted));
    }
    if (selected >= 0 && selected < 9 && selected != selected_) { selected_ = selected; emit(out, java::setHeldSlot(selected_)); }
    const std::pair<int, int> mapping[] = {{0, selected_}, {1, 40}, {5, 36}, {4, 37}, {3, 38}, {2, 39}};
    std::vector<java::Equipment> changed;
    for (const auto& [slot, source] : mapping) {
        const auto& next = slots_[source]; const auto it = localEquipment_.find(slot);
        if ((it == localEquipment_.end() && next.count) || (it != localEquipment_.end() && it->second != next)) changed.push_back({static_cast<std::uint8_t>(slot), next});
        localEquipment_[slot] = next;
    }
    if (!changed.empty()) emit(out, java::setEquipment(1, changed));
}
void JavaWorld::endTick(const PlayerPosition& local, bool localMoved, std::vector<Action>& out) {
    std::vector<EntityPose> poses;
    if (localMoved) poses.push_back({1, local.x, local.y, local.z, local.yaw, local.pitch, local.yaw, local.onGround});
    for (auto& [runtime, entity] : entities_) if (entity.moved) { const auto& p = world_.entities_.at(runtime).position; if (validPosition(p)) poses.push_back(pose(runtime, entity)); entity.moved = false; }
    if (!poses.empty()) out.push_back({4, java::moveEntities(dimensionInfo(options_.dim).key, poses)});
}
void JavaWorld::localInventory(std::vector<Action>& out) const {
    emit(out, java::setEntityData(1, dataValues(localMetadata_)));
    for (int i = 0; i < kHotbarSlots; ++i) if (slots_[i].count) emit(out, java::containerSetSlot(containerSlot(i), slots_[i]));
    emit(out, java::setHeldSlot(selected_)); auto eq = equipment(localEquipment_); if (!eq.empty()) emit(out, java::setEquipment(1, eq));
    emit(out, health()); emit(out, experience());
    for (const auto& [id, effect] : world_.effects_) { const int mapped = jeworld::effect(id); if (mapped >= 0) emit(out, java::updateMobEffect(1, mapped, effect.amplifier, effect.duration, static_cast<std::uint8_t>(4 | (effect.particles ? 2 : 0)))); }
}
void JavaWorld::snapshot(std::vector<Action>& out) const {
    std::vector<java::PlayerInfo> players; for (const auto& [uuid, player] : profiles_) { (void)uuid; players.push_back(player); } if (!players.empty()) emit(out, java::playerInfoUpdate(45, players));
    for (const auto& [runtime, entity] : entities_) { auto copy = entity; spawn(runtime, copy, out); }
    for (const auto& [vehicle, riders] : passengers_) emit(out, java::setPassengers(vehicle, riders));
    const auto localData = dataValues(localMetadata_); emit(out, java::setEntityData(1, localData));
    for (int i = 0; i < kHotbarSlots; ++i) if (slots_[i].count) emit(out, java::containerSetSlot(containerSlot(i), slots_[i]));
    emit(out, java::setHeldSlot(selected_)); auto eq = equipment(localEquipment_); if (!eq.empty()) emit(out, java::setEquipment(1, eq));
    emit(out, health()); emit(out, experience());
    for (const auto& [id, effect] : world_.effects_) { const int mapped = jeworld::effect(id); if (mapped >= 0) emit(out, java::updateMobEffect(1, mapped, effect.amplifier, effect.duration, static_cast<std::uint8_t>(4 | (effect.particles ? 2 : 0)))); }
    if (world_.rain_ > 0) { emit(out, java::gameEvent(1, 0)); emit(out, java::gameEvent(7, world_.rain_)); }
    if (world_.thunder_ > 0) emit(out, java::gameEvent(8, world_.thunder_));
    for (const auto& [id, event] : world_.bosses_) { (void)id; boss(event, out); }
}
}
