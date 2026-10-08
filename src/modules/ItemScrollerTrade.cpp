#include "modules/ItemScroller.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/ContainerUi.h"
#include "game/TradeUi.h"
#include "hooks/Detours.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <set>

namespace tsukuyomi {

namespace {
namespace cui = containerui;
constexpr char kTradeResult[] = "trade2_result_item";
constexpr int kMaxTiers = 8;
constexpr int kMaxOffersPerTier = 64;

}

bool ItemScroller::isTradeScreen() const
{
    return cui::collectionSize(kTradeResult) > 0;
}

std::string ItemScroller::globalTradeKeyOf(const void* offer) const
{
    if (offer == nullptr) {
        return {};
    }
    auto kind = [](const void* st) {
        if (cui::isEmpty(st)) {
            return std::string("-");
        }
        std::string key = cui::itemName(st) + ":" + std::to_string(cui::auxOf(st));
        const std::string enchants = cui::enchantKey(st);
        if (!enchants.empty()) {
            key += "#" + enchants;
        }
        return key;
    };
    return kind(tradeui::offerSell(offer)) + "<" + kind(tradeui::offerBuyA(offer)) + "+"
           + kind(tradeui::offerBuyB(offer));
}

void ItemScroller::refreshVillagerKey()
{
    void* const ctrl = cui::screenController();
    if (ctrl == nullptr || ctrl == m_tradeCtrl) {
        return;
    }
    m_tradeCtrl = ctrl;
    m_villagerKey.clear();
    std::string all;
    for (int tier = 0; tier < kMaxTiers; ++tier) {
        for (int i = 0; i < kMaxOffersPerTier; ++i) {
            const void* o = tradeui::offer(ctrl, tier, i);
            if (o == nullptr) {
                break;
            }
            all += globalTradeKeyOf(o);
            all += ';';
        }
    }
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (unsigned char c : all) {
        h ^= c;
        h *= 0x100000001B3ULL;
    }
    char buf[20]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    m_villagerKey = all.empty() ? std::string() : std::string(buf);
    std::int64_t id = 0;
    const bool byId = tradeui::traderId(ctrl, id);
    if (byId) {
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
        m_villagerKey = std::string("id:") + buf;
    }

}

bool ItemScroller::villagerHasOwnFavorites()
{
    if (!m_favoritesLoaded) {
        loadFavorites();
    }
    const auto it = m_villagerFavorites.find(m_villagerKey);
    return it != m_villagerFavorites.end() && !it->second.empty();
}

bool ItemScroller::isVillagerFavorite(const void* offer)
{
    if (offer == nullptr || !villagerHasOwnFavorites()) {
        return false;
    }
    return m_villagerFavorites[m_villagerKey].count(globalTradeKeyOf(offer)) != 0;
}

bool ItemScroller::isGlobalFavorite(const void* offer)
{
    if (!m_favoritesLoaded) {
        loadFavorites();
    }
    return offer != nullptr && m_globalFavorites.count(globalTradeKeyOf(offer)) != 0;
}

bool ItemScroller::isFavorite(int tier, int index)
{
    void* const ctrl = cui::screenController();
    const void* o = (ctrl != nullptr) ? tradeui::offer(ctrl, tier, index) : nullptr;
    if (o == nullptr) {
        return false;
    }
    if (villagerHasOwnFavorites()) {
        return isVillagerFavorite(o);
    }
    return isGlobalFavorite(o);
}

namespace {
const void* rowOffer(int tier, int index)
{
    void* const ctrl = containerui::screenController();
    return ctrl != nullptr ? tradeui::offer(ctrl, tier, index) : nullptr;
}
}

bool ItemScroller::rowFavVillager(int tier, int index)
{
    ItemScroller& self = instance();
    if (!self.enabled()) {
        return false;
    }
    self.refreshVillagerKey();
    return self.isVillagerFavorite(rowOffer(tier, index));
}

bool ItemScroller::rowFavGlobal(int tier, int index)
{
    ItemScroller& self = instance();
    if (!self.enabled()) {
        return false;
    }
    self.refreshVillagerKey();
    const bool used = !self.villagerHasOwnFavorites();
    return used && self.isGlobalFavorite(rowOffer(tier, index));
}

bool ItemScroller::rowFavGlobalIdle(int tier, int index)
{
    ItemScroller& self = instance();
    if (!self.enabled()) {
        return false;
    }
    self.refreshVillagerKey();
    const bool used = !self.villagerHasOwnFavorites();
    const void* const o = rowOffer(tier, index);
    return !used && self.isGlobalFavorite(o) && !self.isVillagerFavorite(o);
}

void ItemScroller::toggleFavorite(int tier, int index, bool global)
{
    if (!m_favoritesLoaded) {
        loadFavorites();
    }
    void* const ctrl = cui::screenController();
    const void* o = (ctrl != nullptr) ? tradeui::offer(ctrl, tier, index) : nullptr;
    if (o == nullptr) {
        return;
    }
    const std::string key = globalTradeKeyOf(o);
    std::set<std::string>& set = global ? m_globalFavorites : m_villagerFavorites[m_villagerKey];
    const bool now = (set.erase(key) == 0);
    if (now) {
        set.insert(key);
    }
    ++m_favVersion;
    m_favoritesDirty = true;
    saveFavorites();
    cui::requestRefresh();
    log().info(L"ItemScroller: {} favorite {} ({})", global ? L"global" : L"villager",
               now ? L"added" : L"removed", toUtf16(key));
}

std::wstring ItemScroller::favoritesFilePath() const
{
    const std::filesystem::path dir = paths::dataDir() / L"ItemScroller";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / L"villager_favorites.json").wstring();
}

