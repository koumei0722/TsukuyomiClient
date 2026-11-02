#include "replay/JeWorldTables.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace tsukuyomi::replay::jeworld {
namespace {
struct Dictionary {
    std::vector<std::string_view> keys;
    template<std::size_t N> explicit Dictionary(const unsigned char (&data)[N]) {
        for (std::size_t at = 0; at < N && data[at];) {
            const auto* p = reinterpret_cast<const char*>(data + at);
            keys.emplace_back(p, std::strlen(p)); at += keys.back().size() + 1;
        }
    }
    template<std::size_t N> int find(std::string_view key, const unsigned short (&values)[N]) const {
        const auto it = std::lower_bound(keys.begin(), keys.end(), key);
        const auto i = static_cast<std::size_t>(it - keys.begin());
        return i < N && it != keys.end() && *it == key ? values[i] : -1;
    }
};
struct ItemNames {
    std::mutex mutex;
    std::map<int, std::string> names;
    ItemNames() {
        const Dictionary d(kBedrockItemNames);
        for (std::size_t i = 0; i < std::size(kBedrockItemIds); ++i) names.emplace(kBedrockItemIds[i], d.keys[i]);
    }
};
ItemNames& items() { static ItemNames table; return table; }
template<std::size_t N> int pair(const int (&table)[N], int key) {
    std::size_t lo = 0, hi = N / 2;
    while (lo < hi) { const auto mid = (lo + hi) / 2; if (table[mid * 2] < key) lo = mid + 1; else hi = mid; }
    return lo < N / 2 && table[lo * 2] == key ? table[lo * 2 + 1] : -1;
}
}
int blockStateOfHash(std::uint32_t hash) {
    const auto it = std::lower_bound(std::begin(kBlockHashes), std::end(kBlockHashes), hash);
    return it != std::end(kBlockHashes) && *it == hash ? kBlockHashStates[it - std::begin(kBlockHashes)] : -1;
}
int blockOfState(int state) {
    if (state < 0 || state >= kBlockFirstState[std::size(kBlockFirstState) - 1]) return -1;
    return static_cast<int>(std::upper_bound(std::begin(kBlockFirstState), std::end(kBlockFirstState), state) - std::begin(kBlockFirstState)) - 1;
}
std::string_view bedrockItemName(int id) {
    auto& t = items(); std::lock_guard lock(t.mutex);
    thread_local std::string result;
    const auto it = t.names.find(id); result = it == t.names.end() ? std::string{} : it->second; return result;
}
void setBedrockItemNames(std::vector<std::pair<int, std::string>> table) {
    std::map<int, std::string> replacement;
    for (auto& [id, name] : table) replacement[id] = std::move(name);
    auto& t = items(); std::lock_guard lock(t.mutex); t.names = std::move(replacement);
}
int javaItem(std::string_view name, std::uint32_t meta) {
    static const Dictionary d(kItemKeys);
    const auto prefix = std::string(name) + "|";
    const int exact = d.find(prefix + std::to_string(meta), kItemJava);
    return exact >= 0 ? exact : d.find(prefix + "*", kItemJava);
}
int javaEntityType(std::string_view name) { static const Dictionary d(kEntityKeys); return d.find(name, kEntityJava); }
const JavaEntityInfo* javaEntityInfo(int type) { return type >= 0 && type < static_cast<int>(std::size(kJavaEntities)) ? &kJavaEntities[type] : nullptr; }
int levelSound(std::string_view sound, std::string_view entity) {
    static const Dictionary d(kLevelSoundKeys);
    const auto prefix = std::string(sound) + "|";
    const int exact = d.find(prefix + std::string(entity), kLevelSoundJava);
    return exact >= 0 || entity.empty() ? exact : d.find(prefix, kLevelSoundJava);
}
int levelSoundExtra(std::string_view sound, int extra) { static const Dictionary d(kLevelSoundKeys); return d.find(std::string(sound) + "|#" + std::to_string(extra), kLevelSoundJava); }
int playSound(std::string_view name) { static const Dictionary d(kPlaySoundKeys); return d.find(name, kPlaySoundJava); }
int levelEventSound(int event) { return pair(kLevelEventSounds, event); }
const BlockSound* blockSound(int block) { return block >= 0 && block < static_cast<int>(std::size(kBlockSounds)) ? &kBlockSounds[block] : nullptr; }
int soundSource(int sound) { return sound >= 0 && sound < kSoundCount ? kSoundSources[sound] : 0; }
int particle(std::string_view name) { static const Dictionary d(kParticleKeys); return d.find(name, kParticleJava); }
int effect(int id) { return pair(kEffectPairs, id); }
}
