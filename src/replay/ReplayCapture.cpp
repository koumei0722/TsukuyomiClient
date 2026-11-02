#include "replay/ReplayCapture.h"
#include "replay/JeTables.h"
#include "replay/BedrockLang.h"
#include "replay/BedrockPackets.h"
#include "replay/BedrockWorld.h"
#include "replay/JavaWorld.h"
#include "replay/JeWorldTables.h"
#include "replay/PacketTap.h"
#include "bot/BotProtocol.h"
#include "bot/GameCodec.h"
#include "bot/LocalPlayerIds.h"
#include "bot/PlayerInteract.h"
#include "core/Logger.h"
#include "game/GameOptions.h"
#include "core/Paths.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/ChunkMemory.h"
#include "game/ChunkResend.h"
#include "game/GameData.h"
#include "game/GameModeState.h"
#include "game/SubChunkTable.h"
#include "modules/ModuleManager.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <random>
#include <exception>
#include <fstream>
#include <iterator>

namespace tsukuyomi::replay {
namespace {
subchunks::Table g_subs;
std::atomic<bool> g_recording{false};
struct Write { int x, y, z; unsigned index; void* sub; };
std::mutex g_writesMutex;
std::vector<Write> g_writes;
bool g_overflow = false;
void resetPointers()
{
    std::lock_guard lock(g_writesMutex);
    g_subs.clear(); g_writes.clear(); g_overflow = false;
}
std::string timestamp()
{
    const auto now = std::time(nullptr); std::tm local{}; localtime_s(&local, &now);
    char out[32]{}; std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &local); return out;
}
Uuid randomUuid()
{
    std::random_device random;
    Uuid uuid{}; for (auto& byte : uuid) byte = static_cast<std::uint8_t>(random());
    uuid[6] = (uuid[6] & 15) | 64; uuid[8] = (uuid[8] & 63) | 128;
    return uuid;
}
using Pair = std::pair<const void*, const void*>;
struct PairHash {
    std::size_t operator()(const Pair& p) const { return std::hash<const void*>{}(p.first) ^ (std::hash<const void*>{}(p.second) << 1); }
};
bool dimensionOf(const chunkmem::ChunkShape& shape, Dimension& dim)
{
    if (shape.minY == -64 && shape.maxY == 319) dim = Dimension::Overworld;
    else if (shape.minY == 0 && shape.maxY == 127) dim = Dimension::Nether;
    else if (shape.minY == 0 && shape.maxY == 255) dim = Dimension::End;
    else return false;
    return true;
}
PlayerPosition playerPosition()
{
    float x = 0, y = 0, z = 0; GameData::instance().playerFeet(x, y, z);
    const auto view = GameData::instance().playerView(); return {x, y, z, view.yaw, view.pitch, false};
}
std::shared_ptr<const BedrockLang> loadLang()
{
    auto lang = std::make_shared<BedrockLang>();
    std::string language;
    if (!gameoptions::getString("game_language", language) || language.empty()) language = "en_US";
    const auto packs = paths::dataDir().parent_path() / "data" / "resource_packs";
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(packs, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end());
    auto readAll = [&](const std::string& name) {
        for (const auto& dir : dirs) {
            std::ifstream file(dir / "texts" / (name + ".lang"), std::ios::binary);
            if (!file) continue;
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            lang->addFile(text);
        }
    };
    readAll(language);
    if (lang->empty() && language != "en_US") readAll("en_US");
    if (lang->empty()) log().warn(L"Replay: no Bedrock language file under {}; chat translations stay as keys", packs.wstring());
    return lang;
}
void transposeLight(const std::uint8_t* source, Bytes& out)
{
    out.assign(2048, 0);
    for (unsigned x = 0; x < 16; ++x) for (unsigned z = 0; z < 16; ++z) for (unsigned y = 0; y < 16; ++y) {
        const unsigned be = (x << 8) | (z << 4) | y, je = (y << 8) | (z << 4) | x;
        const auto nibble = (source[be / 2] >> ((be & 1) * 4)) & 15;
        out[je / 2] |= static_cast<std::uint8_t>(nibble << ((je & 1) * 4));
    }
}
}
void onSubChunkWrite(void* self, unsigned int layer, unsigned int index)
{
    if (!g_recording.load(std::memory_order_acquire) || layer > 1 || index >= 4096) return;
    std::lock_guard lock(g_writesMutex);
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const auto i = g_subs.findByPointer(self); const auto* slot = g_subs.slot(i);
    if (!slot || i == subchunks::Table::kNone) return;
    if (g_writes.size() >= 65536) { g_overflow = true; return; }
    g_writes.push_back({slot->baseX.load(), slot->baseY.load(), slot->baseZ.load(), index, self});
}
struct ReplayCapture::Impl {
    ReplayWorker worker;
    bool active = false, launched = false, initialDone = false, initialListed = false;
    std::uint64_t initialListedAt = 0;
    std::size_t initialCount = 0;
    bool toldHeight = false;
    Dimension dim = Dimension::Overworld;
    std::uint64_t generation = 0, lastRegionAt = 0, settleUntil = 0, lastScan = 0, lastTick = 0, lastView = 0, lastTime = 0;
    void* region = nullptr; void* source = nullptr;
    struct Signature {
        void* chunk = nullptr; std::uint8_t* table = nullptr;
        std::vector<std::array<const void*, 4>> pointers;
        std::int64_t status = 0;
        bool operator==(const Signature&) const = default;
    };
    struct Tracked { chunkmem::ChunkRef ref{}; Signature signature; std::uint64_t stableSince = 0, loadingSince = 0, askedAt = 0; int asks = 0; bool pending = true, loading = false, read = false; };
    std::map<std::pair<int, int>, Tracked> columns;
    std::set<std::pair<int, int>> initial;
    std::unordered_map<Pair, std::uint16_t, PairHash> blockCache;
    std::unordered_map<const void*, int> biomeCache;
    std::map<std::tuple<int, int, int>, BlockChange> changes;
    BedrockWorld world;
    std::uint64_t worldSeen = 0, idsAt = 0, inventoryAt = 0;
    int selected = 0, sentSelected = -1;
    struct SlotKey { std::int32_t id = -1; std::uint16_t count = 0; std::int32_t netId = 0; bool operator==(const SlotKey&) const = default; };
    std::array<SlotKey, 41> slotKeys{};
    unsigned parseFailures = 0;
    bool overflowTold = false, inventoryTold = false;
    void pump()
    {
        const auto now = GetTickCount64();
        if (const auto generation = blockwrite::worldGeneration(); generation != worldSeen) {
            worldSeen = generation; world.reset(); selected = 0; sentSelected = -1; slotKeys.fill({});
        }
        if (!world.localRuntime() && now - idsAt >= 1000) {
            idsAt = now;
            if (const auto ids = bot::readLocalPlayerIds()) world.setLocal(ids->runtimeId, ids->uniqueId);
        }
        auto taken = PacketTap::instance().take();
        if (PacketTap::instance().takeOverflowed() && !std::exchange(overflowTold, true))
            log().warn(L"Replay: the packet tap overflowed (the game thread did not read it for a long time); the world copy may miss entities");
        if (taken.empty()) return;
        std::vector<bedrock::Received> batch;
        for (auto& tapped : taken) {
            auto parsed = bedrock::parse(tapped.id, tapped.body, tapped.sent);
            if (!parsed) {
                if (parseFailures++ < 8)
                    log().warn(L"Replay: cannot read {} packet {:#x} ({} bytes)", tapped.sent ? L"sent" : L"received", tapped.id, tapped.body.size());
                continue;
            }
            if (const auto* registry = std::get_if<bedrock::ItemRegistry>(&*parsed)) {
                std::vector<std::pair<int, std::string>> names = registry->items;
                for (const auto& [id, name] : names) if (name == "minecraft:shield") bedrock::setShieldNetworkId(id);
                jeworld::setBedrockItemNames(std::move(names));
            }
            if (const auto* equipment = std::get_if<bedrock::MobEquipment>(&*parsed); equipment && tapped.sent && equipment->window == 0)
                selected = equipment->hotBarSlot;
            if (const auto* hotbar = std::get_if<bedrock::PlayerHotBar>(&*parsed); hotbar && !tapped.sent && hotbar->window == 0 && hotbar->select)
                selected = static_cast<int>(hotbar->selected);
            bedrock::Received received{tapped.sent, std::move(*parsed)};
            world.apply(received);
            if (active && launched) batch.push_back(std::move(received));
        }
        if (!batch.empty()) worker.post(ReplayWorker::BedrockPackets{std::move(batch)});
    }
    void sampleInventory(std::uint64_t now)
    {
        if (now - inventoryAt < 250) return;
        inventoryAt = now;
        void* actor = GameData::instance().player();
        if (!actor || !bot::playerinteract::layoutReady()) {
            if (!std::exchange(inventoryTold, true)) log().warn(L"Replay: cannot read your inventory; the replay shows you empty-handed");
            return;
        }
        const auto snap = bot::playerinteract::snapshot(actor);
        if (!snap) return;
        ReplayWorker::LocalSlots out;
        auto consider = [&](int index, const bot::Item& item, std::uint8_t window, std::uint32_t slot) {
            const SlotKey key = item.empty() ? SlotKey{0, 0, 0} : SlotKey{item.id, item.count, item.netId};
            if (key == slotKeys[index]) return;
            const auto bytes = bot::gamecodec::inventorySlotToGame(window, slot, item);
            const auto head = bytes.empty() ? std::nullopt : bot::protocol::readHeader(bot::protocol::bytes(bytes));
            if (!head) return;
            const auto body = std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()).subspan(head->bodyOffset);
            const auto parsed = bedrock::parse(0x32, body, false);
            const auto* packet = parsed ? std::get_if<bedrock::InventorySlot>(&*parsed) : nullptr;
            if (!packet) return;
            out.slots.push_back({index, packet->item}); slotKeys[index] = key;
        };
        for (int i = 0; i < 36; ++i) consider(i, snap->slots[i], 0, static_cast<std::uint32_t>(i));
        for (int i = 0; i < 4; ++i) consider(36 + i, snap->armor[i], 120, static_cast<std::uint32_t>(i));
        consider(40, snap->offhand, 119, 0);
        if (out.slots.empty() && selected == sentSelected) return;
        out.selected = selected; sentSelected = selected;
        worker.post(std::move(out));
    }
    void forgetPointers(int cx, int cz)
    {
        const auto& info = dimensionInfo(dim);
        std::lock_guard lock(g_writesMutex);
        for (int s = 0; s < info.sections; ++s) {
            const auto i = g_subs.find(cx * 16, (s + info.minSection) * 16, cz * 16);
            if (auto* slot = g_subs.slot(i)) slot->ptr[0].store(nullptr);
        }
    }
    void forgetChanges(int cx, int cz)
    {
        std::erase_if(changes, [=](const auto& change) { return (change.second.x >> 4) == cx && (change.second.z >> 4) == cz; });
    }
    void clearColumns()
    {
        resetPointers(); columns.clear(); initial.clear(); changes.clear(); initialDone = false; initialListed = false;
    }
    void stop(bool save)
    {
        if (!launched && save) ModuleManager::instance().postNotice("Replay: stopped before the initial world read; no recording saved");
        if (askedSubChunks > 0) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 32)
                log().info(L"Replay: asked the server for {} chunk sections the game left unrequested ({} chunk asks) during the recording",
                           askedSubChunks, askedColumns);
        }
        active = false; g_recording.store(false, std::memory_order_release); clearColumns();
        blockCache.clear(); biomeCache.clear();
        if (launched) worker.post(ReplayWorker::Stop{save});
    }
    std::uint16_t state(const void* block, const void* extra)
    {
        const Pair key{block, extra};
        if (const auto it = blockCache.find(key); it != blockCache.end()) return it->second;
        char name[256]{}; int javaState = 0;
        if (block && blocks::nameOfBlock(block, name, sizeof(name))) {
            std::vector<structure::StateValue> values;
            std::vector<std::pair<std::string, std::string>> states;
            if (blocks::typedStatesOfBlock(block, values)) for (const auto& v : values) states.emplace_back(v.name, v.normalized());
            javaState = jetables::bedrockToJava(name, states);
        } else if (block) javaState = 1;
        char extraName[256]{};
        if (extra && blocks::nameOfBlock(extra, extraName, sizeof(extraName))
            && (std::string_view(extraName) == "minecraft:water" || std::string_view(extraName) == "minecraft:flowing_water")) {
            if (jetables::stateFlags(javaState) & 1) javaState = jetables::bedrockToJava("minecraft:water", {{"liquid_depth", "0"}});
            else if (const int wet = jetables::waterlogged(javaState); wet >= 0) javaState = wet;
        }
        const auto result = static_cast<std::uint16_t>(javaState); blockCache.emplace(key, result); return result;
    }
    bool signature(const chunkmem::ChunkRef& ref, Signature& sig, bool& loading)
    {
        chunkmem::ChunkShape shape{};
        if (!chunkmem::shapeOf(ref.chunk, shape)) return false;
        Dimension shapeDim;
        if (!dimensionOf(shape, shapeDim) || shapeDim != dim) return false;
        sig.chunk = ref.chunk; sig.table = shape.table; loading = false;
        for (int i = 0; i < shape.subCount; ++i) {
            chunkmem::SubView view{};
            if (!chunkmem::viewSub(shape.table + i * chunkmem::kSubChunkBytes, view)) return false;
            sig.pointers.push_back({view.storage[0], view.storage[1], view.sky, view.block});
            sig.status += view.status;
            if (view.status >= 2 && view.status <= 6) loading = true;
        }
        return true;
    }
    bool read(Tracked& tracked)
    {
        chunkmem::ChunkShape shape{}; if (!chunkmem::shapeOf(tracked.ref.chunk, shape)) return false;
        const auto& info = dimensionInfo(dim);
        Column column{dim, tracked.ref.cx, tracked.ref.cz, {}}; column.sections.resize(info.sections);
        std::vector<std::uint8_t*> pointers(info.sections);
        for (int s = 0; s < info.sections; ++s) {
            auto& section = column.sections[s]; section.biomes.fill(static_cast<std::uint16_t>(info.biome));
            const int y = (s + info.minSection) * 16;
            if (y > shape.maxY) continue;
            bool aboveTop = false;
            auto* sub = static_cast<std::uint8_t*>(chunkmem::subChunkInChunk(static_cast<std::uint8_t*>(tracked.ref.chunk), y, aboveTop));
            if (!sub && !aboveTop) return false;
            if (sub) {
                chunkmem::SubView view{}; if (!chunkmem::viewSub(sub, view)) return false;
                if (!view.storage[0]) { pointers[s] = sub; continue; }
                const void* base[4096]{}; const void* extra[4096]{};
                if (!chunkmem::readStorage(view.storage[0], base) || (view.storage[1] && !chunkmem::readStorage(view.storage[1], extra))) return false;
                section.blocks.resize(4096);
                for (unsigned x = 0; x < 16; ++x) for (unsigned z = 0; z < 16; ++z) for (unsigned ly = 0; ly < 16; ++ly) {
                    const unsigned be = (x << 8) | (z << 4) | ly, je = (ly << 8) | (z << 4) | x;
                    section.blocks[je] = state(base[be], extra[be]);
                }
                std::uint8_t light[2048]{};
                if (view.sky) { if (!chunkmem::copyLight(view.sky, light)) return false; transposeLight(light, section.sky); }
                if (view.block) { if (!chunkmem::copyLight(view.block, light)) return false; transposeLight(light, section.block); }
                pointers[s] = sub;
            }
            const void* biomes[64]{};
            if (chunkmem::readBiomes(tracked.ref.chunk, s, biomes)) for (unsigned i = 0; i < 64; ++i) {
                auto it = biomeCache.find(biomes[i]);
                if (it == biomeCache.end()) {
                    char name[256]{}; int id = -1;
                    if (chunkmem::biomeName(biomes[i], name, sizeof(name))) id = jetables::biomeOf(name);
                    it = biomeCache.emplace(biomes[i], id).first;
                }
                if (it->second >= 0) section.biomes[i] = static_cast<std::uint16_t>(it->second);
            }
        }
        if (dim == Dimension::Overworld) {
            int lowest = -1, highest = -1;
            for (int s = 0; s < info.sections; ++s) {
                if (column.sections[s].sky.size() != 2048) continue;
                if (lowest < 0) lowest = s;
                highest = s;
            }
            for (int s = 0; lowest >= 0 && s < highest; ++s) {
                auto& section = column.sections[s];
                if (section.sky.size() == 2048) continue;
                const bool allAir = std::all_of(section.blocks.begin(), section.blocks.end(),
                                                [](std::uint16_t v) { return (jetables::stateFlags(v) & 1) != 0; });
                if (s < lowest || !allAir) section.sky.assign(2048, 0);
            }
        }
        {
            std::lock_guard lock(g_writesMutex);
            for (int s = 0; s < info.sections; ++s) {
                const int bx = column.cx * 16, by = (s + info.minSection) * 16, bz = column.cz * 16;
                auto i = g_subs.find(bx, by, bz);
                if (i == subchunks::Table::kNone) i = g_subs.add(bx, by, bz);
                auto* slot = g_subs.slot(i);
                if (!slot) return false;
                if (slot->ptr[0].exchange(pointers[s]) != pointers[s] && !g_subs.notePointer(pointers[s], i)) return false;
            }
        }
        worker.post(ReplayWorker::ColumnEvent{std::move(column)}); tracked.read = true; tracked.pending = false;
        initial.erase({tracked.ref.cx, tracked.ref.cz}); return true;
    }
    void drain()
    {
        std::vector<Write> writes;
        { std::lock_guard lock(g_writesMutex); writes.swap(g_writes); }
        for (const auto& w : writes) {
            const auto it = columns.find({w.x >> 4, w.z >> 4});
            if (it == columns.end() || !it->second.read || it->second.pending) continue;
            const auto i = g_subs.find(w.x, w.y, w.z); auto* slot = g_subs.slot(i);
            if (!slot || slot->ptr[0].load() != w.sub) continue;
            chunkmem::SubView view{}; if (!chunkmem::viewSub(static_cast<std::uint8_t*>(w.sub), view)) continue;
            const auto* base = chunkmem::storageGet(view.storage[0], w.index);
            const auto* extra = view.storage[1] ? chunkmem::storageGet(view.storage[1], w.index) : nullptr;
            if (!base || (view.storage[1] && !extra)) continue;
            const int x = w.x + static_cast<int>(w.index >> 8), y = w.y + static_cast<int>(w.index & 15), z = w.z + static_cast<int>((w.index >> 4) & 15);
            changes[{x, y, z}] = {x, y, z, state(base, extra)};
        }
        if (!initialDone && !changes.empty()) flushChanges();
    }
    void flushChanges()
    {
        if (changes.empty()) return;
        ReplayWorker::Blocks blocks;
        for (const auto& [key, b] : changes) { (void)key; blocks.changes.push_back(b); }
        worker.post(std::move(blocks)); changes.clear();
    }
    void scan(std::uint64_t now)
    {
        std::vector<chunkmem::ChunkRef> refs;
        if (!chunkmem::listChunks(region, refs)) return;
        bool overflow = false;
        { std::lock_guard lock(g_writesMutex); overflow = std::exchange(g_overflow, false); }
        std::set<std::pair<int, int>> found;
        for (const auto& ref : refs) {
            Signature sig; bool loading = false;
            if (!signature(ref, sig, loading)) continue;
            const auto key = std::pair{ref.cx, ref.cz}; found.insert(key);
            auto [it, added] = columns.try_emplace(key);
            auto& col = it->second; col.ref = ref;
            if (added || overflow || !(col.signature == sig)) {
                forgetPointers(ref.cx, ref.cz);
                forgetChanges(ref.cx, ref.cz);
                col.signature = std::move(sig); col.stableSince = now; col.pending = true; col.asks = 0;
            }
            if (loading && !col.loading) col.loadingSince = now;
            col.loading = loading;
        }
        for (auto it = columns.begin(); it != columns.end();) {
            if (found.contains(it->first)) { ++it; continue; }
            const auto key = it->first;
            worker.post(ReplayWorker::Forget{key.first, key.second}); forgetPointers(key.first, key.second);
            forgetChanges(key.first, key.second);
            initial.erase(key); it = columns.erase(it);
        }
        if (!initialListed) {
            initialListed = true; initialListedAt = now;
            for (const auto& [key, col] : columns) { (void)col; initial.insert(key); }
            initialCount = initial.size();
            ModuleManager::instance().postNotice("Replay: reading " + std::to_string(initialCount) + " chunks...");
        }
        askPendingSubChunks(now);
    }
    std::uint64_t askedSubChunks = 0, askedColumns = 0;
    bool askTold = false;
    void askPendingSubChunks(std::uint64_t now)
    {
        constexpr int kBudget = 256, kMaxAsks = 3;
        constexpr std::uint64_t kAskIntervalMs = 3000, kGraceMs = 2000;
        std::vector<chunkresend::PendingChunk> batch;
        std::vector<Tracked*> owners;
        for (auto& [key, col] : columns) {
            if (!col.loading || now - col.loadingSince < kGraceMs || col.asks >= kMaxAsks
                || (col.askedAt != 0 && now - col.askedAt < kAskIntervalMs)) continue;
            chunkresend::PendingChunk c;
            c.chunk = col.ref.chunk; c.chunkX = key.first; c.chunkZ = key.second; c.pending = -1;
            batch.push_back(c); owners.push_back(&col);
        }
        if (batch.empty()) return;
        if (!chunkresend::askPending(batch.data(), batch.size(), kBudget)) {
            if (!std::exchange(askTold, true))
                log().warn(L"Replay: cannot ask the server for chunk sections the game left unrequested; deep underground far away may be recorded as air");
            return;
        }
        int queued = 0, columnsQueued = 0;
        for (std::size_t i = 0; i < batch.size(); ++i) {
            const auto& c = batch[i];
            if (c.pending < 0) continue;
            Tracked& col = *owners[i];
            col.askedAt = now;
            if (c.pending > 0) ++col.asks;
            if (c.queued > 0) { col.loadingSince = now; queued += c.queued; ++columnsQueued; }
        }
        askedSubChunks += static_cast<std::uint64_t>(queued); askedColumns += static_cast<std::uint64_t>(columnsQueued);
        static std::atomic<int> logged{0};
        if (queued > 0 && logged.fetch_add(1, std::memory_order_relaxed) < 8)
            log().info(L"Replay: asked the server for {} chunk sections the game left unrequested in {} chunks", queued, columnsQueued);
    }
    void view()
    {
        if (!active) return;
        const auto now = GetTickCount64();
        void* newRegion = blockwrite::renderRegion();
        if (!newRegion || !blockwrite::regionIsAlive(newRegion)) return;
        lastRegionAt = now;
        void* newSource = chunkmem::chunkSource(newRegion);
        if (newRegion != region || newSource != source) {
            region = newRegion; source = newSource; settleUntil = now + 1000;
            blockCache.clear(); biomeCache.clear();
        }
        if (now < settleUntil || !source) { lastTick = now; lastView = now; return; }
        float fx = 0, fy = 0, fz = 0;
        if (!GameData::instance().playerFeet(fx, fy, fz)) return;
        chunkmem::ChunkShape feetShape{}; Dimension newDim;
        void* feetChunk = chunkmem::gridChunkAt(region, static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fz)));
        if (!chunkmem::shapeOf(feetChunk, feetShape)) { lastTick = now; return; }
        if (!dimensionOf(feetShape, newDim)) {
            if (!toldHeight) { toldHeight = true; ModuleManager::instance().postNotice("Replay: this dimension's height is not supported; waiting"); }
            lastTick = now; return;
        }
        if (!launched) {
            const auto stamp = timestamp(); const auto dir = paths::replaysDir(); auto zip = dir / ("Replay-" + stamp + ".zip");
            std::error_code ec; unsigned suffix = 0;
            while (std::filesystem::exists(zip, ec)) zip = dir / ("Replay-" + stamp + "-" + std::to_string(++suffix) + ".zip");
            const auto uuid = randomUuid();
            if (!worker.launch(dir / (".recording-" + stamp + "-" + uuidString(uuid)), zip)) {
                ModuleManager::instance().postNotice("Replay: could not start recording worker"); stop(false); return;
            }
            int mode = GameModeState::instance().currentMode(); mode = mode == 6 ? 3 : (mode >= 0 && mode <= 2 ? mode : 0);
            dim = newDim;
            ReplayWorker::Start start{dim, uuid, mode, static_cast<std::uint64_t>(chunkmem::readDayTime(region)), playerPosition()};
            start.world = world.snapshot();
            start.localRuntime = world.localRuntime(); start.localUnique = world.localUnique();
            for (const auto& received : start.world)
                if (const auto* list = std::get_if<bedrock::PlayerList>(&received.packet))
                    for (const auto& row : list->entries)
                        if (row.add && start.localUnique && row.uniqueId == *start.localUnique) start.localName = row.name;
            start.lang = loadLang();
            worker.post(std::move(start));
            launched = true;
        } else if (newDim != dim) {
            worker.post(ReplayWorker::DimensionEvent{newDim}); clearColumns(); blockCache.clear(); biomeCache.clear(); dim = newDim; lastScan = 0;
        }
        if (now - lastScan >= 100) { scan(now); lastScan = now; }
        LARGE_INTEGER begin{}, frequency{}; QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&frequency);
        for (auto& [key, col] : columns) {
            (void)key;
            if (!col.pending || (col.loading && now - col.loadingSince < 3000) || now - col.stableSince < 500) continue;
            if (!read(col)) col.stableSince = now;
            LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
            if ((end.QuadPart - begin.QuadPart) * 1000 >= frequency.QuadPart * 3) break;
        }
        drain();
        sampleInventory(now);
        if (!initialDone && initialListed && (initial.empty() || now - initialListedAt >= 15000)) {
            worker.post(ReplayWorker::Player{playerPosition()}); worker.post(ReplayWorker::InitialDone{}); initialDone = true; lastTick = now;
            const auto read = initialCount - std::min(initialCount, initial.size());
            ModuleManager::instance().postNotice(initial.empty()
                ? "Replay: read all " + std::to_string(initialCount) + " chunks; recording"
                : "Replay: read " + std::to_string(read) + " of " + std::to_string(initialCount)
                      + " chunks in 15 s; recording (the rest are added as they load)");
        }
        if (now - lastTime >= 1000) { worker.post(ReplayWorker::Time{static_cast<std::uint64_t>(chunkmem::readDayTime(region))}); lastTime = now; }
        if (GameData::instance().msSinceView() >= 200 || (lastView && now - lastView >= 200)) lastTick = now;
        lastView = now;
        if (!initialDone) return;
        unsigned sent = 0;
        while (now - lastTick >= 50 && sent < 20) {
            flushChanges(); worker.post(ReplayWorker::Player{playerPosition()}); worker.post(ReplayWorker::Tick{}); lastTick += 50; ++sent;
        }
    }
};
ReplayCapture::ReplayCapture() : impl_(std::make_unique<Impl>()) {}
ReplayCapture::~ReplayCapture() { stop(); wait(); }
bool ReplayCapture::start()
{
    auto& i = *impl_; if (i.active || !i.worker.finished()) return false;
    i.worker.wait(); i.clearColumns(); i.blockCache.clear(); i.biomeCache.clear();
    i.active = true; i.launched = false; i.toldHeight = false; i.region = nullptr; i.source = nullptr;
    i.generation = blockwrite::worldGeneration(); i.lastRegionAt = GetTickCount64();
    i.settleUntil = i.lastScan = i.lastTick = i.lastView = i.lastTime = 0;
    i.slotKeys.fill({}); i.sentSelected = -1; i.inventoryAt = 0;
    i.askedSubChunks = i.askedColumns = 0; i.askTold = false;
    g_recording.store(true, std::memory_order_release); return true;
}
void ReplayCapture::stop(bool save) { if (impl_->active) impl_->stop(save); }
void ReplayCapture::onPlayerViewUpdate()
{
    try { impl_->pump(); impl_->view(); }
    catch (const std::exception& e) {
        ModuleManager::instance().postNotice(std::string("Replay: could not capture: ") + e.what());
        impl_->stop(true);
    }
}
void ReplayCapture::poll()
{
    auto& i = *impl_;
    if (!i.active) return;
    if ((i.launched && !i.worker.accepting()) || blockwrite::worldGeneration() != i.generation) { i.stop(true); return; }
    const auto& game = GameData::instance();
    const auto sinceSession = game.msSinceClientSession();
    if (sinceSession != ~0ULL && sinceSession >= 3000 && game.msSinceView() >= 3000 && !blockwrite::renderRegion()) {
        log().info(L"Replay: left the world (no client session tick for {} ms, no view for {} ms); saving the recording",
                   sinceSession, game.msSinceView());
        i.stop(true);
    }
}
void ReplayCapture::wait() { impl_->worker.wait(); }
bool ReplayCapture::recording() const { return impl_->active; }
bool ReplayCapture::busy() const { return impl_->active || !impl_->worker.finished(); }
bool ReplayCapture::takeResult(ReplayWorker::Result& out) { return impl_->worker.takeResult(out); }
}