void ItemScroller::loadFavorites()
{
    m_favoritesLoaded = true;
    std::ifstream in{std::filesystem::path(favoritesFilePath())};
    if (!in) return;
    nlohmann::json root;
    try {
        in >> root;
    } catch (const nlohmann::json::exception&) {
        log().warn(L"ItemScroller: the villager favorites file could not be read");
        return;
    }
    if (!itemscrollerlogic::readFavorites(root, m_globalFavorites, m_villagerFavorites)) {
        log().warn(L"ItemScroller: the villager favorites file has invalid field types");
    }
}

void ItemScroller::saveFavorites() const
{
    if (!m_favoritesDirty) {
        return;
    }
    m_favoritesDirty = false;
    nlohmann::json root;
    root["global"] = nlohmann::json::array();
    for (const std::string& k : m_globalFavorites) {
        root["global"].push_back(k);
    }
    root["villagers"] = nlohmann::json::object();
    for (const auto& [villager, keys] : m_villagerFavorites) {
        if (keys.empty() || villager.empty()) {
            continue;
        }
        nlohmann::json list = nlohmann::json::array();
        for (const std::string& k : keys) {
            list.push_back(k);
        }
        root["villagers"][villager] = std::move(list);
    }
    std::ofstream out(std::filesystem::path(favoritesFilePath()), std::ios::binary | std::ios::trunc);
    if (out) {
        out << root.dump(2);
    }
}

void ItemScroller::onTradeSecondary(int tier, int index)
{
    applyDisable();
    if (!enabled()) {
        return;
    }
    m_tradeJobs.clear();
    m_tradeJobs.push_back(TradeJob{tier, index, 0, 0});

}

int ItemScroller::tradeTierLimit(void* ctrl) const
{
    if (m_unlockSent >= 0) {
        return kMaxTiers;
    }
    const int current = tradeui::currentTier(ctrl);
    return current >= 0 ? current : kMaxTiers;
}

int ItemScroller::tradeFavorites()
{
    void* const ctrl = cui::screenController();
    if (ctrl == nullptr) {
        return 0;
    }
    m_tradeJobs.clear();
    const int limit = tradeTierLimit(ctrl);
    int skipped = 0;
    for (int tier = 0; tier < kMaxTiers && tier <= limit; ++tier) {
        for (int i = 0; i < kMaxOffersPerTier; ++i) {
            const void* const o = tradeui::offer(ctrl, tier, i);
            if (o == nullptr) {
                break;
            }
            if (!isFavorite(tier, i)) {
                continue;
            }
            if (tradeui::offerSoldOut(o) || !tradeui::tradePossible(ctrl, o)) {
                ++skipped;
                continue;
            }
            m_tradeJobs.push_back(TradeJob{tier, i, 0, 0});
        }
    }
    log().info(L"ItemScroller: trading {} favorite trade(s) ({} skipped: sold out or cannot pay)", m_tradeJobs.size(),
               skipped);
    return static_cast<int>(m_tradeJobs.size());
}

