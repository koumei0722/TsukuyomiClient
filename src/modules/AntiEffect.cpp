#include "modules/AntiEffect.h"

#include <cstring>
#include <vector>

#include <Windows.h>

#include "config/Config.h"
#include "core/Logger.h"
#include "game/BlockRegistry.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

struct EffectDef {
    std::int32_t id;
    const wchar_t* label;
    const char* key;
};

constexpr EffectDef kEffects[] = {
    {AntiEffect::kDarknessEffectId, L"Darkness", "darkness"},
    {AntiEffect::kBlindnessEffectId, L"Blindness", "blindness"},
    {AntiEffect::kNauseaEffectId, L"Nausea", "nausea"},
};
static_assert(sizeof(kEffects) / sizeof(kEffects[0]) == AntiEffect::kEffectCount);

}

AntiEffect& AntiEffect::instance()
{
    static AntiEffect module;
    return module;
}

bool AntiEffect::available() const
{
    return Scanner::instance().found(Target::GetActorEffect);
}

bool AntiEffect::hides(std::int32_t effectId) const
{
    if (!enabled()) {
        return false;
    }
    for (std::size_t i = 0; i < kEffectCount; ++i) {
        if (kEffects[i].id == effectId) {
            return m_hide[i].load(std::memory_order_relaxed);
        }
    }
    return false;
}

void* AntiEffect::onGetEffect(int effectId, void* original)
{

    if (original == nullptr || !hides(effectId)) {
        return original;
    }

    const unsigned long sim = blocks::simThread();
    if (sim == 0 || GetCurrentThreadId() != sim) {
        return original;
    }
    for (std::size_t i = 0; i < kEffectCount; ++i) {
        if (kEffects[i].id == effectId && !m_toldHidden[i].exchange(true, std::memory_order_relaxed)) {
            log().info(L"AntiEffect: hiding how {} (effect {}) looks; the effect itself stays on",
                       kEffects[i].label, effectId);
        }
    }
    return nullptr;
}

MenuItem AntiEffect::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    for (std::size_t i = 0; i < kEffectCount; ++i) {
        children.push_back(menu::toggle(
            kEffects[i].label, [this, i] { return m_hide[i].load(std::memory_order_relaxed); },
            [this, i] {
                const bool now = !m_hide[i].load(std::memory_order_relaxed);
                m_hide[i].store(now, std::memory_order_relaxed);
                log().info(L"AntiEffect: {} is {}", kEffects[i].label, now ? L"hidden" : L"shown");
            }));
    }
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void AntiEffect::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    for (std::size_t i = 0; i < kEffectCount; ++i) {
        m_hide[i].store(Config::getBool(section, kEffects[i].key, true), std::memory_order_relaxed);
    }
}

void AntiEffect::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    for (std::size_t i = 0; i < kEffectCount; ++i) {
        section[kEffects[i].key] = m_hide[i].load(std::memory_order_relaxed);
    }
}

}
