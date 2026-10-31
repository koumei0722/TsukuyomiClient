#include "modules/MaterialCollector.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "hooks/Detours.h"
#include "modules/MaterialCollectLogic.h"
#include "modules/Schematica.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace tsukuyomi {

namespace {
namespace cui = containerui;
namespace logic = materialcollect;

constexpr int kClicksPerTick = 16;
constexpr unsigned long long kContentWaitMs = 500;
constexpr unsigned long long kNoStorageGiveUpMs = 1000;
constexpr unsigned long long kListWaitMs = 2000;
constexpr unsigned long long kCloseWaitMs = 1500;
constexpr unsigned kMaxLogs = 64;

std::string shortName(const std::string& name)
{
    const auto colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

bool isBoxName(const std::string& name)
{
    return name.find("shulker_box") != std::string::npos;
}
}

MaterialCollector& MaterialCollector::instance()
{
    static MaterialCollector collector;
    return collector;
}

void MaterialCollector::onScansReady()
{
    cui::onScansReady();
    cui::addObserver(this);
}

void MaterialCollector::shutdown()
{
    cui::removeObserver(this);
}

void MaterialCollector::readSettings(const nlohmann::json& section)
{
    const bool next=Config::getBool(section, "materialAutoCollect", false);
    if (!next) m_stopRequested.store(true);
    m_enabled.store(next);
    m_closeGui.store(Config::getBool(section, "materialAutoCollectCloseGui", true));
    m_extraAmount.store(std::clamp(Config::getInt(section, "materialAutoCollectExtraAmount", 0), 0, 128));
    m_roundUpToStack.store(Config::getBool(section, "materialAutoCollectRoundUpToStack", false));
    m_requireSufficientSupply.store(Config::getBool(section, "materialAutoCollectRequireSufficientSupply", false));
    m_retainItem.store(Config::getBool(section, "materialAutoCollectRetainItem", false));
    m_retainItemAmount.store(std::clamp(Config::getInt(section, "materialAutoCollectRetainItemAmount", 1), 1, 63));
    m_takeShulkerBoxes.store(Config::getBool(section, "materialAutoCollectTakeShulkerBoxes", false));
    const float threshold = Config::getFloat(section, "materialAutoCollectShulkerBoxFillThreshold", 1.0F);
    m_shulkerBoxFillThreshold.store(std::isfinite(threshold) ? std::clamp(threshold, 0.0F, 1.0F) : 1.0F);
}

void MaterialCollector::loadConfig(const nlohmann::json& section)
{
    readSettings(section);
}

void MaterialCollector::saveConfig(nlohmann::json& section) const
{
    section["materialAutoCollect"] = m_enabled.load();
    section["materialAutoCollectCloseGui"] = m_closeGui.load();
    section["materialAutoCollectExtraAmount"] = m_extraAmount.load();
    section["materialAutoCollectRoundUpToStack"] = m_roundUpToStack.load();
    section["materialAutoCollectRequireSufficientSupply"] = m_requireSufficientSupply.load();
    section["materialAutoCollectRetainItem"] = m_retainItem.load();
    section["materialAutoCollectRetainItemAmount"] = m_retainItemAmount.load();
    section["materialAutoCollectTakeShulkerBoxes"] = m_takeShulkerBoxes.load();
    section["materialAutoCollectShulkerBoxFillThreshold"] = m_shulkerBoxFillThreshold.load();
}

void MaterialCollector::watchConfigFile()
{
    const unsigned long long now = GetTickCount64();
    if (now - m_cfgCheckMs < 1000) {
        return;
    }
    m_cfgCheckMs = now;
    const std::filesystem::path path = paths::configFile();
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;
    }
    const long long ticks = static_cast<long long>(stamp.time_since_epoch().count());
    if (ticks == m_cfgStamp) {
        return;
    }
    nlohmann::json root;
    try {
        std::ifstream in(path, std::ios::binary);
        in >> root;
    } catch (...) {
        if (!m_cfgWarned) {
            m_cfgWarned = true;
            log().warn(L"Schematica: the config file could not be read; auto collect settings were not reloaded");
        }
        return;
    }
    m_cfgWarned = false;
    const bool first = (m_cfgStamp == 0);
    m_cfgStamp = ticks;
    if (first) {
        return;
    }
    const auto sec = root.find("Schematica");
    if (sec == root.end() || !sec->is_object()) {
        return;
    }
    readSettings(*sec);
}