int ItemScroller::countItemInPlayer(const void* like) const
{
    int n = 0;
    if (cui::isEmpty(like)) {
        return 0;
    }
    for (Group g : {Group::Player, Group::TradeIn}) {
        for (const Slot& s : slotsOf(g)) {
            const void* st = stackOf(s);
            if (!cui::isEmpty(st) && cui::itemName(st) == cui::itemName(like) && cui::auxOf(st) == cui::auxOf(like)) {
                n += cui::countOf(st);
            }
        }
    }
    return n;
}

void ItemScroller::tradeTick()
{
    if (m_tradeJobs.empty() || !budgetLeft()) {
        return;
    }
    void* const ctrl = cui::screenController();
    if (ctrl == nullptr || !isTradeScreen()) {
        m_tradeJobs.clear();
        return;
    }
    TradeJob& job = m_tradeJobs.front();
    const void* o = tradeui::offer(ctrl, job.tier, job.index);
    if (o == nullptr || ++job.steps > 400) {
        m_tradeJobs.erase(m_tradeJobs.begin());
        return;
    }
    const void* sell = tradeui::offerSell(o);
    const Slot result{kTradeResult, 0};
    const void* shown = stackOf(result);
    const bool ready = !cui::isEmpty(shown) && cui::itemName(shown) == cui::itemName(sell);
    if (job.phase == 0) {
        tradeui::selectTrade(ctrl, job.tier, job.index);
        job.phase = 1;
        job.wait = 0;
        return;
    }
    if (!ready) {
        constexpr int kWaitTicks = 40;
        if (++job.wait < kWaitTicks) {
            return;
        }

        m_tradeJobs.erase(m_tradeJobs.begin());
        return;
    }
    const int payBefore = countItemInPlayer(tradeui::offerBuyA(o));
    shiftClick(result);
    const int payAfter = countItemInPlayer(tradeui::offerBuyA(o));

    if (payAfter >= payBefore) {
        m_tradeJobs.erase(m_tradeJobs.begin());
        return;
    }
    job.phase = 0;
}

void ItemScroller::tradeScreenTick(bool offstack)
{
    if (!isTradeScreen() || !tradeui::available()) {
        return;
    }
    refreshVillagerKey();
    updateTradeOrder(cui::screenController());
    autoTradeTick(cui::screenController());

    if (offstack) {

        tradeTick();

        return;
    }
    const bool middle = keyHeld(VK_MBUTTON);
    if (middle && !m_tradeMiddleWas) {
        int tier = -1;
        int index = -1;
        if (tradeui::hovered(tier, index, 1500)) {
            toggleFavorite(tier, index, comboHeld(m_keys[kModifierToggleVillagerGlobalFavorite].keys, true));
        }
    }
    m_tradeMiddleWas = middle;
    void* const ctrl = cui::screenController();
    handleFavInput(ctrl);
    updateFavView(ctrl);

    tradeTick();

}

void ItemScroller::updateFavView(void* ctrl)
{
    FavView next{};
    if (ctrl != nullptr && enabled() && isTradeScreen()) {
        auto iconOf = [](const void* stack) {
            if (cui::isEmpty(stack)) {
                return 0;
            }
            return cui::idAuxOfItem(reinterpret_cast<const void*>(cui::itemKey(stack)), cui::auxOf(stack));
        };
        const int limit = tradeTierLimit(ctrl);
        for (int tier = 0; tier < kMaxTiers && tier <= limit && next.n < kFavEntries; ++tier) {
            for (int i = 0; i < kMaxOffersPerTier && next.n < kFavEntries; ++i) {
                const void* o = tradeui::offer(ctrl, tier, i);
                if (o == nullptr) {
                    break;
                }
                if (!isFavorite(tier, i)) {
                    continue;
                }
                const int k = next.n++;
                next.tier[k] = tier;
                next.index[k] = i;
                next.global[k] = !isVillagerFavorite(o);
                const void* buy = tradeui::offerBuyA(o);
                const void* sell = tradeui::offerSell(o);
                next.buy[k] = iconOf(buy);
                next.sell[k] = iconOf(sell);
                if (cui::countOf(buy) > 1) {
                    std::snprintf(next.buyCount[k], sizeof(next.buyCount[k]), "%d", cui::countOf(buy));
                }
                if (cui::countOf(sell) > 1) {
                    std::snprintf(next.sellCount[k], sizeof(next.sellCount[k]), "%d", cui::countOf(sell));
                }
            }
        }
        next.visible = next.n > 0;
    }
    if (std::memcmp(&next, &m_favView, sizeof(next)) != 0) {
        std::memcpy(&m_favView, &next, sizeof(next));
        cui::requestRefresh();
    }
}

