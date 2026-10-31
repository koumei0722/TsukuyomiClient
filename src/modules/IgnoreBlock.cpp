#include "modules/IgnoreBlock.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "input/GameButtons.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <atomic>
#include <format>
#include <utility>

namespace tsukuyomi {

namespace {

using SetBlockFn = bool(__fastcall*)(void* region, const std::int32_t* pos, const void* block, std::int32_t layer,
                                     std::int32_t updateFlags, void* actor);

constexpr std::int32_t kUpdateFlags = 3;
constexpr std::size_t kMaxPending = 16;

__declspec(noinline) bool setBlockRaw(SetBlockFn fn, void* region, const std::int32_t* pos, const void* block,
                                      std::int32_t layer, DWORD& code)
{
    __try {
        fn(region, pos, block, layer, kUpdateFlags, nullptr);
        return true;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

IgnoreBlock& IgnoreBlock::instance()
{
    static IgnoreBlock module;
    return module;
}

bool IgnoreBlock::available() const
{
    return Scanner::instance().found(Target::BlockSourceSetBlock) && Scanner::instance().found(Target::BuildBlock)
           && m_useButton >= 0;
}

void IgnoreBlock::onScansReady()
{
    m_useButton = GameButtons::instance().watchButton(gamebuttonlogic::button::buildOrInteract);
    if (m_useButton < 0) log().warn(L"IgnoreBlock: the use button cannot be watched; the module is unavailable");
}

bool IgnoreBlock::onBuildBlock(const void* blockPos, bool& handled)
{
    handled = false;
    if (!enabled() || !available() || m_ignoreKey.empty() || !m_ignoreKey.isDown()) return false;
    handled = true;
    if (!m_armed.load(std::memory_order_acquire)) return false;
    std::array<std::int32_t, 3> pos{};
    if (blockPos == nullptr || !memory::copyGuarded(blockPos, pos.data(), sizeof(std::int32_t) * 3)) return false;
    m_armed.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> guard(m_lock);
    if (m_pending.size() < kMaxPending) m_pending.push_back(pos);
    return false;
}

void IgnoreBlock::onPlayerViewUpdate()
{
    if (m_useButton >= 0 && !GameButtons::instance().buttonHeld(m_useButton)) m_armed.store(true, std::memory_order_release);
    std::vector<std::array<std::int32_t, 3>> pending;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_pending.empty()) return;
        pending.swap(m_pending);
    }
    if (!enabled()) return;
    const auto setBlock = Scanner::instance().addressAs<SetBlockFn>(Target::BlockSourceSetBlock);
    void* const region = blockwrite::renderRegion();
    blocks::Table table;
    const void* air = nullptr;
    if (blocks::resolve({"minecraft:air"}, table)) {
        const auto found = table.find("minecraft:air");
        air = found != table.end() ? found->second : nullptr;
    }
    if (setBlock == nullptr || region == nullptr || !blockwrite::regionIsAlive(region) || air == nullptr) {
        log().warn(L"IgnoreBlock: the client's chunks or the air block are not available; {} press(es) dropped",
                   pending.size());
        return;
    }
    static std::atomic<int> logged{0};
    for (const auto& pos : pending) {
        const void* extra = nullptr;
        const bool hasExtra = blockwrite::readWorldExtraAt(pos[0], pos[1], pos[2], extra) && extra != nullptr && extra != air;
        DWORD code = 0;
        bool ok = setBlockRaw(setBlock, region, pos.data(), air, 0, code);
        if (ok && hasExtra) ok = setBlockRaw(setBlock, region, pos.data(), air, 1, code);
        if (!ok) {
            notice::failOnce("IgnoreBlock.setBlock",
                             std::format(L"IgnoreBlock: BlockSource::setBlock faulted at ({}, {}, {}) (exception {:08X})",
                                         pos[0], pos[1], pos[2], code),
                             "IgnoreBlock failed: writing air into the client's chunk faulted");
            return;
        }
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            log().info(L"IgnoreBlock: ({}, {}, {}) is air on this client only{}", pos[0], pos[1], pos[2],
                       hasExtra ? L" (with its second layer)" : L"");
        }
    }
}

MenuItem IgnoreBlock::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"Ignore key", [this] { return m_ignoreKey.combo(); },
        [this](std::vector<int> combo) {
            m_ignoreKey.set(std::move(combo));
            log().info(L"IgnoreBlock: ignore key set to {}", m_ignoreKey.name());
        },
        {}));
    bindPad(children.back(), m_ignoreKey);

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void IgnoreBlock::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    std::vector<int> combo;
    if (const auto it = section.find("ignoreKeys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) combo.push_back(value.get<int>());
        }
    }
    m_ignoreKey.set(std::move(combo));
}

void IgnoreBlock::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["ignoreKeys"] = m_ignoreKey.combo();
}

}