void MaterialCollector::reset()
{
    m_phase = Phase::Idle;
    m_ctrl = nullptr;
    m_wants.clear();
    m_wantAt = 0;
    m_stage = 0;
    m_looseNext = 0;
    m_boxes.clear();
    m_looseAmount = 0;
    m_op = Op{};
}

void MaterialCollector::onScreenCreated(void* ctrl)
{
    reset();
    m_stopRequested.store(false);
    if (!enabled() || !Schematica::instance().enabled() || writes::blocked("Schematica:collect")) {
        return;
    }
    m_phase = Phase::Waiting;
    m_ctrl = ctrl;
    m_createdAt = GetTickCount64();
}

void MaterialCollector::onScreenLost()
{
    if (m_phase == Phase::Running && m_said < kMaxLogs) {
        ++m_said;
        log().warn(L"Schematica: auto collect stopped because the container screen closed");
    }
    reset();
}

void MaterialCollector::onScreenTick()
{
    if (m_phase == Phase::Idle || m_phase == Phase::Finished) {
        return;
    }
    if (!cui::hasScreen() || cui::screenController() != m_ctrl) {
        return;
    }
    m_clicks = 0;
    if (m_stopRequested.load() || !enabled() || !Schematica::instance().enabled()) {
        m_stopRequested.store(true);
        if (m_op.active) {
            if (!m_op.presses.empty() && m_op.presses.front().when == When::CursorEmpty)
                m_op.presses.clear();
            else while (!m_op.presses.empty() && m_op.presses.front().when != When::CursorNotEmpty)
                m_op.presses.pop_front();
            m_op.aborted = true;
            if (!advanceOp()) return;
        }
        m_phase = Phase::Finished;
        return;
    }
    if (m_phase == Phase::Waiting) {
        prepare();
    }
    if (m_phase == Phase::Running) {
        run();
    }
    if (m_phase == Phase::Closing) {
        const unsigned long long waited = GetTickCount64() - m_closingSince;
        if (!pendingPredictions() || waited >= kCloseWaitMs) {
            if (waited >= kCloseWaitMs && m_said < kMaxLogs) {
                ++m_said;
                log().warn(L"Schematica: auto collect closed the screen before the server confirmed every move "
                           L"(waited {} ms)", waited);
            }
            m_phase = Phase::Finished;
            if (!hooks::popTopScreen()) {
                if (m_said < kMaxLogs) {
                    ++m_said;
                    log().error(L"Schematica: auto collect could not close the container screen");
                }
            }
        }
    }
}

std::vector<std::pair<std::string, int>> MaterialCollector::playerSlots() const
{
    std::vector<std::pair<std::string, int>> out;
    const auto add = [&out](const char* coll) {
        const int n = cui::collectionSize(coll);
        for (int i = 0; i < n; ++i) {
            out.emplace_back(coll, i);
        }
    };
    if (cui::collectionSize("inventory_items") > 0 || cui::collectionSize("hotbar_items") > 0) {
        add("inventory_items");
        add("hotbar_items");
    } else {
        add("combined_hotbar_and_inventory_items");
    }
    return out;
}

bool MaterialCollector::hasContents(const void* stack) const
{
    int used = 0;
    int total = 0;
    return cui::nbtContents(stack, used, total) && total > 0;
}

void MaterialCollector::countAvailable(std::map<std::string, long long>& out) const
{
    const auto addStack = [&out](const void* stack) {
        if (stack == nullptr || cui::isEmpty(stack)) {
            return;
        }
        const std::string name = cui::itemName(stack);
        if (name.empty()) {
            return;
        }
        const int count = cui::countOf(stack);
        out[name] += count;
        if (!isBoxName(name)) {
            return;
        }
        const void* tag = cui::userDataOf(stack);
        if (tag == nullptr) {
            return;
        }
        std::vector<cui::NbtItem> contents;
        if (!cui::nbtItemsOfTag(tag, contents)) {
            return;
        }
        for (const auto& item : contents) {
            if (!item.name.empty() && item.count > 0) {
                out[item.name] += static_cast<long long>(item.count) * count;
            }
        }
    };
    for (const auto& [coll, index] : playerSlots()) {
        addStack(cui::stackAt(coll, index));
    }
    addStack(cui::cursorStack());
}