void ItemScroller::handleFavInput(void* ctrl)
{
    const int pressed = m_favPress;
    const bool right = m_favPressRight;
    m_favPress = -1;
    if (pressed < 0 || pressed >= m_favView.n || ctrl == nullptr) {
        return;
    }
    const int tier = m_favView.tier[pressed];
    const int index = m_favView.index[pressed];
    if (right) {
        m_tradeJobs.clear();
        m_tradeJobs.push_back(TradeJob{tier, index, 0, 0});
    } else {
        tradeui::selectTrade(ctrl, tier, index);
    }

}

void ItemScroller::clearTradeInputs()
{
    for (const Slot& s : slotsOf(Group::TradeIn)) {
        if (!isEmptySlot(s)) {
            shiftClick(s);
        }
    }
}

void ItemScroller::fillTradeInputs(bool fullStacks)
{
    for (const Slot& in : slotsOf(Group::TradeIn)) {
        const void* st = stackOf(in);
        if (cui::isEmpty(st)) {
            continue;
        }
        for (const Slot& s : slotsOf(Group::Player)) {
            const void* have = stackOf(in);
            if (cui::countOf(have) >= cui::maxStackOf(have)) {
                break;
            }
            const void* src = stackOf(s);
            if (!cui::isEmpty(src) && cui::sameItem(src, have)) {
                leftClick(s);
                if (fullStacks) {
                    leftClick(in);
                } else {
                    rightClick(in);
                }
                if (!cursorEmpty()) {
                    leftClick(s);
                }
                if (!fullStacks) {
                    break;
                }
            }
        }
    }
}

bool ItemScroller::tryMoveItemsVillager(const Slot& slot, bool toOther, bool fullStacks)
{
    pushJob([this, slot, toOther, fullStacks]() {
        if (!cursorEmpty()) {
            return true;
        }
        if (toOther) {
            if (!isEmptySlot(slot)) {
                if (fullStacks) {
                    shiftClick(slot);
                } else {
                    moveOneSetFromOutput(slot);
                }
            } else if (fullStacks) {
                clearTradeInputs();
            }
        } else {
            fillTradeInputs(fullStacks);
        }
        return true;
    });
    return false;
}

void ItemScroller::updateTradeOrder(void* ctrl)
{
    tradeui::setClientScreen(ctrl);
    if (ctrl == nullptr) {
        return;
    }
    const std::string sig = std::format("{}|{}|{}{}{}{}{}", m_villagerKey, m_favVersion, enabled(),
                                        true, m_villagerTradeUnlockAllTiers,
                                        true, true);
    if (sig == m_orderSig && m_favTierSet && ++m_orderTicks < 30) {
        return;
    }
    m_orderSig = sig;
    m_orderTicks = 0;
    int tiers = 0;
    std::vector<int> counts;
    for (int tier = 0; tier < kMaxTiers; ++tier) {
        int n = 0;
        while (n < kMaxOffersPerTier && tradeui::offer(ctrl, tier, n) != nullptr) {
            ++n;
        }
        if (n == 0) {
            break;
        }
        counts.push_back(n);
        tiers = tier + 1;
    }
    const int unlock = (enabled() && m_villagerTradeUnlockAllTiers && tiers > 0)
                           ? tiers - 1
                           : -1;
    if (unlock != m_unlockSent) {
        tradeui::setUnlockTier(unlock);
        m_unlockSent = unlock;
        cui::requestRefresh();

    }
    std::vector<std::pair<int, int>> fav;
    if (enabled() && tradeui::favoriteTierAvailable()) {
        if (!m_favoritesLoaded) {
            loadFavorites();
        }
        const bool own = villagerHasOwnFavorites();
        const int limit = tradeTierLimit(ctrl);
        for (int tier = 0; tier < tiers && tier <= limit; ++tier) {
            for (int i = 0; i < counts[static_cast<size_t>(tier)]; ++i) {
                const void* o = tradeui::offer(ctrl, tier, i);
                const bool f = own ? isVillagerFavorite(o) : (isGlobalFavorite(o));
                if (f) {
                    fav.emplace_back(tier, i);
                }
            }
        }
    }
    if (fav != m_favTier || counts != m_favTierCounts || !m_favTierSet) {
        m_favTier = fav;
        m_favTierCounts = counts;
        m_favTierSet = true;
        tradeui::setFavoriteTier(fav, counts);
        cui::requestRefresh();

    }
}

