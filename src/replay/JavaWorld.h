#pragma once
#include "replay/BedrockLang.h"
#include "replay/BedrockWorld.h"
#include <functional>
#include <memory>
#include <random>

namespace tsukuyomi::replay {
struct Action { int id; Bytes bytes; };
struct LocalSlot { int slot; bedrock::ItemInstance item; };
class JavaWorld {
public:
    struct Options { Dimension dim; Uuid localUuid; std::string localName; std::shared_ptr<const BedrockLang> lang; std::uint64_t seed; };
    explicit JavaWorld(Options options);
    void setLocalIds(std::optional<std::uint64_t> runtimeId, std::optional<std::int64_t> uniqueId);
    void setDimension(Dimension dim);
    void setBlockLookup(std::function<int(int, int, int)> javaStateAt) { blockAt_ = std::move(javaStateAt); }
    void apply(const bedrock::Received& packet, std::vector<Action>& out);
    void applyLocalSlots(std::span<const LocalSlot> slots, int selected, std::vector<Action>& out);
    void endTick(const PlayerPosition& local, bool localMoved, std::vector<Action>& out);
    void snapshot(std::vector<Action>& out) const;
    void localInventory(std::vector<Action>& out) const;
    std::size_t entityCount() const { return entities_.size(); }
    int localGameMode() const;
private:
    struct Entity {
        int id = 0, type = 0, data = 0; Uuid uuid{}; bool moved = false;
        std::map<int, java::EntityData> metadata;
        std::map<int, java::ItemStack> equipment;
    };
    Options options_;
    BedrockWorld world_;
    std::map<std::uint64_t, Entity> entities_;
    std::map<Uuid, java::PlayerInfo> profiles_;
    std::map<int, std::vector<int>> passengers_;
    std::array<java::ItemStack, 41> slots_{};
    std::map<int, java::ItemStack> localEquipment_;
    std::map<int, java::EntityData> localMetadata_;
    Bytes lastHealth_, lastExperience_;
    int selected_ = 0, nextId_ = 100;
    std::function<int(int, int, int)> blockAt_;
    std::mt19937_64 random_;
    Uuid uuidFor(std::int64_t unique) const;
    int idFor(std::uint64_t runtime) const;
    int idForUnique(std::int64_t unique) const;
    java::ItemStack item(const bedrock::ItemInstance& source) const;
    std::vector<java::EntityData> metadata(int type, const bedrock::Metadata& data, const std::map<std::string, bedrock::Attribute>& attrs, const bedrock::ItemInstance* stack = nullptr) const;
    EntityPose pose(std::uint64_t runtime, const Entity& e) const;
    void spawn(std::uint64_t runtime, Entity& e, std::vector<Action>& out) const;
    void updateEntity(std::uint64_t runtime, std::vector<Action>& out);
    void updateLocal(std::vector<Action>& out);
    void updatePassengers(std::vector<Action>& out);
    std::string rawText(std::string_view json) const;
    Bytes health() const;
    Bytes experience() const;
    void addProfile(const bedrock::PlayerListEntry& player, int mode, std::vector<Action>& out);
    void boss(const bedrock::BossEvent& event, std::vector<Action>& out) const;
};
}