void MaterialCollector::prepare()
{
    const unsigned long long age = GetTickCount64() - m_createdAt;
    const int storage = cui::collectionSize("container_items");
    if (storage <= 0) {
        if (age >= kNoStorageGiveUpMs) {
            m_phase = Phase::Finished;
        }
        return;
    }
    Schematica& mod = Schematica::instance();
    const std::uint64_t placement = mod.materialScopeAll() ? 0 : mod.selectedPlacement();
    if (!mod.materialScopeAll() && placement == 0) {
        say("[Tsukuyomi] Auto collect: no Material List (select a placement first)");
        m_phase = Phase::Finished;
        return;
    }
    const auto [done, total] = mod.materialItemProgress();
    if (done < total) {
        if (age >= kListWaitMs) {
            say("[Tsukuyomi] Auto collect: the Material List is still resolving items");
            m_phase = Phase::Finished;
        }
        return;
    }
    const auto rows = mod.placementMaterials(placement, (std::numeric_limits<std::size_t>::max)(), nullptr, true);
    std::map<std::string, long long> available;
    countAvailable(available);
    const int multiplier = mod.materialMultiplier();
    bool missingUnknown = false;
    std::vector<Want> wants;
    for (const auto& row : rows) {
        if (row.noItem) {
            continue;
        }
        if (multiplier == 1 && !row.missingKnown) {
            missingUnknown = true;
            continue;
        }
        Want want;
        want.name = toUtf8(row.name);
        const auto have = available.find(want.name);
        want.listNeed = logic::requiredAmount(multiplier, static_cast<long long>(row.missing),
                                              static_cast<long long>(row.count),
                                              have == available.end() ? 0 : have->second);
        if (want.listNeed <= 0) {
            continue;
        }
        want.maxStack = row.stackSize;
        want.target = logic::collectionTarget(want.listNeed, m_extraAmount.load(), m_roundUpToStack.load(), want.maxStack);
        wants.push_back(std::move(want));
    }
    if (wants.empty() && missingUnknown) {
        say("[Tsukuyomi] Auto collect: Missing is not counted yet (turn on Render for the placement)");
        m_phase = Phase::Finished;
        return;
    }
    bool supply = false;
    for (int i = 0; i < storage && !supply; ++i) {
        const void* stack = cui::stackAt("container_items", i);
        if (stack == nullptr || cui::isEmpty(stack)) {
            continue;
        }
        const std::string name = cui::itemName(stack);
        for (const Want& want : wants) {
            if (name == want.name || isBoxName(name)) {
                supply = true;
                break;
            }
        }
    }
    if (!wants.empty() && !supply && age < kContentWaitMs) {
        return;
    }
    if (m_said < kMaxLogs) {
        ++m_said;
        log().info(L"Schematica: auto collect started {} ms after the screen opened ({} material(s) short, {} slot(s))",
                   age, wants.size(), storage);
    }
    m_wants = std::move(wants);
    m_wantAt = 0;
    m_stage = 0;
    m_phase = Phase::Running;
}

void MaterialCollector::run()
{
    while (m_clicks < kClicksPerTick) {
        if (m_op.active) {
            if (!advanceOp()) {
                return;
            }
            continue;
        }
        if (!nextOp()) {
            finish();
            return;
        }
    }
}