void ItemScroller::autoTradeTick(void* ctrl)
{
    constexpr int kDelayTicks = 5;
    constexpr int kGiveUpTicks = 1200;
    if (!m_autoTradeWanted || ctrl == nullptr || ctrl != m_autoTradeCtrl || m_autoTradePhase >= 3) {
        return;
    }
    if (!enabled()) {
        if (m_autoTradeOffstack) {
            closeAutoTradeScreen(L"was switched off");
        }
        m_autoTradeWanted = false;
        return;
    }
    if (!m_autoTradeOffstack) {
        m_autoTradeWanted = false;
        notice::failOnce("ItemScroller.autoTradeOffstack",
                         L"ItemScroller: the trade screen could not be kept off the screen stack, so the auto trade "
                         L"is off (the screen opens normally)",
                         "Auto trade is off: the trade screen could not be opened in the background");
        return;
    }
    if (++m_autoTradeTicks >= kGiveUpTicks) {
        closeAutoTradeScreen(L"timed out");
        return;
    }
    switch (m_autoTradePhase) {
    case 0: {
        if (m_autoTradeTicks < kDelayTicks) {
            return;
        }
        const int queued = tradeFavorites();
        m_autoTradeGains.clear();
        if (m_villagerTradeOnOpenThrowResults) {
            for (const TradeJob& j : m_tradeJobs) {
                const void* const o = tradeui::offer(ctrl, j.tier, j.index);
                m_autoTradeGains.push_back(
                    GainBase{j.tier, j.index, o != nullptr ? countItemInPlayer(tradeui::offerSell(o)) : 0});
            }
        }
        if (queued == 0) {
            closeAutoTradeScreen(L"had nothing to trade");
            return;
        }
        m_autoTradePhase = 1;
        return;
    }
    case 1:
        if (!m_tradeJobs.empty() || !m_jobs.empty()) {
            return;
        }
        if (m_villagerTradeOnOpenThrowResults && !m_autoTradeGains.empty()) {
            startThrowResults(ctrl);
            m_autoTradePhase = 2;
            return;
        }
        closeAutoTradeScreen(L"finished");
        return;
    case 2:
        if (!m_jobs.empty()) {
            return;
        }
        closeAutoTradeScreen(L"finished and threw the results away");
        return;
    default:
        return;
    }
}

void ItemScroller::startThrowResults(void* ctrl)
{
    struct Want {
        std::string name;
        int aux = 0;
        int count = 0;
    };
    auto wants = std::make_shared<std::vector<Want>>();
    std::set<std::string> seen;
    for (const GainBase& g : m_autoTradeGains) {
        const void* const o = tradeui::offer(ctrl, g.tier, g.index);
        if (o == nullptr) {
            continue;
        }
        const void* const sell = tradeui::offerSell(o);
        if (cui::isEmpty(sell)) {
            continue;
        }
        const std::string name = cui::itemName(sell);
        const int aux = cui::auxOf(sell);
        if (!seen.insert(name + "|" + std::to_string(aux)).second) {
            continue;
        }
        const int gained = countItemInPlayer(sell) - g.before;
        if (gained > 0) {
            wants->push_back(Want{name, aux, gained});
        }
    }
    if (!wants->empty()) {
        std::wstring text;
        for (const Want& w : *wants) {
            text += std::format(L" {} x{}", toUtf16(w.name), w.count);
        }
        log().info(L"ItemScroller: throwing the auto trade results away:{}", text.empty() ? L" (nothing gained)" : text);
    }
    pushJob([this, wants]() {
        for (Want& w : *wants) {
            while (w.count > 0) {
                if (!budgetLeft()) {
                    return false;
                }
                Slot found;
                int have = 0;
                for (const Slot& s : slotsOf(Group::Player)) {
                    const void* const st = stackOf(s);
                    if (!cui::isEmpty(st) && cui::itemName(st) == w.name && cui::auxOf(st) == w.aux) {
                        found = s;
                        have = cui::countOf(st);
                        break;
                    }
                }
                if (!found.valid() || have <= 0) {
                    w.count = 0;
                    break;
                }
                if (have <= w.count) {
                    dropStack(found);
                    w.count -= have;
                } else {
                    dropOne(found);
                    --w.count;
                }
            }
        }
        return true;
    });
}

