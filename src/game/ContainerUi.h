#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::containerui {

constexpr std::uint32_t buttonId(const char* name)
{
    std::uint32_t h = 0x811C9DC5u;
    for (; *name != 0; ++name) {
        h ^= static_cast<std::uint8_t>(*name);
        h *= 0x01000193u;
    }
    return h;
}

namespace button {
inline constexpr std::uint32_t kTakeAllPlaceAll = buttonId("button.container_take_all_place_all");
inline constexpr std::uint32_t kTakeHalfPlaceOne = buttonId("button.container_take_half_place_one");
inline constexpr std::uint32_t kAutoPlace = buttonId("button.container_auto_place");
inline constexpr std::uint32_t kDropOne = buttonId("button.drop_one");
inline constexpr std::uint32_t kDropAll = buttonId("button.drop_all");
inline constexpr std::uint32_t kCoalesce = buttonId("button.coalesce_stack");
inline constexpr std::uint32_t kHover = buttonId("button.shape_drawing");
inline constexpr std::uint32_t kSlotHovered = buttonId("button.container_slot_hovered");
inline constexpr std::uint32_t kCursorDropAll = buttonId("button.cursor_drop_all");
inline constexpr std::uint32_t kCursorDropOne = buttonId("button.cursor_drop_one");
inline constexpr std::uint32_t kOutputPrimary = buttonId("button.crafting_output_primary");
inline constexpr std::uint32_t kOutputSecondary = buttonId("button.crafting_output_secondary");
inline constexpr std::uint32_t kOutputTertiary = buttonId("button.crafting_output_tertiary");
inline constexpr std::uint32_t kRecipeSelect = buttonId("button.recipe_select");
inline constexpr std::uint32_t kRecipeSecondary = buttonId("button.recipe_secondary");
inline constexpr std::uint32_t kRecipeTertiary = buttonId("button.recipe_tertiary");
}

inline constexpr int kStatePressed = 0;
inline constexpr int kStateHeld = 1;
inline constexpr int kStateReleased = 2;

enum class Click {
    Left,
    Right,
    Shift,
    DropOne,
    DropAll,
    Double,
};

class Listener {
public:
    virtual ~Listener() = default;

    virtual bool onSlotButton(std::uint32_t id, int state, const std::string& coll, int index) = 0;

    virtual void onScreenTick() = 0;

    virtual void onScreenLost() = 0;

    virtual void onScreenCreated(void* ctrl) { (void)ctrl; }
};

void setListener(Listener* listener);

void addObserver(Listener* observer);
void removeObserver(Listener* observer);

void onScansReady();
bool available();

using SmHandleFn = int(__fastcall*)(void* sm, std::uint32_t id, int state, const void* coll,
                                    int index);
void setSmOriginal(SmHandleFn original);

bool onSmHandle(void* sm, std::uint32_t id, int state, const void* coll, int index, int& result);

std::uint32_t onScreenTickHook(void* ctrl);

void onScreenConstructed(void* ctrl);
void setCtorAddress(const void* ctor);

void setHudCtorAddress(const void* ctor);
using HudCreatedFn = void (*)(void* ctrl);
void onHudConstructed(void* ctrl);
void addHudObserver(HudCreatedFn fn);
void setHudGetItemSite(const void* site, const void* getItem);
void* hudContainerManager(void* ctrl);

void onScreenDestroyed(void* ctrl);

bool hasScreen();
std::uintptr_t screenKind();
void* screenController();

int collectionSize(const std::string& coll);

const void* stackAt(const std::string& coll, int index);
const void* cursorStack();

bool isEmpty(const void* stack);
int countOf(const void* stack);
int maxStackOf(const void* stack);
bool sameItem(const void* a, const void* b);
std::string itemName(const void* stack);
int auxOf(const void* stack);
std::uintptr_t itemKey(const void* stack);

bool click(const std::string& coll, int index, Click kind);
bool dropCursor(bool all);

int itemRarityOf(const void* item);
int rarityOf(const void* stack);

int creativeCategoryOf(const void* stack);

bool storageFill(const void* stack, int& current, int& capacity);
bool nbtContents(const void* stack, int& usedSlots, int& totalItems);
struct NbtItem {
    int slot = -1;
    std::string name;
    int count = 0;
    int aux = 0;
    bool enchanted = false;
    const void* elem = nullptr;
};
bool nbtItemsOfTag(const void* root, std::vector<NbtItem>& out);
const void* userDataOf(const void* stack);
std::string enchantKey(const void* stack);
int nbtTagCount(const void* stack);
bool nbtInt(const void* stack, std::string_view key, std::int32_t& out);
bool itemsListBounds(const void* root, const void*& first, const void*& last);

inline constexpr int kGlintBit = 0x8000;

int maxStackOfItem(const void* item);
int screenCollectionSize(const std::string& coll);
const void* screenStackAt(const std::string& coll, int index);
bool press(std::uint32_t id, const std::string& coll, int index);

bool hovered(std::string& coll, int& index);
bool lastHovered(std::string& coll, int& index);

int slotPitchPixels();

bool slotScreenPos(const std::string& coll, int index, int& x, int& y);

using BoolGetter = bool (*)(std::uintptr_t arg);
using IntGetter = int (*)(std::uintptr_t arg);
using TextGetter = void (*)(std::uintptr_t arg, char* out, std::size_t cap);
using ButtonHandler = void (*)(std::uintptr_t arg);

bool bindingsAvailable();
bool bindBool(void* ctrl, const char* name, BoolGetter fn, std::uintptr_t arg);
bool bindInt(void* ctrl, const char* name, IntGetter fn, std::uintptr_t arg);
bool bindText(void* ctrl, const char* name, TextGetter fn, std::uintptr_t arg);
inline constexpr int kPersistentSlots = 256;
bool floatBindingsAvailable();
volatile std::uint8_t* persistentBool(int slot);
volatile float* persistentFloat(int slot);
bool bindPersistentBool(void* ctrl, const char* name, int slot);
bool bindPersistentFloat(void* ctrl, const char* name, int slot);

inline constexpr int kPersistentTextSlots = 96;
inline constexpr int kPersistentTextBytes = 128;
inline constexpr int kPersistentTextMax = kPersistentTextBytes - 17;
bool writePersistentText(int slot, const char* text, std::size_t length);
bool bindPersistentText(void* ctrl, const char* name, int slot);
void detachPersistentText();
bool onButtonPressed(void* ctrl, const char* buttonName, ButtonHandler fn, std::uintptr_t arg);
bool onButtonHovered(void* ctrl, const char* buttonName, ButtonHandler fn, std::uintptr_t arg);
void requestRefresh();

using CollIntGetter = int (*)(void* ctrl, const std::string& coll, int index, std::uintptr_t arg);
bool collectionBindingsAvailable();
bool bindCollectionInt(void* ctrl, const char* name, CollIntGetter fn, std::uintptr_t arg);
const void* screenStackOf(void* ctrl, const std::string& coll, int index);

int idAuxOfItem(const void* item, int aux);

struct Stats {
    unsigned long long smEvents = 0;
    unsigned long long hoverEvents = 0;
    unsigned long long ticks = 0;
    unsigned long long tickCalls = 0;
};
Stats stats();

}