bool MaterialCollector::advanceOp()
{
    while (!m_op.presses.empty()) {
        if (m_clicks >= kClicksPerTick) {
            return false;
        }
        const Press press = m_op.presses.front();
        m_op.presses.pop_front();
        const void* cursor = cui::cursorStack();
        const bool cursorEmpty = cursor == nullptr || cui::isEmpty(cursor);
        bool ok = true;
        switch (press.when) {
        case When::Always:
            break;
        case When::CursorEmpty:
            ok = cursorEmpty;
            break;
        case When::CursorHoldsWant:
            ok = !cursorEmpty && m_wantAt < m_wants.size() && cui::itemName(cursor) == m_wants[m_wantAt].name;
            break;
        case When::CursorNotEmpty:
            ok = !cursorEmpty;
            break;
        }
        if (!ok) {
            if (press.when == When::CursorEmpty) {
                m_op.aborted = true;
                m_op.presses.clear();
            } else if (press.when == When::CursorHoldsWant) {
                m_op.aborted = true;
                while (!m_op.presses.empty() && m_op.presses.front().when != When::CursorNotEmpty) {
                    m_op.presses.pop_front();
                }
            }
            continue;
        }
        ++m_clicks;
        if (press.when == When::CursorNotEmpty) {
            const void* destination=cui::stackAt(press.coll,press.index);
            if (destination && !cui::isEmpty(destination) && !cui::sameItem(destination,cursor)) {
                m_op.presses.push_front(press);return false;
            }
        }
        const bool clicked=cui::click(press.coll, press.index, press.kind);
        if (press.when == When::CursorNotEmpty) {
            const void* remaining=cui::cursorStack();
            if (!clicked || (remaining && !cui::isEmpty(remaining))) {
                m_op.presses.push_front(press);return false;
            }
        }
    }
    const void* now = cui::stackAt("container_items", m_op.index);
    const int after = (now == nullptr || cui::isEmpty(now)) ? 0 : cui::countOf(now);
    const int moved = std::max(0, m_op.before - after);
    if (m_wantAt < m_wants.size()) {
        Want& want = m_wants[m_wantAt];
        want.taken += m_op.box ? static_cast<long long>(moved) * m_op.boxAmount : moved;
        if (moved == 0) {
            want.stopped = true;
        }
    }
    if (m_op.aborted && m_said < kMaxLogs) {
        ++m_said;
        log().warn(L"Schematica: auto collect could not hold the stack it picked up; the partial move was cut short");
    }
    m_op = Op{};
    return true;
}

bool MaterialCollector::nextOp()
{
    const int storage = cui::collectionSize("container_items");
    while (m_wantAt < m_wants.size()) {
        Want& want = m_wants[m_wantAt];
        const long long remaining = want.target - want.taken;
        if (want.stopped || want.skipped || remaining <= 0) {
            ++m_wantAt;
            m_stage = 0;
            continue;
        }
        if (m_stage == 0) {
            m_boxes.clear();
            m_looseAmount = 0;
            const bool takeBoxes = m_takeShulkerBoxes.load() && want.maxStack > 0;
            for (int i = 0; i < storage; ++i) {
                const void* stack = cui::stackAt("container_items", i);
                if (stack == nullptr || cui::isEmpty(stack)) {
                    continue;
                }
                const std::string name = cui::itemName(stack);
                if (name == want.name && !hasContents(stack)) {
                    m_looseAmount += logic::movableAmount(cui::countOf(stack), m_retainItem.load(), m_retainItemAmount.load());
                } else if (takeBoxes && isBoxName(name)) {
                    const void* tag = cui::userDataOf(stack);
                    std::vector<cui::NbtItem> items;
                    if (tag == nullptr || !cui::nbtItemsOfTag(tag, items)) {
                        continue;
                    }
                    std::vector<logic::BoxItem> contents;
                    for (const auto& item : items) {
                        contents.push_back({item.name, item.count});
                    }
                    const int amount = logic::singleMaterialBoxAmount(contents, want.name, want.maxStack,
                                                                      m_shulkerBoxFillThreshold.load());
                    for (int n = 0; amount > 0 && n < cui::countOf(stack); ++n) {
                        m_boxes.push_back(Box{i, amount});
                    }
                }
            }
            std::stable_sort(m_boxes.begin(), m_boxes.end(), [](const Box& a, const Box& b) { return a.amount > b.amount; });
            long long supply = m_looseAmount;
            for (const Box& box : m_boxes) {
                supply += box.amount;
            }
            if (m_requireSufficientSupply.load() && supply < want.target) {
                want.skipped = true;
                continue;
            }
            m_stage = 1;
        }
        if (m_stage == 1) {
            std::vector<int> amounts;
            for (const Box& box : m_boxes) {
                amounts.push_back(box.amount);
            }
            const int pick = logic::pickBox(amounts, remaining, m_looseAmount);
            if (pick >= 0) {
                const Box box = m_boxes[static_cast<std::size_t>(pick)];
                m_boxes.erase(m_boxes.begin() + pick);
                const void* stack = cui::stackAt("container_items", box.index);
                if (stack == nullptr || cui::isEmpty(stack)) {
                    continue;
                }
                m_op = Op{};
                m_op.active = true;
                m_op.box = true;
                m_op.boxAmount = box.amount;
                m_op.index = box.index;
                m_op.before = cui::countOf(stack);
                m_op.presses.push_back(Press{"container_items", box.index, cui::Click::Shift, When::Always});
                return true;
            }
            m_stage = 2;
            m_looseNext = 0;
        }
        while (m_looseNext < storage) {
            const int i = m_looseNext++;
            const void* stack = cui::stackAt("container_items", i);
            if (stack == nullptr || cui::isEmpty(stack) || cui::itemName(stack) != want.name || hasContents(stack)) {
                continue;
            }
            const int count = cui::countOf(stack);
            const long long tryMove = std::min<long long>(
                remaining, logic::movableAmount(count, m_retainItem.load(), m_retainItemAmount.load()));
            if (tryMove <= 0) {
                continue;
            }
            m_op = Op{};
            m_op.index = i;
            m_op.before = count;
            if (tryMove == count) {
                m_op.presses.push_back(Press{"container_items", i, cui::Click::Shift, When::Always});
            } else {
                const auto slots = playerSlots();
                std::vector<logic::PlayerSlot> view;
                for (const auto& [coll, index] : slots) {
                    const void* s = cui::stackAt(coll, index);
                    logic::PlayerSlot one;
                    one.empty = (s == nullptr || cui::isEmpty(s));
                    if (!one.empty) {
                        one.same = cui::sameItem(s, stack);
                        one.count = cui::countOf(s);
                        one.maxStack = cui::maxStackOf(s);
                    }
                    view.push_back(one);
                }
                const auto plan = logic::distributeRightClicks(view, static_cast<int>(tryMove));
                if (plan.empty()) {
                    want.stopped = true;
                    break;
                }
                m_op.presses.push_back(Press{"container_items", i, cui::Click::Left, When::CursorEmpty});
                for (const auto& [slot, clicks] : plan) {
                    const auto& target = slots[static_cast<std::size_t>(slot)];
                    for (int n = 0; n < clicks; ++n) {
                        m_op.presses.push_back(Press{target.first, target.second, cui::Click::Right, When::CursorHoldsWant});
                    }
                }
                m_op.presses.push_back(Press{"container_items", i, cui::Click::Left, When::CursorNotEmpty});
            }
            m_op.active = true;
            return true;
        }
        ++m_wantAt;
        m_stage = 0;
    }
    return false;
}

