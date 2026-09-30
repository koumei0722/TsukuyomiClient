#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tsukuyomi::tradeui {

void onScansReady();
bool available();

void onHoverInvoke(const void* bag);
void onSecondaryInvoke(void* ctrl, const void* bag);

class Listener {
public:
    virtual ~Listener() = default;
    virtual void onTradeSecondary(int tier, int index) = 0;
};
void setListener(Listener* listener);

const void* offer(void* ctrl, int tier, int index);
bool traderId(void* ctrl, std::int64_t& out);
const void* offerBuyA(const void* offer);
const void* offerBuyB(const void* offer);
const void* offerSell(const void* offer);

bool hovered(int& tier, int& index, unsigned long long withinMs);
void forgetHover();

bool tradePossible(void* ctrl, const void* offer);
bool offerSoldOut(const void* offer);

bool selectTrade(void* ctrl, int tier, int index);

using RowBoolGetter = bool (*)(int tier, int index);
bool bindRowBool(void* ctrl, const char* name, RowBoolGetter fn);

void setClientScreen(void* ctrl);
const void* offerRaw(void* ctrl, int tier, int rawIndex);
void setUnlockTier(int maxTier);

int currentTier(void* ctrl);
bool selectedOffer(void* ctrl, int& tier, int& rawIndex);

int overrideTier(void* owner, int value, const void* returnAddress);
void setCurrentTierOriginal(const void* original);

void setFavoriteTier(const std::vector<std::pair<int, int>>& favorites, const std::vector<int>& tierCounts);

enum class TierBinding { SelectorTotal, TierTotal, TierVisible, TierUnlocked, TierName, Count };
const void* resolveTierBinding(TierBinding which);
void setFavoriteTierHooked(bool hooked);
bool favoriteTierAvailable();

bool favoriteTierShown(const void* self);
bool favoriteTierShownAny();
int favoriteTierRows();
bool restRows(int rawTier, int& count, bool& emptied);
void writeFavoriteTierName(void* ret);
void afterSelParse(void* sel);
using TranslateFn = bool (*)(void* out, const char* key);
void setTranslator(TranslateFn fn);

}
