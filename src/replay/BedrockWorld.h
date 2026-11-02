#pragma once
#include "replay/BedrockPackets.h"
#include <set>

namespace tsukuyomi::replay {
class JavaWorld;
class BedrockWorld {
public:
    void apply(const bedrock::Received& packet);
    void setLocal(std::uint64_t runtimeId, std::int64_t uniqueId);
    std::optional<std::uint64_t> localRuntime() const { return localRuntime_; }
    std::optional<std::int64_t> localUnique() const { return localUnique_; }
    void reset();
    std::vector<bedrock::Received> snapshot() const;
private:
    friend class JavaWorld;
    struct Entity {
        std::variant<bedrock::AddActor, bedrock::AddPlayer, bedrock::AddItemActor> spawn;
        std::int64_t uniqueId = 0;
        bedrock::Vec3 position;
        float pitch = 0, yaw = 0, headYaw = 0;
        bool onGround = false;
        bedrock::Metadata metadata;
        std::map<std::string, bedrock::Attribute> attributes;
        std::map<int, bedrock::ItemInstance> equipment;
    };
    struct ListedPlayer { bedrock::PlayerListEntry entry; int gameMode = 0; };
    std::map<std::uint64_t, Entity> entities_;
    std::map<Uuid, ListedPlayer> players_;
    std::map<std::int64_t, bedrock::BossEvent> bosses_;
    std::vector<bedrock::EntityLink> links_;
    std::optional<std::uint64_t> localRuntime_;
    std::optional<std::int64_t> localUnique_;
    bedrock::Metadata localMetadata_;
    std::map<std::string, bedrock::Attribute> localAttributes_;
    std::map<int, bedrock::MobEffect> effects_;
    std::set<int> localInput_;
    int gameMode_ = 0, selected_ = 0;
    float rain_ = 0, thunder_ = 0;
    std::optional<bedrock::ItemRegistry> registry_;
};
}