void MaterialCollector::finish()
{
    report();
    if (m_closeGui.load()) {
        m_phase = Phase::Closing;
        m_closingSince = GetTickCount64();
    } else {
        m_phase = Phase::Finished;
    }
}

void MaterialCollector::report()
{
    std::string line;
    long long kinds = 0;
    for (const Want& want : m_wants) {
        if (want.taken <= 0) {
            continue;
        }
        if (!line.empty()) {
            line += ", ";
        }
        line += "+" + std::to_string(want.taken) + " " + shortName(want.name);
        if (want.taken < want.listNeed) {
            line += " (" + std::to_string(want.listNeed - want.taken) + " still missing)";
        } else if (want.taken > want.listNeed) {
            line += " (" + std::to_string(want.taken - want.listNeed) + " over)";
        }
        ++kinds;
    }
    if (m_said < kMaxLogs) {
        ++m_said;
        log().info(L"Schematica: auto collect took {} kind(s) of {} short: {}", kinds, m_wants.size(),
                   toUtf16(line.empty() ? std::string("nothing") : line));
    }
    say(line.empty() ? std::string("[Tsukuyomi] Auto collect: nothing to take from this container")
                     : "[Tsukuyomi] Auto collect: " + line);
}

bool MaterialCollector::pendingPredictions() const
{
    const auto pending = [](const void* stack) { return stack != nullptr && cui::netTagOf(stack) > 0; };
    for (const auto& [coll, index] : playerSlots()) {
        if (pending(cui::stackAt(coll, index))) {
            return true;
        }
    }
    const int storage = cui::collectionSize("container_items");
    for (int i = 0; i < storage; ++i) {
        if (pending(cui::stackAt("container_items", i))) {
            return true;
        }
    }
    return pending(cui::cursorStack());
}

void MaterialCollector::say(const std::string& chat)
{
    Schematica::instance().queueChat(chat);
}

}