void ItemScroller::closeAutoTradeScreen(const wchar_t* why)
{
    m_autoTradePhase = 3;
    m_tradeJobs.clear();
    log().info(L"ItemScroller: auto trade {} after {} tick(s) — releasing the trade screen that was never opened",
               why, m_autoTradeTicks);
}

void ItemScroller::onPlayerViewUpdate()
{
    if (m_unloadRelease.load(std::memory_order_acquire) == 1) {
        if (hooks::offstackScreenHeld()) {
            hooks::releaseOffstackScreen(L"unloading");
        }
        m_unloadRelease.store(2, std::memory_order_release);
        return;
    }
    applyDisable();
    if (!m_autoTradeOffstack || !hooks::offstackScreenHeld()) {
        return;
    }
    if (!enabled()) {
        hooks::releaseOffstackScreen(L"the module was switched off");
        return;
    }
    constexpr unsigned long long kTickIntervalMs = 16;
    const unsigned long long now = GetTickCount64();
    if (now - m_offstackTickMs < kTickIntervalMs) {
        return;
    }
    m_offstackTickMs = now;
    if (!hooks::tickOffstackScreen()) {
        hooks::releaseOffstackScreen(L"the trade screen tick faulted");
        return;
    }
    if (m_autoTradePhase >= 3 || !m_autoTradeWanted) {
        hooks::releaseOffstackScreen(L"the auto trade finished");
    }
}

void* ItemScroller::offstackTradeController() const
{
    if (!enabled() || !m_autoTradeWanted || m_autoTradeOffstack || m_autoTradeTicks != 0
        || m_autoTradePhase != 0) {
        return nullptr;
    }
    return m_autoTradeCtrl;
}

void ItemScroller::onTradeScreenHeldOffstack()
{
    m_autoTradeOffstack = true;
    m_offstackTickMs = 0;
}

void ItemScroller::onOffstackTradeReleased()
{
    m_autoTradeOffstack = false;
    m_autoTradeWanted = false;
    m_offstackTickMs = 0;
}

std::string ItemScroller::favoriteStarJson()
{
    auto mark = [](const char* name, const char* texture, int x, const char* binding, int layer) {
        nlohmann::json m = {{"type", "image"},
                            {"texture", texture},
                            {"size", {8, 8}},
                            {"anchor_from", "top_left"},
                            {"anchor_to", "top_left"},
                            {"offset", {x, 1}},
                            {"layer", layer},
                            {"bindings", nlohmann::json::array({
                                             {{"binding_type", "collection_details"}},
                                             {{"binding_name", binding},
                                              {"binding_name_override", "#visible"},
                                              {"binding_condition", "always"}},
                                         })}};
        return nlohmann::json{{name, m}};
    };
    return nlohmann::json::array({
                                     mark("tk_is_fav_star", "textures/ui/filledStar", 1, "#tk_is_fav_v", 30),
                                     mark("tk_is_fav_star_g", "textures/ui/filledStarFocus", 1, "#tk_is_fav_g", 30),
                                     mark("tk_is_fav_star_gx", "textures/ui/emptyStar", 1, "#tk_is_fav_gx", 30),
                                 })
        .dump();
}

void ItemScroller::registerTradeUi(void* ctrl)
{
    const bool ok = tradeui::bindRowBool(ctrl, "#tk_is_fav_v", &ItemScroller::rowFavVillager)
                    && tradeui::bindRowBool(ctrl, "#tk_is_fav_g", &ItemScroller::rowFavGlobal)
                    && tradeui::bindRowBool(ctrl, "#tk_is_fav_gx", &ItemScroller::rowFavGlobalIdle);
    if (!ok) {
        static bool said = false;
        if (!said) {
            said = true;
            log().warn(L"ItemScroller: the favorite marks could not be bound (trade rows)");
        }
    }
}

}
