#include "game/UiProbe.h"

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/GameVersion.h"
#include "game/OreUiPatch.h"
#include "game/StackCount.h"
#include "game/UiTree.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "input/Keys.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/Module.h"
#include "modules/ModuleManager.h"
#include "modules/Schematica.h"

#include <Windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <format>
#include <functional>
#include <mutex>
#include <set>
#include <map>
#include <string>
#include <vector>

namespace tsukuyomi::uiprobe {
namespace {

std::atomic<unsigned long long> g_ownNavInvokedAt{0};

std::atomic<bool> g_ownSettingsRoute{false};

std::atomic<unsigned long long> g_ownKeyOpenAt{0};

constexpr char kOwnNavId[] = "json-navigation-level_texture_pack-jsonui";

std::atomic<unsigned long long> g_lookupCount{0};

constexpr std::ptrdiff_t kStringSize = 0x10;
constexpr std::ptrdiff_t kStringCapacity = 0x18;
constexpr size_t kSsoCapacity = 15;
constexpr size_t kMaxNameLength = 64;

constexpr size_t kMaxUnique = 3000;

constexpr unsigned long long kHeartbeatEvery = 1000;
constexpr int kMaxHeartbeats = 60;

constexpr unsigned long long kMaxOptions = 4000;

std::mutex g_mutex;
std::set<std::wstring> g_seen;
std::atomic<unsigned long long> g_calls{0};
std::atomic<bool> g_alive{false};
std::atomic<int> g_heartbeats{0};
bool g_capped = false;

std::atomic<unsigned long long> g_options{0};
std::atomic<void*> g_optionSelf{nullptr};
std::atomic<bool> g_optionsCapped{false};

constexpr const wchar_t* kWatched[] = {
    L"how_to_play/how_to_play_screen",
    L"how_to_play/how_to_play_selector_stack_panel",
    L"how_to_play/how_to_play_section_content_panels",
    L"how_to_play/moving_around_button",

};
std::set<std::wstring> g_dumped;
std::atomic<bool> g_substituted{false};

constexpr bool kSubstituteEnabled = false;

constexpr const char* kFromSpace = "pause";
constexpr const char* kFromName = "settings_button";

constexpr std::ptrdiff_t kNodeLeft = 0x00;
constexpr std::ptrdiff_t kNodeParent = 0x08;
constexpr std::ptrdiff_t kNodeIsNil = 0x19;
constexpr std::ptrdiff_t kNodeValue = 0x20;
constexpr std::ptrdiff_t kNodeRight = 0x10;
constexpr unsigned kTagString = 4;
constexpr unsigned kTagArray = 6;
constexpr unsigned kTagObject = 7;

constexpr int kMaxNodes = 256;
constexpr size_t kMaxDumpElems = 12;
constexpr size_t kMaxKeyLength = 96;

std::wstring readString(const void* text)
{
    if (!memory::isReadable(text, static_cast<size_t>(kStringCapacity) + sizeof(size_t))) {
        return {};
    }
    const auto* const base = static_cast<const std::byte*>(text);
    size_t length = 0;
    size_t capacity = 0;
    std::memcpy(&length, base + kStringSize, sizeof(length));
    std::memcpy(&capacity, base + kStringCapacity, sizeof(capacity));
    if (length == 0 || length > kMaxNameLength || capacity < length) {
        return {};
    }

    const char* chars = nullptr;
    if (capacity <= kSsoCapacity) {
        chars = reinterpret_cast<const char*>(base);
    } else {
        const char* heap = nullptr;
        std::memcpy(&heap, base, sizeof(heap));
        if (!memory::isReadable(heap, length)) {
            return {};
        }
        chars = heap;
    }

    std::wstring out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(chars[i]);
        if (ch < 0x20 || ch > 0x7E) {
            return {};
        }
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

std::wstring readKey(std::uintptr_t at)
{
    if (at == 0 || !memory::isReadable(reinterpret_cast<const void*>(at), 1)) {
        return {};
    }
    const auto* const chars = reinterpret_cast<const char*>(at);
    std::wstring out;
    for (size_t i = 0; i < kMaxKeyLength; ++i) {
        if (!memory::isReadable(chars + i, 1)) {
            return {};
        }
        const unsigned char ch = static_cast<unsigned char>(chars[i]);
        if (ch == 0) {
            return out;
        }
        if (ch < 0x20 || ch > 0x7E) {
            return {};
        }
        out.push_back(static_cast<wchar_t>(ch));
    }
    return {};
}

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool lookupGuarded(void* self, const void* space, const void* name, void*& out)
{
    __try {
        out = hooks::callUiDefLookup(self, space, name);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

#pragma pack(push, 8)
struct TreeNode {
    std::uintptr_t left;
    std::uintptr_t parent;
    std::uintptr_t right;
    unsigned char color;
    unsigned char isnil;
    unsigned char pad[6];
    std::uintptr_t key;
    std::uintptr_t value;
    std::uintptr_t tag;
    std::uintptr_t seq;
};
#pragma pack(pop)
static_assert(sizeof(TreeNode) == 0x40);

constexpr int kMaxOwnKeys = 256;

constexpr size_t kMaxMenuItems = 24;
constexpr size_t kMenuNameBytes = 32;

char g_menuTextBuf[kMaxMenuItems][kMenuNameBytes]{};
size_t g_menuCount = 0;

Module* g_menuModules[kMaxMenuItems]{};

std::atomic<int> g_pendingMenu{-1};

std::atomic<std::uintptr_t> g_toggleCtl{0};

TreeNode g_ownHeads[kMaxMenuItems]{};
TreeNode g_ownNodes[kMaxMenuItems][kMaxOwnKeys]{};
std::uintptr_t g_ownNodePtrs[kMaxMenuItems][4]{};
std::uintptr_t g_ownValues[kMaxMenuItems][4]{};

alignas(8) unsigned char g_ownTextRecs[kMaxMenuItems][16]{};

constexpr char kOwnButtonId[] = "button.menu_settings";
alignas(8) unsigned char g_ownButtonIdRec[16]{};

constexpr size_t kIdPageSize = 4096;
void* g_idPage = nullptr;

std::atomic<unsigned long long> g_armAt{0};
std::atomic<int> g_watchHits{0};
constexpr int kMaxWatchHits = 12;
constexpr unsigned long long kRearmDelayMs = 2000;

const char* ownButtonIdText()
{
    if (g_idPage != nullptr) {
        return static_cast<const char*>(g_idPage);
    }
    g_idPage = VirtualAlloc(nullptr, kIdPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_idPage == nullptr) {
        log().warn(L"UiProbe: could not allocate the guard page");
        return kOwnButtonId;
    }
    std::memcpy(g_idPage, kOwnButtonId, sizeof(kOwnButtonId));
    return static_cast<const char*>(g_idPage);
}

void armWatchNow()
{
    if (g_idPage == nullptr || g_watchHits.load() >= kMaxWatchHits) {
        return;
    }
    g_armAt.store(0);
    DWORD previous = 0;
    if (VirtualProtect(g_idPage, kIdPageSize, PAGE_READWRITE | PAGE_GUARD, &previous) == 0) {
        log().warn(L"UiProbe: could not arm the button-id guard page (err {})", GetLastError());
    }
}

bool g_ownReady[kMaxMenuItems]{};

constexpr size_t kMaxNumbersPerModule = 3;
constexpr size_t kNumTextBytes = 32;

struct NumberSetting {
    std::function<float()> get;
    std::function<void(float)> set;
    bool integer = false;
    char label[kNumTextBytes]{};
    char shown[kNumTextBytes]{};
};
NumberSetting g_numbers[kMaxMenuItems][kMaxNumbersPerModule];
size_t g_numberCount[kMaxMenuItems]{};

constexpr size_t kMaxKeysPerModule = 3;

struct KeybindSetting {
    std::function<std::vector<int>()> get;
    std::function<void(std::vector<int>)> set;
    std::vector<int> defaults;
    char label[kNumTextBytes]{};
    char shown[kNumTextBytes]{};
};
KeybindSetting g_keybinds[kMaxMenuItems][kMaxKeysPerModule];
size_t g_keybindCount[kMaxMenuItems]{};

constexpr size_t kMaxRowsPerModule = kMaxNumbersPerModule + kMaxKeysPerModule;

constexpr size_t kEditBufBytes = 24;
constexpr int kEditKindKey = 0x10000;
std::atomic<int> g_editRequest{-1};

std::atomic<int> g_resetRequest{-1};
int g_editTarget = -1;
bool g_editIsKey = false;
unsigned long long g_keyWaitFrom = 0;

bool g_keyWaitHeld[256]{};
unsigned g_keyWaitTicks = 0;
int g_keyWaitLogs = 0;
char g_editBuf[kEditBufBytes]{};
size_t g_editLen = 0;

void buildMenuTexts()
{
    if (g_menuCount != 0) {
        return;
    }
    std::snprintf(g_menuTextBuf[0], kMenuNameBytes, "Tsukuyomi");
    g_menuModules[0] = nullptr;
    g_menuCount = 1;

    for (Module* one : ModuleManager::instance().modules()) {
        if (g_menuCount >= kMaxMenuItems) {
            log().warn(L"UiProbe: menu item limit ({}) reached, skipping the rest",
                       kMaxMenuItems);
            break;
        }
        if (one == nullptr || !one->available()) {
            continue;
        }
        const wchar_t* const wide = one->name();
        if (wide == nullptr) {
            continue;
        }
        size_t at = 0;
        for (; wide[at] != L'\0' && at + 1 < kMenuNameBytes; ++at) {
            const wchar_t ch = wide[at];
            g_menuTextBuf[g_menuCount][at] = (ch < 0x80) ? static_cast<char>(ch) : '?';
        }
        g_menuTextBuf[g_menuCount][at] = '\0';
        g_menuModules[g_menuCount] = one;
        ++g_menuCount;
    }
}

void collectNumbers()
{
    for (size_t i = 0; i < g_menuCount; ++i) {
        g_numberCount[i] = 0;
        Module* const one = g_menuModules[i];
        if (one == nullptr) {
            continue;
        }
        const MenuItem menu = one->buildMenu();
        for (const MenuItem& child : menu.children) {
            if (child.kind != MenuItemKind::Number || !child.getNumber || !child.setNumber) {
                continue;
            }
            if (g_numberCount[i] >= kMaxNumbersPerModule) {
                log().warn(L"UiProbe: {} exceeds the numeric-setting limit ({}), skipping the rest",
                           one->name(), kMaxNumbersPerModule);
                break;
            }
            NumberSetting& slot = g_numbers[i][g_numberCount[i]];
            slot.get = child.getNumber;
            slot.set = child.setNumber;
            slot.integer = child.numberIsInteger;

            const std::wstring label = child.labelText();
            size_t at = 0;
            for (; at < label.size() && at + 1 < kNumTextBytes; ++at) {
                slot.label[at] = (label[at] < 0x80) ? static_cast<char>(label[at]) : '?';
            }
            slot.label[at] = '\0';

            const float now = slot.get();
            if (slot.integer) {
                std::snprintf(slot.shown, kNumTextBytes, "%d  (Enter)", static_cast<int>(now));
            } else {
                std::snprintf(slot.shown, kNumTextBytes, "%g  (Enter)",
                              static_cast<double>(now));
            }
            ++g_numberCount[i];
        }
    }
}

bool gameKeyName(int keyCode, std::wstring& out)
{
    thread_local alignas(8) unsigned char slot[0x20]{};
    std::memset(slot, 0, sizeof(slot));
    const std::uintptr_t room = kSsoCapacity;
    std::memcpy(slot + kStringCapacity, &room, sizeof(room));

    if (!hooks::callKeyDisplayName(slot, keyCode)) {
        return false;
    }
    std::uintptr_t after = 0;
    std::memcpy(&after, slot + kStringCapacity, sizeof(after));
    if (after > kSsoCapacity) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            log().warn(L"UiProbe: key name did not fit in SSO (capacity {}); one alloc leaked",
                       after);
        }
    }
    out = readString(slot);
    return !out.empty();
}

std::wstring gameComboName(const std::vector<int>& combo)
{
    if (combo.empty()) {
        std::wstring none;
        return gameKeyName(0, none) ? none : keys::comboName(combo);
    }
    std::wstring text;
    for (const int key : combo) {
        std::wstring one;
        if (!gameKeyName(key, one)) {
            return keys::comboName(combo);
        }
        if (!text.empty()) {
            text += L" + ";
        }
        text += one;
    }
    return text;
}

void collectKeybinds()
{
    for (size_t i = 0; i < g_menuCount; ++i) {
        g_keybindCount[i] = 0;
        Module* const one = g_menuModules[i];
        if (one == nullptr) {
            continue;
        }
        const MenuItem menu = one->buildMenu();
        for (const MenuItem& child : menu.children) {
            if (child.kind != MenuItemKind::Keybind || !child.getKeys || !child.setKeys) {
                continue;
            }
            if (g_keybindCount[i] >= kMaxKeysPerModule) {
                log().warn(L"UiProbe: {} exceeds the key-binding limit ({}), skipping the rest",
                           one->name(), kMaxKeysPerModule);
                break;
            }
            KeybindSetting& slot = g_keybinds[i][g_keybindCount[i]];
            slot.get = child.getKeys;
            slot.set = child.setKeys;
            slot.defaults = child.defaultKeys;

            const std::wstring label = child.labelText();
            size_t at = 0;
            for (; at < label.size() && at + 1 < kNumTextBytes; ++at) {
                slot.label[at] = (label[at] < 0x80) ? static_cast<char>(label[at]) : '?';
            }
            slot.label[at] = '\0';

            const std::vector<int> combo = slot.get();
            {
                const std::wstring shown = gameComboName(combo);
                size_t s = 0;
                for (; s < shown.size() && s + 1 < kNumTextBytes; ++s) {
                    slot.shown[s] = (shown[s] < 0x80) ? static_cast<char>(shown[s]) : '?';
                }
                slot.shown[s] = '\0';
            }
            ++g_keybindCount[i];
        }
    }
}

constexpr char kChildKeyA[] = "tk_a";
constexpr char kChildKeyB[] = "tk_b";

TreeNode g_childHeadA{};
TreeNode g_childNodesA[1]{};
std::uintptr_t g_childNodeA[4]{};
std::uintptr_t g_childValueA[4]{};

TreeNode g_childHeadB{};
TreeNode g_childNodesB[1]{};
std::uintptr_t g_childNodeB[4]{};
std::uintptr_t g_childValueB[4]{};

std::uintptr_t g_elems[2]{};
std::uintptr_t g_vec[4]{};

constexpr int kMaxNavKeys = 32;
TreeNode g_navHead{};
TreeNode g_navNodes[kMaxNavKeys]{};
std::uintptr_t g_navNode[4]{};
std::uintptr_t g_navValue[4]{};

const char* const kNavItemKeys[kMaxMenuItems] = {
    "tk_nav00", "tk_nav01", "tk_nav02", "tk_nav03", "tk_nav04", "tk_nav05",
    "tk_nav06", "tk_nav07", "tk_nav08", "tk_nav09", "tk_nav10", "tk_nav11",
    "tk_nav12", "tk_nav13", "tk_nav14", "tk_nav15", "tk_nav16", "tk_nav17",
    "tk_nav18", "tk_nav19", "tk_nav20", "tk_nav21", "tk_nav22", "tk_nav23",
};
static_assert(std::size(kNavItemKeys) == kMaxMenuItems);
TreeNode g_navItemHeads[kMaxMenuItems]{};
TreeNode g_navItemNodes[kMaxMenuItems][1]{};
std::uintptr_t g_navItemNodePtrs[kMaxMenuItems][4]{};
std::uintptr_t g_navItemValues[kMaxMenuItems][4]{};

constexpr bool kNavKeepVanilla = false;

std::uintptr_t g_navElems[kMaxMenuItems + 1]{};
std::uintptr_t g_navVec[4]{};
bool g_navReady = false;

constexpr size_t kMaxPaneElems = 64;
TreeNode g_paneHead{};
TreeNode g_paneNodes[kMaxNavKeys]{};
std::uintptr_t g_paneNode[4]{};
std::uintptr_t g_paneElems[kMaxPaneElems]{};
std::uintptr_t g_paneVec[4]{};

TreeNode g_paneWrapHead{};
TreeNode g_paneWrapNodes[1]{};
std::uintptr_t g_paneWrapNode[4]{};
std::uintptr_t g_paneWrapValue[4]{};

constexpr size_t kPaneKeyBytes = 96;
char g_paneItemKeys[kMaxMenuItems][kPaneKeyBytes]{};
TreeNode g_paneItemHeads[kMaxMenuItems]{};
TreeNode g_paneItemNodes[kMaxMenuItems][1]{};
std::uintptr_t g_paneItemNodePtrs[kMaxMenuItems][4]{};
std::uintptr_t g_paneItemValues[kMaxMenuItems][4]{};

constexpr size_t kPaneTopicBytes = 16;
char g_paneTopics[kMaxMenuItems][kPaneTopicBytes]{};
alignas(8) unsigned char g_paneTopicRecs[kMaxMenuItems][16]{};
TreeNode g_paneOverHeads[kMaxMenuItems]{};
TreeNode g_paneOverNodes[kMaxMenuItems][3]{};
std::uintptr_t g_paneOverNodePtrs[kMaxMenuItems][4]{};

constexpr char kOwnTextVar[] = "$button_text";
alignas(8) unsigned char g_paneTextRecs[kMaxMenuItems][16]{};

TreeNode g_labelHead{};
TreeNode g_labelNodes[kMaxOwnKeys]{};
std::uintptr_t g_labelNode[4]{};
alignas(8) unsigned char g_labelTextRec[16]{};
TreeNode g_labelWrapHead{};
TreeNode g_labelWrapNodes[1]{};
std::uintptr_t g_labelWrapNode[4]{};
std::uintptr_t g_labelWrapValue[4]{};
std::uintptr_t g_labelElems[1]{};
std::uintptr_t g_labelVec[4]{};
TreeNode g_labelTopHead{};
TreeNode g_labelTopNodes[kMaxNavKeys]{};
std::uintptr_t g_labelTopNode[4]{};
std::uintptr_t g_labelTopValue[4]{};
bool g_labelReady = false;

constexpr char kHeaderTitle[] = "Schematica";

constexpr int kScreenMenu = 0;
constexpr int kScreenBlueprints = 1;
constexpr int kScreenBlueprint = 2;
constexpr int kScreenSettings = 3;
constexpr int kScreenMaterials = 4;
constexpr int kScreenVerifier = 5;
constexpr int kScreenLayers = 6;
constexpr int kScreenCount = 7;
std::atomic<int> g_pgScreen{kScreenMenu};

char g_hdrTitleText[96] = "Schematica";

const char* pageHeaderTitle()
{
    const auto ascii = [](char* out, size_t cap, const std::wstring& text) {
        size_t at = 0;
        for (wchar_t ch : text) {
            if (at + 1 >= cap) {
                break;
            }
            out[at++] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '?';
        }
        out[at] = 0;
    };
    switch (g_pgScreen.load(std::memory_order_relaxed)) {
    case kScreenBlueprints:
        std::snprintf(g_hdrTitleText, sizeof(g_hdrTitleText), "%s => Schematic Placements",
                      kHeaderTitle);
        break;
    case kScreenBlueprint: {
        char name[64]{};
        Schematica& mod = Schematica::instance();
        const int at = mod.editingIndex();
        if (at >= 0) {
            ascii(name, sizeof(name), mod.blueprintName(static_cast<size_t>(at)));
        }
        std::snprintf(g_hdrTitleText, sizeof(g_hdrTitleText), "%s => Configure: %s", kHeaderTitle,
                      (name[0] != 0) ? name : "(none)");
        break;
    }
    case kScreenSettings:
        std::snprintf(g_hdrTitleText, sizeof(g_hdrTitleText), "%s => Configuration menu",
                      kHeaderTitle);
        break;
    case kScreenLayers:
        std::snprintf(g_hdrTitleText, sizeof(g_hdrTitleText), "%s => Render Layers",
                      kHeaderTitle);
        break;
    case kScreenMaterials:
    case kScreenVerifier: {
        char name[64]{};
        Schematica& mod2 = Schematica::instance();
        const int at2 = mod2.editingIndex();
        if (at2 >= 0) {
            ascii(name, sizeof(name), mod2.blueprintName(static_cast<size_t>(at2)));
        }
        std::snprintf(g_hdrTitleText, sizeof(g_hdrTitleText), "%s => %s: %s", kHeaderTitle,
                      (g_pgScreen.load(std::memory_order_relaxed) == kScreenMaterials)
                          ? "Material List"
                          : "Schematic Verifier",
                      (name[0] != 0) ? name : "(none)");
        break;
    }
    default:
        std::snprintf(g_hdrTitleText, sizeof(g_hdrTitleText), "%s", kHeaderTitle);
        break;
    }
    return g_hdrTitleText;
}
constexpr char kHeaderBindNone[] = "none";
alignas(8) unsigned char g_hdrTitleRec[16]{};
constexpr char kHeaderSizeKey[] = "size";
constexpr char kHeaderWidth[] = "100%";
constexpr char kHeaderHeight[] = "23px";
constexpr char kHeaderAnchorFromKey[] = "anchor_from";
constexpr char kHeaderAnchorToKey[] = "anchor_to";
constexpr char kHeaderAnchor[] = "top_left";
alignas(8) unsigned char g_hdrAnchorRecs[2][16]{};
alignas(8) unsigned char g_hdrSizeRecs[2][16]{};
std::uintptr_t g_hdrSizeElems[2][4]{};
std::uintptr_t g_hdrSizeElemPtrs[2]{};
std::uintptr_t g_hdrSizeVec[4]{};
alignas(8) unsigned char g_hdrBindRec[16]{};
constexpr size_t kMaxHdrElems = 16;

TreeNode g_hdrTitleHead{};
TreeNode g_hdrTitleNodes[kMaxNavKeys]{};
std::uintptr_t g_hdrTitleNode[4]{};
TreeNode g_hdrTitleWrapHead{};
TreeNode g_hdrTitleWrapNodes[1]{};
std::uintptr_t g_hdrTitleWrapNode[4]{};
std::uintptr_t g_hdrTitleWrapValue[4]{};

std::uintptr_t g_hdrBarElems[kMaxHdrElems]{};
std::uintptr_t g_hdrBarVec[4]{};
TreeNode g_hdrBarHead{};
TreeNode g_hdrBarNodes[kMaxNavKeys]{};
std::uintptr_t g_hdrBarNode[4]{};
TreeNode g_hdrBarWrapHead{};
TreeNode g_hdrBarWrapNodes[1]{};
std::uintptr_t g_hdrBarWrapNode[4]{};
std::uintptr_t g_hdrBarWrapValue[4]{};

std::uintptr_t g_hdrElems[1]{};
std::uintptr_t g_hdrVec[4]{};
TreeNode g_hdrHead{};
TreeNode g_hdrNodes[kMaxNavKeys]{};
std::uintptr_t g_hdrNode[4]{};
std::uintptr_t g_hdrValue[4]{};
bool g_hdrReady = false;

constexpr bool kNavPaneOwnItems = true;

constexpr bool kOwnMenuScreen = false;

constexpr size_t kNavPaneOwnCount = 0;
static_assert(kNavPaneOwnCount <= kMaxMenuItems);

constexpr bool kPaneKeepVanilla = false;

constexpr bool kContentPanelEnabled = true;

TreeNode g_contentHead{};
TreeNode g_contentNodes[kMaxNavKeys]{};
std::uintptr_t g_contentNode[4]{};
std::uintptr_t g_contentValue[4]{};
std::uintptr_t g_contentElems[1]{};
std::uintptr_t g_contentVec[4]{};
bool g_contentReady = false;

constexpr size_t kMaxSecElems = 64;
TreeNode g_secBodyHead{};
TreeNode g_secBodyNodes[kMaxNavKeys]{};
std::uintptr_t g_secBodyNode[4]{};
std::uintptr_t g_secElems[kMaxSecElems]{};
std::uintptr_t g_secVec[4]{};
TreeNode g_secWrapHead{};
TreeNode g_secWrapNodes[1]{};
std::uintptr_t g_secWrapNode[4]{};
std::uintptr_t g_secWrapValue[4]{};

constexpr size_t kSecKeyBytes = 96;
char g_secItemKeys[kMaxMenuItems][kSecKeyBytes]{};
constexpr size_t kSecNameBytes = 32;
char g_secToggleNames[kMaxMenuItems][kSecNameBytes]{};
alignas(8) unsigned char g_secNameRecs[kMaxMenuItems][16]{};
constexpr int kMaxBindKeys = 8;
TreeNode g_secBindHeads[kMaxMenuItems]{};
TreeNode g_secBindNodes[kMaxMenuItems][kMaxBindKeys]{};
std::uintptr_t g_secBindNodePtrs[kMaxMenuItems][4]{};
std::uintptr_t g_secBindValues[kMaxMenuItems][4]{};
std::uintptr_t g_secBindElems[kMaxMenuItems][1]{};
std::uintptr_t g_secBindVec[kMaxMenuItems][4]{};
TreeNode g_secOverHeads[kMaxMenuItems]{};
TreeNode g_secOverNodes[kMaxMenuItems][2]{};
std::uintptr_t g_secOverNodePtrs[kMaxMenuItems][4]{};
TreeNode g_secItemHeads[kMaxMenuItems]{};
TreeNode g_secItemNodes[kMaxMenuItems][1]{};
std::uintptr_t g_secItemNodePtrs[kMaxMenuItems][4]{};
std::uintptr_t g_secItemValues[kMaxMenuItems][4]{};

constexpr char kSecBindCond[] = "always_when_visible";

constexpr bool kProbeRealBinding = false;
constexpr size_t kProbeBindingIndex = 1;
constexpr char kProbeBindingName[] = "#hide_chat";
constexpr char kSecCtlNameVar[] = "$control_name";
constexpr char kSecEnabledVar[] = "$enabled";
constexpr char kSecFocusIdVar[] = "$focus_id";
constexpr char kSecOneLineVar[] = "$one_line_layout";
constexpr char kSecBindNameVar[] = "$option_binding_name";
constexpr char kSecStateBindVar[] = "$toggle_state_binding_name";
constexpr char kSecLabelVar[] = "$option_label";
constexpr char kSecBindCondVar[] = "$toggle_binding_condition";
constexpr char kSecDefaultVar[] = "$toggle_default_state";
constexpr char kSecToggleNameVar[] = "$toggle_name";
constexpr size_t kSecCtlNameBytes = 48;
char g_secCtlNames[kMaxMenuItems][kSecCtlNameBytes]{};
alignas(8) unsigned char g_secCtlNameRecs[kMaxMenuItems][16]{};
alignas(8) unsigned char g_secLabelRecs[kMaxMenuItems][16]{};
char g_secBindNames[kMaxMenuItems][kSecCtlNameBytes]{};
char g_secToggleNameBufs[kMaxMenuItems][kSecCtlNameBytes]{};
char g_secFocusIds[kMaxMenuItems][kSecCtlNameBytes]{};
alignas(8) unsigned char g_secBindNameRecs[kMaxMenuItems][16]{};
alignas(8) unsigned char g_secToggleNameRecs[kMaxMenuItems][16]{};
alignas(8) unsigned char g_secFocusIdRecs[kMaxMenuItems][16]{};
alignas(8) unsigned char g_secBindCondRec[16]{};
char g_secBtnKeys[kMaxMenuItems][kSecKeyBytes]{};
constexpr int kMaxSecOverKeys = 10;
TreeNode g_secBtnOverHeads[kMaxMenuItems]{};
TreeNode g_secBtnOverNodes[kMaxMenuItems][kMaxSecOverKeys]{};
std::uintptr_t g_secBtnOverNodePtrs[kMaxMenuItems][4]{};
TreeNode g_secBtnHeads[kMaxMenuItems]{};
TreeNode g_secBtnNodes[kMaxMenuItems][1]{};
std::uintptr_t g_secBtnNodePtrs[kMaxMenuItems][4]{};
std::uintptr_t g_secBtnValues[kMaxMenuItems][4]{};
std::uintptr_t g_secCtlElems[kMaxMenuItems][1 + kMaxRowsPerModule]{};
std::uintptr_t g_secCtlVec[kMaxMenuItems][4]{};

constexpr char kNumHintVar[] = "$option_place_holder_text";

constexpr char kTgTypeVar[] = "$toggle_binding_type";
constexpr char kTgTypeNone[] = "none";
constexpr char kTgTypeGlobal[] = "global";
constexpr char kTgDefaultVar[] = "$toggle_default_state";
constexpr char kTgBindVar[] = "$toggle_state_binding_name";
constexpr char kTgNoBind[] = "#not_data_bound";

constexpr char kTabRef[] = "tk_pt%u@settings_common.section_toggle_base";
constexpr char kTabRadioVar[] = "$radio_toggle_group";
constexpr char kTabViewVar[] = "$toggle_view_binding_name";
constexpr char kTabGlyphVar[] = "$glyph_texture";
constexpr char kTabGlyphColorVar[] = "$glyph_color_texture";
constexpr char kTabGlyphSizeVar[] = "$glyph_size";

constexpr char kTgTrueKey[] = "focus_magnet_enabled";
constexpr char kTgFalseKey[] = "$toggle_default_state|default";

constexpr char kBtnTextVar[] = "$button_text";

constexpr char kBtnTexVar[] = "$default_button_texture";
constexpr char kBtnTexValue[] = "textures/ui/text_label_box";

constexpr char kBtnSizeW[] = "100%";
constexpr char kBtnSizeH[] = "100%";

constexpr char kKeyBtnW[] = "100%";
constexpr char kKeyBtnH[] = "30px";

constexpr char kBtnImgSizeVar[] = "$button_image_size_override";
constexpr char kBtnSizeVar[] = "$button_size_override";
constexpr char kBtnBorderSizeVar[] = "$focus_border_size_override";
constexpr size_t kBtnSizeSlots = 3;

char g_numKeys[kMaxMenuItems][kMaxRowsPerModule][kSecKeyBytes]{};
char g_numCtlNames[kMaxMenuItems][kMaxRowsPerModule][kSecCtlNameBytes]{};
char g_numBindNames[kMaxMenuItems][kMaxRowsPerModule][kSecCtlNameBytes]{};
alignas(8) unsigned char g_numCtlRecs[kMaxMenuItems][kMaxRowsPerModule][16]{};
alignas(8) unsigned char g_numBindRecs[kMaxMenuItems][kMaxRowsPerModule][16]{};
alignas(8) unsigned char g_numLabelRecs[kMaxMenuItems][kMaxRowsPerModule][16]{};
alignas(8) unsigned char g_numHintRecs[kMaxMenuItems][kMaxRowsPerModule][16]{};
alignas(8) unsigned char g_numTexRecs[kMaxMenuItems][kMaxRowsPerModule][16]{};

alignas(8) unsigned char g_numSizeStrRecs[kMaxMenuItems][kMaxRowsPerModule][kBtnSizeSlots][2][16]{};
std::uintptr_t g_numSizeElems[kMaxMenuItems][kMaxRowsPerModule][kBtnSizeSlots][2][4]{};
std::uintptr_t g_numSizeElemPtrs[kMaxMenuItems][kMaxRowsPerModule][kBtnSizeSlots][2]{};
std::uintptr_t g_numSizeVec[kMaxMenuItems][kMaxRowsPerModule][kBtnSizeSlots][4]{};

TreeNode g_numOverHeads[kMaxMenuItems][kMaxRowsPerModule]{};
TreeNode g_numOverNodes[kMaxMenuItems][kMaxRowsPerModule][9]{};
std::uintptr_t g_numOverPtrs[kMaxMenuItems][kMaxRowsPerModule][9]{};
TreeNode g_numHeads[kMaxMenuItems][kMaxRowsPerModule]{};
TreeNode g_numNodes[kMaxMenuItems][kMaxRowsPerModule][1]{};
std::uintptr_t g_numNodePtrs[kMaxMenuItems][kMaxRowsPerModule][4]{};
std::uintptr_t g_numValues[kMaxMenuItems][kMaxRowsPerModule][4]{};

std::atomic<int> g_selectedMenu{-1};

TreeNode g_wrapHead{};
TreeNode g_wrapNodes[3]{};
std::uintptr_t g_wrapNode[4]{};
std::uintptr_t g_wrapValue[4]{};

bool g_wrapReady = false;

std::uintptr_t linkBalanced(TreeNode* nodes, TreeNode* head, int lo, int hi,
                            std::uintptr_t parent)
{
    if (lo >= hi) {
        return reinterpret_cast<std::uintptr_t>(head);
    }
    const int mid = lo + (hi - lo) / 2;
    TreeNode& node = nodes[mid];
    node.parent = parent;
    node.isnil = 0;
    node.color = 1;
    const std::uintptr_t self = reinterpret_cast<std::uintptr_t>(&node);
    node.left = linkBalanced(nodes, head, lo, mid, self);
    node.right = linkBalanced(nodes, head, mid + 1, hi, self);
    return self;
}

void finishMap(TreeNode* head, TreeNode* nodes, int count, std::uintptr_t* node)
{
    head->isnil = 1;
    head->color = 1;
    if (count <= 0) {
        const auto self = reinterpret_cast<std::uintptr_t>(head);
        head->parent = self;
        head->left = self;
        head->right = self;
        node[0] = self;
        node[1] = 0;
        node[2] = 0;
        node[3] = 0;
        return;
    }
    head->parent = linkBalanced(nodes, head, 0, count, reinterpret_cast<std::uintptr_t>(head));
    head->left = reinterpret_cast<std::uintptr_t>(&nodes[0]);
    head->right = reinterpret_cast<std::uintptr_t>(&nodes[count - 1]);
    node[0] = reinterpret_cast<std::uintptr_t>(head);
    node[1] = static_cast<std::uintptr_t>(count);
    node[2] = 0;
    node[3] = 0;
}

std::wstring hexAscii(const void* at, size_t count)
{
    if (!memory::isReadable(at, count)) {
        return L"<unreadable>";
    }
    const auto* const bytes = static_cast<const unsigned char*>(at);
    std::wstring hex;
    std::wstring text;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ch = bytes[i];
        const wchar_t digits[] = L"0123456789abcdef";
        hex.push_back(digits[ch >> 4]);
        hex.push_back(digits[ch & 0x0F]);
        hex.push_back(L' ');
        text.push_back((ch >= 0x20 && ch <= 0x7E) ? static_cast<wchar_t>(ch) : L'.');
    }
    return hex + L" |" + text + L"|";
}

struct Entry {
    std::wstring keyText;
    std::uintptr_t key = 0;
    std::uintptr_t value = 0;
    std::uintptr_t tag = 0;
    std::uintptr_t seq = 0;
};

bool collectEntries(const void* value, std::vector<Entry>& out, std::uintptr_t& count)
{
    if (!memory::isReadable(value, sizeof(void*) * 2)) {
        return false;
    }
    const std::uintptr_t node = *static_cast<const std::uintptr_t*>(value);
    if (!memory::isReadable(reinterpret_cast<const void*>(node), sizeof(void*) * 2)) {
        return false;
    }
    const auto* const nodeBytes = reinterpret_cast<const std::byte*>(node);
    std::uintptr_t head = 0;
    std::memcpy(&head, nodeBytes, sizeof(head));
    std::memcpy(&count, nodeBytes + sizeof(std::uintptr_t), sizeof(count));
    if (!memory::isReadable(reinterpret_cast<const void*>(head), kNodeValue)) {
        return false;
    }

    std::uintptr_t stack[kMaxNodes]{};
    int top = 0;
    std::memcpy(&stack[top++], reinterpret_cast<const std::byte*>(head) + kNodeParent,
                sizeof(std::uintptr_t));

    int visited = 0;
    while (top > 0 && visited < kMaxNodes) {
        const std::uintptr_t at = stack[--top];
        if (at == 0 || at == head
            || !memory::isReadable(reinterpret_cast<const void*>(at),
                                   static_cast<size_t>(kNodeValue) + 0x20)) {
            continue;
        }
        const auto* const entry = reinterpret_cast<const std::byte*>(at);
        if (entry[kNodeIsNil] != std::byte{0}) {
            continue;
        }
        ++visited;
        const auto* const myval = reinterpret_cast<const std::uintptr_t*>(entry + kNodeValue);
        Entry item;
        item.keyText = readKey(myval[0]);
        if (item.keyText.empty()) {
            return false;
        }
        item.key = myval[0];
        item.value = myval[1];
        item.tag = myval[2];
        item.seq = myval[3];
        out.push_back(std::move(item));
        if (top + 2 <= kMaxNodes) {
            std::memcpy(&stack[top++], entry + kNodeLeft, sizeof(std::uintptr_t));
            std::memcpy(&stack[top++], entry + kNodeRight, sizeof(std::uintptr_t));
        }
    }
    return visited == static_cast<int>(count);
}

void dumpVector(const void* at)
{
    if (!memory::isReadable(at, 0x20)) {
        log().warn(L"UiProbe:     cannot read the array");
        return;
    }
    const auto* const bytes = static_cast<const std::byte*>(at);
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, bytes, sizeof(begin));
    std::memcpy(&end, bytes + 0x08, sizeof(end));
    if (end < begin || (end - begin) > 0x1000) {
        log().warn(L"UiProbe:     array bounds look wrong (begin {:#x} end {:#x})", begin, end);
        return;
    }
    const size_t span = static_cast<size_t>(end - begin);
    if (span == 0) {
        return;
    }

    const size_t elems = span / sizeof(std::uintptr_t);
    for (size_t i = 0; i < elems && i < kMaxDumpElems; ++i) {
        std::uintptr_t at = 0;
        std::memcpy(&at, reinterpret_cast<const std::byte*>(begin) + i * sizeof(at), sizeof(at));
        if (!memory::isReadable(reinterpret_cast<const void*>(at), 32)) {
            continue;
        }
    }
}

void dumpNode(const std::wstring& label, const void* value)
{
    if (!memory::isReadable(value, sizeof(void*) * 2)) {
        log().warn(L"UiProbe: [{}] cannot read the return value", label);
        return;
    }
    const std::uintptr_t node = *static_cast<const std::uintptr_t*>(value);
    if (!memory::isReadable(reinterpret_cast<const void*>(node), sizeof(void*) * 2)) {
        log().warn(L"UiProbe: [{}] cannot read the node", label);
        return;
    }
    const auto* const nodeBytes = reinterpret_cast<const std::byte*>(node);
    std::uintptr_t head = 0;
    std::uintptr_t count = 0;
    std::memcpy(&head, nodeBytes, sizeof(head));
    std::memcpy(&count, nodeBytes + sizeof(std::uintptr_t), sizeof(count));
    if (!memory::isReadable(reinterpret_cast<const void*>(head), kNodeValue)) {
        log().warn(L"UiProbe: [{}] cannot read the head of the map", label);
        return;
    }

    std::uintptr_t stack[kMaxNodes]{};
    int top = 0;
    std::memcpy(&stack[top++], reinterpret_cast<const std::byte*>(head) + kNodeParent,
                sizeof(std::uintptr_t));

    int visited = 0;
    while (top > 0 && visited < kMaxNodes) {
        const std::uintptr_t at = stack[--top];
        if (at == 0 || at == head
            || !memory::isReadable(reinterpret_cast<const void*>(at),
                                   static_cast<size_t>(kNodeValue) + 0x20)) {
            continue;
        }
        const auto* const entry = reinterpret_cast<const std::byte*>(at);
        if (entry[kNodeIsNil] != std::byte{0}) {
            continue;
        }
        ++visited;
        const auto* const myval = reinterpret_cast<const std::uintptr_t*>(entry + kNodeValue);
        const std::wstring key = readKey(myval[0]);
        const unsigned tag = static_cast<unsigned>(myval[2] & 0xFF);
        if (tag == kTagString) {
            std::uintptr_t inner = 0;
            if (memory::isReadable(reinterpret_cast<const void*>(myval[1]), sizeof(inner))) {
                std::memcpy(&inner, reinterpret_cast<const void*>(myval[1]), sizeof(inner));
            }
            const std::wstring viaPtr = readKey(inner);
        } else {
            if (tag == kTagArray) {
                dumpVector(reinterpret_cast<const void*>(myval[1]));
            }
        }
        if (top + 2 <= kMaxNodes) {
            std::memcpy(&stack[top++], entry + kNodeLeft, sizeof(std::uintptr_t));
            std::memcpy(&stack[top++], entry + kNodeRight, sizeof(std::uintptr_t));
        }
    }
    if (visited != static_cast<int>(count)) {
        log().warn(L"UiProbe: [{}] walked {} nodes (the map holds {})", label, visited, count);
    }
}

bool retargetString(std::vector<Entry>& entries, const wchar_t* key, const char* text,
                    unsigned char (&record)[16])
{
    for (Entry& item : entries) {
        if (item.keyText != key) {
            continue;
        }
        if (!memory::isReadable(reinterpret_cast<const void*>(item.value), sizeof(record))) {
            log().warn(L"UiProbe: cannot read the value record of {}", key);
            return false;
        }
        std::memcpy(record, reinterpret_cast<const void*>(item.value), sizeof(record));
        std::memcpy(record, &text, sizeof(text));
        item.value = reinterpret_cast<std::uintptr_t>(record);
        return true;
    }
    log().warn(L"UiProbe: the donor has no {}", key);
    return false;
}

bool buildStringPairArray(std::uintptr_t vecSrc, std::uintptr_t elemSrc, std::uintptr_t strSrc,
                          const char* first, const char* second, unsigned char (&recs)[2][16],
                          std::uintptr_t (&elems)[2][4], std::uintptr_t (&ptrs)[2],
                          std::uintptr_t (&vec)[4])
{
    if (!memory::isReadable(reinterpret_cast<const void*>(vecSrc), sizeof(vec))
        || !memory::isReadable(reinterpret_cast<const void*>(elemSrc), sizeof(elems[0]))
        || !memory::isReadable(reinterpret_cast<const void*>(strSrc), sizeof(recs[0]))) {
        log().warn(L"UiProbe: cannot read the array donor");
        return false;
    }
    const char* const parts[2] = {first, second};
    for (size_t e = 0; e < 2; ++e) {
        std::memcpy(recs[e], reinterpret_cast<const void*>(strSrc), sizeof(recs[e]));
        std::memcpy(recs[e], &parts[e], sizeof(parts[e]));

        std::memcpy(elems[e], reinterpret_cast<const void*>(elemSrc), sizeof(elems[e]));
        elems[e][0] = reinterpret_cast<std::uintptr_t>(recs[e]);
        elems[e][1] = kTagString;
        ptrs[e] = reinterpret_cast<std::uintptr_t>(elems[e]);
    }
    std::memcpy(vec, reinterpret_cast<const void*>(vecSrc), sizeof(vec));
    vec[0] = reinterpret_cast<std::uintptr_t>(ptrs);
    vec[1] = reinterpret_cast<std::uintptr_t>(ptrs + 2);
    vec[2] = vec[1];
    return true;
}

constexpr size_t kArenaNodes = 8192;
constexpr size_t kArenaWords = 32768;
constexpr size_t kArenaRecords = 8192;
constexpr size_t kArenaText = 98304;
uitree::TreeNode g_arenaNodes[kArenaNodes]{};
std::uintptr_t g_arenaWords[kArenaWords]{};
alignas(8) unsigned char g_arenaRecords[kArenaRecords][uitree::kRecordBytes]{};
char g_arenaText[kArenaText]{};
uitree::Arena g_pageArena(g_arenaNodes, kArenaNodes, g_arenaWords, kArenaWords,
                          &g_arenaRecords[0][0], kArenaRecords, g_arenaText, kArenaText);

struct PageDonors {
    std::uintptr_t arrTag = 0;
    std::uintptr_t arrSeq = 0;
    std::uintptr_t strTag = 0;
    std::uintptr_t strSeq = 0;
    std::uintptr_t objTag = uitree::kTagObject;
    std::uintptr_t elemVal[uitree::kValueWords]{};
    std::uintptr_t vecSrc[uitree::kVectorWords]{};
    unsigned char strRec[uitree::kRecordBytes]{};
    std::uintptr_t intTag = 0;
    std::uintptr_t intSeq = 0;
    bool ok = false;
};

bool collectPageDonors(void* self, PageDonors& out)
{
    out = PageDonors{};
    static const std::string kBaseSpace = "settings_common";
    static const std::string kBaseName = "dialog_content_fullscreen";
    void* base = nullptr;
    if (!lookupGuarded(self, &kBaseSpace, &kBaseName, base) || base == nullptr) {
        log().warn(L"UiProbe: could not resolve the template (dialog_content_fullscreen)");
        return false;
    }
    std::vector<Entry> baseEntries;
    std::uintptr_t baseCount = 0;
    if (!collectEntries(base, baseEntries, baseCount) || baseEntries.empty()) {
        log().warn(L"UiProbe: could not walk the template");
        return false;
    }
    const Entry* controls = nullptr;
    for (const Entry& one : baseEntries) {
        if (one.keyText == L"controls") {
            controls = &one;
            break;
        }
    }
    if (controls == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(controls->value),
                               sizeof(out.vecSrc))) {
        log().warn(L"UiProbe: the template has no readable controls");
        return false;
    }
    std::memcpy(out.vecSrc, reinterpret_cast<const void*>(controls->value), sizeof(out.vecSrc));
    out.arrTag = controls->tag;
    out.arrSeq = controls->seq;
    if (out.vecSrc[1] <= out.vecSrc[0]
        || !memory::isReadable(reinterpret_cast<const void*>(out.vecSrc[0]),
                               sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the template's controls are empty");
        return false;
    }
    std::uintptr_t firstElem = 0;
    std::memcpy(&firstElem, reinterpret_cast<const void*>(out.vecSrc[0]), sizeof(firstElem));
    if (!memory::isReadable(reinterpret_cast<const void*>(firstElem), sizeof(out.elemVal))) {
        log().warn(L"UiProbe: the template's elements could not be read");
        return false;
    }
    std::memcpy(out.elemVal, reinterpret_cast<const void*>(firstElem), sizeof(out.elemVal));
    {
        std::vector<Entry> sample;
        std::uintptr_t sampleCount = 0;
        if (collectEntries(reinterpret_cast<const void*>(firstElem), sample, sampleCount)
            && !sample.empty()) {
            out.objTag = sample[0].tag;
        }
    }
    static const std::string kBtnSpace = "common_buttons";
    static const std::string kBtnName = "light_text_button";
    void* btn = nullptr;
    if (!lookupGuarded(self, &kBtnSpace, &kBtnName, btn) || btn == nullptr) {
        log().warn(L"UiProbe: could not resolve the string template (light_text_button)");
        return false;
    }
    std::vector<Entry> btnEntries;
    std::uintptr_t btnCount = 0;
    if (!collectEntries(btn, btnEntries, btnCount) || btnEntries.empty()) {
        log().warn(L"UiProbe: could not walk the string template");
        return false;
    }
    for (const Entry& one : btnEntries) {
        if ((one.tag & 0xFF) == kTagString
            && memory::isReadable(reinterpret_cast<const void*>(one.value), sizeof(out.strRec))) {
            std::memcpy(out.strRec, reinterpret_cast<const void*>(one.value), sizeof(out.strRec));
            out.strTag = one.tag;
            out.strSeq = one.seq;
            break;
        }
    }
    if (out.strTag == 0) {
        log().warn(L"UiProbe: could not borrow a string value record");
        return false;
    }
    {
        static const std::string kIntSpace = "common";
        static const std::string kIntName = "toggle";
        void* tg = nullptr;
        std::vector<Entry> tgEntries;
        std::uintptr_t tgCount = 0;
        if (lookupGuarded(self, &kIntSpace, &kIntName, tg) && tg != nullptr
            && collectEntries(tg, tgEntries, tgCount)) {
            for (const Entry& one : tgEntries) {
                if (one.keyText == L"layer" && (one.tag & 0xFF) == 1) {
                    out.intTag = one.tag;
                    out.intSeq = one.seq;
                    break;
                }
            }
        }
        static std::atomic<bool> told{false};
    }
    out.ok = true;
    return true;
}

PageDonors g_pgDonors;

struct Over {
    enum class Kind { Text, Pair, Raw, Int, Bag };
    const char* key = nullptr;
    Kind kind = Kind::Text;
    std::string text;
    std::string second;
    std::uintptr_t value = 0;
    std::uintptr_t tag = 0;
    std::uintptr_t seq = 0;
    std::int64_t number = 0;
    std::vector<Over> items;
};

Over overText(const char* key, std::string text)
{
    Over over;
    over.key = key;
    over.kind = Over::Kind::Text;
    over.text = std::move(text);
    return over;
}

Over overPair(const char* key, std::string first, std::string second)
{
    Over over;
    over.key = key;
    over.kind = Over::Kind::Pair;
    over.text = std::move(first);
    over.second = std::move(second);
    return over;
}

Over overRaw(const char* key, const Entry& donor)
{
    Over over;
    over.key = key;
    over.kind = Over::Kind::Raw;
    over.value = donor.value;
    over.tag = donor.tag;
    over.seq = donor.seq;
    return over;
}

Over overInt(const char* key, std::int64_t number)
{
    Over over;
    over.key = key;
    over.kind = Over::Kind::Int;
    over.number = number;
    return over;
}

Over overBag(const char* key, std::vector<Over> items)
{
    Over over;
    over.key = key;
    over.kind = Over::Kind::Bag;
    over.items = std::move(items);
    return over;
}

struct Part {
    std::string name;
    std::string def;
    std::vector<Over> overs;
    std::vector<Part> kids;
};

bool fillOverride(uitree::Arena& arena, const PageDonors& donors, const Over& over,
                  uitree::KeyValue& out)
{
    out = uitree::KeyValue{};
    out.key = over.key;
    switch (over.kind) {
    case Over::Kind::Raw:
        out.value = over.value;
        out.tag = over.tag;
        out.seq = over.seq;
        return true;
    case Over::Kind::Text: {
        const char* const body = arena.takeText(over.text.c_str());
        const unsigned char* const record = arena.makeStringRecord(donors.strRec, body);
        if (record == nullptr) {
            return false;
        }
        out.value = reinterpret_cast<std::uintptr_t>(record);
        out.tag = donors.strTag;
        out.seq = donors.strSeq;
        return true;
    }
    case Over::Kind::Pair: {
        const unsigned char* const firstRec =
            arena.makeStringRecord(donors.strRec, arena.takeText(over.text.c_str()));
        const unsigned char* const secondRec =
            arena.makeStringRecord(donors.strRec, arena.takeText(over.second.c_str()));
        if (firstRec == nullptr || secondRec == nullptr) {
            return false;
        }
        const std::uintptr_t* const firstElem =
            arena.makeStringElement(donors.elemVal, firstRec);
        const std::uintptr_t* const secondElem =
            arena.makeStringElement(donors.elemVal, secondRec);
        if (firstElem == nullptr || secondElem == nullptr) {
            return false;
        }
        const std::uintptr_t* const pair[2] = {firstElem, secondElem};
        const std::uintptr_t* const vec = arena.makeVector(donors.vecSrc, pair, 2);
        if (vec == nullptr) {
            return false;
        }
        out.value = reinterpret_cast<std::uintptr_t>(vec);
        out.tag = donors.arrTag;
        out.seq = donors.arrSeq;
        return true;
    }
    case Over::Kind::Int: {
        if (donors.intTag == 0) {
            return false;
        }
        out.value = static_cast<std::uintptr_t>(over.number);
        out.tag = donors.intTag;
        out.seq = donors.intSeq;
        return true;
    }
    case Over::Kind::Bag: {
        std::vector<uitree::KeyValue> inner;
        inner.reserve(over.items.size());
        for (const Over& one : over.items) {
            uitree::KeyValue kv{};
            if (!fillOverride(arena, donors, one, kv)) {
                return false;
            }
            inner.push_back(kv);
        }
        const std::uintptr_t* const node =
            arena.makeMap(inner.empty() ? nullptr : inner.data(), inner.size());
        if (node == nullptr) {
            return false;
        }
        out.value = reinterpret_cast<std::uintptr_t>(node);
        out.tag = donors.objTag;
        out.seq = 0;
        return true;
    }
    }
    return false;
}

bool fillChildren(uitree::Arena& arena, const PageDonors& donors, const std::vector<Part>& kids,
                  uitree::KeyValue& out);

std::uintptr_t* buildPartElement(uitree::Arena& arena, const PageDonors& donors, const Part& part)
{
    if (!donors.ok) {
        return nullptr;
    }
    std::vector<uitree::KeyValue> items;
    items.reserve(part.overs.size() + 1);

    if (!part.kids.empty()) {
        uitree::KeyValue kv{};
        if (!fillChildren(arena, donors, part.kids, kv)) {
            return nullptr;
        }
        items.push_back(kv);
    }
    for (const Over& over : part.overs) {
        uitree::KeyValue kv{};
        if (!fillOverride(arena, donors, over, kv)) {
            return nullptr;
        }
        items.push_back(kv);
    }

    const std::uintptr_t* const overNode =
        arena.makeMap(items.empty() ? nullptr : items.data(), items.size());
    if (overNode == nullptr) {
        return nullptr;
    }
    std::string keyText = part.name;
    if (!part.def.empty()) {
        keyText += '@';
        keyText += part.def;
    }
    const char* const key = arena.takeText(keyText.c_str());
    if (key == nullptr) {
        return nullptr;
    }
    uitree::KeyValue one{};
    one.key = key;
    one.value = reinterpret_cast<std::uintptr_t>(overNode);
    one.tag = donors.objTag;
    one.seq = 0;
    const std::uintptr_t* const elemNode = arena.makeMap(&one, 1);
    if (elemNode == nullptr) {
        return nullptr;
    }
    return arena.makeValue(donors.elemVal, elemNode);
}

bool fillChildren(uitree::Arena& arena, const PageDonors& donors, const std::vector<Part>& kids,
                  uitree::KeyValue& out)
{
    std::vector<const std::uintptr_t*> built;
    built.reserve(kids.size());
    for (const Part& kid : kids) {
        const std::uintptr_t* const one = buildPartElement(arena, donors, kid);
        if (one == nullptr) {
            return false;
        }
        built.push_back(one);
    }
    const std::uintptr_t* const vec = arena.makeVector(donors.vecSrc, built.data(), built.size());
    if (vec == nullptr) {
        return false;
    }
    out = uitree::KeyValue{};
    out.key = "controls";
    out.value = reinterpret_cast<std::uintptr_t>(vec);
    out.tag = donors.arrTag;
    out.seq = donors.arrSeq;
    return true;
}

std::uintptr_t* buildPartDefinition(void* self, uitree::Arena& arena, const PageDonors& donors,
                                    const char* space, const char* name,
                                    const std::vector<Over>& overs, const std::vector<Part>& kids)
{
    if (!donors.ok) {
        return nullptr;
    }
    const std::string spaceText = space;
    const std::string nameText = name;
    void* donor = nullptr;
    if (!lookupGuarded(self, &spaceText, &nameText, donor) || donor == nullptr) {
        return nullptr;
    }
    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(donor, entries, count) || entries.empty()) {
        return nullptr;
    }
    std::uintptr_t donorWords[uitree::kValueWords]{};
    if (!memory::isReadable(donor, sizeof(donorWords))) {
        return nullptr;
    }
    std::memcpy(donorWords, donor, sizeof(donorWords));

    std::vector<uitree::KeyValue> items;
    items.reserve(entries.size() + overs.size() + 1);
    for (const Entry& one : entries) {
        uitree::KeyValue kv{};
        kv.key = reinterpret_cast<const char*>(one.key);
        kv.value = one.value;
        kv.tag = one.tag;
        kv.seq = one.seq;
        items.push_back(kv);
    }
    if (!kids.empty()) {
        uitree::KeyValue kv{};
        if (!fillChildren(arena, donors, kids, kv)) {
            return nullptr;
        }
        items.push_back(kv);
    }
    for (const Over& over : overs) {
        uitree::KeyValue kv{};
        if (!fillOverride(arena, donors, over, kv)) {
            return nullptr;
        }
        items.push_back(kv);
    }

    const std::uintptr_t* const node = arena.makeMap(items.data(), items.size());
    if (node == nullptr) {
        return nullptr;
    }
    return arena.makeValue(donorWords, node);
}

void* buildRelabeled(void* self, size_t idx)
{
    buildMenuTexts();
    if (idx >= g_menuCount) {
        return nullptr;
    }
    if (g_ownReady[idx]) {
        return g_ownValues[idx];
    }

    static const std::string kSpace = kFromSpace;
    static const std::string kFrom = kFromName;
    void* vanilla = nullptr;
    if (!lookupGuarded(self, &kSpace, &kFrom, vanilla) || vanilla == nullptr) {
        log().warn(L"UiProbe: could not look up the donor node");
        return nullptr;
    }

    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(vanilla, entries, count)) {
        log().warn(L"UiProbe: node only walked {} of {}", entries.size(), count);
        return nullptr;
    }
    if (entries.empty() || entries.size() > static_cast<size_t>(kMaxOwnKeys)) {
        log().warn(L"UiProbe: key count out of range ({})", entries.size());
        return nullptr;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    if (!retargetString(entries, L"$button_text", g_menuTextBuf[idx], g_ownTextRecs[idx])) {
        return nullptr;
    }
    if (!retargetString(entries, L"$pressed_button_name", ownButtonIdText(), g_ownButtonIdRec)) {
        return nullptr;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        g_ownNodes[idx][i].key = entries[i].key;
        g_ownNodes[idx][i].value = entries[i].value;
        g_ownNodes[idx][i].tag = entries[i].tag;
        g_ownNodes[idx][i].seq = entries[i].seq;
    }
    finishMap(&g_ownHeads[idx], g_ownNodes[idx], static_cast<int>(entries.size()),
              g_ownNodePtrs[idx]);

    std::memcpy(g_ownValues[idx], vanilla, sizeof(g_ownValues[idx]));
    g_ownValues[idx][0] = reinterpret_cast<std::uintptr_t>(g_ownNodePtrs[idx]);

    g_ownReady[idx] = true;
    const std::string text = g_menuTextBuf[idx];
    return g_ownValues[idx];
}

TreeNode g_keyBtnHead{};
TreeNode g_keyBtnNodes[kMaxOwnKeys]{};
std::uintptr_t g_keyBtnNodePtrs[4]{};
std::uintptr_t g_keyBtnValue[4]{};
bool g_keyBtnReady = false;
alignas(8) unsigned char g_keyBtnSizeRecs[2][16]{};
std::uintptr_t g_keyBtnSizeElems[2][4]{};
std::uintptr_t g_keyBtnSizeElemPtrs[2]{};
std::uintptr_t g_keyBtnSizeVec[4]{};

void* buildSizedKeyButton(void* self)
{
    if (g_keyBtnReady) {
        return g_keyBtnValue;
    }

    static const std::string kSpace = "common_buttons";
    static const std::string kName = "light_text_form_fitting_button";
    void* vanilla = nullptr;
    if (!lookupGuarded(self, &kSpace, &kName, vanilla) || vanilla == nullptr) {
        log().warn(L"UiProbe: could not look up the key-button donor");
        return nullptr;
    }

    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(vanilla, entries, count)) {
        log().warn(L"UiProbe: key-button node only walked {} of {}", entries.size(),
                   count);
        return nullptr;
    }
    if (entries.empty() || entries.size() > static_cast<size_t>(kMaxOwnKeys)) {
        log().warn(L"UiProbe: key-button key count out of range ({})", entries.size());
        return nullptr;
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    Entry* sizeEntry = nullptr;
    for (Entry& one : entries) {
        if (one.keyText == L"size" && (one.tag & 0xFF) == kTagArray) {
            sizeEntry = &one;
            break;
        }
    }
    if (sizeEntry == nullptr) {
        log().warn(L"UiProbe: the key button has no array-shaped size");
        return nullptr;
    }

    std::uintptr_t begin = 0;
    std::uintptr_t elemSrc = 0;
    std::uintptr_t strSrc = 0;
    if (!memory::isReadable(reinterpret_cast<const void*>(sizeEntry->value), sizeof(begin))) {
        log().warn(L"UiProbe: cannot read the size array");
        return nullptr;
    }
    std::memcpy(&begin, reinterpret_cast<const void*>(sizeEntry->value), sizeof(begin));
    if (!memory::isReadable(reinterpret_cast<const void*>(begin), sizeof(elemSrc))) {
        log().warn(L"UiProbe: cannot read the size element list");
        return nullptr;
    }
    std::memcpy(&elemSrc, reinterpret_cast<const void*>(begin), sizeof(elemSrc));
    if (!memory::isReadable(reinterpret_cast<const void*>(elemSrc), sizeof(strSrc))) {
        log().warn(L"UiProbe: cannot read a size element");
        return nullptr;
    }
    std::memcpy(&strSrc, reinterpret_cast<const void*>(elemSrc), sizeof(strSrc));

    if (!buildStringPairArray(sizeEntry->value, elemSrc, strSrc, kKeyBtnW, kKeyBtnH,
                              g_keyBtnSizeRecs, g_keyBtnSizeElems, g_keyBtnSizeElemPtrs,
                              g_keyBtnSizeVec)) {
        return nullptr;
    }
    sizeEntry->value = reinterpret_cast<std::uintptr_t>(g_keyBtnSizeVec);

    for (size_t i = 0; i < entries.size(); ++i) {
        g_keyBtnNodes[i].key = entries[i].key;
        g_keyBtnNodes[i].value = entries[i].value;
        g_keyBtnNodes[i].tag = entries[i].tag;
        g_keyBtnNodes[i].seq = entries[i].seq;
    }
    finishMap(&g_keyBtnHead, g_keyBtnNodes, static_cast<int>(entries.size()), g_keyBtnNodePtrs);

    std::memcpy(g_keyBtnValue, vanilla, sizeof(g_keyBtnValue));
    g_keyBtnValue[0] = reinterpret_cast<std::uintptr_t>(g_keyBtnNodePtrs);

    g_keyBtnReady = true;
    return g_keyBtnValue;
}

constexpr char kKeyPanelSpace[] = "controls_section";
constexpr char kKeyPanelName[] = "keymapping_item";
constexpr char kResetBtnSpace[] = "controls_section";
constexpr char kResetBtnName[] = "reset_binding_button";
constexpr char kKeyPartBtnSpace[] = "common_buttons";
constexpr char kKeyPartBtnName[] = "light_text_form_fitting_button";

constexpr char kKeyPanelW[] = "100%";
constexpr char kKeyPanelH[] = "30px";
constexpr char kKeyPartW[] = "83%";
constexpr char kKeyPartH[] = "100%";
constexpr char kResetPartW[] = "17%";
constexpr char kResetPartH[] = "100%";

constexpr char kSizeKey[] = "size";

constexpr int kMaxKeyPanelKeys = 32;
constexpr size_t kKeyPanelParts = 2;

alignas(8) unsigned char g_kpPanelSizeRecs[2][16]{};
std::uintptr_t g_kpPanelSizeElems[2][4]{};
std::uintptr_t g_kpPanelSizeElemPtrs[2]{};
std::uintptr_t g_kpPanelSizeVec[4]{};
alignas(8) unsigned char g_kpPartSizeRecs[kKeyPanelParts][2][16]{};
std::uintptr_t g_kpPartSizeElems[kKeyPanelParts][2][4]{};
std::uintptr_t g_kpPartSizeElemPtrs[kKeyPanelParts][2]{};
std::uintptr_t g_kpPartSizeVec[kKeyPanelParts][4]{};
TreeNode g_kpPartOverHeads[kKeyPanelParts]{};
TreeNode g_kpPartOverNodes[kKeyPanelParts][1]{};
std::uintptr_t g_kpPartOverPtrs[kKeyPanelParts][4]{};
bool g_kpSharedReady = false;

char g_kpElemKeys[kMaxMenuItems][kMaxKeysPerModule][kKeyPanelParts][kSecKeyBytes]{};
TreeNode g_kpElemHeads[kMaxMenuItems][kMaxKeysPerModule][kKeyPanelParts]{};
TreeNode g_kpElemNodes[kMaxMenuItems][kMaxKeysPerModule][kKeyPanelParts][1]{};
std::uintptr_t g_kpElemNodePtrs[kMaxMenuItems][kMaxKeysPerModule][kKeyPanelParts][4]{};
std::uintptr_t g_kpElemValues[kMaxMenuItems][kMaxKeysPerModule][kKeyPanelParts][4]{};
std::uintptr_t g_kpCtlElems[kMaxMenuItems][kMaxKeysPerModule][kKeyPanelParts]{};
std::uintptr_t g_kpCtlVec[kMaxMenuItems][kMaxKeysPerModule][4]{};
TreeNode g_kpHeads[kMaxMenuItems][kMaxKeysPerModule]{};
TreeNode g_kpNodes[kMaxMenuItems][kMaxKeysPerModule][kMaxKeyPanelKeys]{};
std::uintptr_t g_kpNodePtrs[kMaxMenuItems][kMaxKeysPerModule][4]{};
std::uintptr_t g_kpValues[kMaxMenuItems][kMaxKeysPerModule][4]{};
bool g_kpReady[kMaxMenuItems][kMaxKeysPerModule]{};

bool g_kpGaveUp = false;

bool buildKeyPanelShared(std::uintptr_t sizeVecSrc, std::uintptr_t elemSrc, std::uintptr_t strSrc,
                         std::uintptr_t sizeTag, std::uintptr_t sizeSeq)
{
    if (g_kpSharedReady) {
        return true;
    }
    if (!buildStringPairArray(sizeVecSrc, elemSrc, strSrc, kKeyPanelW, kKeyPanelH,
                              g_kpPanelSizeRecs, g_kpPanelSizeElems, g_kpPanelSizeElemPtrs,
                              g_kpPanelSizeVec)) {
        return false;
    }
    const char* const partW[kKeyPanelParts] = {kKeyPartW, kResetPartW};
    const char* const partH[kKeyPanelParts] = {kKeyPartH, kResetPartH};
    for (size_t p = 0; p < kKeyPanelParts; ++p) {
        if (!buildStringPairArray(sizeVecSrc, elemSrc, strSrc, partW[p], partH[p],
                                  g_kpPartSizeRecs[p], g_kpPartSizeElems[p],
                                  g_kpPartSizeElemPtrs[p], g_kpPartSizeVec[p])) {
            return false;
        }
        g_kpPartOverNodes[p][0].key = reinterpret_cast<std::uintptr_t>(kSizeKey);
        g_kpPartOverNodes[p][0].value = reinterpret_cast<std::uintptr_t>(g_kpPartSizeVec[p]);
        g_kpPartOverNodes[p][0].tag = sizeTag;
        g_kpPartOverNodes[p][0].seq = sizeSeq;
        finishMap(&g_kpPartOverHeads[p], g_kpPartOverNodes[p], 1, g_kpPartOverPtrs[p]);
    }
    g_kpSharedReady = true;
    return true;
}

void* buildKeyRowPanel(void* self, size_t item, size_t slot)
{
    if (item >= kMaxMenuItems || slot >= kMaxKeysPerModule || g_kpGaveUp) {
        return nullptr;
    }
    if (g_kpReady[item][slot]) {
        return g_kpValues[item][slot];
    }
    g_kpGaveUp = true;

    static const std::string kPanelSpace = kKeyPanelSpace;
    static const std::string kPanelName = kKeyPanelName;
    void* vanilla = nullptr;
    if (!lookupGuarded(self, &kPanelSpace, &kPanelName, vanilla) || vanilla == nullptr) {
        log().warn(L"UiProbe: could not look up keymapping_item (no reset button)");
        return nullptr;
    }

    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(vanilla, entries, count)) {
        log().warn(L"UiProbe: keymapping_item node only walked {} of {}", entries.size(),
                   count);
        return nullptr;
    }
    if (entries.empty() || entries.size() > static_cast<size_t>(kMaxKeyPanelKeys)) {
        log().warn(L"UiProbe: keymapping_item key count out of range ({})",
                   entries.size());
        return nullptr;
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    Entry* sizeEntry = nullptr;
    Entry* ctlEntry = nullptr;
    for (Entry& one : entries) {
        if (one.keyText == L"size" && (one.tag & 0xFF) == kTagArray) {
            sizeEntry = &one;
        } else if (one.keyText == L"controls" && (one.tag & 0xFF) == kTagArray) {
            ctlEntry = &one;
        }
    }
    if (sizeEntry == nullptr || ctlEntry == nullptr) {
        log().warn(L"UiProbe: keymapping_item is missing size or controls");
        return nullptr;
    }

    std::uintptr_t begin = 0;
    std::uintptr_t elemSrc = 0;
    std::uintptr_t strSrc = 0;
    if (!memory::isReadable(reinterpret_cast<const void*>(sizeEntry->value), sizeof(begin))) {
        log().warn(L"UiProbe: cannot read the keymapping_item size array");
        return nullptr;
    }
    std::memcpy(&begin, reinterpret_cast<const void*>(sizeEntry->value), sizeof(begin));
    if (!memory::isReadable(reinterpret_cast<const void*>(begin), sizeof(elemSrc))) {
        log().warn(L"UiProbe: cannot read the keymapping_item size element list");
        return nullptr;
    }
    std::memcpy(&elemSrc, reinterpret_cast<const void*>(begin), sizeof(elemSrc));
    if (!memory::isReadable(reinterpret_cast<const void*>(elemSrc), sizeof(strSrc))) {
        log().warn(L"UiProbe: cannot read a keymapping_item size element");
        return nullptr;
    }
    std::memcpy(&strSrc, reinterpret_cast<const void*>(elemSrc), sizeof(strSrc));

    std::uintptr_t ctlBegin = 0;
    std::uintptr_t srcCtlElem = 0;
    if (!memory::isReadable(reinterpret_cast<const void*>(ctlEntry->value), sizeof(ctlBegin))) {
        log().warn(L"UiProbe: cannot read keymapping_item controls");
        return nullptr;
    }
    std::memcpy(&ctlBegin, reinterpret_cast<const void*>(ctlEntry->value), sizeof(ctlBegin));
    if (!memory::isReadable(reinterpret_cast<const void*>(ctlBegin), sizeof(srcCtlElem))) {
        log().warn(L"UiProbe: cannot read the keymapping_item controls list");
        return nullptr;
    }
    std::memcpy(&srcCtlElem, reinterpret_cast<const void*>(ctlBegin), sizeof(srcCtlElem));
    if (!memory::isReadable(reinterpret_cast<const void*>(srcCtlElem), sizeof(std::uintptr_t) * 4)) {
        log().warn(L"UiProbe: cannot read a keymapping_item controls element");
        return nullptr;
    }
    std::vector<Entry> ctlOne;
    std::uintptr_t ctlOneCount = 0;
    if (!collectEntries(reinterpret_cast<const void*>(srcCtlElem), ctlOne, ctlOneCount)
        || ctlOne.size() != 1) {
        log().warn(L"UiProbe: a keymapping_item child is not a single-key node ({} keys)",
                   ctlOne.size());
        return nullptr;
    }
    const std::uintptr_t childTag = ctlOne[0].tag;
    const std::uintptr_t childSeq = ctlOne[0].seq;

    if (!buildKeyPanelShared(sizeEntry->value, elemSrc, strSrc, sizeEntry->tag, sizeEntry->seq)) {
        return nullptr;
    }

    {
        static const std::string kRstSpace = kResetBtnSpace;
        static const std::string kRstName = kResetBtnName;
        void* reset = nullptr;
        if (!lookupGuarded(self, &kRstSpace, &kRstName, reset) || reset == nullptr) {
            log().warn(L"UiProbe: could not look up reset_binding_button (no reset button)");
            return nullptr;
        }
    }

    const char* const partDefs[kKeyPanelParts] = {kKeyPartBtnName, kResetBtnName};
    const char* const partSpaces[kKeyPanelParts] = {kKeyPartBtnSpace, kResetBtnSpace};
    const char partTails[kKeyPanelParts] = {'b', 'r'};
    for (size_t p = 0; p < kKeyPanelParts; ++p) {
        std::snprintf(g_kpElemKeys[item][slot][p], kSecKeyBytes, "tk%02u_k%u_%c@%s.%s",
                      static_cast<unsigned>(item), static_cast<unsigned>(slot), partTails[p],
                      partSpaces[p], partDefs[p]);
        g_kpElemNodes[item][slot][p][0].key =
            reinterpret_cast<std::uintptr_t>(g_kpElemKeys[item][slot][p]);
        g_kpElemNodes[item][slot][p][0].value =
            reinterpret_cast<std::uintptr_t>(g_kpPartOverPtrs[p]);
        g_kpElemNodes[item][slot][p][0].tag = childTag;
        g_kpElemNodes[item][slot][p][0].seq = childSeq;
        finishMap(&g_kpElemHeads[item][slot][p], g_kpElemNodes[item][slot][p], 1,
                  g_kpElemNodePtrs[item][slot][p]);
        std::memcpy(g_kpElemValues[item][slot][p], reinterpret_cast<const void*>(srcCtlElem),
                    sizeof(g_kpElemValues[item][slot][p]));
        g_kpElemValues[item][slot][p][0] =
            reinterpret_cast<std::uintptr_t>(g_kpElemNodePtrs[item][slot][p]);
        g_kpCtlElems[item][slot][p] =
            reinterpret_cast<std::uintptr_t>(g_kpElemValues[item][slot][p]);
    }
    std::memcpy(g_kpCtlVec[item][slot], reinterpret_cast<const void*>(ctlEntry->value),
                sizeof(g_kpCtlVec[item][slot]));
    g_kpCtlVec[item][slot][0] = reinterpret_cast<std::uintptr_t>(&g_kpCtlElems[item][slot][0]);
    g_kpCtlVec[item][slot][1] =
        reinterpret_cast<std::uintptr_t>(&g_kpCtlElems[item][slot][kKeyPanelParts]);
    g_kpCtlVec[item][slot][2] = g_kpCtlVec[item][slot][1];

    sizeEntry->value = reinterpret_cast<std::uintptr_t>(g_kpPanelSizeVec);
    ctlEntry->value = reinterpret_cast<std::uintptr_t>(g_kpCtlVec[item][slot]);

    for (size_t i = 0; i < entries.size(); ++i) {
        g_kpNodes[item][slot][i].key = entries[i].key;
        g_kpNodes[item][slot][i].value = entries[i].value;
        g_kpNodes[item][slot][i].tag = entries[i].tag;
        g_kpNodes[item][slot][i].seq = entries[i].seq;
    }
    finishMap(&g_kpHeads[item][slot], g_kpNodes[item][slot], static_cast<int>(entries.size()),
              g_kpNodePtrs[item][slot]);

    std::memcpy(g_kpValues[item][slot], vanilla, sizeof(g_kpValues[item][slot]));
    g_kpValues[item][slot][0] = reinterpret_cast<std::uintptr_t>(g_kpNodePtrs[item][slot]);

    g_kpReady[item][slot] = true;
    g_kpGaveUp = false;
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 6) {
        const std::string key = g_kpElemKeys[item][slot][0];
    }
    return g_kpValues[item][slot];
}

void* buildWrapper(void* self)
{
    if (g_wrapReady) {
        return g_wrapValue;
    }

    static const std::string kPauseSpace = kFromSpace;
    static const std::string kBtn = kFromName;
    static const std::string kStackSpace = "settings";
    static const std::string kStackName = "selector_stack_panel";

    void* vanillaBtn = nullptr;
    if (!lookupGuarded(self, &kPauseSpace, &kBtn, vanillaBtn) || vanillaBtn == nullptr
        || !memory::isReadable(vanillaBtn, sizeof(g_wrapValue))) {
        log().warn(L"UiProbe: could not look up the button to wrap");
        return nullptr;
    }
    void* stack = nullptr;
    if (!lookupGuarded(self, &kStackSpace, &kStackName, stack) || stack == nullptr) {
        log().warn(L"UiProbe: could not look up the stack_panel donor");
        return nullptr;
    }

    std::vector<Entry> borrowed;
    std::uintptr_t count = 0;
    if (!collectEntries(stack, borrowed, count)) {
        log().warn(L"UiProbe: donor node not fully walked");
        return nullptr;
    }
    const Entry* type = nullptr;
    const Entry* orientation = nullptr;
    const Entry* controls = nullptr;
    for (const Entry& item : borrowed) {
        if (item.keyText == L"type") {
            type = &item;
        } else if (item.keyText == L"orientation") {
            orientation = &item;
        } else if (item.keyText == L"controls") {
            controls = &item;
        }
    }
    if (type == nullptr || orientation == nullptr || controls == nullptr) {
        log().warn(L"UiProbe: the donor is missing keys we need");
        return nullptr;
    }
    if (!memory::isReadable(reinterpret_cast<const void*>(controls->value), sizeof(g_vec))) {
        log().warn(L"UiProbe: cannot read the donor array");
        return nullptr;
    }

    std::uintptr_t borrowedVec[4]{};
    std::memcpy(borrowedVec, reinterpret_cast<const void*>(controls->value), sizeof(borrowedVec));
    std::uintptr_t objectTag = kTagObject;
    if (borrowedVec[1] > borrowedVec[0]
        && memory::isReadable(reinterpret_cast<const void*>(borrowedVec[0]),
                              sizeof(std::uintptr_t))) {
        std::uintptr_t first = 0;
        std::memcpy(&first, reinterpret_cast<const void*>(borrowedVec[0]), sizeof(first));
        dumpNode(L"vanilla controls[0]", reinterpret_cast<const void*>(first));

        std::vector<Entry> sample;
        std::uintptr_t sampleCount = 0;
        if (collectEntries(reinterpret_cast<const void*>(first), sample, sampleCount)) {
            for (const Entry& item : sample) {
                if ((item.tag & 0xFF) == kTagObject) {
                    objectTag = item.tag;
                    break;
                }
            }
        }
    }

    if (buildRelabeled(self, 0) == nullptr) {
        log().warn(L"UiProbe: could not build child B (the label-substituted copy)");
        return nullptr;
    }

    const auto* const btnWords = static_cast<const std::uintptr_t*>(vanillaBtn);
    g_childNodesA[0].key = reinterpret_cast<std::uintptr_t>(kChildKeyA);
    g_childNodesA[0].value = btnWords[0];
    g_childNodesA[0].tag = objectTag;
    g_childNodesA[0].seq = 0;
    finishMap(&g_childHeadA, g_childNodesA, 1, g_childNodeA);
    std::memcpy(g_childValueA, vanillaBtn, sizeof(g_childValueA));
    g_childValueA[0] = reinterpret_cast<std::uintptr_t>(g_childNodeA);

    g_childNodesB[0].key = reinterpret_cast<std::uintptr_t>(kChildKeyB);
    g_childNodesB[0].value = reinterpret_cast<std::uintptr_t>(g_ownNodePtrs[0]);
    g_childNodesB[0].tag = objectTag;
    g_childNodesB[0].seq = 0;
    finishMap(&g_childHeadB, g_childNodesB, 1, g_childNodeB);
    std::memcpy(g_childValueB, vanillaBtn, sizeof(g_childValueB));
    g_childValueB[0] = reinterpret_cast<std::uintptr_t>(g_childNodeB);

    g_elems[0] = reinterpret_cast<std::uintptr_t>(g_childValueA);
    g_elems[1] = reinterpret_cast<std::uintptr_t>(g_childValueB);
    std::memcpy(g_vec, reinterpret_cast<const void*>(controls->value), sizeof(g_vec));
    g_vec[0] = reinterpret_cast<std::uintptr_t>(&g_elems[0]);
    g_vec[1] = reinterpret_cast<std::uintptr_t>(&g_elems[2]);
    g_vec[2] = reinterpret_cast<std::uintptr_t>(&g_elems[2]);

    g_wrapNodes[0].key = controls->key;
    g_wrapNodes[0].value = reinterpret_cast<std::uintptr_t>(g_vec);
    g_wrapNodes[0].tag = controls->tag;
    g_wrapNodes[0].seq = controls->seq;
    g_wrapNodes[1].key = orientation->key;
    g_wrapNodes[1].value = orientation->value;
    g_wrapNodes[1].tag = orientation->tag;
    g_wrapNodes[1].seq = orientation->seq;
    g_wrapNodes[2].key = type->key;
    g_wrapNodes[2].value = type->value;
    g_wrapNodes[2].tag = type->tag;
    g_wrapNodes[2].seq = type->seq;
    finishMap(&g_wrapHead, g_wrapNodes, 3, g_wrapNode);

    std::memcpy(g_wrapValue, vanillaBtn, sizeof(g_wrapValue));
    g_wrapValue[0] = reinterpret_cast<std::uintptr_t>(g_wrapNode);

    g_wrapReady = true;
    armWatchNow();
    return g_wrapValue;
}

constexpr bool kNavItemFromVanilla = false;

constexpr bool kNavItemFromToggle = false;

constexpr bool kNavItemFirst = true;

void* buildNavExtra(void* self)
{
    if (g_navReady) {
        return g_navValue;
    }

    static const std::string kSpace = "how_to_play";
    static const std::string kNav = "how_to_play_selector_stack_panel";
    static const std::string kItem = "moving_around_button";

    void* nav = nullptr;
    if (!lookupGuarded(self, &kSpace, &kNav, nav) || nav == nullptr
        || !memory::isReadable(nav, sizeof(g_navValue))) {
        log().warn(L"UiProbe: could not look up the left-nav original");
        return nullptr;
    }
    static const std::string kToggleSpace = "settings_common";
    static const std::string kToggleName = "option_toggle";
    const std::string* const itemSpace = kNavItemFromToggle ? &kToggleSpace : &kSpace;
    const std::string* const itemName = kNavItemFromToggle ? &kToggleName : &kItem;

    void* item = nullptr;
    if (!lookupGuarded(self, itemSpace, itemName, item) || item == nullptr
        || !memory::isReadable(item, sizeof(g_navItemValues[0]))) {
        log().warn(L"UiProbe: could not look up the body of the item to add ({}/{})",
                   std::wstring(itemSpace->begin(), itemSpace->end()),
                   std::wstring(itemName->begin(), itemName->end()));
        return nullptr;
    }

    constexpr bool kNavProbeVanillaMix = false;
    static const std::string kMixNames[] = {
        "moving_around_button",
        "gathering_resources_button",
        "selecting_items_button",
        "placing_blocks_button",
    };
    constexpr size_t kMixCount = std::size(kMixNames);
    void* mixItems[kMixCount]{};
    if (kNavProbeVanillaMix) {
        for (size_t i = 0; i < kMixCount; ++i) {
            if (!lookupGuarded(self, &kSpace, &kMixNames[i], mixItems[i]) || mixItems[i] == nullptr
                || !memory::isReadable(mixItems[i], sizeof(g_navItemValues[0]))) {
                log().warn(L"UiProbe: could not look up the probe item {}", i);
                return nullptr;
            }
        }
    }

    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(nav, entries, count)) {
        log().warn(L"UiProbe: left-nav node only walked {} of {}",
                   entries.size(), count);
        return nullptr;
    }
    if (entries.empty() || entries.size() > static_cast<size_t>(kMaxNavKeys)) {
        log().warn(L"UiProbe: left-nav key count out of range ({})", entries.size());
        return nullptr;
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    const Entry* controls = nullptr;
    for (const Entry& one : entries) {
        if (one.keyText == L"controls") {
            controls = &one;
            break;
        }
    }
    if (controls == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(controls->value), sizeof(g_navVec))) {
        log().warn(L"UiProbe: cannot read the left-nav controls");
        return nullptr;
    }

    std::uintptr_t vanillaVec[4]{};
    std::memcpy(vanillaVec, reinterpret_cast<const void*>(controls->value), sizeof(vanillaVec));
    if (vanillaVec[1] <= vanillaVec[0]
        || !memory::isReadable(reinterpret_cast<const void*>(vanillaVec[0]),
                               sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the left-nav controls are empty or unreadable");
        return nullptr;
    }
    std::uintptr_t firstElem = 0;
    std::memcpy(&firstElem, reinterpret_cast<const void*>(vanillaVec[0]), sizeof(firstElem));

    std::uintptr_t objectTag = kTagObject;
    std::vector<Entry> sample;
    std::uintptr_t sampleCount = 0;
    if (collectEntries(reinterpret_cast<const void*>(firstElem), sample, sampleCount)) {
        for (const Entry& one : sample) {
            if ((one.tag & 0xFF) == kTagObject) {
                objectTag = one.tag;
                break;
            }
        }
    }

    const bool useOwn = !kNavItemFromVanilla && !kNavItemFromToggle;

    if (kNavPaneOwnItems) {
        buildMenuTexts();

        std::vector<Entry> paneEntries;
        std::uintptr_t paneCount = 0;
        if (!collectEntries(reinterpret_cast<const void*>(firstElem), paneEntries, paneCount)
            || paneEntries.empty()
            || paneEntries.size() > static_cast<size_t>(kMaxNavKeys)) {
            log().warn(L"UiProbe: could not walk the pane node ({} keys)", paneEntries.size());
            return nullptr;
        }
        if (paneEntries.size() != 1) {
            log().warn(L"UiProbe: the pane wrapper is not a single-key node ({} keys)",
                       paneEntries.size());
            return nullptr;
        }
        const std::uintptr_t wrapKey = paneEntries[0].key;
        const std::uintptr_t wrapTag = paneEntries[0].tag;
        std::uintptr_t paneBox[4]{};
        paneBox[0] = paneEntries[0].value;

        std::vector<Entry> bodyEntries;
        std::uintptr_t bodyCount = 0;
        if (!collectEntries(paneBox, bodyEntries, bodyCount) || bodyEntries.empty()
            || bodyEntries.size() > static_cast<size_t>(kMaxNavKeys)) {
            log().warn(L"UiProbe: could not walk the pane body ({} keys)", bodyEntries.size());
            return nullptr;
        }
        std::sort(bodyEntries.begin(), bodyEntries.end(),
                  [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

        const Entry* paneControls = nullptr;
        for (const Entry& one : bodyEntries) {
            if (one.keyText == L"controls") {
                paneControls = &one;
                break;
            }
        }
        if (paneControls == nullptr
            || !memory::isReadable(reinterpret_cast<const void*>(paneControls->value),
                                   sizeof(g_paneVec))) {
            log().warn(L"UiProbe: cannot read the pane body controls ({} keys)",
                       bodyEntries.size());
            return nullptr;
        }

        std::uintptr_t srcVec[4]{};
        std::memcpy(srcVec, reinterpret_cast<const void*>(paneControls->value), sizeof(srcVec));
        const size_t srcCount =
            (srcVec[1] > srcVec[0]) ? (srcVec[1] - srcVec[0]) / sizeof(std::uintptr_t) : 0;
        const size_t ownCount = (kNavPaneOwnCount != 0) ? kNavPaneOwnCount : g_menuCount;
        if (ownCount == 0 || ownCount > kMaxMenuItems) {
            log().warn(L"UiProbe: the number of items to add is out of range ({})", ownCount);
            return nullptr;
        }
        if (srcCount == 0 || srcCount + ownCount > kMaxPaneElems) {
            log().warn(L"UiProbe: pane controls element count out of range (vanilla {} + ours {})",
                       srcCount, ownCount);
            return nullptr;
        }
        std::memcpy(&g_paneElems[ownCount], reinterpret_cast<const void*>(srcVec[0]),
                    srcCount * sizeof(std::uintptr_t));

        static const std::wstring kTemplateName = L"moving_around_button";
        Entry tplEntry;
        std::uintptr_t tplElem = 0;
        int dumped = 0;
        for (size_t i = 0; i < srcCount; ++i) {
            const std::uintptr_t elem = g_paneElems[ownCount + i];
            std::vector<Entry> one;
            std::uintptr_t oneCount = 0;
            if (!collectEntries(reinterpret_cast<const void*>(elem), one, oneCount)
                || one.size() != 1) {
                continue;
            }
            if (dumped < 4 && one[0].keyText.find(L"_button") != std::wstring::npos) {
                std::uintptr_t overBox[4]{};
                overBox[0] = one[0].value;
                std::vector<Entry> over;
                ++dumped;
            }
            if (tplElem == 0 && one[0].keyText.rfind(kTemplateName, 0) == 0) {
                tplElem = elem;
                tplEntry = one[0];
            }
        }
        if (tplElem == 0
            || !memory::isReadable(reinterpret_cast<const void*>(tplElem),
                                   sizeof(g_paneItemValues[0]))) {
            log().warn(L"UiProbe: the vanilla item to copy ({}) is not in the pane", kTemplateName);
            return nullptr;
        }

        std::vector<Entry> overSrc;
        std::uintptr_t overSrcCount = 0;
        {
            std::uintptr_t overBox[4]{};
            overBox[0] = tplEntry.value;
            if (!collectEntries(overBox, overSrc, overSrcCount) || overSrc.empty()) {
                log().warn(L"UiProbe: could not walk the donor override node ({} keys)",
                           overSrcCount);
                return nullptr;
            }
        }
        const Entry* idxSrc = nullptr;
        for (const Entry& one : overSrc) {
            if (one.keyText == L"$toggle_group_forced_index") {
                idxSrc = &one;
                break;
            }
        }
        if (idxSrc == nullptr) {
            log().warn(L"UiProbe: the donor has no $toggle_group_forced_index ({} keys)",
                       overSrc.size());
            return nullptr;
        }

        std::vector<Entry> defEntries;
        std::uintptr_t defCount = 0;
        if (!collectEntries(item, defEntries, defCount) || defEntries.empty()) {
            log().warn(L"UiProbe: item definition only walked {} of {} keys", defEntries.size(),
                       defCount);
            return nullptr;
        }
        const Entry* topicSrc = nullptr;
        for (const Entry& one : defEntries) {
            if (one.keyText == L"$section_topic") {
                topicSrc = &one;
                break;
            }
        }
        if (topicSrc == nullptr
            || !memory::isReadable(reinterpret_cast<const void*>(topicSrc->value),
                                   sizeof(g_paneTopicRecs[0]))) {
            log().warn(L"UiProbe: the item definition has no readable $section_topic ({} keys)",
                       defEntries.size());
            return nullptr;
        }

        const size_t atPos = tplEntry.keyText.find(L'@');
        for (size_t i = 0; i < ownCount; ++i) {
            char* const key = g_paneItemKeys[i];
            int at = std::snprintf(key, kPaneKeyBytes, "tk_pane%02u", static_cast<unsigned>(i));
            if (at < 0) {
                log().warn(L"UiProbe: could not build the key for our item {}", i);
                return nullptr;
            }
            if (static_cast<size_t>(at) >= kPaneKeyBytes) {
                at = static_cast<int>(kPaneKeyBytes) - 1;
            }
            if (atPos != std::wstring::npos) {
                for (size_t k = atPos;
                     k < tplEntry.keyText.size() && static_cast<size_t>(at) + 1 < kPaneKeyBytes;
                     ++k) {
                    key[at++] = static_cast<char>(tplEntry.keyText[k]);
                }
                key[at] = '\0';
            }
            std::snprintf(g_paneTopics[i], kPaneTopicBytes, "tk%02u", static_cast<unsigned>(i));
            const char* const topic = g_paneTopics[i];
            std::memcpy(g_paneTopicRecs[i], reinterpret_cast<const void*>(topicSrc->value),
                        sizeof(g_paneTopicRecs[i]));
            std::memcpy(g_paneTopicRecs[i], &topic, sizeof(topic));

            const char* const label = g_menuTextBuf[i];
            std::memcpy(g_paneTextRecs[i], reinterpret_cast<const void*>(topicSrc->value),
                        sizeof(g_paneTextRecs[i]));
            std::memcpy(g_paneTextRecs[i], &label, sizeof(label));

            g_paneOverNodes[i][0].key = reinterpret_cast<std::uintptr_t>(kOwnTextVar);
            g_paneOverNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_paneTextRecs[i]);
            g_paneOverNodes[i][0].tag = topicSrc->tag;
            g_paneOverNodes[i][0].seq = topicSrc->seq;
            g_paneOverNodes[i][1].key = topicSrc->key;
            g_paneOverNodes[i][1].value = reinterpret_cast<std::uintptr_t>(g_paneTopicRecs[i]);
            g_paneOverNodes[i][1].tag = topicSrc->tag;
            g_paneOverNodes[i][1].seq = topicSrc->seq;
            g_paneOverNodes[i][2].key = idxSrc->key;
            g_paneOverNodes[i][2].value = static_cast<std::uintptr_t>(i);
            g_paneOverNodes[i][2].tag = idxSrc->tag;
            g_paneOverNodes[i][2].seq = idxSrc->seq;
            finishMap(&g_paneOverHeads[i], g_paneOverNodes[i], 3, g_paneOverNodePtrs[i]);

            g_paneItemNodes[i][0].key = reinterpret_cast<std::uintptr_t>(key);
            g_paneItemNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_paneOverNodePtrs[i]);
            g_paneItemNodes[i][0].tag = tplEntry.tag;
            g_paneItemNodes[i][0].seq = tplEntry.seq;
            finishMap(&g_paneItemHeads[i], g_paneItemNodes[i], 1, g_paneItemNodePtrs[i]);
            std::memcpy(g_paneItemValues[i], reinterpret_cast<const void*>(tplElem),
                        sizeof(g_paneItemValues[i]));
            g_paneItemValues[i][0] = reinterpret_cast<std::uintptr_t>(g_paneItemNodePtrs[i]);
            g_paneElems[i] = reinterpret_cast<std::uintptr_t>(g_paneItemValues[i]);
        }
        const size_t total = ownCount + (kPaneKeepVanilla ? srcCount : 0);

        std::memcpy(g_paneVec, reinterpret_cast<const void*>(paneControls->value),
                    sizeof(g_paneVec));
        g_paneVec[0] = reinterpret_cast<std::uintptr_t>(&g_paneElems[0]);
        g_paneVec[1] = reinterpret_cast<std::uintptr_t>(&g_paneElems[total]);
        g_paneVec[2] = reinterpret_cast<std::uintptr_t>(&g_paneElems[total]);

        for (size_t i = 0; i < bodyEntries.size(); ++i) {
            g_paneNodes[i].key = bodyEntries[i].key;
            g_paneNodes[i].value = (bodyEntries[i].keyText == L"controls")
                                       ? reinterpret_cast<std::uintptr_t>(g_paneVec)
                                       : bodyEntries[i].value;
            g_paneNodes[i].tag = bodyEntries[i].tag;
            g_paneNodes[i].seq = bodyEntries[i].seq;
        }
        finishMap(&g_paneHead, g_paneNodes, static_cast<int>(bodyEntries.size()), g_paneNode);

        g_paneWrapNodes[0].key = wrapKey;
        g_paneWrapNodes[0].value = reinterpret_cast<std::uintptr_t>(g_paneNode);
        g_paneWrapNodes[0].tag = wrapTag;
        g_paneWrapNodes[0].seq = 0;
        finishMap(&g_paneWrapHead, g_paneWrapNodes, 1, g_paneWrapNode);
        std::memcpy(g_paneWrapValue, reinterpret_cast<const void*>(firstElem),
                    sizeof(g_paneWrapValue));
        g_paneWrapValue[0] = reinterpret_cast<std::uintptr_t>(g_paneWrapNode);

        g_navElems[0] = reinterpret_cast<std::uintptr_t>(g_paneWrapValue);
        std::memcpy(g_navVec, reinterpret_cast<const void*>(controls->value), sizeof(g_navVec));
        g_navVec[0] = reinterpret_cast<std::uintptr_t>(&g_navElems[0]);
        g_navVec[1] = reinterpret_cast<std::uintptr_t>(&g_navElems[1]);
        g_navVec[2] = reinterpret_cast<std::uintptr_t>(&g_navElems[1]);

        for (size_t i = 0; i < entries.size(); ++i) {
            g_navNodes[i].key = entries[i].key;
            g_navNodes[i].value = (entries[i].keyText == L"controls")
                                      ? reinterpret_cast<std::uintptr_t>(g_navVec)
                                      : entries[i].value;
            g_navNodes[i].tag = entries[i].tag;
            g_navNodes[i].seq = entries[i].seq;
        }
        finishMap(&g_navHead, g_navNodes, static_cast<int>(entries.size()), g_navNode);
        std::memcpy(g_navValue, nav, sizeof(g_navValue));
        g_navValue[0] = reinterpret_cast<std::uintptr_t>(g_navNode);

        g_navReady = true;
        const std::string firstKey = g_paneItemKeys[0];
        const std::string lastTopic = g_paneTopics[ownCount - 1];
        return g_navValue;
    }

    buildMenuTexts();

    const size_t itemCount = kNavProbeVanillaMix ? kMixCount : g_menuCount;

    for (size_t i = 0; i < itemCount; ++i) {
        const void* source = kNavProbeVanillaMix ? mixItems[i] : item;
        const auto* const sourceWords = static_cast<const std::uintptr_t*>(source);
        std::uintptr_t itemNode = sourceWords[0];
        const void* itemTemplate = source;
        if (useOwn && !kNavProbeVanillaMix) {
            if (buildRelabeled(self, i) == nullptr) {
                log().warn(L"UiProbe: could not build the body of item {} (label copy)", i);
                return nullptr;
            }
            itemNode = reinterpret_cast<std::uintptr_t>(g_ownNodePtrs[i]);
            itemTemplate = g_ownValues[i];
        }

        const char* const keyText =
            kNavProbeVanillaMix ? kMixNames[i].c_str() : kNavItemKeys[i];
        g_navItemNodes[i][0].key = reinterpret_cast<std::uintptr_t>(keyText);
        g_navItemNodes[i][0].value = itemNode;
        g_navItemNodes[i][0].tag = objectTag;
        g_navItemNodes[i][0].seq = 0;
        finishMap(&g_navItemHeads[i], g_navItemNodes[i], 1, g_navItemNodePtrs[i]);
        std::memcpy(g_navItemValues[i], itemTemplate, sizeof(g_navItemValues[i]));
        g_navItemValues[i][0] = reinterpret_cast<std::uintptr_t>(g_navItemNodePtrs[i]);
    }

    size_t used = 0;
    if (kNavItemFirst) {
        for (size_t i = 0; i < itemCount; ++i) {
            g_navElems[used++] = reinterpret_cast<std::uintptr_t>(g_navItemValues[i]);
        }
        if (kNavKeepVanilla) {
            g_navElems[used++] = firstElem;
        }
    } else {
        if (kNavKeepVanilla) {
            g_navElems[used++] = firstElem;
        }
        for (size_t i = 0; i < itemCount; ++i) {
            g_navElems[used++] = reinterpret_cast<std::uintptr_t>(g_navItemValues[i]);
        }
    }
    std::memcpy(g_navVec, reinterpret_cast<const void*>(controls->value), sizeof(g_navVec));
    g_navVec[0] = reinterpret_cast<std::uintptr_t>(&g_navElems[0]);
    g_navVec[1] = reinterpret_cast<std::uintptr_t>(&g_navElems[used]);
    g_navVec[2] = reinterpret_cast<std::uintptr_t>(&g_navElems[used]);

    for (size_t i = 0; i < entries.size(); ++i) {
        g_navNodes[i].key = entries[i].key;
        g_navNodes[i].value = (entries[i].keyText == L"controls")
                                  ? reinterpret_cast<std::uintptr_t>(g_navVec)
                                  : entries[i].value;
        g_navNodes[i].tag = entries[i].tag;
        g_navNodes[i].seq = entries[i].seq;
    }
    finishMap(&g_navHead, g_navNodes, static_cast<int>(entries.size()), g_navNode);
    std::memcpy(g_navValue, nav, sizeof(g_navValue));
    g_navValue[0] = reinterpret_cast<std::uintptr_t>(g_navNode);

    g_navReady = true;
    return g_navValue;
}

void* buildToggleLabel(void* self)
{
    if (g_labelReady) {
        return g_labelTopValue;
    }

    static const std::string kSpace = "how_to_play_common";
    static const std::string kName = "toggle_button_control";

    void* top = nullptr;
    if (!lookupGuarded(self, &kSpace, &kName, top) || top == nullptr
        || !memory::isReadable(top, sizeof(g_labelTopValue))) {
        log().warn(L"UiProbe: could not look up the label original");
        return nullptr;
    }

    std::vector<Entry> topEntries;
    std::uintptr_t topCount = 0;
    if (!collectEntries(top, topEntries, topCount) || topEntries.empty()
        || topEntries.size() > static_cast<size_t>(kMaxNavKeys)) {
        log().warn(L"UiProbe: could not walk the label node ({} of {} keys)",
                   topEntries.size(), topCount);
        return nullptr;
    }
    std::sort(topEntries.begin(), topEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    const Entry* topControls = nullptr;
    for (const Entry& one : topEntries) {
        if (one.keyText == L"controls") {
            topControls = &one;
            break;
        }
    }
    if (topControls == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(topControls->value),
                               sizeof(g_labelVec))) {
        log().warn(L"UiProbe: cannot read the label controls ({} keys)", topEntries.size());
        return nullptr;
    }
    std::uintptr_t srcVec[4]{};
    std::memcpy(srcVec, reinterpret_cast<const void*>(topControls->value), sizeof(srcVec));
    if (srcVec[1] <= srcVec[0]
        || !memory::isReadable(reinterpret_cast<const void*>(srcVec[0]), sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the label controls are empty or unreadable");
        return nullptr;
    }
    std::uintptr_t wrapElem = 0;
    std::memcpy(&wrapElem, reinterpret_cast<const void*>(srcVec[0]), sizeof(wrapElem));

    std::vector<Entry> wrapEntries;
    std::uintptr_t wrapCount = 0;
    if (!collectEntries(reinterpret_cast<const void*>(wrapElem), wrapEntries, wrapCount)
        || wrapEntries.size() != 1) {
        log().warn(L"UiProbe: the label wrapper is not a single-key node ({} keys)",
                   wrapEntries.size());
        return nullptr;
    }
    std::uintptr_t labelBox[4]{};
    labelBox[0] = wrapEntries[0].value;

    std::vector<Entry> labelEntries;
    std::uintptr_t labelCount = 0;
    if (!collectEntries(labelBox, labelEntries, labelCount) || labelEntries.empty()
        || labelEntries.size() > static_cast<size_t>(kMaxOwnKeys)) {
        log().warn(L"UiProbe: could not walk the label body ({} of {} keys)", labelEntries.size(),
                   labelCount);
        return nullptr;
    }
    std::sort(labelEntries.begin(), labelEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    if (!retargetString(labelEntries, L"text", kOwnTextVar, g_labelTextRec)) {
        return nullptr;
    }
    for (size_t i = 0; i < labelEntries.size(); ++i) {
        g_labelNodes[i].key = labelEntries[i].key;
        g_labelNodes[i].value = labelEntries[i].value;
        g_labelNodes[i].tag = labelEntries[i].tag;
        g_labelNodes[i].seq = labelEntries[i].seq;
    }
    finishMap(&g_labelHead, g_labelNodes, static_cast<int>(labelEntries.size()), g_labelNode);

    g_labelWrapNodes[0].key = wrapEntries[0].key;
    g_labelWrapNodes[0].value = reinterpret_cast<std::uintptr_t>(g_labelNode);
    g_labelWrapNodes[0].tag = wrapEntries[0].tag;
    g_labelWrapNodes[0].seq = wrapEntries[0].seq;
    finishMap(&g_labelWrapHead, g_labelWrapNodes, 1, g_labelWrapNode);
    std::memcpy(g_labelWrapValue, reinterpret_cast<const void*>(wrapElem),
                sizeof(g_labelWrapValue));
    g_labelWrapValue[0] = reinterpret_cast<std::uintptr_t>(g_labelWrapNode);

    g_labelElems[0] = reinterpret_cast<std::uintptr_t>(g_labelWrapValue);
    std::memcpy(g_labelVec, reinterpret_cast<const void*>(topControls->value), sizeof(g_labelVec));
    g_labelVec[0] = reinterpret_cast<std::uintptr_t>(&g_labelElems[0]);
    g_labelVec[1] = reinterpret_cast<std::uintptr_t>(&g_labelElems[1]);
    g_labelVec[2] = reinterpret_cast<std::uintptr_t>(&g_labelElems[1]);

    for (size_t i = 0; i < topEntries.size(); ++i) {
        g_labelTopNodes[i].key = topEntries[i].key;
        g_labelTopNodes[i].value = (topEntries[i].keyText == L"controls")
                                       ? reinterpret_cast<std::uintptr_t>(g_labelVec)
                                       : topEntries[i].value;
        g_labelTopNodes[i].tag = topEntries[i].tag;
        g_labelTopNodes[i].seq = topEntries[i].seq;
    }
    finishMap(&g_labelTopHead, g_labelTopNodes, static_cast<int>(topEntries.size()),
              g_labelTopNode);
    std::memcpy(g_labelTopValue, top, sizeof(g_labelTopValue));
    g_labelTopValue[0] = reinterpret_cast<std::uintptr_t>(g_labelTopNode);

    g_labelReady = true;
    return g_labelTopValue;
}

void* buildHeader(void* self)
{
    if (g_hdrReady) {
        return g_hdrValue;
    }

    static const std::string kSpace = "how_to_play_common";
    static const std::string kName = "how_to_play_header";

    void* top = nullptr;
    if (!lookupGuarded(self, &kSpace, &kName, top) || top == nullptr
        || !memory::isReadable(top, sizeof(g_hdrValue))) {
        log().warn(L"UiProbe: could not look up the header original");
        return nullptr;
    }

    struct Step {
        std::vector<Entry> entries;
        const Entry* controls;
        std::uintptr_t vec[4];
        size_t count;
        std::vector<std::uintptr_t> elems;
    };
    auto readControls = [](const void* value, Step& out, const wchar_t* label) -> bool {
        std::uintptr_t n = 0;
        if (!collectEntries(value, out.entries, n) || out.entries.empty()
            || out.entries.size() > static_cast<size_t>(kMaxNavKeys)) {
            log().warn(L"UiProbe: could not walk the {} node ({} keys)", label, out.entries.size());
            return false;
        }
        std::sort(out.entries.begin(), out.entries.end(),
                  [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
        out.controls = nullptr;
        for (const Entry& one : out.entries) {
            if (one.keyText == L"controls") {
                out.controls = &one;
                break;
            }
        }
        if (out.controls == nullptr
            || !memory::isReadable(reinterpret_cast<const void*>(out.controls->value),
                                   sizeof(out.vec))) {
            log().warn(L"UiProbe: cannot read the {} controls ({} keys)",
                       label, out.entries.size());
            return false;
        }
        std::memcpy(out.vec, reinterpret_cast<const void*>(out.controls->value), sizeof(out.vec));
        out.count = (out.vec[1] > out.vec[0])
                        ? (out.vec[1] - out.vec[0]) / sizeof(std::uintptr_t)
                        : 0;
        if (out.count == 0 || out.count > kMaxHdrElems) {
            log().warn(L"UiProbe: {} element count out of range ({})", label, out.count);
            return false;
        }
        out.elems.assign(out.count, 0);
        std::memcpy(out.elems.data(), reinterpret_cast<const void*>(out.vec[0]),
                    out.count * sizeof(std::uintptr_t));
        return true;
    };

    Step outer;
    if (!readControls(top, outer, L"header")) {
        return nullptr;
    }
    std::vector<Entry> barWrap;
    std::uintptr_t n = 0;
    if (!collectEntries(reinterpret_cast<const void*>(outer.elems[0]), barWrap, n)
        || barWrap.size() != 1) {
        log().warn(L"UiProbe: the header wrapper is not a single-key node ({} keys)",
                   barWrap.size());
        return nullptr;
    }

    std::uintptr_t barBox[4]{};
    barBox[0] = barWrap[0].value;
    Step bar;
    if (!readControls(barBox, bar, L"top row")) {
        return nullptr;
    }

    size_t titleAt = bar.count;
    std::vector<Entry> titleWrap;
    for (size_t i = 0; i < bar.count; ++i) {
        std::vector<Entry> one;
        std::uintptr_t m = 0;
        if (!collectEntries(reinterpret_cast<const void*>(bar.elems[i]), one, m)
            || one.size() != 1) {
            continue;
        }
        if (one[0].keyText.rfind(L"how_to_play_title", 0) == 0) {
            titleAt = i;
            titleWrap = one;
        }
    }
    if (titleAt >= bar.count || titleWrap.size() != 1) {
        log().warn(L"UiProbe: the header has no title element");
        return nullptr;
    }

    std::uintptr_t titleBox[4]{};
    titleBox[0] = titleWrap[0].value;
    std::vector<Entry> titleEntries;
    std::uintptr_t titleCount = 0;
    if (!collectEntries(titleBox, titleEntries, titleCount) || titleEntries.empty()
        || titleEntries.size() > static_cast<size_t>(kMaxNavKeys)) {
        log().warn(L"UiProbe: could not walk the title override ({} keys)", titleEntries.size());
        return nullptr;
    }
    std::sort(titleEntries.begin(), titleEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
    std::uintptr_t hdrStrDonor = 0;
    std::uintptr_t hdrStrTag = kTagString;
    std::uintptr_t hdrStrSeq = 0;
    for (const Entry& one : titleEntries) {
        if ((one.tag & 0xFF) == kTagString
            && memory::isReadable(reinterpret_cast<const void*>(one.value),
                                  sizeof(g_hdrSizeRecs[0]))) {
            hdrStrDonor = one.value;
            hdrStrTag = one.tag;
            hdrStrSeq = one.seq;
            break;
        }
    }
    if (!retargetString(titleEntries, L"$screen_header_title", pageHeaderTitle(), g_hdrTitleRec)
        || !retargetString(titleEntries, L"$screen_header_title_binding_type", kHeaderBindNone,
                           g_hdrBindRec)) {
        return nullptr;
    }
    for (size_t i = 0; i < titleEntries.size(); ++i) {
        g_hdrTitleNodes[i].key = titleEntries[i].key;
        g_hdrTitleNodes[i].value = titleEntries[i].value;
        g_hdrTitleNodes[i].tag = titleEntries[i].tag;
        g_hdrTitleNodes[i].seq = titleEntries[i].seq;
    }
    finishMap(&g_hdrTitleHead, g_hdrTitleNodes, static_cast<int>(titleEntries.size()),
              g_hdrTitleNode);

    g_hdrTitleWrapNodes[0].key = titleWrap[0].key;
    g_hdrTitleWrapNodes[0].value = reinterpret_cast<std::uintptr_t>(g_hdrTitleNode);
    g_hdrTitleWrapNodes[0].tag = titleWrap[0].tag;
    g_hdrTitleWrapNodes[0].seq = titleWrap[0].seq;
    finishMap(&g_hdrTitleWrapHead, g_hdrTitleWrapNodes, 1, g_hdrTitleWrapNode);
    std::memcpy(g_hdrTitleWrapValue, reinterpret_cast<const void*>(bar.elems[titleAt]),
                sizeof(g_hdrTitleWrapValue));
    g_hdrTitleWrapValue[0] = reinterpret_cast<std::uintptr_t>(g_hdrTitleWrapNode);

    std::memcpy(g_hdrBarElems, bar.elems.data(), bar.count * sizeof(std::uintptr_t));
    g_hdrBarElems[titleAt] = reinterpret_cast<std::uintptr_t>(g_hdrTitleWrapValue);
    std::memcpy(g_hdrBarVec, bar.vec, sizeof(g_hdrBarVec));
    g_hdrBarVec[0] = reinterpret_cast<std::uintptr_t>(&g_hdrBarElems[0]);
    g_hdrBarVec[1] = reinterpret_cast<std::uintptr_t>(&g_hdrBarElems[bar.count]);
    g_hdrBarVec[2] = reinterpret_cast<std::uintptr_t>(&g_hdrBarElems[bar.count]);

    for (size_t i = 0; i < bar.entries.size(); ++i) {
        g_hdrBarNodes[i].key = bar.entries[i].key;
        g_hdrBarNodes[i].value = (bar.entries[i].keyText == L"controls")
                                     ? reinterpret_cast<std::uintptr_t>(g_hdrBarVec)
                                     : bar.entries[i].value;
        g_hdrBarNodes[i].tag = bar.entries[i].tag;
        g_hdrBarNodes[i].seq = bar.entries[i].seq;
    }
    finishMap(&g_hdrBarHead, g_hdrBarNodes, static_cast<int>(bar.entries.size()), g_hdrBarNode);

    g_hdrBarWrapNodes[0].key = barWrap[0].key;
    g_hdrBarWrapNodes[0].value = reinterpret_cast<std::uintptr_t>(g_hdrBarNode);
    g_hdrBarWrapNodes[0].tag = barWrap[0].tag;
    g_hdrBarWrapNodes[0].seq = barWrap[0].seq;
    finishMap(&g_hdrBarWrapHead, g_hdrBarWrapNodes, 1, g_hdrBarWrapNode);
    std::memcpy(g_hdrBarWrapValue, reinterpret_cast<const void*>(outer.elems[0]),
                sizeof(g_hdrBarWrapValue));
    g_hdrBarWrapValue[0] = reinterpret_cast<std::uintptr_t>(g_hdrBarWrapNode);

    g_hdrElems[0] = reinterpret_cast<std::uintptr_t>(g_hdrBarWrapValue);
    std::memcpy(g_hdrVec, outer.vec, sizeof(g_hdrVec));
    g_hdrVec[0] = reinterpret_cast<std::uintptr_t>(&g_hdrElems[0]);
    g_hdrVec[1] = reinterpret_cast<std::uintptr_t>(&g_hdrElems[1]);
    g_hdrVec[2] = reinterpret_cast<std::uintptr_t>(&g_hdrElems[1]);

    if (hdrStrDonor != 0
        && buildStringPairArray(outer.controls->value, outer.elems[0], hdrStrDonor, kHeaderWidth,
                                kHeaderHeight, g_hdrSizeRecs, g_hdrSizeElems, g_hdrSizeElemPtrs,
                                g_hdrSizeVec)) {
        Entry* found = nullptr;
        for (Entry& one : outer.entries) {
            if (one.keyText == L"size") {
                found = &one;
                break;
            }
        }
        if (found != nullptr) {
            found->value = reinterpret_cast<std::uintptr_t>(g_hdrSizeVec);
            found->tag = outer.controls->tag;
        } else if (outer.entries.size() + 1 <= static_cast<size_t>(kMaxNavKeys)) {
            Entry add;
            add.keyText = L"size";
            add.key = reinterpret_cast<std::uintptr_t>(kHeaderSizeKey);
            add.value = reinterpret_cast<std::uintptr_t>(g_hdrSizeVec);
            add.tag = outer.controls->tag;
            add.seq = outer.controls->seq;
            outer.entries.push_back(add);
        }
        const struct {
            const wchar_t* keyText;
            const char* key;
            unsigned char* rec;
        } kAnchors[] = {
            {L"anchor_from", kHeaderAnchorFromKey, g_hdrAnchorRecs[0]},
            {L"anchor_to", kHeaderAnchorToKey, g_hdrAnchorRecs[1]},
        };
        for (const auto& one : kAnchors) {
            std::memcpy(one.rec, reinterpret_cast<const void*>(hdrStrDonor), 16);
            const char* const anchor = kHeaderAnchor;
            std::memcpy(one.rec, &anchor, sizeof(anchor));
            Entry* found = nullptr;
            for (Entry& e : outer.entries) {
                if (e.keyText == one.keyText) {
                    found = &e;
                    break;
                }
            }
            if (found != nullptr) {
                found->value = reinterpret_cast<std::uintptr_t>(one.rec);
                found->tag = hdrStrTag;
                continue;
            }
            if (outer.entries.size() + 1 > static_cast<size_t>(kMaxNavKeys)) {
                continue;
            }
            Entry add;
            add.keyText = one.keyText;
            add.key = reinterpret_cast<std::uintptr_t>(one.key);
            add.value = reinterpret_cast<std::uintptr_t>(one.rec);
            add.tag = hdrStrTag;
            add.seq = hdrStrSeq;
            outer.entries.push_back(add);
        }
        std::sort(outer.entries.begin(), outer.entries.end(),
                  [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
    }
    for (size_t i = 0; i < outer.entries.size(); ++i) {
        g_hdrNodes[i].key = outer.entries[i].key;
        g_hdrNodes[i].value = (outer.entries[i].keyText == L"controls")
                                  ? reinterpret_cast<std::uintptr_t>(g_hdrVec)
                                  : outer.entries[i].value;
        g_hdrNodes[i].tag = outer.entries[i].tag;
        g_hdrNodes[i].seq = outer.entries[i].seq;
    }
    finishMap(&g_hdrHead, g_hdrNodes, static_cast<int>(outer.entries.size()), g_hdrNode);
    std::memcpy(g_hdrValue, top, sizeof(g_hdrValue));
    g_hdrValue[0] = reinterpret_cast<std::uintptr_t>(g_hdrNode);

    g_hdrReady = true;
    return g_hdrValue;
}

void* buildContentPanel(void* self)
{
    if (g_contentReady) {
        return g_contentValue;
    }

    static const std::string kSpace = "how_to_play";
    static const std::string kPanel = "how_to_play_section_content_panels";
    static const std::string kSectionDef = "moving_around_section";

    buildMenuTexts();
    collectNumbers();
    collectKeybinds();
    const size_t ownCount = (kNavPaneOwnCount != 0) ? kNavPaneOwnCount : g_menuCount;
    if (ownCount == 0 || ownCount > kMaxMenuItems) {
        log().warn(L"UiProbe: content pane count out of range ({})", ownCount);
        return nullptr;
    }

    void* panel = nullptr;
    if (!lookupGuarded(self, &kSpace, &kPanel, panel) || panel == nullptr
        || !memory::isReadable(panel, sizeof(g_contentValue))) {
        log().warn(L"UiProbe: could not look up the content-pane original");
        return nullptr;
    }

    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(panel, entries, count)) {
        log().warn(L"UiProbe: content-pane node only walked {} of {}", entries.size(),
                   count);
        return nullptr;
    }
    if (entries.empty() || entries.size() > static_cast<size_t>(kMaxNavKeys)) {
        log().warn(L"UiProbe: content-pane key count out of range ({})", entries.size());
        return nullptr;
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    const Entry* controls = nullptr;
    for (const Entry& one : entries) {
        if (one.keyText == L"controls") {
            controls = &one;
            break;
        }
    }
    if (controls == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(controls->value),
                               sizeof(g_contentVec))) {
        log().warn(L"UiProbe: cannot read the content-pane controls");
        return nullptr;
    }

    std::uintptr_t vanillaVec[4]{};
    std::memcpy(vanillaVec, reinterpret_cast<const void*>(controls->value), sizeof(vanillaVec));
    if (vanillaVec[1] <= vanillaVec[0]
        || !memory::isReadable(reinterpret_cast<const void*>(vanillaVec[0]),
                               sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the content-pane controls are empty or unreadable");
        return nullptr;
    }
    std::uintptr_t firstElem = 0;
    std::memcpy(&firstElem, reinterpret_cast<const void*>(vanillaVec[0]), sizeof(firstElem));

    std::vector<Entry> wrapEntries;
    std::uintptr_t wrapCount = 0;
    if (!collectEntries(reinterpret_cast<const void*>(firstElem), wrapEntries, wrapCount)
        || wrapEntries.size() != 1) {
        log().warn(L"UiProbe: the content-pane wrapper is not a single-key node ({} keys)",
                   wrapEntries.size());
        return nullptr;
    }
    const std::uintptr_t wrapKey = wrapEntries[0].key;
    const std::uintptr_t wrapTag = wrapEntries[0].tag;
    std::uintptr_t bodyBox[4]{};
    bodyBox[0] = wrapEntries[0].value;

    std::vector<Entry> bodyEntries;
    std::uintptr_t bodyCount = 0;
    if (!collectEntries(bodyBox, bodyEntries, bodyCount) || bodyEntries.empty()
        || bodyEntries.size() > static_cast<size_t>(kMaxNavKeys)) {
        log().warn(L"UiProbe: could not walk the content-pane body ({} keys)", bodyEntries.size());
        return nullptr;
    }
    std::sort(bodyEntries.begin(), bodyEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    const Entry* bodyControls = nullptr;
    for (const Entry& one : bodyEntries) {
        if (one.keyText == L"controls") {
            bodyControls = &one;
            break;
        }
    }
    if (bodyControls == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(bodyControls->value),
                               sizeof(g_secVec))) {
        log().warn(L"UiProbe: cannot read the content-pane body controls ({} keys)",
                   bodyEntries.size());
        return nullptr;
    }

    std::uintptr_t srcVec[4]{};
    std::memcpy(srcVec, reinterpret_cast<const void*>(bodyControls->value), sizeof(srcVec));
    const size_t srcCount =
        (srcVec[1] > srcVec[0]) ? (srcVec[1] - srcVec[0]) / sizeof(std::uintptr_t) : 0;
    if (srcCount == 0 || srcCount > kMaxSecElems) {
        log().warn(L"UiProbe: content-pane element count out of range ({})", srcCount);
        return nullptr;
    }
    std::vector<std::uintptr_t> vanillaElems(srcCount, 0);
    std::memcpy(vanillaElems.data(), reinterpret_cast<const void*>(srcVec[0]),
                srcCount * sizeof(std::uintptr_t));

    std::uintptr_t headerElem = 0;
    std::uintptr_t footerElem = 0;
    std::vector<std::uintptr_t> candElems;
    std::vector<Entry> candEntries;
    for (size_t i = 0; i < srcCount; ++i) {
        std::vector<Entry> one;
        std::uintptr_t oneCount = 0;
        if (!collectEntries(reinterpret_cast<const void*>(vanillaElems[i]), one, oneCount)
            || one.size() != 1) {
            continue;
        }
        if (one[0].keyText.rfind(L"section_contents_header", 0) == 0) {
            headerElem = vanillaElems[i];
            continue;
        }
        if (one[0].keyText.rfind(L"section_contents_footer", 0) == 0) {
            footerElem = vanillaElems[i];
            continue;
        }
        if (one[0].keyText.find(L"_section@") == std::wstring::npos) {
            continue;
        }
        std::uintptr_t overBox[4]{};
        overBox[0] = one[0].value;
        std::vector<Entry> over;
        std::uintptr_t overCount = 0;
        if (!collectEntries(overBox, over, overCount) || !over.empty()) {
            continue;
        }
        candElems.push_back(vanillaElems[i]);
        candEntries.push_back(one[0]);
    }
    if (candElems.size() < ownCount) {
        log().warn(L"UiProbe: not enough sections to copy ({} < {})", candElems.size(),
                   ownCount);
        return nullptr;
    }

    void* secDef = nullptr;
    if (!lookupGuarded(self, &kSpace, &kSectionDef, secDef) || secDef == nullptr) {
        log().warn(L"UiProbe: could not look up the section definition");
        return nullptr;
    }
    std::vector<Entry> defEntries;
    std::uintptr_t defCount = 0;
    if (!collectEntries(secDef, defEntries, defCount) || defEntries.empty()) {
        log().warn(L"UiProbe: section definition only walked {} of {} keys", defEntries.size(),
                   defCount);
        return nullptr;
    }
    const Entry* bindSrc = nullptr;
    for (const Entry& one : defEntries) {
        if (one.keyText == L"bindings") {
            bindSrc = &one;
            break;
        }
    }
    if (bindSrc == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(bindSrc->value),
                               sizeof(g_secBindVec[0]))) {
        log().warn(L"UiProbe: the section definition has no readable bindings ({} keys)",
                   defEntries.size());
        return nullptr;
    }
    std::uintptr_t srcBindVec[4]{};
    std::memcpy(srcBindVec, reinterpret_cast<const void*>(bindSrc->value), sizeof(srcBindVec));
    if (srcBindVec[1] <= srcBindVec[0]
        || !memory::isReadable(reinterpret_cast<const void*>(srcBindVec[0]),
                               sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the bindings array is empty or unreadable");
        return nullptr;
    }
    std::uintptr_t srcBindElem = 0;
    std::memcpy(&srcBindElem, reinterpret_cast<const void*>(srcBindVec[0]), sizeof(srcBindElem));

    std::vector<Entry> bindEntries;
    std::uintptr_t bindCount = 0;
    if (!collectEntries(reinterpret_cast<const void*>(srcBindElem), bindEntries, bindCount)
        || bindEntries.empty() || bindEntries.size() > static_cast<size_t>(kMaxBindKeys)) {
        log().warn(L"UiProbe: could not walk the binding node ({} keys)", bindEntries.size());
        return nullptr;
    }
    std::sort(bindEntries.begin(), bindEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
    const Entry* nameSrc = nullptr;
    for (const Entry& one : bindEntries) {
        if (one.keyText == L"source_control_name") {
            nameSrc = &one;
        }
    }
    if (nameSrc == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(nameSrc->value),
                               sizeof(g_secNameRecs[0]))) {
        log().warn(L"UiProbe: the binding has no readable source_control_name");
        return nullptr;
    }

    const Entry* ctlSrc = nullptr;
    for (const Entry& one : defEntries) {
        if (one.keyText == L"controls") {
            ctlSrc = &one;
            break;
        }
    }
    if (ctlSrc == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(ctlSrc->value),
                               sizeof(g_secCtlVec[0]))) {
        log().warn(L"UiProbe: the section definition has no readable controls");
        return nullptr;
    }
    std::uintptr_t srcCtlVec[4]{};
    std::memcpy(srcCtlVec, reinterpret_cast<const void*>(ctlSrc->value), sizeof(srcCtlVec));
    if (srcCtlVec[1] <= srcCtlVec[0]
        || !memory::isReadable(reinterpret_cast<const void*>(srcCtlVec[0]),
                               sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the section controls are empty or unreadable");
        return nullptr;
    }
    std::uintptr_t srcCtlElem = 0;
    std::memcpy(&srcCtlElem, reinterpret_cast<const void*>(srcCtlVec[0]), sizeof(srcCtlElem));
    std::vector<Entry> ctlOne;
    std::uintptr_t ctlOneCount = 0;
    if (!collectEntries(reinterpret_cast<const void*>(srcCtlElem), ctlOne, ctlOneCount)
        || ctlOne.size() != 1) {
        log().warn(L"UiProbe: a section child is not a single-key node ({} keys)", ctlOne.size());
        return nullptr;
    }
    const std::uintptr_t childTag = ctlOne[0].tag;
    const std::uintptr_t childSeq = ctlOne[0].seq;

    static const std::string kScreenName = "how_to_play_screen";
    static const std::string kToggleSpace = "settings_common";
    static const std::string kToggleCtl = "option_toggle_control";

    const Entry* trueSrc = nullptr;
    const Entry* falseSrc = nullptr;
    std::vector<Entry> screenEntries;
    std::vector<Entry> ctlEntries;
    {
        void* screen = nullptr;
        std::uintptr_t n = 0;
        if (lookupGuarded(self, &kSpace, &kScreenName, screen) && screen != nullptr
            && collectEntries(screen, screenEntries, n)) {
            for (const Entry& one : screenEntries) {
                if (one.keyText == L"$is_full_screen_layout") {
                    trueSrc = &one;
                    break;
                }
            }
        }
        void* ctl = nullptr;
        n = 0;
        if (lookupGuarded(self, &kToggleSpace, &kToggleCtl, ctl) && ctl != nullptr
            && collectEntries(ctl, ctlEntries, n)) {
            for (const Entry& one : ctlEntries) {
                if (one.keyText == L"$enable_directional_toggling") {
                    falseSrc = &one;
                    break;
                }
            }
        }
    }
    if (trueSrc == nullptr || falseSrc == nullptr) {
        log().warn(L"UiProbe: no boolean donors (true={} false={})", trueSrc != nullptr,
                   falseSrc != nullptr);
        return nullptr;
    }

    const std::uintptr_t boolTag = trueSrc->tag;
    const std::uintptr_t trueVal = trueSrc->value;
    constexpr std::uintptr_t kFalseVal = 0;

    {
        const char* const bindCond = kSecBindCond;
        std::memcpy(g_secBindCondRec, reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secBindCondRec));
        std::memcpy(g_secBindCondRec, &bindCond, sizeof(bindCond));
    }

    for (size_t i = 0; i < ownCount; ++i) {
        const Entry& cand = candEntries[i];

        char* const key = g_secItemKeys[i];
        int at = std::snprintf(key, kSecKeyBytes, "tk%02u_section", static_cast<unsigned>(i));
        if (at < 0) {
            log().warn(L"UiProbe: could not build the key for section {}", i);
            return nullptr;
        }
        if (static_cast<size_t>(at) >= kSecKeyBytes) {
            at = static_cast<int>(kSecKeyBytes) - 1;
        }
        const size_t atPos = cand.keyText.find(L'@');
        if (atPos != std::wstring::npos) {
            for (size_t k = atPos;
                 k < cand.keyText.size() && static_cast<size_t>(at) + 1 < kSecKeyBytes; ++k) {
                key[at++] = static_cast<char>(cand.keyText[k]);
            }
            key[at] = '\0';
        }

        std::snprintf(g_secToggleNames[i], kSecNameBytes, "tk%02u_button_toggle",
                      static_cast<unsigned>(i));
        const char* const toggleName = g_secToggleNames[i];
        std::memcpy(g_secNameRecs[i], reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secNameRecs[i]));
        std::memcpy(g_secNameRecs[i], &toggleName, sizeof(toggleName));

        for (size_t k = 0; k < bindEntries.size(); ++k) {
            g_secBindNodes[i][k].key = bindEntries[k].key;
            g_secBindNodes[i][k].value =
                (bindEntries[k].keyText == L"source_control_name")
                    ? reinterpret_cast<std::uintptr_t>(g_secNameRecs[i])
                    : bindEntries[k].value;
            g_secBindNodes[i][k].tag = bindEntries[k].tag;
            g_secBindNodes[i][k].seq = bindEntries[k].seq;
        }
        finishMap(&g_secBindHeads[i], g_secBindNodes[i], static_cast<int>(bindEntries.size()),
                  g_secBindNodePtrs[i]);

        std::memcpy(g_secBindValues[i], reinterpret_cast<const void*>(srcBindElem),
                    sizeof(g_secBindValues[i]));
        g_secBindValues[i][0] = reinterpret_cast<std::uintptr_t>(g_secBindNodePtrs[i]);
        g_secBindElems[i][0] = reinterpret_cast<std::uintptr_t>(g_secBindValues[i]);
        std::memcpy(g_secBindVec[i], reinterpret_cast<const void*>(bindSrc->value),
                    sizeof(g_secBindVec[i]));
        g_secBindVec[i][0] = reinterpret_cast<std::uintptr_t>(&g_secBindElems[i][0]);
        g_secBindVec[i][1] = reinterpret_cast<std::uintptr_t>(&g_secBindElems[i][1]);
        g_secBindVec[i][2] = reinterpret_cast<std::uintptr_t>(&g_secBindElems[i][1]);

        const Module* const owner = g_menuModules[i];

        std::snprintf(g_secCtlNames[i], kSecCtlNameBytes, "settings_common.tk%02u_tgl",
                      static_cast<unsigned>(i));
        const char* const ctlName = g_secCtlNames[i];
        std::memcpy(g_secCtlNameRecs[i], reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secCtlNameRecs[i]));
        std::memcpy(g_secCtlNameRecs[i], &ctlName, sizeof(ctlName));

        const char* const label = g_menuTextBuf[i];
        std::memcpy(g_secLabelRecs[i], reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secLabelRecs[i]));
        std::memcpy(g_secLabelRecs[i], &label, sizeof(label));

        if (kProbeRealBinding && i == kProbeBindingIndex) {
            std::snprintf(g_secBindNames[i], kSecCtlNameBytes, "%s", kProbeBindingName);
        } else {
            std::snprintf(g_secBindNames[i], kSecCtlNameBytes, "#tk_tgl_%02u",
                          static_cast<unsigned>(i));
        }
        const char* const bindName = g_secBindNames[i];
        std::memcpy(g_secBindNameRecs[i], reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secBindNameRecs[i]));
        std::memcpy(g_secBindNameRecs[i], &bindName, sizeof(bindName));

        std::snprintf(g_secToggleNameBufs[i], kSecCtlNameBytes, "tk_tgl_%02u",
                      static_cast<unsigned>(i));
        const char* const tglName = g_secToggleNameBufs[i];
        std::memcpy(g_secToggleNameRecs[i], reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secToggleNameRecs[i]));
        std::memcpy(g_secToggleNameRecs[i], &tglName, sizeof(tglName));

        std::snprintf(g_secFocusIds[i], kSecCtlNameBytes, "tk_tgl_%02u_focus",
                      static_cast<unsigned>(i));
        const char* const focusId = g_secFocusIds[i];
        std::memcpy(g_secFocusIdRecs[i], reinterpret_cast<const void*>(nameSrc->value),
                    sizeof(g_secFocusIdRecs[i]));
        std::memcpy(g_secFocusIdRecs[i], &focusId, sizeof(focusId));

        int keys = 0;
        auto putKey = [&](const void* key, std::uintptr_t value, std::uintptr_t tag,
                          std::uintptr_t seq) {
            g_secBtnOverNodes[i][keys].key = reinterpret_cast<std::uintptr_t>(key);
            g_secBtnOverNodes[i][keys].value = value;
            g_secBtnOverNodes[i][keys].tag = tag;
            g_secBtnOverNodes[i][keys].seq = seq;
            ++keys;
        };
        putKey(kSecCtlNameVar, reinterpret_cast<std::uintptr_t>(g_secCtlNameRecs[i]), nameSrc->tag,
               nameSrc->seq);
        if (owner == nullptr) {
            putKey(kSecEnabledVar, kFalseVal, boolTag, trueSrc->seq);
        }
        putKey(kSecFocusIdVar, reinterpret_cast<std::uintptr_t>(g_secFocusIdRecs[i]), nameSrc->tag,
               nameSrc->seq);
        putKey(kSecOneLineVar, trueVal, boolTag, trueSrc->seq);
        putKey(kSecBindNameVar, reinterpret_cast<std::uintptr_t>(g_secBindNameRecs[i]),
               nameSrc->tag, nameSrc->seq);
        putKey(kSecLabelVar, reinterpret_cast<std::uintptr_t>(g_secLabelRecs[i]), nameSrc->tag,
               nameSrc->seq);
        putKey(kSecBindCondVar, reinterpret_cast<std::uintptr_t>(g_secBindCondRec), nameSrc->tag,
               nameSrc->seq);
        const bool on = (owner != nullptr && owner->enabled());
        putKey(kSecDefaultVar, on ? trueVal : kFalseVal, boolTag, trueSrc->seq);
        putKey(kSecToggleNameVar, reinterpret_cast<std::uintptr_t>(g_secToggleNameRecs[i]),
               nameSrc->tag, nameSrc->seq);
        putKey(kSecStateBindVar, reinterpret_cast<std::uintptr_t>(g_secBindNameRecs[i]),
               nameSrc->tag, nameSrc->seq);
        finishMap(&g_secBtnOverHeads[i], g_secBtnOverNodes[i], keys, g_secBtnOverNodePtrs[i]);

        std::snprintf(g_secBtnKeys[i], kSecKeyBytes, "tk%02u_tgl@settings_common.option_toggle",
                      static_cast<unsigned>(i));
        g_secBtnNodes[i][0].key = reinterpret_cast<std::uintptr_t>(g_secBtnKeys[i]);
        g_secBtnNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_secBtnOverNodePtrs[i]);
        g_secBtnNodes[i][0].tag = childTag;
        g_secBtnNodes[i][0].seq = childSeq;
        finishMap(&g_secBtnHeads[i], g_secBtnNodes[i], 1, g_secBtnNodePtrs[i]);
        std::memcpy(g_secBtnValues[i], reinterpret_cast<const void*>(srcCtlElem),
                    sizeof(g_secBtnValues[i]));
        g_secBtnValues[i][0] = reinterpret_cast<std::uintptr_t>(g_secBtnNodePtrs[i]);

        g_secCtlElems[i][0] = reinterpret_cast<std::uintptr_t>(g_secBtnValues[i]);

        size_t ctls = 1;

        const auto putEditRow = [&](char kind, size_t idx, const char* label, const char* hint) {
            if (ctls >= 1 + kMaxRowsPerModule) {
                return;
            }
            const size_t r = ctls - 1;

            std::snprintf(g_numCtlNames[i][r], kSecCtlNameBytes, "settings_common.tk%02u_%c%u",
                          static_cast<unsigned>(i), kind, static_cast<unsigned>(idx));
            const char* const rowCtlName = g_numCtlNames[i][r];
            std::memcpy(g_numCtlRecs[i][r], reinterpret_cast<const void*>(nameSrc->value),
                        sizeof(g_numCtlRecs[i][r]));
            std::memcpy(g_numCtlRecs[i][r], &rowCtlName, sizeof(rowCtlName));

            std::memcpy(g_numLabelRecs[i][r], reinterpret_cast<const void*>(nameSrc->value),
                        sizeof(g_numLabelRecs[i][r]));
            std::memcpy(g_numLabelRecs[i][r], &label, sizeof(label));

            std::memcpy(g_numHintRecs[i][r], reinterpret_cast<const void*>(nameSrc->value),
                        sizeof(g_numHintRecs[i][r]));
            std::memcpy(g_numHintRecs[i][r], &hint, sizeof(hint));

            std::snprintf(g_numBindNames[i][r], kSecCtlNameBytes, "#tk_%c_%02u_%u", kind,
                          static_cast<unsigned>(i), static_cast<unsigned>(idx));
            const char* const bindName = g_numBindNames[i][r];
            std::memcpy(g_numBindRecs[i][r], reinterpret_cast<const void*>(nameSrc->value),
                        sizeof(g_numBindRecs[i][r]));
            std::memcpy(g_numBindRecs[i][r], &bindName, sizeof(bindName));

            std::memcpy(g_numTexRecs[i][r], reinterpret_cast<const void*>(nameSrc->value),
                        sizeof(g_numTexRecs[i][r]));
            const char* const texValue = kBtnTexValue;
            std::memcpy(g_numTexRecs[i][r], &texValue, sizeof(texValue));

            size_t over = 0;
            const auto putOver = [&](const void* key, const void* rec, std::uintptr_t tag,
                                     std::uintptr_t seq) {
                g_numOverNodes[i][r][over].key = reinterpret_cast<std::uintptr_t>(key);
                g_numOverNodes[i][r][over].value = reinterpret_cast<std::uintptr_t>(rec);
                g_numOverNodes[i][r][over].tag = tag;
                g_numOverNodes[i][r][over].seq = seq;
                ++over;
            };

            size_t sizeSlot = 0;
            const auto putSize = [&](const void* key) {
                const size_t s = sizeSlot++;
                if (!buildStringPairArray(ctlSrc->value, srcCtlElem, nameSrc->value, kBtnSizeW,
                                          kBtnSizeH, g_numSizeStrRecs[i][r][s],
                                          g_numSizeElems[i][r][s], g_numSizeElemPtrs[i][r][s],
                                          g_numSizeVec[i][r][s])) {
                    return;
                }
                putOver(key, g_numSizeVec[i][r][s], ctlSrc->tag, ctlSrc->seq);
            };

            if (kind == 'k') {
                putSize(kBtnImgSizeVar);
                putSize(kBtnSizeVar);
            }

            putOver(kBtnTextVar, g_numHintRecs[i][r], nameSrc->tag, nameSrc->seq);
            putOver(kSecCtlNameVar, g_numCtlRecs[i][r], nameSrc->tag, nameSrc->seq);
            putOver(kBtnTexVar, g_numTexRecs[i][r], nameSrc->tag, nameSrc->seq);
            if (kind == 'k') {
                putSize(kBtnBorderSizeVar);
            }
            putOver(kSecBindNameVar, g_numBindRecs[i][r], nameSrc->tag, nameSrc->seq);
            putOver(kSecLabelVar, g_numLabelRecs[i][r], nameSrc->tag, nameSrc->seq);
            putOver(kNumHintVar, g_numHintRecs[i][r], nameSrc->tag, nameSrc->seq);
            finishMap(&g_numOverHeads[i][r], g_numOverNodes[i][r], static_cast<int>(over),
                      g_numOverPtrs[i][r]);

            std::snprintf(g_numKeys[i][r], kSecKeyBytes,
                          "tk%02u_%c%u@settings_common.option_text_edit",
                          static_cast<unsigned>(i), kind, static_cast<unsigned>(idx));
            g_numNodes[i][r][0].key = reinterpret_cast<std::uintptr_t>(g_numKeys[i][r]);
            g_numNodes[i][r][0].value = reinterpret_cast<std::uintptr_t>(g_numOverPtrs[i][r]);
            g_numNodes[i][r][0].tag = childTag;
            g_numNodes[i][r][0].seq = childSeq;
            finishMap(&g_numHeads[i][r], g_numNodes[i][r], 1, g_numNodePtrs[i][r]);
            std::memcpy(g_numValues[i][r], reinterpret_cast<const void*>(srcCtlElem),
                        sizeof(g_numValues[i][r]));
            g_numValues[i][r][0] = reinterpret_cast<std::uintptr_t>(g_numNodePtrs[i][r]);

            g_secCtlElems[i][ctls++] = reinterpret_cast<std::uintptr_t>(g_numValues[i][r]);
        };

        for (size_t k = 0; k < g_numberCount[i]; ++k) {
            putEditRow('n', k, g_numbers[i][k].label, g_numbers[i][k].shown);
        }
        for (size_t k = 0; k < g_keybindCount[i]; ++k) {
            putEditRow('k', k, g_keybinds[i][k].label, g_keybinds[i][k].shown);
        }

        std::memcpy(g_secCtlVec[i], reinterpret_cast<const void*>(ctlSrc->value),
                    sizeof(g_secCtlVec[i]));
        g_secCtlVec[i][0] = reinterpret_cast<std::uintptr_t>(&g_secCtlElems[i][0]);
        g_secCtlVec[i][1] = reinterpret_cast<std::uintptr_t>(&g_secCtlElems[i][ctls]);
        g_secCtlVec[i][2] = reinterpret_cast<std::uintptr_t>(&g_secCtlElems[i][ctls]);

        g_secOverNodes[i][0].key = bindSrc->key;
        g_secOverNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_secBindVec[i]);
        g_secOverNodes[i][0].tag = bindSrc->tag;
        g_secOverNodes[i][0].seq = bindSrc->seq;
        g_secOverNodes[i][1].key = ctlSrc->key;
        g_secOverNodes[i][1].value = reinterpret_cast<std::uintptr_t>(g_secCtlVec[i]);
        g_secOverNodes[i][1].tag = ctlSrc->tag;
        g_secOverNodes[i][1].seq = ctlSrc->seq;
        finishMap(&g_secOverHeads[i], g_secOverNodes[i], 2, g_secOverNodePtrs[i]);

        g_secItemNodes[i][0].key = reinterpret_cast<std::uintptr_t>(key);
        g_secItemNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_secOverNodePtrs[i]);
        g_secItemNodes[i][0].tag = cand.tag;
        g_secItemNodes[i][0].seq = cand.seq;
        finishMap(&g_secItemHeads[i], g_secItemNodes[i], 1, g_secItemNodePtrs[i]);
        std::memcpy(g_secItemValues[i], reinterpret_cast<const void*>(candElems[i]),
                    sizeof(g_secItemValues[i]));
        g_secItemValues[i][0] = reinterpret_cast<std::uintptr_t>(g_secItemNodePtrs[i]);
    }

    size_t used = 0;
    if (headerElem != 0) {
        g_secElems[used++] = headerElem;
    }
    for (size_t i = 0; i < ownCount; ++i) {
        g_secElems[used++] = reinterpret_cast<std::uintptr_t>(g_secItemValues[i]);
    }
    if (footerElem != 0) {
        g_secElems[used++] = footerElem;
    }
    std::memcpy(g_secVec, reinterpret_cast<const void*>(bodyControls->value), sizeof(g_secVec));
    g_secVec[0] = reinterpret_cast<std::uintptr_t>(&g_secElems[0]);
    g_secVec[1] = reinterpret_cast<std::uintptr_t>(&g_secElems[used]);
    g_secVec[2] = reinterpret_cast<std::uintptr_t>(&g_secElems[used]);

    for (size_t i = 0; i < bodyEntries.size(); ++i) {
        g_secBodyNodes[i].key = bodyEntries[i].key;
        g_secBodyNodes[i].value = (bodyEntries[i].keyText == L"controls")
                                      ? reinterpret_cast<std::uintptr_t>(g_secVec)
                                      : bodyEntries[i].value;
        g_secBodyNodes[i].tag = bodyEntries[i].tag;
        g_secBodyNodes[i].seq = bodyEntries[i].seq;
    }
    finishMap(&g_secBodyHead, g_secBodyNodes, static_cast<int>(bodyEntries.size()), g_secBodyNode);

    g_secWrapNodes[0].key = wrapKey;
    g_secWrapNodes[0].value = reinterpret_cast<std::uintptr_t>(g_secBodyNode);
    g_secWrapNodes[0].tag = wrapTag;
    g_secWrapNodes[0].seq = 0;
    finishMap(&g_secWrapHead, g_secWrapNodes, 1, g_secWrapNode);
    std::memcpy(g_secWrapValue, reinterpret_cast<const void*>(firstElem), sizeof(g_secWrapValue));
    g_secWrapValue[0] = reinterpret_cast<std::uintptr_t>(g_secWrapNode);

    g_contentElems[0] = reinterpret_cast<std::uintptr_t>(g_secWrapValue);
    std::memcpy(g_contentVec, reinterpret_cast<const void*>(controls->value), sizeof(g_contentVec));
    g_contentVec[0] = reinterpret_cast<std::uintptr_t>(&g_contentElems[0]);
    g_contentVec[1] = reinterpret_cast<std::uintptr_t>(&g_contentElems[1]);
    g_contentVec[2] = reinterpret_cast<std::uintptr_t>(&g_contentElems[1]);

    for (size_t i = 0; i < entries.size(); ++i) {
        g_contentNodes[i].key = entries[i].key;
        g_contentNodes[i].value = (entries[i].keyText == L"controls")
                                      ? reinterpret_cast<std::uintptr_t>(g_contentVec)
                                      : entries[i].value;
        g_contentNodes[i].tag = entries[i].tag;
        g_contentNodes[i].seq = entries[i].seq;
    }
    finishMap(&g_contentHead, g_contentNodes, static_cast<int>(entries.size()), g_contentNode);
    std::memcpy(g_contentValue, panel, sizeof(g_contentValue));
    g_contentValue[0] = reinterpret_cast<std::uintptr_t>(g_contentNode);

    g_contentReady = true;
    const std::string firstName = g_secToggleNames[0];
    return g_contentValue;
}

struct OwnRegion {
    const void* base;
    size_t size;
    const wchar_t* name;
};

const OwnRegion kOwnRegions[] = {
    {g_wrapValue, sizeof(g_wrapValue), L"wrapValue"},
    {g_wrapNode, sizeof(g_wrapNode), L"wrap"},
    {&g_wrapHead, sizeof(g_wrapHead), L"wrapHead"},
    {g_wrapNodes, sizeof(g_wrapNodes), L"wrapNodes"},
    {g_vec, sizeof(g_vec), L"vec"},
    {g_elems, sizeof(g_elems), L"elems"},
    {g_childValueA, sizeof(g_childValueA), L"childValueA"},
    {g_childNodeA, sizeof(g_childNodeA), L"childA"},
    {&g_childHeadA, sizeof(g_childHeadA), L"childHeadA"},
    {g_childNodesA, sizeof(g_childNodesA), L"childNodesA"},
    {g_childValueB, sizeof(g_childValueB), L"childValueB"},
    {g_childNodeB, sizeof(g_childNodeB), L"childB"},
    {&g_childHeadB, sizeof(g_childHeadB), L"childHeadB"},
    {g_childNodesB, sizeof(g_childNodesB), L"childNodesB"},
    {g_ownValues, sizeof(g_ownValues), L"ownValues"},
    {g_ownNodePtrs, sizeof(g_ownNodePtrs), L"ownNodePtrs"},
    {g_ownHeads, sizeof(g_ownHeads), L"ownHeads"},
    {g_ownNodes, sizeof(g_ownNodes), L"ownNodes"},
    {g_ownTextRecs, sizeof(g_ownTextRecs), L"ownTextRecs"},
};

std::wstring ownName(std::uintptr_t at)
{
    if (at == 0) {
        return {};
    }
    for (const OwnRegion& region : kOwnRegions) {
        const auto base = reinterpret_cast<std::uintptr_t>(region.base);
        if (at >= base && at < base + region.size) {
            return std::format(L" ({}+{:#x})", region.name, at - base);
        }
    }
    return {};
}

std::atomic<int> g_afterInvokeLogs{0};

std::uintptr_t g_exeBase = 0;
std::uintptr_t g_exeSize = 0;
std::uintptr_t g_selfBase = 0;
std::uintptr_t g_selfSize = 0;

void noteModule(HMODULE module, std::uintptr_t& base, std::uintptr_t& size)
{
    base = reinterpret_cast<std::uintptr_t>(module);
    size = 0;
    if (base == 0 || !memory::isReadable(module, sizeof(IMAGE_DOS_HEADER))) {
        return;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!memory::isReadable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    size = nt->OptionalHeader.SizeOfImage;
}

std::wstring codeName(std::uintptr_t at)
{
    if (g_exeSize != 0 && at >= g_exeBase && at < g_exeBase + g_exeSize) {
        return std::format(L"Minecraft.Windows.exe+{:#x}", at - g_exeBase);
    }
    if (g_selfSize != 0 && at >= g_selfBase && at < g_selfBase + g_selfSize) {
        return std::format(L"Tsukuyomi.dll+{:#x}", at - g_selfBase);
    }
    return L"(not in any module)";
}

void dumpStackOnce(const wchar_t* why, int skip = 0, int limit = 2)
{
    static std::atomic<int> said{0};
    const int n = said.fetch_add(1);
    if (n < skip || n >= skip + limit) {
        return;
    }
    if (g_exeSize == 0) {
        noteModule(GetModuleHandleW(nullptr), g_exeBase, g_exeSize);
    }
    void* frames[40]{};
    const USHORT got = CaptureStackBackTrace(0, 40, frames, nullptr);
    for (USHORT i = 0; i < got; ++i) {
        const auto at = reinterpret_cast<std::uintptr_t>(frames[i]);
        log().info(L"UiProbe: {} stack #{} {}", why, i, codeName(at));
    }
}

constexpr int kMaxCrashLogs = 8;
std::atomic<int> g_crashLogs{0};
std::atomic<bool> g_inHandler{false};
PVOID g_vehHandle = nullptr;

constexpr DWORD kGuardPageViolation = 0x80000001;

std::atomic<int> g_hwStage{0};
std::atomic<int> g_hwHits{0};
std::atomic<unsigned long long> g_hwArmAt{0};
DWORD g_hwThreadId = 0;
constexpr int kMaxHwHits = 40;

thread_local void* t_mappingOut = nullptr;

thread_local void* t_varBag = nullptr;

std::atomic<bool> g_htpStackTaken{false};

constexpr std::uintptr_t kBagResolvedOffset = 0x58;
std::atomic<void*> g_idMappingOut{nullptr};
std::atomic<bool> g_mappingDumped{false};

constexpr size_t kMaxWatchSlots = 4;
std::uintptr_t g_watchSlots[kMaxWatchSlots]{};
size_t g_watchSlotCount = 0;

constexpr size_t kMaxCopyDest = kMaxWatchSlots;
std::uintptr_t g_copyDest[kMaxCopyDest]{};
size_t g_copyDestCount = 0;

constexpr unsigned long long kSwapDelayMs = 3000;
std::atomic<unsigned long long> g_swapAt{0};

constexpr size_t kMaxCandidates = 32;
std::uintptr_t g_candidates[kMaxCandidates]{};
std::uintptr_t g_owners[kMaxCandidates]{};

constexpr unsigned long long kHwArmDelayMs = 8000;

DWORD64 makeDr7(const std::uintptr_t* addresses, size_t count)
{
    DWORD64 dr7 = 0;
    for (size_t i = 0; i < count && i < kMaxWatchSlots; ++i) {
        const DWORD64 lenBits = ((addresses[i] & 7) == 0) ? 0x2ull : 0x0ull;
        dr7 |= (0x1ull << (2 * i));
        dr7 |= (0x3ull << (16 + 4 * i));
        dr7 |= (lenBits << (18 + 4 * i));
    }
    return dr7;
}

void setHardwareWatchHere(CONTEXT* ctx, std::uintptr_t at)
{
    ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    ctx->Dr0 = at;
    ctx->Dr6 = 0;
    ctx->Dr7 = 0x1ull | (0x3ull << 16);
    g_watchSlots[0] = at;
    g_watchSlotCount = 1;
    g_hwThreadId = GetCurrentThreadId();
}

void clearHardwareWatch(CONTEXT* ctx)
{
    ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    ctx->Dr0 = 0;
    ctx->Dr1 = 0;
    ctx->Dr2 = 0;
    ctx->Dr3 = 0;
    ctx->Dr6 = 0;
    ctx->Dr7 = 0;
    g_watchSlotCount = 0;
}

LONG onWatchedRead(EXCEPTION_POINTERS* info, const EXCEPTION_RECORD& record)
{
    const auto base = reinterpret_cast<std::uintptr_t>(g_idPage);
    if (base == 0 || record.NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uintptr_t touched = record.ExceptionInformation[1];
    if (touched < base || touched >= base + kIdPageSize) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* const ctx = (info != nullptr) ? info->ContextRecord : nullptr;

    if (!g_inHandler.exchange(true)) {
        g_watchHits.fetch_add(1);

        if (t_mappingOut != nullptr && g_idMappingOut.load() == nullptr) {
            g_idMappingOut.store(t_mappingOut);
        }

        if (ctx != nullptr && g_hwStage.load() == 0) {
            g_hwStage.store(2);
            setHardwareWatchHere(ctx, base);
        }
        g_inHandler.store(false);
    }

    g_armAt.store(GetTickCount64() + kRearmDelayMs);
    return EXCEPTION_CONTINUE_EXECUTION;
}

size_t scanRegion(const char* base, size_t size, const char* needle, size_t needleLength,
                  std::uintptr_t* out, size_t maxOut, size_t found)
{
    __try {
        for (size_t i = 0; i + needleLength <= size && found < maxOut; ++i) {
            if (base[i] != needle[0]) {
                continue;
            }
            if (std::memcmp(base + i, needle, needleLength) != 0) {
                continue;
            }
            if (i == 0) {
                continue;
            }
            out[found++] = reinterpret_cast<std::uintptr_t>(base + i);
        }
    } __except (accessViolationFilter(GetExceptionCode())) {
    }
    return found;
}

size_t scanHeapForId(std::uintptr_t* out, size_t maxOut)
{
    const char* const needle = kOwnButtonId;
    const size_t needleLength = sizeof(kOwnButtonId);
    const auto ownPage = reinterpret_cast<std::uintptr_t>(g_idPage);

    size_t found = 0;
    unsigned long long scanned = 0;
    int regions = 0;
    MEMORY_BASIC_INFORMATION info{};
    std::uintptr_t at = 0;
    while (found < maxOut && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))
                                 == sizeof(info)) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const size_t size = info.RegionSize;
        if (size == 0) {
            break;
        }
        const bool readable = info.Protect == PAGE_READWRITE || info.Protect == PAGE_READONLY
                              || info.Protect == PAGE_WRITECOPY;
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && readable
            && base != ownPage && size >= needleLength) {
            found = scanRegion(reinterpret_cast<const char*>(base), size, needle, needleLength, out,
                               maxOut, found);
            scanned += size;
            ++regions;
        }
        at = base + size;
    }
    return found;
}

size_t scanRegionForPointers(const std::uintptr_t* base, size_t words,
                             const std::uintptr_t* targets, size_t targetCount,
                             std::uintptr_t* out, size_t maxOut, size_t found)
{
    __try {
        for (size_t i = 0; i < words && found < maxOut; ++i) {
            const std::uintptr_t value = base[i];
            if (value == 0) {
                continue;
            }
            for (size_t t = 0; t < targetCount; ++t) {
                if (value == targets[t]) {
                    out[found++] = reinterpret_cast<std::uintptr_t>(base + i);
                    break;
                }
            }
        }
    } __except (accessViolationFilter(GetExceptionCode())) {
    }
    return found;
}

size_t scanHeapForPointers(const std::uintptr_t* targets, size_t targetCount,
                           std::uintptr_t* out, size_t maxOut)
{
    size_t found = 0;
    int regions = 0;
    MEMORY_BASIC_INFORMATION info{};
    std::uintptr_t at = 0;
    while (found < maxOut
           && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info)) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const size_t size = info.RegionSize;
        if (size == 0) {
            break;
        }
        const bool readable = info.Protect == PAGE_READWRITE || info.Protect == PAGE_READONLY
                              || info.Protect == PAGE_WRITECOPY;
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && readable && (base & 7) == 0) {
            found = scanRegionForPointers(reinterpret_cast<const std::uintptr_t*>(base),
                                          size / sizeof(std::uintptr_t), targets, targetCount, out,
                                          maxOut, found);
            ++regions;
        }
        at = base + size;
    }
    return found;
}

LONG onHardwareHit(EXCEPTION_POINTERS* info, const EXCEPTION_RECORD& record)
{
    CONTEXT* const ctx = (info != nullptr) ? info->ContextRecord : nullptr;
    if (ctx == nullptr || (ctx->Dr6 & 0xFull) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD64 which = ctx->Dr6 & 0xFull;
    if (g_inHandler.exchange(true)) {
        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    int slot = 0;
    for (int i = 0; i < static_cast<int>(kMaxWatchSlots); ++i) {
        if ((which & (0x1ull << i)) != 0) {
            slot = i;
            break;
        }
    }

    const int hit = g_hwHits.fetch_add(1) + 1;

    if (t_varBag != nullptr && g_copyDestCount < kMaxCopyDest) {
        const auto box = reinterpret_cast<std::uintptr_t>(t_varBag) + kBagResolvedOffset;
        std::uintptr_t inner = 0;
        if (memory::isReadable(reinterpret_cast<const void*>(box), sizeof(inner))) {
            std::memcpy(&inner, reinterpret_cast<const void*>(box), sizeof(inner));
        }
        bool known = (inner == 0);
        for (size_t i = 0; i < g_copyDestCount && !known; ++i) {
            known = (g_copyDest[i] == inner);
        }
        if (!known && memory::isReadable(reinterpret_cast<const void*>(inner), 0x20)) {
            g_copyDest[g_copyDestCount++] = inner;
            g_swapAt.store(GetTickCount64() + kSwapDelayMs);
        }
    }

    if (t_mappingOut != nullptr && g_idMappingOut.load() == nullptr) {
        g_idMappingOut.store(t_mappingOut);
    }

    if (hit >= kMaxHwHits) {
        clearHardwareWatch(ctx);
        g_hwStage.store(4);
        log().warn(L"UiProbe: hit the watch limit ({}), removing the watch", kMaxHwHits);
    } else {
        ctx->Dr6 = 0;
    }
    g_inHandler.store(false);
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG CALLBACK crashProbe(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if (record.ExceptionCode == kGuardPageViolation) {
        return onWatchedRead(info, record);
    }
    if (record.ExceptionCode == static_cast<DWORD>(EXCEPTION_SINGLE_STEP)) {
        return onHardwareHit(info, record);
    }
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION
        && record.ExceptionCode != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_inHandler.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (g_crashLogs.fetch_add(1) < kMaxCrashLogs) {
        const auto at = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        std::uintptr_t kind = 0;
        std::uintptr_t touched = 0;
        if (record.NumberParameters >= 2) {
            kind = record.ExceptionInformation[0];
            touched = record.ExceptionInformation[1];
        }
        const wchar_t* const verb =
            (kind == 1) ? L"write" : (kind == 8) ? L"execute (DEP)" : L"read";
        log().error(L"UiProbe: access violation code={:#x} at {} ({:#x}) / {} address {:#x}{}",
                    record.ExceptionCode, codeName(at), at, verb, touched, ownName(touched));

        const CONTEXT* const ctx = info->ContextRecord;
        if (ctx != nullptr && memory::isReadable(ctx, sizeof(CONTEXT))) {
            log().error(L"UiProbe:   rax={:#x}{} rbx={:#x}{} rcx={:#x}{} rdx={:#x}{}", ctx->Rax,
                        ownName(ctx->Rax), ctx->Rbx, ownName(ctx->Rbx), ctx->Rcx,
                        ownName(ctx->Rcx), ctx->Rdx, ownName(ctx->Rdx));
            log().error(L"UiProbe:   rsi={:#x}{} rdi={:#x}{} rbp={:#x}{} rsp={:#x} r8={:#x}{}",
                        ctx->Rsi, ownName(ctx->Rsi), ctx->Rdi, ownName(ctx->Rdi), ctx->Rbp,
                        ownName(ctx->Rbp), ctx->Rsp, ctx->R8, ownName(ctx->R8));
        }
    }

    g_inHandler.store(false);
    return EXCEPTION_CONTINUE_SEARCH;
}

void maybeArmWatch()
{
    unsigned long long at = g_armAt.load();
    if (at == 0 || g_idPage == nullptr || GetTickCount64() < at) {
        return;
    }
    if (!g_armAt.compare_exchange_strong(at, 0)) {
        return;
    }
    armWatchNow();
}

bool writeDebugRegisters(DWORD threadId, const std::uintptr_t* addresses, size_t count)
{
    const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                     FALSE, threadId);
    if (thread == nullptr) {
        return false;
    }
    bool done = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &ctx) != 0) {
            ctx.Dr0 = (count > 0) ? addresses[0] : 0;
            ctx.Dr1 = (count > 1) ? addresses[1] : 0;
            ctx.Dr2 = (count > 2) ? addresses[2] : 0;
            ctx.Dr3 = (count > 3) ? addresses[3] : 0;
            ctx.Dr6 = 0;
            ctx.Dr7 = makeDr7(addresses, count);
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            done = SetThreadContext(thread, &ctx) != 0;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return done;
}

size_t armOnAllThreads(const std::uintptr_t* addresses, size_t count)
{
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    size_t done = 0;
    if (Thread32First(snapshot, &entry) != 0) {
        do {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
                continue;
            }
            if (writeDebugRegisters(entry.th32ThreadID, addresses, count)) {
                ++done;
            }
        } while (Thread32Next(snapshot, &entry) != 0);
    }
    CloseHandle(snapshot);
    return done;
}

void maybeArmHardwareWatch()
{
    if (g_hwStage.load() != 1) {
        return;
    }
    const unsigned long long at = g_hwArmAt.load();
    if (at == 0 || GetTickCount64() < at) {
        return;
    }
    g_hwStage.store(3);

    const size_t candidates = scanHeapForId(g_candidates, kMaxCandidates);
    if (candidates == 0) {
        log().warn(L"UiProbe: the id payload was not found on the heap");
        return;
    }

    const size_t owners = scanHeapForPointers(g_candidates, candidates, g_owners, kMaxCandidates);
    if (owners == 0) {
        log().warn(L"UiProbe: found nothing pointing at the payload");
        return;
    }

    constexpr std::uintptr_t kIdLength = sizeof(kOwnButtonId) - 1;
    g_watchSlotCount = 0;
    for (size_t i = 0; i < owners; ++i) {
        const auto owner = g_owners[i];
        std::uintptr_t size = 0;
        if (memory::isReadable(reinterpret_cast<const void*>(owner + 0x10), sizeof(size))) {
            std::memcpy(&size, reinterpret_cast<const void*>(owner + 0x10), sizeof(size));
        }
        const bool isString = size == kIdLength;
        if (isString) {
        }
        if (isString && g_watchSlotCount < kMaxWatchSlots) {
            g_watchSlots[g_watchSlotCount++] = owner;
        }
    }
    if (g_watchSlotCount == 0) {
        log().warn(L"UiProbe: could not find the std::string body");
        return;
    }
    const size_t threads = armOnAllThreads(g_watchSlots, g_watchSlotCount);
    if (threads > 0) {
        g_hwStage.store(2);
    } else {
        g_watchSlotCount = 0;
        log().warn(L"UiProbe: could not arm the watch (err {})", GetLastError());
    }
}

void maybeSwapToCopyDest()
{
    const unsigned long long at = g_swapAt.load();
    if (at == 0 || g_copyDestCount == 0 || GetTickCount64() < at) {
        return;
    }
    g_swapAt.store(0);

    const size_t threads = armOnAllThreads(g_copyDest, g_copyDestCount);
    if (threads == 0) {
        log().warn(L"UiProbe: could not swap in our own copy (err {})", GetLastError());
        return;
    }
    for (size_t i = 0; i < g_copyDestCount; ++i) {
        g_watchSlots[i] = g_copyDest[i];
    }
    g_watchSlotCount = g_copyDestCount;
    g_hwHits.store(0);
    g_hwStage.store(2);
}

void disarmHardwareWatch()
{
    if (g_watchSlotCount == 0) {
        return;
    }
    g_hwStage.store(3);
    g_watchSlotCount = 0;
    g_hwThreadId = 0;
}

std::atomic<bool> g_watchStop{false};
HANDLE g_watchThread = nullptr;

DWORD WINAPI watchThreadMain(LPVOID)
{
    while (!g_watchStop.load()) {
        maybeArmWatch();
        maybeArmHardwareWatch();
        maybeSwapToCopyDest();
        if (g_hwStage.load() == 4) {
            g_watchSlotCount = kMaxWatchSlots;
            disarmHardwareWatch();
        }
        Sleep(200);
    }
    return 0;
}

}

int menuIndexFromName(const std::wstring& name, const wchar_t* suffix);

void forgetNumBags();

void installCrashProbe()
{
    if (!kSubstituteEnabled || g_vehHandle != nullptr) {
        return;
    }

    noteModule(GetModuleHandleW(nullptr), g_exeBase, g_exeSize);
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&installCrashProbe), &self)
        != 0) {
        noteModule(self, g_selfBase, g_selfSize);
    }

    g_vehHandle = AddVectoredExceptionHandler(1, &crashProbe);
    if (g_vehHandle == nullptr) {
        log().warn(L"UiProbe: could not install the crash probe");
        return;
    }

    g_watchStop.store(false);
    g_watchThread = CreateThread(nullptr, 0, &watchThreadMain, nullptr, 0, nullptr);
    if (g_watchThread == nullptr) {
        log().warn(L"UiProbe: could not create the watch polling thread");
    }
}

void removeCrashProbe()
{
    if (g_watchThread != nullptr) {
        g_watchStop.store(true);
        WaitForSingleObject(g_watchThread, 3000);
        CloseHandle(g_watchThread);
        g_watchThread = nullptr;
    }
    if (g_idPage != nullptr) {
        DWORD previous = 0;
        VirtualProtect(g_idPage, kIdPageSize, PAGE_READWRITE, &previous);
    }
    disarmHardwareWatch();

    if (g_vehHandle == nullptr) {
        return;
    }
    RemoveVectoredExceptionHandler(g_vehHandle);
    g_vehHandle = nullptr;

    for (int i = 0; i < 100 && g_inHandler.load(); ++i) {
        Sleep(1);
    }
}

void onLookup(const void* space, const void* name)
{
    const unsigned long long calls = g_calls.fetch_add(1) + 1;

    std::wstring text = readString(space);
    if (text.empty()) {
        return;
    }
    const std::wstring nameText = readString(name);

    if (const unsigned long long at = g_ownNavInvokedAt.load(); at != 0) {
        if (GetTickCount64() - at > 4000) {
            g_ownNavInvokedAt.store(0);
        } else {
            if (g_afterInvokeLogs.fetch_add(1) < 60) {
                log().info(L"UiProbe: after invoke -> {}.{}", text, nameText);
            }
            if (nameText.rfind(L"screen_", 0) == 0) {
                dumpStackOnce(L"screen", 0, 2);
            }
        }
    }

    if (nameText.empty()) {
        return;
    }
    text.push_back(L'/');
    text += nameText;

    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_capped) {
        return;
    }
    const auto [inserted, isNew] = g_seen.insert(std::move(text));
    if (!isNew) {
        return;
    }
    if (g_seen.size() > kMaxUnique) {
        g_capped = true;
        log().warn(L"UiProbe: stopped listing at {} kinds (calls {})", kMaxUnique, calls);
        return;
    }
}

void onLookupResult(const void* space, const void* name, void* value)
{
    if (value == nullptr) {
        return;
    }

    std::wstring text = readString(space);
    if (text.empty()) {
        return;
    }
    const std::wstring nameText = readString(name);
    if (nameText.empty()) {
        return;
    }
    text.push_back(L'/');
    text += nameText;

    bool watched = false;
    for (const wchar_t* const candidate : kWatched) {
        if (text == candidate) {
            watched = true;
            break;
        }
    }
    if (!watched) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!g_dumped.insert(text).second) {
            return;
        }
    }
    dumpNode(text, value);
}

constexpr size_t kMaxPageRows = 96;
constexpr size_t kPageKeyBytes = 96;
constexpr size_t kPageTextBytes = 64;

constexpr size_t kValueWords = 8;

char g_pgKeys[kMaxPageRows][kPageKeyBytes]{};
char g_pgTexts[kMaxPageRows][kPageTextBytes]{};
alignas(8) unsigned char g_pgTextRecs[kMaxPageRows][16]{};
TreeNode g_pgOverHeads[kMaxPageRows]{};
TreeNode g_pgOverNodes[kMaxPageRows][10]{};
std::uintptr_t g_pgOverNodePtrs[kMaxPageRows][10]{};
alignas(8) unsigned char g_pgTgTypeRecs[kMaxPageRows][16]{};
alignas(8) unsigned char g_pgTgBindRecs[kMaxPageRows][16]{};

char g_pgCtlNames[kMaxPageRows][kPageKeyBytes]{};
char g_pgBindNames[kMaxPageRows][32]{};
char g_pgLabels[kMaxPageRows][kPageTextBytes]{};
char g_pgHints[kMaxPageRows][kPageTextBytes]{};
alignas(8) unsigned char g_pgCtlRecs[kMaxPageRows][16]{};
alignas(8) unsigned char g_pgBindRecs[kMaxPageRows][16]{};
alignas(8) unsigned char g_pgLabelRecs[kMaxPageRows][16]{};
alignas(8) unsigned char g_pgHintRecs[kMaxPageRows][16]{};
bool g_pgIsEdit[kMaxPageRows]{};
constexpr int kRowButton = 0;
constexpr int kRowEdit = 1;
constexpr int kRowSlider = 2;
constexpr int kRowToggle = 3;
constexpr int kRowLabel = 4;
constexpr int kRowHeader = 5;
constexpr int kRowDivider = 6;
constexpr int kRowColumns = 7;

bool pageRowIsPlain(int kind)
{
    return kind == kRowLabel || kind == kRowHeader || kind == kRowDivider
           || kind == kRowColumns;
}
int g_pgRowKind[kMaxPageRows]{};
std::int32_t g_pgRowIcon[kMaxPageRows]{};
bool g_pgRowHasIcon[kMaxPageRows]{};
bool g_pgRowIconCol[kMaxPageRows]{};
int g_pgRowNameW[kMaxPageRows]{};

constexpr int kActItem = 0;
constexpr int kActGoto = 1;
constexpr int kActNone = 2;
constexpr int kActShow = 3;
int g_pgRowAct[kMaxPageRows]{};
int g_pgRowArg[kMaxPageRows]{};

bool g_pgRowChild[kMaxPageRows]{};
int g_pgRowKids[kMaxPageRows]{};
char g_pgRowWidth[kMaxPageRows][8]{};
std::uintptr_t g_pgTopElems[kMaxPageRows]{};
size_t g_pgTopCount = 0;
int g_pgRowMenuTab[kMaxPageRows]{};
int g_pgRowMenuIdx[kMaxPageRows]{};
int g_pgRowTab[kMaxPageRows]{};
int g_pgRowBlueprint[kMaxPageRows]{};
constexpr size_t kMaxTabsForRows = 20;
size_t g_pgTabStart[kMaxTabsForRows]{};
size_t g_pgTabRows[kMaxTabsForRows]{};

bool pageRowBorrows(int kind)
{
    return kind == kRowEdit || kind == kRowSlider || kind == kRowToggle;
}

const char* pageRowControlName(int kind)
{
    switch (kind) {
    case kRowSlider: return "option_slider_control";
    case kRowToggle: return "option_toggle_control";
    default:         return "option_text_edit_control";
    }
}
std::atomic<std::uintptr_t> g_pgCtl[kMaxPageRows]{};
char g_pgLastText[kMaxPageRows][kPageTextBytes]{};
TreeNode g_pgItemHeads[kMaxPageRows]{};
TreeNode g_pgItemNodes[kMaxPageRows][2]{};
std::uintptr_t g_pgItemNodePtrs[kMaxPageRows][4]{};
std::uintptr_t g_pgItemValues[kMaxPageRows][kValueWords]{};
std::uintptr_t g_pgElems[kMaxPageRows]{};
std::uintptr_t g_pgVec[4]{};
alignas(8) unsigned char g_pgSizeRecs[kMaxPageRows][2][16]{};
std::uintptr_t g_pgSizeElems[kMaxPageRows][2][4]{};
std::uintptr_t g_pgSizeElemPtrs[kMaxPageRows][2]{};
std::uintptr_t g_pgSizeVec[kMaxPageRows][4]{};
constexpr char kPageSizeKey[] = "size";
constexpr char kPageRowWidth[] = "100% - 4px";
constexpr char kPageRowHeight[] = "24px";
constexpr char kPageTabColWidth[] = "30%";
constexpr char kPageOneColWidth[] = "100% - 4px";
constexpr char kPageRowColWidth[] = "70% - 16px";
constexpr char kPageColHeight[] = "100%";
constexpr char kPageOffsetKey[] = "offset";
constexpr char kPageInsetX[] = "8px";
constexpr char kPageInsetY[] = "21px";
constexpr char kPageBodyWidth[] = "100% - 16px";
constexpr char kPageBodyHeight[] = "100% - 21px";
char g_pgBodyW[16] = "58%";
char g_pgBodyX[16] = "0px";
constexpr char kBodyWideW[] = "78%";
constexpr char kBodyNarrowW[] = "58%";
alignas(8) unsigned char g_pgInsetRecs[2][2][16]{};
std::uintptr_t g_pgInsetElems[2][2][4]{};
std::uintptr_t g_pgInsetElemPtrs[2][2]{};
std::uintptr_t g_pgInsetVec[2][4]{};
TreeNode g_pgHead{};
TreeNode g_pgNodes[kMaxOwnKeys]{};
std::uintptr_t g_pgNode[4]{};
std::uintptr_t g_pgValue[kValueWords]{};
bool g_pgReady = false;
std::atomic<void*> g_pgBag{nullptr};

std::atomic<int> g_pgPressed{-1};

constexpr const char* kPageTabNames[] = {"General", "Blueprint"};
constexpr size_t kMaxPageTabs = std::size(kPageTabNames);
constexpr size_t kLeftTabCount = 1;
constexpr int kEmptyTab = 1;

constexpr size_t kMaxPageFiles = 14;
constexpr size_t kMaxLeftRows = kMaxPageTabs + kMaxPageFiles;
constexpr int kLeftTab = 0;
constexpr int kLeftFile = 1;
int g_ptKind[kMaxLeftRows]{};
int g_ptIndex[kMaxLeftRows]{};
size_t g_ptCount = 0;
std::atomic<int> g_ptLastPressed{-1};

constexpr char kPtListName[] = "settings_common.tk_ptlist";
constexpr char kPtListVar[] = "$scrolling_content";
constexpr char kPtScrollSizeVar[] = "$scroll_size";
TreeNode g_ptListHead{};
TreeNode g_ptListNodes[kMaxOwnKeys]{};
constexpr size_t kListWords = 8;
std::uintptr_t g_ptListNodePtrs[kListWords]{};
std::uintptr_t g_ptListValue[kListWords]{};
bool g_ptListReady = false;

constexpr size_t kViewWords = 8;
TreeNode g_pgViewHead{};
TreeNode g_pgViewNodes[8]{};
std::uintptr_t g_pgViewNodePtrs[kViewWords]{};
std::uintptr_t g_pgViewValue[kViewWords]{};
std::uintptr_t g_pgViewElem[2]{};
std::uintptr_t g_pgViewVec[kViewWords]{};
char g_pgViewName[48]{};
alignas(8) unsigned char g_pgViewNameRec[16]{};
std::uintptr_t g_pgViewKey = 0;
bool g_pgViewReady = false;

constexpr size_t kBodyCount = kMaxLeftRows;
char g_pgBodyKeys[kBodyCount][kPageKeyBytes]{};
std::uintptr_t g_pgBodyRowVec[kBodyCount][kViewWords]{};
TreeNode g_pgBodyHeads[kBodyCount]{};
TreeNode g_pgBodyNodes[kBodyCount][kMaxOwnKeys]{};
std::uintptr_t g_pgBodyNodePtrs[kBodyCount][kViewWords]{};
std::uintptr_t g_pgBodyValue[kBodyCount][kViewWords]{};
std::uintptr_t g_pgBodyElems[kBodyCount]{};
std::uintptr_t g_pgBodyVec[kViewWords]{};
TreeNode g_pgBodyViewHeads[kBodyCount]{};
TreeNode g_pgBodyViewNodes[kBodyCount][8]{};
std::uintptr_t g_pgBodyViewNodePtrs[kBodyCount][kViewWords]{};
std::uintptr_t g_pgBodyViewValue[kBodyCount][kViewWords]{};
std::uintptr_t g_pgBodyViewElem[kBodyCount][2]{};
std::uintptr_t g_pgBodyViewVec[kBodyCount][kViewWords]{};
char g_pgBodyViewNames[kBodyCount][48]{};
alignas(8) unsigned char g_pgBodyViewNameRecs[kBodyCount][16]{};
alignas(8) unsigned char g_pgBodyPropRec[16]{};
TreeNode g_pgItemHeads2[kBodyCount]{};
TreeNode g_pgItemNodes2[kBodyCount][1]{};
std::uintptr_t g_pgItemNodePtrs2[kBodyCount][kViewWords]{};
std::uintptr_t g_pgItemValues2[kBodyCount][kViewWords]{};
bool g_pgBodyReady = false;

alignas(8) unsigned char g_ptListSizeRecs[2][16]{};
std::uintptr_t g_ptListSizeElems[2][4]{};
std::uintptr_t g_ptListSizeElemPtrs[2]{};
std::uintptr_t g_ptListSizeVec[4]{};
alignas(8) unsigned char g_ptListNameRec[16]{};

std::atomic<int> g_pgTab{0};
std::atomic<int> g_ptPressed{-1};

char g_ptKeys[kMaxLeftRows][kPageKeyBytes]{};
char g_ptTexts[kMaxLeftRows][kPageTextBytes]{};
alignas(8) unsigned char g_ptTextRecs[kMaxLeftRows][16]{};
alignas(8) unsigned char g_ptExitRecs[kMaxLeftRows][16]{};
TreeNode g_ptOverHeads[kMaxLeftRows]{};
TreeNode g_ptOverNodes[kMaxLeftRows][12]{};
std::uintptr_t g_ptOverNodePtrs[kMaxLeftRows][12]{};
alignas(8) unsigned char g_ptGlyphRecs[kMaxLeftRows][2][16]{};
alignas(8) unsigned char g_ptRadioRecs[kMaxLeftRows][16]{};
alignas(8) unsigned char g_ptTypeRecs[kMaxLeftRows][16]{};
alignas(8) unsigned char g_ptBindRecs2[kMaxLeftRows][16]{};
alignas(8) unsigned char g_ptViewRecs[kMaxLeftRows][16]{};
char g_ptBindNames[kMaxLeftRows][32]{};
char g_ptViewNames[kMaxLeftRows][32]{};
TreeNode g_ptItemHeads[kMaxLeftRows]{};
TreeNode g_ptItemNodes[kMaxLeftRows][2]{};
std::uintptr_t g_ptItemNodePtrs[kMaxLeftRows][4]{};
std::uintptr_t g_ptItemValues[kMaxLeftRows][kValueWords]{};
std::uintptr_t g_ptElems[kMaxLeftRows]{};
std::uintptr_t g_ptVec[4]{};
alignas(8) unsigned char g_ptSizeRecs[kMaxLeftRows][2][16]{};
std::uintptr_t g_ptSizeElems[kMaxLeftRows][2][4]{};
std::uintptr_t g_ptSizeElemPtrs[kMaxLeftRows][2]{};
std::uintptr_t g_ptSizeVec[kMaxLeftRows][4]{};

char g_pgColKeys[2][kPageKeyBytes]{};
TreeNode g_pgColOverHeads[2]{};
TreeNode g_pgColOverNodes[2][4]{};
std::uintptr_t g_pgColOverNodePtrs[2][4]{};
TreeNode g_pgColHeads[2]{};
TreeNode g_pgColNodes[2][2]{};
std::uintptr_t g_pgColNodePtrs[2][4]{};
std::uintptr_t g_pgColValues[2][kValueWords]{};
std::uintptr_t g_pgColElems[2]{};
std::uintptr_t g_pgColVec[4]{};
alignas(8) unsigned char g_pgColSizeRecs[2][2][16]{};
std::uintptr_t g_pgColSizeElems[2][2][4]{};
std::uintptr_t g_pgColSizeElemPtrs[2][2]{};
std::uintptr_t g_pgColSizeVec[2][4]{};

constexpr char kPageExitId[] = "button.menu_exit";
constexpr char kPageExitVar[] = "$pressed_button_name";
alignas(8) unsigned char g_pgExitRecs[kMaxPageRows][16]{};

std::atomic<bool> g_pgCloseOnPress{false};

std::atomic<unsigned long long> g_pgReopenAt{0};

std::atomic<unsigned long long> g_pgSelfEscUntil{0};

std::atomic<unsigned long long> g_pgCloseDeadline{0};
constexpr unsigned long long kPageCloseWaitMs = 2000;
constexpr unsigned long long kPageAfterCloseMs = 80;

void noteScreenClosed()
{
    if (g_pgCloseDeadline.exchange(0) == 0) {
        return;
    }
    g_pgReopenAt.store(GetTickCount64() + kPageAfterCloseMs);
}

bool selfEscapeInFlight()
{
    const unsigned long long until = g_pgSelfEscUntil.load(std::memory_order_relaxed);
    return until != 0 && GetTickCount64() < until;
}

void sendEscapeToGame()
{
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front != nullptr) {
        GetWindowThreadProcessId(front, &pid);
    }
    if (pid != GetCurrentProcessId()) {
        static std::atomic<int> saidSkip{0};
        if (saidSkip.fetch_add(1) < 8) {
            log().warn(L"UiProbe: the game is not in the foreground, so ESC was not sent (one "
                       L"screen stays stacked)");
        }
        return;
    }
    INPUT keys[2]{};
    keys[0].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_ESCAPE;
    keys[1] = keys[0];
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    g_pgSelfEscUntil.store(GetTickCount64() + 500, std::memory_order_relaxed);
    SendInput(2, keys, sizeof(INPUT));
}

void cancelPageReopen()
{
    g_ptPressed.store(-1);
    g_pgPressed.store(-1);
}
constexpr unsigned long long kPageReopenDelayMs = 120;

bool schematicaRowText(size_t index, char* out, size_t cap);

MenuItem* schematicaRow(size_t index);
MenuItem* schematicaRowOf(int tab, size_t index);
bool schematicaRowTextOf(int tab, size_t index, char* out, size_t cap);
void copyAscii(char* dest, size_t cap, const wchar_t* text);

void forgetPageSliders();
void forgetToggleState();

size_t buildPageTexts()
{
    (void)pageHeaderTitle();
    forgetPageSliders();
    forgetToggleState();
    Schematica& mod = Schematica::instance();
    size_t rows = 0;
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        g_pgRowIcon[i] = 0;
        g_pgRowHasIcon[i] = false;
        g_pgRowIconCol[i] = false;
        g_pgRowNameW[i] = 0;
    }

    const auto addItem = [&](int tab, size_t index) -> bool {
        if (rows >= kMaxPageRows) {
            return false;
        }
        const MenuItem* const row = schematicaRowOf(tab, index);
        if (row == nullptr) {
            return false;
        }
        if (!schematicaRowTextOf(tab, index, g_pgTexts[rows], kPageTextBytes)) {
            return false;
        }
        copyAscii(g_pgLabels[rows], kPageTextBytes, row->labelText().c_str());
        copyAscii(g_pgHints[rows], kPageTextBytes, row->valueText().c_str());
        g_pgRowAct[rows] = kActItem;
        g_pgRowArg[rows] = 0;
        g_pgRowMenuTab[rows] = tab;
        g_pgRowMenuIdx[rows] = static_cast<int>(index);
        g_pgRowTab[rows] = 0;
        g_pgRowBlueprint[rows] = (tab == 1) ? mod.editingIndex() : -1;
        if (row->kind == MenuItemKind::Text && row->getText && row->setText) {
            g_pgRowKind[rows] = kRowEdit;
        } else if (row->kind == MenuItemKind::Number && row->getNumber && row->setNumber) {
            g_pgRowKind[rows] = kRowSlider;
        } else if (row->kind == MenuItemKind::Toggle) {
            g_pgRowKind[rows] = kRowToggle;
        } else {
            g_pgRowKind[rows] = kRowButton;
        }
        g_pgIsEdit[rows] = (g_pgRowKind[rows] == kRowEdit);
        g_pgRowChild[rows] = false;
        g_pgRowKids[rows] = 0;
        g_pgRowWidth[rows][0] = 0;
        ++rows;
        return true;
    };

    const auto addPlain = [&](int kind, const char* text) {
        if (rows >= kMaxPageRows) {
            return;
        }
        std::snprintf(g_pgTexts[rows], kPageTextBytes, "%s", (text != nullptr) ? text : "");
        g_pgLabels[rows][0] = 0;
        g_pgHints[rows][0] = 0;
        g_pgRowAct[rows] = kActNone;
        g_pgRowArg[rows] = 0;
        g_pgRowMenuTab[rows] = -1;
        g_pgRowMenuIdx[rows] = -1;
        g_pgRowTab[rows] = 0;
        g_pgRowBlueprint[rows] = -1;
        g_pgRowKind[rows] = kind;
        g_pgIsEdit[rows] = false;
        g_pgRowChild[rows] = false;
        g_pgRowKids[rows] = 0;
        g_pgRowWidth[rows][0] = 0;
        ++rows;
    };

    const auto addGoto = [&](const char* text, int screen, int blueprint) {
        if (rows >= kMaxPageRows) {
            return;
        }
        std::snprintf(g_pgTexts[rows], kPageTextBytes, "%s", text);
        g_pgLabels[rows][0] = '\0';
        g_pgHints[rows][0] = '\0';
        g_pgRowAct[rows] = kActGoto;
        g_pgRowArg[rows] = screen;
        g_pgRowMenuTab[rows] = -1;
        g_pgRowMenuIdx[rows] = -1;
        g_pgRowTab[rows] = 0;
        g_pgRowBlueprint[rows] = blueprint;
        g_pgRowKind[rows] = kRowButton;
        g_pgIsEdit[rows] = false;
        g_pgRowChild[rows] = false;
        g_pgRowKids[rows] = 0;
        g_pgRowWidth[rows][0] = 0;
        ++rows;
    };

    const auto addColumns = [&](const char* name, const char* count, bool header, bool iconCol,
                                bool hasIcon, std::int32_t icon, int nameW,
                                const char* stacks = nullptr) {
        if (rows >= kMaxPageRows) {
            return;
        }
        const auto putText = [](char* dest, const char* src) {
            std::snprintf(dest, kPageTextBytes, "%s", (src != nullptr) ? src : "");
            if (dest[0] == '(') {
                dest[0] = '[';
            }
        };
        putText(g_pgTexts[rows], name);
        putText(g_pgHints[rows], count);
        if (stacks != nullptr && stacks[0] != 0) {
            putText(g_pgLabels[rows], stacks);
        } else {
            g_pgLabels[rows][0] = 0;
        }
        g_pgRowAct[rows] = kActNone;
        g_pgRowArg[rows] = header ? 1 : 0;
        g_pgRowMenuTab[rows] = -1;
        g_pgRowMenuIdx[rows] = -1;
        g_pgRowTab[rows] = 0;
        g_pgRowBlueprint[rows] = -1;
        g_pgRowKind[rows] = kRowColumns;
        g_pgIsEdit[rows] = false;
        g_pgRowChild[rows] = false;
        g_pgRowKids[rows] = 0;
        g_pgRowWidth[rows][0] = 0;
        g_pgRowIconCol[rows] = iconCol;
        g_pgRowHasIcon[rows] = iconCol && hasIcon && !header;
        g_pgRowIcon[rows] = icon;
        g_pgRowNameW[rows] = nameW;
        ++rows;
    };
    const auto nameWidthFor = [](size_t longest) {
        const int want = static_cast<int>(longest) * 6 + 24;
        return std::clamp(want, 96, 420);
    };

    const auto addListRow = [&](size_t which) {
        if (rows + 4 > kMaxPageRows) {
            return;
        }
        char name[kPageTextBytes]{};
        copyAscii(name, sizeof(name), mod.blueprintName(which).c_str());
        const bool shown = mod.blueprintVisible(which);

        const size_t parent = rows;
        g_pgTexts[parent][0] = 0;
        g_pgLabels[parent][0] = 0;
        g_pgHints[parent][0] = 0;
        g_pgRowAct[parent] = kActNone;
        g_pgRowArg[parent] = 0;
        g_pgRowMenuTab[parent] = -1;
        g_pgRowMenuIdx[parent] = -1;
        g_pgRowTab[parent] = 0;
        g_pgRowBlueprint[parent] = static_cast<int>(which);
        g_pgRowKind[parent] = kRowButton;
        g_pgIsEdit[parent] = false;
        g_pgRowChild[parent] = false;
        g_pgRowKids[parent] = 3;
        g_pgRowWidth[parent][0] = 0;
        ++rows;

        const auto kid = [&](const char* text, int act, int arg, const char* width) {
            std::snprintf(g_pgTexts[rows], kPageTextBytes, "%s", text);
            g_pgLabels[rows][0] = 0;
            g_pgHints[rows][0] = 0;
            g_pgRowAct[rows] = act;
            g_pgRowArg[rows] = arg;
            g_pgRowMenuTab[rows] = -1;
            g_pgRowMenuIdx[rows] = -1;
            g_pgRowTab[rows] = 0;
            g_pgRowBlueprint[rows] = static_cast<int>(which);
            g_pgRowKind[rows] = kRowButton;
            g_pgIsEdit[rows] = false;
            g_pgRowChild[rows] = true;
            g_pgRowKids[rows] = 0;
            std::snprintf(g_pgRowWidth[rows], sizeof(g_pgRowWidth[rows]), "%s", width);
            ++rows;
        };
        char label[kPageTextBytes]{};
        std::snprintf(label, sizeof(label), "%s %s", shown ? "*" : " ", name);
        kid(label, kActGoto, kScreenBlueprint, "52%");
        kid(shown ? "Placement: ON" : "Placement: OFF", kActShow, 0, "26%");
        {
            const MenuItem* const del = schematicaRowOf(1, 4);
            std::wstring armed = (del != nullptr) ? del->valueText() : std::wstring{};
            char armedText[kPageTextBytes]{};
            copyAscii(armedText, sizeof(armedText), armed.c_str());
            const bool armedHere =
                (armed.rfind(L"press again: ", 0) == 0)
                && (std::strstr(armedText, name) != nullptr) && (name[0] != 0);
            std::snprintf(g_pgTexts[rows], kPageTextBytes, "%s",
                          armedHere ? "Remove: sure?" : "Remove");
            g_pgLabels[rows][0] = 0;
            g_pgHints[rows][0] = 0;
            g_pgRowAct[rows] = kActItem;
            g_pgRowArg[rows] = 0;
            g_pgRowMenuTab[rows] = 1;
            g_pgRowMenuIdx[rows] = 4;
            g_pgRowTab[rows] = 0;
            g_pgRowBlueprint[rows] = static_cast<int>(which);
            g_pgRowKind[rows] = kRowButton;
            g_pgIsEdit[rows] = false;
            g_pgRowChild[rows] = true;
            g_pgRowKids[rows] = 0;
            std::snprintf(g_pgRowWidth[rows], sizeof(g_pgRowWidth[rows]), "%s", "20%");
            ++rows;
        }
    };

    switch (g_pgScreen.load(std::memory_order_relaxed)) {
    case kScreenBlueprints: {
        const size_t count = mod.blueprintCount();
        for (size_t i = 0; i < count && rows + 5 < kMaxPageRows; ++i) {
            addListRow(i);
        }
        if (count == 0) {
            addPlain(kRowLabel, "No blueprints: use Import .mcstructure");
        }
        addGoto("< Schematica menu", kScreenMenu, -1);
        break;
    }
    case kScreenBlueprint: {
        {
            char name[kPageTextBytes]{};
            const int at = mod.editingIndex();
            if (at >= 0) {
                copyAscii(name, sizeof(name), mod.blueprintName(static_cast<size_t>(at)).c_str());
            }
            char text[kPageTextBytes]{};
            std::snprintf(text, sizeof(text), "Schematic: %s", (name[0] != 0) ? name : "(none)");
            addPlain(kRowLabel, text);
        }
        {
            const int at = mod.editingIndex();
            int sx = 0;
            int sy = 0;
            int sz = 0;
            std::size_t solid = 0;
            char text[kPageTextBytes]{};
            if (at >= 0 && mod.blueprintInfo(static_cast<size_t>(at), sx, sy, sz, solid)) {
                std::snprintf(text, sizeof(text), "Enclosing size: %d x %d x %d  (%u blocks)",
                              sx, sy, sz, static_cast<unsigned>(solid));
            } else {
                std::snprintf(text, sizeof(text), "Enclosing size: not loaded yet");
            }
            addPlain(kRowLabel, text);
        }
        addItem(1, 0);
        addPlain(kRowHeader, "Placement origin");
        addItem(1, 1);
        addItem(1, 2);
        addItem(1, 3);
        addItem(1, 6);
        addItem(1, 5);
        addPlain(kRowDivider, nullptr);
        addGoto("Material List", kScreenMaterials, -1);
        addGoto("Schematic Verifier", kScreenVerifier, -1);
        addPlain(kRowDivider, nullptr);
        addItem(1, 4);
        addGoto("< Blueprints", kScreenBlueprints, -1);
        break;
    }
    case kScreenMaterials: {
        const int at = mod.editingIndex();
        std::size_t kinds = 0;
        const size_t room = (kMaxPageRows > rows + 5) ? (kMaxPageRows - rows - 5) : 0;
        const auto list = (at >= 0)
                              ? mod.blueprintMaterials(static_cast<size_t>(at), room, &kinds)
                              : std::vector<Schematica::MaterialRow>{};
        std::vector<std::string> names;
        names.reserve(list.size());
        size_t longest = std::strlen("Item");
        for (const auto& one : list) {
            char name[kPageTextBytes]{};
            copyAscii(name, sizeof(name), one.name.c_str());
            const char* body = name;
            if (const char* colon = std::strchr(name, ':'); colon != nullptr) {
                body = colon + 1;
            }
            std::string shown(body);
            if (shown.size() > 40) {
                shown = shown.substr(0, 37) + "...";
            }
            longest = std::max(longest, shown.size());
            names.push_back(std::move(shown));
        }
        const int nameW = nameWidthFor(longest);
        addColumns("Item", "Total", true, true, false, 0, nameW, "SB+stacks");
        if (at < 0 || list.empty()) {
            addPlain(kRowLabel, "- nothing loaded yet: turn Show on once");
        }
        for (size_t i = 0; i < list.size(); ++i) {
            char count[24]{};
            std::snprintf(count, sizeof(count), "%u", static_cast<unsigned>(list[i].count));
            char rawName[kPageTextBytes]{};
            copyAscii(rawName, sizeof(rawName), list[i].name.c_str());
            const int stackSize = (list[i].stackSize > 0)
                                      ? list[i].stackSize
                                      : stackcount::maxStackSize(rawName);
            const std::string stacks = stackcount::formatStacks(list[i].count, stackSize);
            addColumns(names[i].c_str(), count, false, true, list[i].hasIcon, list[i].iconIdAux,
                       nameW, stacks.c_str());
        }
        if (kinds > list.size()) {
            char more[kPageTextBytes]{};
            std::snprintf(more, sizeof(more), "... and %u more (not shown)",
                          static_cast<unsigned>(kinds - list.size()));
            addPlain(kRowLabel, more);
        }
        addPlain(kRowDivider, nullptr);
        addGoto("< Configure", kScreenBlueprint, -1);
        break;
    }
    case kScreenVerifier: {
        std::size_t tally[6]{};
        mod.diffTally(tally);
        const std::size_t seen = tally[0] + tally[1] + tally[2] + tally[3] + tally[4] + tally[5];
        const struct {
            const char* label;
            std::size_t at;
        } kRowsOf[] = {
            {"Correct", 0}, {"Missing", 1},        {"Wrong block", 2},
            {"Wrong state", 3}, {"World not read", 4}, {"State unresolved", 5},
        };
        size_t longest = std::strlen("Status");
        for (const auto& one : kRowsOf) {
            longest = std::max(longest, std::strlen(one.label));
        }
        const int nameW = nameWidthFor(longest);
        addColumns("Status", "Count", true, false, false, 0, nameW);
        if (seen == 0) {
            addPlain(kRowLabel, "- no result yet: turn Show on and wait");
        }
        for (const auto& one : kRowsOf) {
            char count[24]{};
            std::snprintf(count, sizeof(count), "%u", static_cast<unsigned>(tally[one.at]));
            addColumns(one.label, count, false, false, false, 0, nameW);
        }
        addPlain(kRowDivider, nullptr);
        addGoto("< Configure", kScreenBlueprint, -1);
        break;
    }
    case kScreenSettings: {
        addPlain(kRowHeader, "Visuals");
        for (size_t i = 1; i < kMaxPageRows; ++i) {
            if (!addItem(0, i)) {
                break;
            }
        }
        addGoto("< Schematica menu", kScreenMenu, -1);
        break;
    }
    case kScreenLayers: {
        const int layerMode = mod.layerMode();
        addItem(2, 0);
        addItem(2, 1);
        if (layerMode == Schematica::kLayerSingle || layerMode == Schematica::kLayerBelow
            || layerMode == Schematica::kLayerAbove) {
            addItem(2, 2);
        } else if (layerMode == Schematica::kLayerRange) {
            addItem(2, 3);
            addItem(2, 4);
        }
        {
            bool on = false;
            int axis = 1;
            int lo = 0;
            int hi = 0;
            mod.layerRangeOffsets(on, axis, lo, hi);
            const char axisName = (axis == 0) ? 'x' : ((axis == 2) ? 'z' : 'y');
            {
                int ox = 0;
                int oy = 0;
                int oz = 0;
                std::wstring who;
                char name[kPageTextBytes]{};
                char text[kPageTextBytes]{};
                if (mod.layerOrigin(ox, oy, oz, &who)) {
                    copyAscii(name, sizeof(name), who.c_str());
                    std::snprintf(text, sizeof(text), "Offset from: %s  [%d, %d, %d]", name,
                                  ox, oy, oz);
                } else {
                    std::snprintf(text, sizeof(text),
                                  "Offset from: no schematic - using world coordinates");
                }
                addPlain(kRowLabel, text);
            }
            char text[kPageTextBytes]{};
            constexpr int kFarShown = 1 << 20;
            const auto edge = [](char* into, size_t room, int value) {
                if (value <= -kFarShown) {
                    std::snprintf(into, room, "...");
                } else if (value >= kFarShown) {
                    std::snprintf(into, room, "...");
                } else {
                    std::snprintf(into, room, "%d", value);
                }
            };
            char lowText[16]{};
            char highText[16]{};
            edge(lowText, sizeof(lowText), lo);
            edge(highText, sizeof(highText), hi);
            if (!on) {
                std::snprintf(text, sizeof(text), "Showing: all layers");
            } else {
                bool worldOn = false;
                int worldAxis = 1;
                int worldLo = 0;
                int worldHi = 0;
                mod.layerRange(worldOn, worldAxis, worldLo, worldHi);
                if (lo == hi) {
                    std::snprintf(text, sizeof(text), "Showing: %c offset %s  [world %c = %d]",
                                  axisName, lowText, axisName, worldLo);
                } else {
                    std::snprintf(text, sizeof(text),
                                  "Showing: %s <= %c offset <= %s  [world %d..%d]", lowText,
                                  axisName, highText, worldLo, worldHi);
                }
            }
            addPlain(kRowLabel, text);
        }
        if (layerMode != Schematica::kLayerAll) {
            addItem(2, 5);
            addItem(2, 6);
            addItem(2, 7);
        }
        addPlain(kRowDivider, nullptr);
        addGoto("< Schematica menu", kScreenMenu, -1);
        break;
    }
    default: {
        char text[kPageTextBytes]{};
        std::snprintf(text, sizeof(text), "Schematic Placements (%u)",
                      static_cast<unsigned>(mod.blueprintCount()));
        addGoto(text, kScreenBlueprints, -1);
        addItem(0, 0);
        addPlain(kRowDivider, nullptr);
        addGoto("Configuration menu", kScreenSettings, -1);
        {
            static constexpr const char* kModeNames[Schematica::kLayerModeCount] = {
                "All", "Single layer", "All below", "All above", "Layer range"};
            char layers[kPageTextBytes]{};
            std::snprintf(layers, sizeof(layers), "Render Layers: %s",
                          kModeNames[std::clamp(mod.layerMode(), 0,
                                                Schematica::kLayerModeCount - 1)]);
            addGoto(layers, kScreenLayers, -1);
        }
        break;
    }
    }

    {
        const int screen = g_pgScreen.load(std::memory_order_relaxed);
        const bool wide = (screen == kScreenMaterials) || (screen == kScreenVerifier)
                          || (screen == kScreenBlueprints);
        std::snprintf(g_pgBodyW, sizeof(g_pgBodyW), "%s", wide ? kBodyWideW : kBodyNarrowW);
        std::snprintf(g_pgBodyX, sizeof(g_pgBodyX), "%s", "0px");
    }

    g_pgTabStart[0] = 0;
    g_pgTabRows[0] = rows;
    return rows;
}
int pageSelectedLeftRow();

bool pageLeftRowSelected(size_t row)
{
    if (row >= g_ptCount) {
        return false;
    }
    if (g_ptKind[row] == kLeftTab) {
        return g_ptIndex[row] == g_pgTab.load(std::memory_order_relaxed);
    }
    return g_ptIndex[row] == Schematica::instance().editingIndex();
}

constexpr size_t kMaxTabKeys = 32;
constexpr char kTabTopicVar[] = "$section_topic";
constexpr char kTabIndexVar[] = "$default_selector_toggle_index";
constexpr char kTabForcedVar[] = "$toggle_group_forced_index";
char g_ptTopics[kMaxLeftRows][16]{};
alignas(8) unsigned char g_ptTopicRecs[kMaxLeftRows][16]{};
TreeNode g_ptOwnHeads[kMaxLeftRows]{};
TreeNode g_ptOwnNodes[kMaxLeftRows][kMaxTabKeys]{};
std::uintptr_t g_ptOwnNodePtrs[kMaxLeftRows][4]{};
std::uintptr_t g_ptOwnValues[kMaxLeftRows][kValueWords]{};

const char* pageLeftLabel(size_t row)
{
    return (row < kMaxLeftRows) ? g_ptTexts[row] : "";
}

int pageLeftRowFromKey(const char* key)
{
    constexpr char kPrefix[] = "howtoplay.tk_pt";
    constexpr size_t kLen = sizeof(kPrefix) - 1;
    if (key == nullptr || std::strncmp(key, kPrefix, kLen) != 0) {
        return -1;
    }
    int row = -1;
    for (size_t d = kLen; key[d] >= '0' && key[d] <= '9'; ++d) {
        row = (row < 0 ? 0 : row) * 10 + (key[d] - '0');
    }
    return (row >= 0 && static_cast<size_t>(row) < kMaxLeftRows) ? row : -1;
}

void* buildLeftTab(void* self, size_t row, const void* strSrc)
{
    static const std::string kSpace = "how_to_play_common";
    static const std::string kName = "section_toggle_button";
    void* donor = nullptr;
    if (!lookupGuarded(self, &kSpace, &kName, donor) || donor == nullptr) {
        log().warn(L"UiProbe: could not resolve the How to Play row definition");
        return nullptr;
    }
    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(donor, entries, count) || entries.empty()
        || entries.size() + 6 > kMaxTabKeys) {
        log().warn(L"UiProbe: could not walk the How to Play row ({} keys)", entries.size());
        return nullptr;
    }

    static Entry tgTrueRec{};
    static Entry tgFalseRec{};
    static Entry tgIntRec{};
    static bool tgReady = false;
    if (!tgReady) {
        static const std::string kTgSpace3 = "common";
        static const std::string kTgName3 = "toggle";
        void* tg = nullptr;
        std::vector<Entry> tgAll;
        std::uintptr_t tgCount = 0;
        if (lookupGuarded(self, &kTgSpace3, &kTgName3, tg) && tg != nullptr
            && collectEntries(tg, tgAll, tgCount)) {
            for (const Entry& one : tgAll) {
                if (one.keyText == L"focus_magnet_enabled") {
                    tgTrueRec = one;
                } else if (one.keyText == L"$toggle_default_state|default") {
                    tgFalseRec = one;
                } else if (one.keyText == L"layer") {
                    tgIntRec = one;
                }
            }
            tgReady = (tgIntRec.tag != 0);
        }
    }

    const auto addStr = [&](const char* var, unsigned char (&rec)[16], const char* text) {
        std::memcpy(rec, strSrc, sizeof(rec));
        std::memcpy(rec, &text, sizeof(text));
        Entry one{};
        one.keyText = toUtf16(var);
        one.key = reinterpret_cast<std::uintptr_t>(var);
        one.value = reinterpret_cast<std::uintptr_t>(rec);
        one.tag = entries[0].tag;
        one.seq = entries[0].seq;
        entries.push_back(one);
    };
    const auto addBorrowed = [&](const char* var, const Entry& src) {
        Entry one{};
        one.keyText = toUtf16(var);
        one.key = reinterpret_cast<std::uintptr_t>(var);
        one.value = src.value;
        one.tag = src.tag;
        one.seq = src.seq;
        entries.push_back(one);
    };

    if (tgReady) {
        Entry pick{};
        pick.keyText = toUtf16(kTabIndexVar);
        pick.key = reinterpret_cast<std::uintptr_t>(kTabIndexVar);
        pick.value = static_cast<std::uintptr_t>(pageSelectedLeftRow());
        pick.tag = tgIntRec.tag;
        pick.seq = tgIntRec.seq;
        entries.push_back(pick);
    }

    std::snprintf(g_ptTopics[row], sizeof(g_ptTopics[row]), "tk_pt%u",
                  static_cast<unsigned>(row));
    const char* const topic = g_ptTopics[row];
    std::memcpy(g_ptTopicRecs[row], strSrc, sizeof(g_ptTopicRecs[row]));
    std::memcpy(g_ptTopicRecs[row], &topic, sizeof(topic));
    Entry topicEntry{};
    topicEntry.keyText = toUtf16(kTabTopicVar);
    topicEntry.key = reinterpret_cast<std::uintptr_t>(kTabTopicVar);
    topicEntry.value = reinterpret_cast<std::uintptr_t>(g_ptTopicRecs[row]);
    topicEntry.tag = entries[0].tag;
    topicEntry.seq = entries[0].seq;
    entries.push_back(topicEntry);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
    for (size_t i = 0; i < entries.size(); ++i) {
        g_ptOwnNodes[row][i].key = entries[i].key;
        g_ptOwnNodes[row][i].value = entries[i].value;
        g_ptOwnNodes[row][i].tag = entries[i].tag;
        g_ptOwnNodes[row][i].seq = entries[i].seq;
    }
    finishMap(&g_ptOwnHeads[row], g_ptOwnNodes[row], static_cast<int>(entries.size()),
              g_ptOwnNodePtrs[row]);
    std::memcpy(g_ptOwnValues[row], donor, sizeof(g_ptOwnValues[row]));
    g_ptOwnValues[row][0] = reinterpret_cast<std::uintptr_t>(g_ptOwnNodePtrs[row]);
    return g_ptOwnValues[row];
}

int pageSelectedLeftRow()
{
    const int last = g_ptLastPressed.load(std::memory_order_relaxed);
    if (last >= 0 && static_cast<size_t>(last) < g_ptCount) {
        return last;
    }
    for (size_t t = 0; t < g_ptCount; ++t) {
        if (g_ptKind[t] == kLeftTab
            && g_ptIndex[t] == g_pgTab.load(std::memory_order_relaxed)) {
            return static_cast<int>(t);
        }
    }
    return 0;
}

void buildPageTabTexts()
{
    {
        static unsigned long long lastScanAt = 0;
        const unsigned long long now = GetTickCount64();
        if (now - lastScanAt > 400) {
            lastScanAt = now;
            Schematica::instance().refreshFiles();
        }
    }
    const int now = g_pgTab.load(std::memory_order_relaxed);
    size_t rows = 0;
    for (size_t i = 0; i < kLeftTabCount && rows < kMaxLeftRows; ++i) {
        std::snprintf(g_ptTexts[rows], kPageTextBytes, "%s", kPageTabNames[i]);
        g_ptKind[rows] = kLeftTab;
        g_ptIndex[rows] = static_cast<int>(i);
        ++rows;
    }

    Schematica& mod = Schematica::instance();
    const size_t count = mod.blueprintCount();
    const int editing = mod.editingIndex();
    for (size_t i = 0; i < count && rows < kMaxLeftRows; ++i) {
        const bool shown = mod.blueprintVisible(i);
        const bool picked = (static_cast<int>(i) == editing);
        char name[kPageTextBytes]{};
        copyAscii(name, sizeof(name), mod.blueprintName(i).c_str());
        (void)picked;
        std::snprintf(g_ptTexts[rows], kPageTextBytes, "%c %s", shown ? '*' : ' ', name);
        g_ptKind[rows] = kLeftFile;
        g_ptIndex[rows] = static_cast<int>(i);
        ++rows;
    }
    if (count > kMaxPageFiles) {
        log().warn(L"UiProbe: {} schematics found - only {} of them can be listed on the left",
                   count,
                   kMaxPageFiles);
    }
    g_ptCount = rows;
}

constexpr bool kPageReuseProbe = false;

constexpr char kCountColW[] = "72px";
constexpr char kStackColW[] = "108px";
constexpr char kRowLineTexture[] = "textures/ui/list_item_divider_line_light";
constexpr std::int64_t kIconLayer = 5;

Part buildColumnsRow(size_t i, bool iconsUsable)
{
    const bool header = (g_pgRowArg[i] == 1);
    const bool iconCol = g_pgRowIconCol[i];
    const char* const rowH = header ? "16px" : (iconCol ? "18px" : "14px");
    const char* const outerH = header ? "17px" : (iconCol ? "19px" : "15px");
    const char* const textY = header ? "3px" : (iconCol ? "4px" : "2px");

    Part row;
    row.name = "r";
    row.def = "common.horizontal_stack_panel";
    row.overs.push_back(overPair("size", "100%", rowH));

    int nameW = (g_pgRowNameW[i] > 0) ? g_pgRowNameW[i] : 160;
    if (iconCol) {
        if (header) {
            nameW += 20;
        } else {
            Part ico;
            ico.name = "ico";
            if (g_pgRowHasIcon[i] && iconsUsable) {
                ico.def = "beacon.item_renderer";
                ico.overs.push_back(
                    overBag("property_bag", {overInt("#item_id_aux", g_pgRowIcon[i])}));
                ico.overs.push_back(overInt("layer", kIconLayer));
            } else {
                ico.def = "common.empty_panel";
            }
            ico.overs.push_back(overPair("size", "16px", "16px"));
            row.kids.push_back(std::move(ico));

            Part gap;
            gap.name = "gap";
            gap.def = "common.empty_panel";
            gap.overs.push_back(overPair("size", "4px", rowH));
            row.kids.push_back(std::move(gap));
        }
    }
    const auto cell = [&](const char* cellName, const char* text, const std::string& width,
                          bool right) {
        Part box;
        box.name = cellName;
        box.def = "common.empty_panel";
        box.overs.push_back(overPair("size", width, rowH));
        Part label;
        label.name = "t";
        label.def = "common.single_line_label";
        label.overs.push_back(overText("$single_line_label_text", text));
        label.overs.push_back(overPair("$single_line_label_offset", "2px", textY));
        if (right) {
            label.overs.push_back(overText("anchor_from", "top_right"));
            label.overs.push_back(overText("anchor_to", "top_right"));
        } else {
            label.overs.push_back(overPair("$size", "100%", "100%"));
            label.overs.push_back(overText("anchor_from", "top_left"));
            label.overs.push_back(overText("anchor_to", "top_left"));
        }
        box.kids.push_back(std::move(label));
        return box;
    };
    row.kids.push_back(cell("nm", g_pgTexts[i], std::to_string(nameW) + "px", false));
    row.kids.push_back(cell("ct", g_pgHints[i], kCountColW, true));
    if (g_pgLabels[i][0] != 0) {
        row.kids.push_back(cell("sk", g_pgLabels[i], kStackColW, true));
    }

    Part line;
    line.name = "ln";
    line.def = "common.dialog_divider";
    line.overs.push_back(overPair("size", "100%", "1px"));
    line.overs.push_back(overText("texture", kRowLineTexture));
    line.overs.push_back(overInt("layer", kIconLayer));

    Part outer;
    outer.name = g_pgKeys[i];
    outer.def = "common.vertical_stack_panel";
    outer.overs.push_back(overPair("size", kPageRowWidth, outerH));
    outer.kids.push_back(std::move(row));
    outer.kids.push_back(std::move(line));
    return outer;
}

void* buildSchematicaPage(void* self)
{
    if (kPageReuseProbe && g_pgReady && g_pgValue[0] != 0) {
        static std::atomic<int> said{0};
        return g_pgValue;
    }
    g_pgBag.store(nullptr);
    g_pgReady = false;
    g_pageArena.reset();
    if (!collectPageDonors(self, g_pgDonors)) {
        log().warn(L"UiProbe: the templates could not be collected (clickable rows cannot be "
                   L"built)");
    }
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        g_pgCtl[i].store(0);
        g_pgLastText[i][0] = '\0';
        g_pgIsEdit[i] = false;
        g_pgRowKind[i] = kRowButton;
    }
    static const std::string kBaseSpace = "settings_common";
    static const std::string kBaseName = "dialog_content_fullscreen";
    void* base = nullptr;
    if (!lookupGuarded(self, &kBaseSpace, &kBaseName, base) || base == nullptr
        || !memory::isReadable(base, sizeof(g_pgValue))) {
        log().warn(L"UiProbe: could not look up the page base");
        return nullptr;
    }
    std::vector<Entry> baseEntries;
    std::uintptr_t baseCount = 0;
    if (!collectEntries(base, baseEntries, baseCount) || baseEntries.empty()
        || baseEntries.size() > static_cast<size_t>(kMaxOwnKeys)) {
        log().warn(L"UiProbe: could not walk the page base ({} keys)", baseEntries.size());
        return nullptr;
    }
    std::sort(baseEntries.begin(), baseEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });

    const Entry* controls = nullptr;
    for (const Entry& one : baseEntries) {
        if (one.keyText == L"controls") {
            controls = &one;
            break;
        }
    }
    if (controls == nullptr
        || !memory::isReadable(reinterpret_cast<const void*>(controls->value), sizeof(g_pgVec))) {
        log().warn(L"UiProbe: the page base has no readable controls");
        return nullptr;
    }
    std::uintptr_t srcVec[4]{};
    std::memcpy(srcVec, reinterpret_cast<const void*>(controls->value), sizeof(srcVec));
    if (srcVec[1] <= srcVec[0]
        || !memory::isReadable(reinterpret_cast<const void*>(srcVec[0]), sizeof(std::uintptr_t))) {
        log().warn(L"UiProbe: the page base controls are empty");
        return nullptr;
    }
    std::uintptr_t firstElem = 0;
    std::memcpy(&firstElem, reinterpret_cast<const void*>(srcVec[0]), sizeof(firstElem));
    if (!memory::isReadable(reinterpret_cast<const void*>(firstElem), sizeof(g_pgItemValues[0]))) {
        log().warn(L"UiProbe: the first element of the page base is unreadable");
        return nullptr;
    }
    std::uintptr_t objectTag = kTagObject;
    {
        std::vector<Entry> sample;
        std::uintptr_t sampleCount = 0;
        if (collectEntries(reinterpret_cast<const void*>(firstElem), sample, sampleCount)
            && !sample.empty()) {
            objectTag = sample[0].tag;
        }
    }

    static const std::string kBtnSpace = "common_buttons";
    static const std::string kBtnName = "light_text_button";
    void* btn = nullptr;
    if (!lookupGuarded(self, &kBtnSpace, &kBtnName, btn) || btn == nullptr) {
        log().warn(L"UiProbe: could not look up the row button");
        return nullptr;
    }
    const Entry* tgTrue = nullptr;
    const Entry* tgFalse = nullptr;
    static std::vector<Entry> tgEntries;
    const Entry* tabGlyphSize = nullptr;
    static std::vector<Entry> navEntries;
    {
        static const std::string kTgSpace = "common";
        static const std::string kTgName = "toggle";
        void* tg = nullptr;
        std::uintptr_t tgCount = 0;
        tgEntries.clear();
        if (lookupGuarded(self, &kTgSpace, &kTgName, tg) && tg != nullptr
            && collectEntries(tg, tgEntries, tgCount)) {
            for (const Entry& one : tgEntries) {
                if (one.keyText == toUtf16(kTgTrueKey)) {
                    tgTrue = &one;
                } else if (one.keyText == toUtf16(kTgFalseKey)) {
                    tgFalse = &one;
                }
            }
        }
    }

    {
        static const std::string kNavSpace = "general_section";
        static const std::string kNavName = "how_to_play_button";
        void* nav = nullptr;
        std::uintptr_t navCount = 0;
        navEntries.clear();
        if (lookupGuarded(self, &kNavSpace, &kNavName, nav) && nav != nullptr
            && collectEntries(nav, navEntries, navCount)) {
            for (const Entry& one : navEntries) {
                if (one.keyText == L"$glyph_size") {
                    tabGlyphSize = &one;
                }
            }
        }
    }

    std::vector<Entry> btnEntries;
    std::uintptr_t btnCount = 0;
    if (!collectEntries(btn, btnEntries, btnCount) || btnEntries.empty()) {
        log().warn(L"UiProbe: could not walk the row button ({} keys)", btnEntries.size());
        return nullptr;
    }
    {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::wstring all;
            for (const Entry& one : btnEntries) {
                if (one.keyText.find(L"binding") == std::wstring::npos
                    && one.keyText.find(L"text") == std::wstring::npos) {
                    continue;
                }
                if (!all.empty()) {
                    all += L" ";
                }
                all += one.keyText;
            }
            log().info(L"UiProbe: light_text_button keys with text/binding: {}", all);
        }
    }
    const Entry* textSrc = nullptr;
    for (const Entry& one : btnEntries) {
        if ((one.tag & 0xFF) == kTagString
            && memory::isReadable(reinterpret_cast<const void*>(one.value),
                                  sizeof(g_pgTextRecs[0]))) {
            textSrc = &one;
            break;
        }
    }
    if (textSrc == nullptr) {
        log().warn(L"UiProbe: the row button has no string value to copy ({} keys)",
                   btnEntries.size());
        return nullptr;
    }

    buildPageTabTexts();
    const size_t rows = buildPageTexts();
    {
        static std::atomic<int> said{0};
    }
    if (rows == 0) {
        log().warn(L"UiProbe: the page has no rows (left {})", g_ptCount);
        return nullptr;
    }
    bool iconsUsable = false;
    {
        static const std::string kIconSpace = "beacon";
        static const std::string kIconName = "item_renderer";
        void* iconDef = nullptr;
        iconsUsable = g_pgDonors.ok && g_pgDonors.intTag != 0
                      && lookupGuarded(self, &kIconSpace, &kIconName, iconDef)
                      && iconDef != nullptr;
        static std::atomic<bool> told{false};
    }
    size_t iconRows = 0;
    g_pgTopCount = 0;
    for (size_t i = 0; i < rows; ++i) {
        if (g_pgRowChild[i]) {
            continue;
        }
        if (g_pgRowKids[i] > 0) {
            std::snprintf(g_pgKeys[i], kPageKeyBytes, "tk_pg%u", static_cast<unsigned>(i));
            Part row;
            row.name = g_pgKeys[i];
            row.def = "common.horizontal_stack_panel";
            row.overs.push_back(overPair("size", kPageRowWidth, kPageRowHeight));
            for (int k = 1; k <= g_pgRowKids[i]; ++k) {
                const size_t at = i + static_cast<size_t>(k);
                if (at >= rows) {
                    break;
                }
                std::snprintf(g_pgKeys[at], kPageKeyBytes, "tk_pg%u",
                              static_cast<unsigned>(at));
                Part kid;
                kid.name = g_pgKeys[at];
                kid.def = "common_buttons.light_text_button";
                kid.overs.push_back(overText(kOwnTextVar, g_pgTexts[at]));
                if (g_pgCloseOnPress.load()) {
                    kid.overs.push_back(overText(kPageExitVar, kPageExitId));
                }
                kid.overs.push_back(overPair("size",
                                             (g_pgRowWidth[at][0] != 0) ? g_pgRowWidth[at] : "33%",
                                             "100%"));
                row.kids.push_back(std::move(kid));
            }
            std::uintptr_t* const elem = buildPartElement(g_pageArena, g_pgDonors, row);
            if (elem == nullptr) {
                log().warn(L"UiProbe: could not build the side-by-side row {} (arena overflow "
                           L"= {})",
                           i,
                           g_pageArena.overflowed() ? 1 : 0);
                return nullptr;
            }
            g_pgElems[i] = reinterpret_cast<std::uintptr_t>(elem);
            g_pgTopElems[g_pgTopCount++] = g_pgElems[i];
            continue;
        }
        if (pageRowIsPlain(g_pgRowKind[i])) {
            std::snprintf(g_pgKeys[i], kPageKeyBytes, "tk_pg%u", static_cast<unsigned>(i));
            Part part;
            part.name = g_pgKeys[i];
            switch (g_pgRowKind[i]) {
            case kRowHeader:
                part.def = "common.single_line_label";
                part.overs.push_back(overText("$single_line_label_text", g_pgTexts[i]));
                part.overs.push_back(overPair("$size", kPageRowWidth, "16px"));
                break;
            case kRowDivider:
                part.def = "common.empty_panel";
                part.overs.push_back(overPair("size", kPageRowWidth, "8px"));
                break;
            case kRowColumns:
                part = buildColumnsRow(i, iconsUsable);
                if (g_pgRowHasIcon[i] && iconsUsable) {
                    ++iconRows;
                }
                break;
            default:
                part.def = "common.single_line_label";
                part.overs.push_back(overText("$single_line_label_text", g_pgTexts[i]));
                part.overs.push_back(overPair("$size", kPageRowWidth, "14px"));
                break;
            }
            std::uintptr_t* const elem = buildPartElement(g_pageArena, g_pgDonors, part);
            if (elem == nullptr) {
                log().warn(L"UiProbe: could not build row {} ({}) (arena overflow = {})",
                           i,
                           toUtf16(g_pgTexts[i]),
                           g_pageArena.overflowed() ? 1 : 0);
                return nullptr;
            }
            g_pgElems[i] = reinterpret_cast<std::uintptr_t>(elem);
            g_pgTopElems[g_pgTopCount++] = g_pgElems[i];
            continue;
        }
        if (pageRowBorrows(g_pgRowKind[i])) {
            std::snprintf(g_pgCtlNames[i], kPageKeyBytes, "settings_common.tk_pg%u",
                          static_cast<unsigned>(i));
            std::snprintf(g_pgBindNames[i], sizeof(g_pgBindNames[i]), "#tk_pg_%u",
                          static_cast<unsigned>(i));
            const struct {
                const char* var;
                unsigned char* rec;
                const char* text;
            } kParts[] = {
                {kSecCtlNameVar, g_pgCtlRecs[i], g_pgCtlNames[i]},
                {kSecBindNameVar, g_pgBindRecs[i], g_pgBindNames[i]},
                {kSecLabelVar, g_pgLabelRecs[i], g_pgLabels[i]},
                {kNumHintVar, g_pgHintRecs[i], g_pgHints[i]},
            };
            int over = 0;
            for (const auto& part : kParts) {
                std::memcpy(part.rec, reinterpret_cast<const void*>(textSrc->value), 16);
                std::memcpy(part.rec, &part.text, sizeof(part.text));
                g_pgOverNodes[i][over].key = reinterpret_cast<std::uintptr_t>(part.var);
                g_pgOverNodes[i][over].value = reinterpret_cast<std::uintptr_t>(part.rec);
                g_pgOverNodes[i][over].tag = textSrc->tag;
                g_pgOverNodes[i][over].seq = textSrc->seq;
                ++over;
            }
            if (g_pgRowKind[i] == kRowToggle) {
                const MenuItem* const tgItem = schematicaRow(i);
                const bool tgOn = (tgItem != nullptr) && tgItem->toggleState();
                {
                    const char* const type = tgOn ? kTgTypeNone : kTgTypeGlobal;
                    std::memcpy(g_pgTgTypeRecs[i], reinterpret_cast<const void*>(textSrc->value),
                                sizeof(g_pgTgTypeRecs[i]));
                    std::memcpy(g_pgTgTypeRecs[i], &type, sizeof(type));
                    g_pgOverNodes[i][over].key = reinterpret_cast<std::uintptr_t>(kTgTypeVar);
                    g_pgOverNodes[i][over].value =
                        reinterpret_cast<std::uintptr_t>(g_pgTgTypeRecs[i]);
                    g_pgOverNodes[i][over].tag = textSrc->tag;
                    g_pgOverNodes[i][over].seq = textSrc->seq;
                    ++over;
                }
                if (const Entry* const src = tgOn ? tgTrue : tgFalse; src != nullptr) {
                    g_pgOverNodes[i][over].key = reinterpret_cast<std::uintptr_t>(kTgDefaultVar);
                    g_pgOverNodes[i][over].value = src->value;
                    g_pgOverNodes[i][over].tag = src->tag;
                    g_pgOverNodes[i][over].seq = src->seq;
                    ++over;
                }
                {
                    static const char* const noBind = kTgNoBind;
                    std::memcpy(g_pgTgBindRecs[i], reinterpret_cast<const void*>(textSrc->value),
                                sizeof(g_pgTgBindRecs[i]));
                    std::memcpy(g_pgTgBindRecs[i], &noBind, sizeof(noBind));
                    g_pgOverNodes[i][over].key = reinterpret_cast<std::uintptr_t>(kTgBindVar);
                    g_pgOverNodes[i][over].value =
                        reinterpret_cast<std::uintptr_t>(g_pgTgBindRecs[i]);
                    g_pgOverNodes[i][over].tag = textSrc->tag;
                    g_pgOverNodes[i][over].seq = textSrc->seq;
                    ++over;
                }
            }
            finishMap(&g_pgOverHeads[i], g_pgOverNodes[i], over, g_pgOverNodePtrs[i]);
            const char* const borrowed =
                (g_pgRowKind[i] == kRowSlider)   ? "option_slider"
                : (g_pgRowKind[i] == kRowToggle) ? "option_toggle"
                                                 : "option_text_edit";
            std::snprintf(g_pgKeys[i], kPageKeyBytes, "tk_pg%u@settings_common.%s",
                          static_cast<unsigned>(i), borrowed);
            g_pgItemNodes[i][0].key = reinterpret_cast<std::uintptr_t>(g_pgKeys[i]);
            g_pgItemNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_pgOverNodePtrs[i]);
            g_pgItemNodes[i][0].tag = objectTag;
            g_pgItemNodes[i][0].seq = 0;
            finishMap(&g_pgItemHeads[i], g_pgItemNodes[i], 1, g_pgItemNodePtrs[i]);
            std::memcpy(g_pgItemValues[i], reinterpret_cast<const void*>(firstElem),
                        sizeof(g_pgItemValues[i]));
            g_pgItemValues[i][0] = reinterpret_cast<std::uintptr_t>(g_pgItemNodePtrs[i]);
            g_pgElems[i] = reinterpret_cast<std::uintptr_t>(g_pgItemValues[i]);
            g_pgTopElems[g_pgTopCount++] = g_pgElems[i];
            continue;
        }
        std::snprintf(g_pgKeys[i], kPageKeyBytes, "tk_pg%u@common_buttons.light_text_button",
                      static_cast<unsigned>(i));
        std::memcpy(g_pgTextRecs[i], reinterpret_cast<const void*>(textSrc->value),
                    sizeof(g_pgTextRecs[i]));
        const char* const label = g_pgTexts[i];
        std::memcpy(g_pgTextRecs[i], &label, sizeof(label));

        g_pgOverNodes[i][0].key = reinterpret_cast<std::uintptr_t>(kOwnTextVar);
        g_pgOverNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_pgTextRecs[i]);
        g_pgOverNodes[i][0].tag = textSrc->tag;
        g_pgOverNodes[i][0].seq = textSrc->seq;
        int over = 1;
        if (g_pgCloseOnPress.load()) {
            std::memcpy(g_pgExitRecs[i], reinterpret_cast<const void*>(textSrc->value),
                        sizeof(g_pgExitRecs[i]));
            const char* const exitId = kPageExitId;
            std::memcpy(g_pgExitRecs[i], &exitId, sizeof(exitId));
            g_pgOverNodes[i][over].key = reinterpret_cast<std::uintptr_t>(kPageExitVar);
            g_pgOverNodes[i][over].value = reinterpret_cast<std::uintptr_t>(g_pgExitRecs[i]);
            g_pgOverNodes[i][over].tag = textSrc->tag;
            g_pgOverNodes[i][over].seq = textSrc->seq;
            ++over;
        }
        if (!buildStringPairArray(controls->value, firstElem, textSrc->value, kPageRowWidth,
                                  kPageRowHeight, g_pgSizeRecs[i], g_pgSizeElems[i],
                                  g_pgSizeElemPtrs[i], g_pgSizeVec[i])) {
            return nullptr;
        }
        g_pgOverNodes[i][over].key = reinterpret_cast<std::uintptr_t>(kPageSizeKey);
        g_pgOverNodes[i][over].value = reinterpret_cast<std::uintptr_t>(g_pgSizeVec[i]);
        g_pgOverNodes[i][over].tag = controls->tag;
        g_pgOverNodes[i][over].seq = controls->seq;
        ++over;
        finishMap(&g_pgOverHeads[i], g_pgOverNodes[i], over, g_pgOverNodePtrs[i]);

        g_pgItemNodes[i][0].key = reinterpret_cast<std::uintptr_t>(g_pgKeys[i]);
        g_pgItemNodes[i][0].value = reinterpret_cast<std::uintptr_t>(g_pgOverNodePtrs[i]);
        g_pgItemNodes[i][0].tag = objectTag;
        g_pgItemNodes[i][0].seq = 0;
        finishMap(&g_pgItemHeads[i], g_pgItemNodes[i], 1, g_pgItemNodePtrs[i]);

        std::memcpy(g_pgItemValues[i], reinterpret_cast<const void*>(firstElem),
                    sizeof(g_pgItemValues[i]));
        g_pgItemValues[i][0] = reinterpret_cast<std::uintptr_t>(g_pgItemNodePtrs[i]);
        g_pgElems[i] = reinterpret_cast<std::uintptr_t>(g_pgItemValues[i]);
        g_pgTopElems[g_pgTopCount++] = g_pgElems[i];
    }
    {
        size_t columnRows = 0;
        int nameW = 0;
        for (size_t i = 0; i < rows; ++i) {
            if (g_pgRowKind[i] == kRowColumns) {
                ++columnRows;
                nameW = (nameW == 0) ? g_pgRowNameW[i] : nameW;
            }
        }
        static std::atomic<int> said{0};
    }
    std::memcpy(g_pgVec, srcVec, sizeof(g_pgVec));
    g_pgVec[0] = reinterpret_cast<std::uintptr_t>(&g_pgTopElems[0]);
    g_pgVec[1] = reinterpret_cast<std::uintptr_t>(&g_pgTopElems[g_pgTopCount]);
    g_pgVec[2] = reinterpret_cast<std::uintptr_t>(&g_pgTopElems[g_pgTopCount]);

    static Entry tgIntRec{};
    if (tgIntRec.tag == 0) {
        static const std::string kTgSpace = "common";
        static const std::string kTgName = "toggle";
        void* tg = nullptr;
        std::vector<Entry> tgAll;
        std::uintptr_t tgN = 0;
        if (lookupGuarded(self, &kTgSpace, &kTgName, tg) && tg != nullptr
            && collectEntries(tg, tgAll, tgN)) {
            for (const Entry& one : tgAll) {
                if (one.keyText == L"layer") {
                    tgIntRec = one;
                }
            }
        }
    }

    for (size_t t = 0; t < g_ptCount; ++t) {
        std::snprintf(g_ptKeys[t], kPageKeyBytes,
                      "tk_pt%u@how_to_play_common.section_toggle_button",
                      static_cast<unsigned>(t));
        int tabOver = 0;
        std::snprintf(g_ptTopics[t], sizeof(g_ptTopics[t]), "tk_pt%u",
                      static_cast<unsigned>(t));
        const char* const topic = g_ptTopics[t];
        std::memcpy(g_ptTopicRecs[t], reinterpret_cast<const void*>(textSrc->value),
                    sizeof(g_ptTopicRecs[t]));
        std::memcpy(g_ptTopicRecs[t], &topic, sizeof(topic));
        g_ptOverNodes[t][tabOver].key = reinterpret_cast<std::uintptr_t>(kTabTopicVar);
        g_ptOverNodes[t][tabOver].value = reinterpret_cast<std::uintptr_t>(g_ptTopicRecs[t]);
        g_ptOverNodes[t][tabOver].tag = textSrc->tag;
        g_ptOverNodes[t][tabOver].seq = textSrc->seq;
        ++tabOver;
        if (tgIntRec.tag != 0) {
            const size_t picked = static_cast<size_t>(pageSelectedLeftRow());
            const size_t forced = (t == picked) ? 0 : (t < picked ? t + 1 : t);
            g_ptOverNodes[t][tabOver].key = reinterpret_cast<std::uintptr_t>(kTabForcedVar);
            g_ptOverNodes[t][tabOver].value = static_cast<std::uintptr_t>(forced);
            g_ptOverNodes[t][tabOver].tag = tgIntRec.tag;
            g_ptOverNodes[t][tabOver].seq = tgIntRec.seq;
            ++tabOver;
        }
        finishMap(&g_ptOverHeads[t], g_ptOverNodes[t], tabOver, g_ptOverNodePtrs[t]);

        g_ptItemNodes[t][0].key = reinterpret_cast<std::uintptr_t>(g_ptKeys[t]);
        g_ptItemNodes[t][0].value = reinterpret_cast<std::uintptr_t>(g_ptOverNodePtrs[t]);
        g_ptItemNodes[t][0].tag = objectTag;
        g_ptItemNodes[t][0].seq = 0;
        finishMap(&g_ptItemHeads[t], g_ptItemNodes[t], 1, g_ptItemNodePtrs[t]);
        std::memcpy(g_ptItemValues[t], reinterpret_cast<const void*>(firstElem),
                    sizeof(g_ptItemValues[t]));
        g_ptItemValues[t][0] = reinterpret_cast<std::uintptr_t>(g_ptItemNodePtrs[t]);
        g_ptElems[t] = reinterpret_cast<std::uintptr_t>(g_ptItemValues[t]);
    }
    std::memcpy(g_ptVec, srcVec, sizeof(g_ptVec));
    g_ptVec[0] = reinterpret_cast<std::uintptr_t>(&g_ptElems[0]);
    g_ptVec[1] = reinterpret_cast<std::uintptr_t>(&g_ptElems[g_ptCount]);
    g_ptVec[2] = g_ptVec[1];

    g_pgViewReady = false;
    {
        static const std::string kSecSpace = "how_to_play";
        static const std::string kSecName = "moving_around_section";
        void* sec = nullptr;
        std::vector<Entry> secEntries;
        std::uintptr_t secCount = 0;
        if (lookupGuarded(self, &kSecSpace, &kSecName, sec) && sec != nullptr
            && collectEntries(sec, secEntries, secCount)) {
            const Entry* bindSrc = nullptr;
            for (const Entry& one : secEntries) {
                if (one.keyText == L"bindings") {
                    bindSrc = &one;
                    break;
                }
            }
            std::uintptr_t srcVec2[4]{};
            std::uintptr_t srcElem = 0;
            std::vector<Entry> bindEntries;
            std::uintptr_t bindCount = 0;
            if (bindSrc != nullptr
                && memory::isReadable(reinterpret_cast<const void*>(bindSrc->value),
                                      sizeof(srcVec2))) {
                std::memcpy(srcVec2, reinterpret_cast<const void*>(bindSrc->value),
                            sizeof(srcVec2));
                if (srcVec2[1] > srcVec2[0]
                    && memory::isReadable(reinterpret_cast<const void*>(srcVec2[0]),
                                          sizeof(srcElem))) {
                    std::memcpy(&srcElem, reinterpret_cast<const void*>(srcVec2[0]),
                                sizeof(srcElem));
                    collectEntries(reinterpret_cast<const void*>(srcElem), bindEntries,
                                   bindCount);
                }
            }
            if (!bindEntries.empty() && bindEntries.size() <= std::size(g_pgViewNodes)) {
                std::sort(bindEntries.begin(), bindEntries.end(),
                          [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
                std::snprintf(g_pgViewName, sizeof(g_pgViewName), "tk_pt0_button_toggle");
                const char* const viewName = g_pgViewName;
                std::memcpy(g_pgViewNameRec, reinterpret_cast<const void*>(textSrc->value),
                            sizeof(g_pgViewNameRec));
                std::memcpy(g_pgViewNameRec, &viewName, sizeof(viewName));
                for (size_t i = 0; i < bindEntries.size(); ++i) {
                    g_pgViewNodes[i].key = bindEntries[i].key;
                    g_pgViewNodes[i].value =
                        (bindEntries[i].keyText == L"source_control_name")
                            ? reinterpret_cast<std::uintptr_t>(g_pgViewNameRec)
                            : bindEntries[i].value;
                    g_pgViewNodes[i].tag = bindEntries[i].tag;
                    g_pgViewNodes[i].seq = bindEntries[i].seq;
                }
                finishMap(&g_pgViewHead, g_pgViewNodes, static_cast<int>(bindEntries.size()),
                          g_pgViewNodePtrs);
                std::memcpy(g_pgViewValue, reinterpret_cast<const void*>(srcElem),
                            sizeof(g_pgViewValue));
                g_pgViewValue[0] = reinterpret_cast<std::uintptr_t>(g_pgViewNodePtrs);
                g_pgViewElem[0] = reinterpret_cast<std::uintptr_t>(g_pgViewValue);
                std::memcpy(g_pgViewVec, reinterpret_cast<const void*>(bindSrc->value),
                            sizeof(g_pgViewVec));
                g_pgViewVec[0] = reinterpret_cast<std::uintptr_t>(&g_pgViewElem[0]);
                g_pgViewVec[1] = reinterpret_cast<std::uintptr_t>(&g_pgViewElem[1]);
                g_pgViewVec[2] = g_pgViewVec[1];
                g_pgViewKey = bindSrc->key;
                g_pgViewReady = true;
                for (size_t b = 0; b < kBodyCount; ++b) {
                    std::snprintf(g_pgBodyViewNames[b], sizeof(g_pgBodyViewNames[b]),
                                  "tk_pt%u_button_toggle", static_cast<unsigned>(b));
                    const char* const bodyName = g_pgBodyViewNames[b];
                    std::memcpy(g_pgBodyViewNameRecs[b],
                                reinterpret_cast<const void*>(textSrc->value),
                                sizeof(g_pgBodyViewNameRecs[b]));
                    std::memcpy(g_pgBodyViewNameRecs[b], &bodyName, sizeof(bodyName));
                    for (size_t i = 0; i < bindEntries.size(); ++i) {
                        g_pgBodyViewNodes[b][i].key = bindEntries[i].key;
                        std::uintptr_t use = bindEntries[i].value;
                        if (bindEntries[i].keyText == L"source_control_name") {
                            use = reinterpret_cast<std::uintptr_t>(g_pgBodyViewNameRecs[b]);
                        }
                        g_pgBodyViewNodes[b][i].value = use;
                        g_pgBodyViewNodes[b][i].tag = bindEntries[i].tag;
                        g_pgBodyViewNodes[b][i].seq = bindEntries[i].seq;
                    }
                    finishMap(&g_pgBodyViewHeads[b], g_pgBodyViewNodes[b],
                              static_cast<int>(bindEntries.size()), g_pgBodyViewNodePtrs[b]);
                    std::memcpy(g_pgBodyViewValue[b], reinterpret_cast<const void*>(srcElem),
                                sizeof(g_pgBodyViewValue[b]));
                    g_pgBodyViewValue[b][0] =
                        reinterpret_cast<std::uintptr_t>(g_pgBodyViewNodePtrs[b]);
                    g_pgBodyViewElem[b][0] =
                        reinterpret_cast<std::uintptr_t>(g_pgBodyViewValue[b]);
                    std::memcpy(g_pgBodyViewVec[b], reinterpret_cast<const void*>(bindSrc->value),
                                sizeof(g_pgBodyViewVec[b]));
                    g_pgBodyViewVec[b][0] =
                        reinterpret_cast<std::uintptr_t>(&g_pgBodyViewElem[b][0]);
                    g_pgBodyViewVec[b][1] =
                        reinterpret_cast<std::uintptr_t>(&g_pgBodyViewElem[b][1]);
                    g_pgBodyViewVec[b][2] = g_pgBodyViewVec[b][1];
                }
            }
        }
    }

    g_pgBodyReady = false;
    if (g_pgViewReady) {
        static const std::string kVSpace2 = "common";
        static const std::string kVName2 = "vertical_stack_panel";
        void* vstack2 = nullptr;
        std::vector<Entry> vEnt;
        std::uintptr_t vN = 0;
        if (lookupGuarded(self, &kVSpace2, &kVName2, vstack2) && vstack2 != nullptr
            && collectEntries(vstack2, vEnt, vN) && !vEnt.empty()
            && vEnt.size() + 2 <= static_cast<size_t>(kMaxOwnKeys)
            && memory::isReadable(vstack2, sizeof(g_pgBodyValue[0]))) {
            bool ok = true;
            for (size_t b = 0; b < g_ptCount && b < kBodyCount && ok; ++b) {
                const size_t start = g_pgTabStart[b];
                const size_t count = g_pgTabRows[b];
                std::memcpy(g_pgBodyRowVec[b], srcVec, sizeof(g_pgBodyRowVec[b]));
                g_pgBodyRowVec[b][0] = reinterpret_cast<std::uintptr_t>(&g_pgElems[start]);
                g_pgBodyRowVec[b][1] =
                    reinterpret_cast<std::uintptr_t>(&g_pgElems[start + count]);
                g_pgBodyRowVec[b][2] = g_pgBodyRowVec[b][1];

                std::vector<Entry> mine = vEnt;
                bool hasControls = false;
                for (Entry& one : mine) {
                    if (one.keyText == L"controls") {
                        one.value = reinterpret_cast<std::uintptr_t>(g_pgBodyRowVec[b]);
                        hasControls = true;
                    }
                }
                if (!hasControls) {
                    Entry one{};
                    one.keyText = L"controls";
                    one.key = controls->key;
                    one.value = reinterpret_cast<std::uintptr_t>(g_pgBodyRowVec[b]);
                    one.tag = controls->tag;
                    one.seq = controls->seq;
                    mine.push_back(one);
                }
                {
                    Entry one{};
                    one.keyText = L"bindings";
                    one.key = g_pgViewKey;
                    one.value = reinterpret_cast<std::uintptr_t>(g_pgBodyViewVec[b]);
                    one.tag = controls->tag;
                    one.seq = controls->seq;
                    mine.push_back(one);
                }
                std::sort(mine.begin(), mine.end(),
                          [](const Entry& a, const Entry& c) { return a.keyText < c.keyText; });
                if (mine.size() > static_cast<size_t>(kMaxOwnKeys)) {
                    ok = false;
                    break;
                }
                for (size_t i = 0; i < mine.size(); ++i) {
                    g_pgBodyNodes[b][i].key = mine[i].key;
                    g_pgBodyNodes[b][i].value = mine[i].value;
                    g_pgBodyNodes[b][i].tag = mine[i].tag;
                    g_pgBodyNodes[b][i].seq = mine[i].seq;
                }
                finishMap(&g_pgBodyHeads[b], g_pgBodyNodes[b], static_cast<int>(mine.size()),
                          g_pgBodyNodePtrs[b]);
                std::memcpy(g_pgBodyValue[b], vstack2, sizeof(g_pgBodyValue[b]));
                g_pgBodyValue[b][0] = reinterpret_cast<std::uintptr_t>(g_pgBodyNodePtrs[b]);

                std::snprintf(g_pgBodyKeys[b], kPageKeyBytes, "tk_body%u",
                              static_cast<unsigned>(b));
                g_pgItemNodes2[b][0].key = reinterpret_cast<std::uintptr_t>(g_pgBodyKeys[b]);
                g_pgItemNodes2[b][0].value =
                    reinterpret_cast<std::uintptr_t>(g_pgBodyNodePtrs[b]);
                g_pgItemNodes2[b][0].tag = objectTag;
                g_pgItemNodes2[b][0].seq = 0;
                finishMap(&g_pgItemHeads2[b], g_pgItemNodes2[b], 1, g_pgItemNodePtrs2[b]);
                std::memcpy(g_pgItemValues2[b], reinterpret_cast<const void*>(firstElem),
                            sizeof(g_pgItemValues2[b]));
                g_pgItemValues2[b][0] = reinterpret_cast<std::uintptr_t>(g_pgItemNodePtrs2[b]);
                g_pgBodyElems[b] = reinterpret_cast<std::uintptr_t>(g_pgItemValues2[b]);
            }
            if (ok) {
                std::memcpy(g_pgBodyVec, srcVec, sizeof(g_pgBodyVec));
                g_pgBodyVec[0] = reinterpret_cast<std::uintptr_t>(&g_pgBodyElems[0]);
                g_pgBodyVec[1] = reinterpret_cast<std::uintptr_t>(&g_pgBodyElems[g_ptCount]);
                g_pgBodyVec[2] = g_pgBodyVec[1];
                g_pgBodyReady = true;
            }
        }
    }

    {
        static const std::string kVSpace = "common";
        static const std::string kVName = "vertical_stack_panel";
        void* vstack = nullptr;
        std::vector<Entry> vEntries;
        std::uintptr_t vCount = 0;
        g_ptListReady = false;
        if (lookupGuarded(self, &kVSpace, &kVName, vstack) && vstack != nullptr
            && collectEntries(vstack, vEntries, vCount) && !vEntries.empty()
            && vEntries.size() + 2 <= static_cast<size_t>(kMaxOwnKeys)) {
            std::uintptr_t* const listRows = g_pgVec;
            bool hasControls = false;
            bool hasSize = false;
            for (Entry& one : vEntries) {
                if (one.keyText == L"controls") {
                    one.value = reinterpret_cast<std::uintptr_t>(listRows);
                    hasControls = true;
                } else if (one.keyText == L"size") {
                    hasSize = true;
                }
            }
            if (!hasControls) {
                Entry one{};
                one.keyText = L"controls";
                one.key = controls->key;
                one.value = reinterpret_cast<std::uintptr_t>(listRows);
                one.tag = controls->tag;
                one.seq = controls->seq;
                vEntries.push_back(one);
            }
            if (!hasSize
                && buildStringPairArray(controls->value, firstElem, textSrc->value, "100%",
                                        "100%c", g_ptSizeRecs[0], g_ptSizeElems[0],
                                        g_ptSizeElemPtrs[0], g_ptSizeVec[0])) {
                Entry one{};
                one.keyText = L"size";
                one.key = reinterpret_cast<std::uintptr_t>(kPageSizeKey);
                one.value = reinterpret_cast<std::uintptr_t>(g_ptSizeVec[0]);
                one.tag = controls->tag;
                one.seq = controls->seq;
                vEntries.push_back(one);
            }
            std::sort(vEntries.begin(), vEntries.end(),
                      [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
            for (size_t i = 0; i < vEntries.size(); ++i) {
                g_ptListNodes[i].key = vEntries[i].key;
                g_ptListNodes[i].value = vEntries[i].value;
                g_ptListNodes[i].tag = vEntries[i].tag;
                g_ptListNodes[i].seq = vEntries[i].seq;
            }
            finishMap(&g_ptListHead, g_ptListNodes, static_cast<int>(vEntries.size()),
                      g_ptListNodePtrs);
            if (memory::isReadable(vstack, sizeof(g_ptListValue))) {
                std::memcpy(g_ptListValue, vstack, sizeof(g_ptListValue));
                g_ptListValue[0] = reinterpret_cast<std::uintptr_t>(g_ptListNodePtrs);
                g_ptListReady = true;
            }
        }
    }

    for (size_t c = 0; c < 1; ++c) {
        std::snprintf(g_pgColKeys[c], kPageKeyBytes,
                      (c == 0 && g_ptListReady) ? "tk_col%u@common.scrolling_panel"
                                                : "tk_col%u@common.vertical_stack_panel",
                      static_cast<unsigned>(c));
        int colOver = 0;
        if (c == 0 && g_ptListReady) {
            const char* const listName = kPtListName;
            std::memcpy(g_ptListNameRec, reinterpret_cast<const void*>(textSrc->value),
                        sizeof(g_ptListNameRec));
            std::memcpy(g_ptListNameRec, &listName, sizeof(listName));
        }
        (void)tgIntRec;
        if (c == 0 && g_ptListReady) {
            if (!buildStringPairArray(controls->value, firstElem, textSrc->value, "5px",
                                      "100% - 4px", g_ptListSizeRecs, g_ptListSizeElems,
                                      g_ptListSizeElemPtrs, g_ptListSizeVec)) {
                return nullptr;
            }
            g_pgColOverNodes[c][colOver].key = reinterpret_cast<std::uintptr_t>(kPtScrollSizeVar);
            g_pgColOverNodes[c][colOver].value =
                reinterpret_cast<std::uintptr_t>(g_ptListSizeVec);
            g_pgColOverNodes[c][colOver].tag = controls->tag;
            g_pgColOverNodes[c][colOver].seq = controls->seq;
            ++colOver;
            g_pgColOverNodes[c][colOver].key = reinterpret_cast<std::uintptr_t>(kPtListVar);
            g_pgColOverNodes[c][colOver].value =
                reinterpret_cast<std::uintptr_t>(g_ptListNameRec);
            g_pgColOverNodes[c][colOver].tag = textSrc->tag;
            g_pgColOverNodes[c][colOver].seq = textSrc->seq;
            ++colOver;
        } else {
            g_pgColOverNodes[c][colOver].key = controls->key;
            g_pgColOverNodes[c][colOver].value = reinterpret_cast<std::uintptr_t>(g_pgVec);
            g_pgColOverNodes[c][colOver].tag = controls->tag;
            g_pgColOverNodes[c][colOver].seq = controls->seq;
            ++colOver;
        }
        if (!buildStringPairArray(controls->value, firstElem, textSrc->value,
                                  kPageOneColWidth, kPageColHeight,
                                  g_pgColSizeRecs[c], g_pgColSizeElems[c],
                                  g_pgColSizeElemPtrs[c], g_pgColSizeVec[c])) {
            return nullptr;
        }
        g_pgColOverNodes[c][colOver].key = reinterpret_cast<std::uintptr_t>(kPageSizeKey);
        g_pgColOverNodes[c][colOver].value = reinterpret_cast<std::uintptr_t>(g_pgColSizeVec[c]);
        g_pgColOverNodes[c][colOver].tag = controls->tag;
        g_pgColOverNodes[c][colOver].seq = controls->seq;
        ++colOver;
        finishMap(&g_pgColOverHeads[c], g_pgColOverNodes[c], colOver, g_pgColOverNodePtrs[c]);

        g_pgColNodes[c][0].key = reinterpret_cast<std::uintptr_t>(g_pgColKeys[c]);
        g_pgColNodes[c][0].value = reinterpret_cast<std::uintptr_t>(g_pgColOverNodePtrs[c]);
        g_pgColNodes[c][0].tag = objectTag;
        g_pgColNodes[c][0].seq = 0;
        finishMap(&g_pgColHeads[c], g_pgColNodes[c], 1, g_pgColNodePtrs[c]);
        std::memcpy(g_pgColValues[c], reinterpret_cast<const void*>(firstElem),
                    sizeof(g_pgColValues[c]));
        g_pgColValues[c][0] = reinterpret_cast<std::uintptr_t>(g_pgColNodePtrs[c]);
        g_pgColElems[c] = reinterpret_cast<std::uintptr_t>(g_pgColValues[c]);
    }
    std::memcpy(g_pgColVec, srcVec, sizeof(g_pgColVec));
    g_pgColVec[0] = reinterpret_cast<std::uintptr_t>(&g_pgColElems[0]);
    g_pgColVec[1] = reinterpret_cast<std::uintptr_t>(&g_pgColElems[1]);
    g_pgColVec[2] = g_pgColVec[1];

    static const std::string kStackSpace = "common";
    static const std::string kStackName = "horizontal_stack_panel";
    void* stack = nullptr;
    if (!lookupGuarded(self, &kStackSpace, &kStackName, stack) || stack == nullptr
        || !memory::isReadable(stack, sizeof(g_pgValue))) {
        log().warn(L"UiProbe: could not look up the horizontal stack panel");
        return nullptr;
    }
    std::vector<Entry> stackEntries;
    std::uintptr_t stackCount = 0;
    if (!collectEntries(stack, stackEntries, stackCount) || stackEntries.empty()
        || stackEntries.size() + 2 > static_cast<size_t>(kMaxOwnKeys)) {
        log().warn(L"UiProbe: could not walk the horizontal stack panel ({} keys)",
                   stackEntries.size());
        return nullptr;
    }
    Entry ours;
    ours.keyText = L"controls";
    ours.key = controls->key;
    ours.value = reinterpret_cast<std::uintptr_t>(g_pgColVec);
    ours.tag = controls->tag;
    ours.seq = controls->seq;
    stackEntries.push_back(ours);

    {
        const struct {
            const wchar_t* keyText;
            const char* key;
            const char* first;
            const char* second;
            size_t slot;
        } kInsets[] = {
            {L"offset", kPageOffsetKey, g_pgBodyX, kPageInsetY, 0},
            {L"size", kPageSizeKey, g_pgBodyW, kPageBodyHeight, 1},
        };
        for (const auto& one : kInsets) {
            if (!buildStringPairArray(controls->value, firstElem, textSrc->value, one.first,
                                      one.second, g_pgInsetRecs[one.slot],
                                      g_pgInsetElems[one.slot], g_pgInsetElemPtrs[one.slot],
                                      g_pgInsetVec[one.slot])) {
                continue;
            }
            Entry* found = nullptr;
            for (Entry& e : stackEntries) {
                if (e.keyText == one.keyText) {
                    found = &e;
                    break;
                }
            }
            if (found != nullptr) {
                found->value = reinterpret_cast<std::uintptr_t>(g_pgInsetVec[one.slot]);
                found->tag = controls->tag;
                continue;
            }
            Entry add;
            add.keyText = one.keyText;
            add.key = reinterpret_cast<std::uintptr_t>(one.key);
            add.value = reinterpret_cast<std::uintptr_t>(g_pgInsetVec[one.slot]);
            add.tag = controls->tag;
            add.seq = controls->seq;
            stackEntries.push_back(add);
        }
    }
    if (stackEntries.size() > static_cast<size_t>(kMaxOwnKeys)) {
        log().warn(L"UiProbe: too many keys for the page body ({})", stackEntries.size());
        return nullptr;
    }
    std::sort(stackEntries.begin(), stackEntries.end(),
              [](const Entry& a, const Entry& b) { return a.keyText < b.keyText; });
    for (size_t i = 0; i < stackEntries.size(); ++i) {
        g_pgNodes[i].key = stackEntries[i].key;
        g_pgNodes[i].value = stackEntries[i].value;
        g_pgNodes[i].tag = stackEntries[i].tag;
        g_pgNodes[i].seq = stackEntries[i].seq;
    }
    finishMap(&g_pgHead, g_pgNodes, static_cast<int>(stackEntries.size()), g_pgNode);
    std::memcpy(g_pgValue, stack, sizeof(g_pgValue));
    g_pgValue[0] = reinterpret_cast<std::uintptr_t>(g_pgNode);
    g_pgReady = true;
    log().info(L"UiProbe: built the Schematica page (tab {}, {} row(s), base had {} keys)",
               g_pgTab.load(), rows, baseEntries.size());
    return g_pgValue;
}

void* substituteOwnPage(void* self, const void* space, const void* name)
{
    if (self == nullptr) {
        return nullptr;
    }

    if (const unsigned long long keyAt = g_ownKeyOpenAt.load(); keyAt != 0) {
        if (GetTickCount64() - keyAt > 6000) {
            g_ownKeyOpenAt.store(0);
        } else {
            const std::wstring keySpace = readString(space);
            const std::wstring keyName = readString(name);
            if (g_afterInvokeLogs.fetch_add(1) < 80) {
                log().info(L"UiProbe: borrowed screen -> {}.{}", keySpace, keyName);
            }
            if (keySpace == L"how_to_play_common" && keyName == L"dialog_content") {
                g_pgCloseOnPress.store(true);
                if (void* const page = buildSchematicaPage(self)) {
                    log().info(L"UiProbe: served our page inside How to Play");
                    return page;
                }
            }
            if (keySpace == L"how_to_play_common" && keyName == L"how_to_play_header") {
                std::lock_guard<std::mutex> guard(g_mutex);
                if (void* const header = buildHeader(self)) {
                    return header;
                }
            }
        }
    }

    const unsigned long long at = g_ownNavInvokedAt.load();
    if (at == 0 || GetTickCount64() - at > 4000) {
        return nullptr;
    }
    g_lookupCount.fetch_add(1, std::memory_order_relaxed);
    const std::wstring spaceText = readString(space);

    if (spaceText == L"common" && readString(name) == L"fullscreen_header"
        && g_ownSettingsRoute.load()) {
        static std::atomic<unsigned long long> servedFor{0};
        if (servedFor.exchange(at) != at) {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (void* const header = buildHeader(self)) {
                log().info(L"UiProbe: served our header (fullscreen_header)");
                return header;
            }
        }
        return nullptr;
    }

    if (spaceText != L"settings_common") {
        return nullptr;
    }
    {
        const std::wstring editName = readString(name);
        if (editName == L"tk_ptlist" && g_ptListReady) {
            return g_ptListValue;
        }
        if (editName.rfind(L"tk_pg", 0) == 0) {
            int which = 0;
            for (size_t d = 5; d < editName.size()
                               && editName[d] >= L'0' && editName[d] <= L'9'; ++d) {
                which = which * 10 + (editName[d] - L'0');
            }
            const int kind = (static_cast<size_t>(which) < kMaxPageRows)
                                 ? g_pgRowKind[which] : kRowEdit;
            static const std::string kEditSpace = "settings_common";
            const std::string kEditName = pageRowControlName(kind);
            void* ctl = nullptr;
            if (lookupGuarded(self, &kEditSpace, &kEditName, ctl) && ctl != nullptr) {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 3) {
                    log().info(L"UiProbe: served the page edit box ({})", editName);
                }
                return ctl;
            }
            log().warn(L"UiProbe: could not look up option_text_edit_control");
            return nullptr;
        }
    }
    const std::wstring nameText = readString(name);
    if (nameText != L"dialog_content_fullscreen") {
        return nullptr;
    }
    if (!g_ownSettingsRoute.load()) {
        static std::atomic<int> said{0};
        return nullptr;
    }
    g_pgCloseOnPress.store(false);
    void* const page = buildSchematicaPage(self);
    if (page == nullptr) {
        return nullptr;
    }
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 3) {
        log().info(L"UiProbe: served our page body (dialog_content_fullscreen)");
    }
    return page;
}

void* substitute(void* self, const void* space, const void* name)
{
    if (void* const own = substituteOwnPage(self, space, name)) {
        return own;
    }
    if (!kSubstituteEnabled || self == nullptr) {
        return nullptr;
    }
    static const std::string kSpace = kFromSpace;
    static const std::string kFrom = kFromName;

    const std::wstring spaceText = readString(space);
    if (spaceText.empty()) {
        return nullptr;
    }
    const std::wstring nameText = readString(name);
    if (nameText.empty()) {
        return nullptr;
    }

    if (kOwnMenuScreen && kNavPaneOwnItems && spaceText == L"how_to_play"
        && nameText == L"how_to_play_screen") {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_selectedMenu.store(0);
        g_contentReady = false;
        forgetNumBags();
    }

    if (spaceText == L"how_to_play" && nameText == L"how_to_play_selector_stack_panel") {
        std::lock_guard<std::mutex> guard(g_mutex);
        return buildNavExtra(self);
    }

    if (kContentPanelEnabled && spaceText == L"how_to_play"
        && nameText == L"how_to_play_section_content_panels") {
        std::lock_guard<std::mutex> guard(g_mutex);
        return buildContentPanel(self);
    }

    if (kOwnMenuScreen && kNavPaneOwnItems && spaceText == L"how_to_play_common"
        && nameText == L"toggle_button_control") {
        std::lock_guard<std::mutex> guard(g_mutex);
        return buildToggleLabel(self);
    }

    if (kOwnMenuScreen && kNavPaneOwnItems && spaceText == L"how_to_play_common"
        && nameText == L"how_to_play_header") {
        std::lock_guard<std::mutex> guard(g_mutex);
        return buildHeader(self);
    }

    if (kOwnMenuScreen && kNavPaneOwnItems && spaceText == L"settings_common") {
        const wchar_t tail2 = (nameText.size() > 2) ? nameText[nameText.size() - 2] : L'\0';
        const wchar_t tail1 = (nameText.size() > 2) ? nameText[nameText.size() - 1] : L'\0';
        const bool isToggle = (menuIndexFromName(nameText, L"_tgl") >= 0);
        const bool isEdit = ((tail2 == L'n' || tail2 == L'k') && tail1 >= L'0' && tail1 <= L'9'
                             && menuIndexFromName(nameText.substr(0, nameText.size() - 2), L"_")
                                    >= 0);
        if (isToggle || isEdit) {
            const bool isKeybind = isEdit && (tail2 == L'k');

            if (isKeybind) {
                const int owner =
                    menuIndexFromName(nameText.substr(0, nameText.size() - 2), L"_");
                const int slot = tail1 - L'0';
                if (owner >= 0 && slot >= 0 && static_cast<size_t>(slot) < kMaxKeysPerModule) {
                    std::lock_guard<std::mutex> guard(g_mutex);
                    void* const panel = buildKeyRowPanel(self, static_cast<size_t>(owner),
                                                         static_cast<size_t>(slot));
                    if (panel != nullptr) {
                        return panel;
                    }
                }
            }

            static const std::string kSpaceStr = "settings_common";
            static const std::string kBtnSpaceStr = "common_buttons";
            static const std::string kToggleStr = "option_toggle_control";
            static const std::string kEditStr = "option_text_edit_control";
            static const std::string kKeyBtnStr = "light_text_form_fitting_button";
            void* ctl = nullptr;
            const std::string* const want =
                isToggle ? &kToggleStr : (isKeybind ? &kKeyBtnStr : &kEditStr);
            const std::string* const space = isKeybind ? &kBtnSpaceStr : &kSpaceStr;
            if (lookupGuarded(self, space, want, ctl) && ctl != nullptr) {
                constexpr bool kDumpButtonNode = false;
                if (kDumpButtonNode && isKeybind) {
                    static std::atomic<bool> dumped{false};
                    if (!dumped.exchange(true)) {
                        dumpNode(L"section " + std::wstring(want->begin(), want->end()), ctl);
                    }
                }

                if (isKeybind) {
                    void* const sized = buildSizedKeyButton(self);
                    if (sized != nullptr) {
                        return sized;
                    }
                    log().warn(L"UiProbe: could not build the sized copy; using the vanilla one");
                }

                return ctl;
            }
            log().warn(L"UiProbe: could not look up the forwarding target ({})", nameText);
            return nullptr;
        }
    }

    if (spaceText != std::wstring(kSpace.begin(), kSpace.end())) {
        return nullptr;
    }
    if (nameText != std::wstring(kFrom.begin(), kFrom.end())) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(g_mutex);
    return buildWrapper(self);
}

void onBagLookup(void* self, const char* key, void* result)
{
    if (!kSubstituteEnabled || key == nullptr || result == nullptr) {
        return;
    }
    if (!memory::isReadable(key, 8)) {
        return;
    }
    const auto slot = reinterpret_cast<std::uintptr_t>(result);

    if (std::memcmp(key, "pressed_", 8) != 0 && std::memcmp(key, "$pressed", 8) != 0) {
        return;
    }
    for (size_t i = 0; i < g_copyDestCount; ++i) {
        if (g_copyDest[i] == slot) {
            return;
        }
    }
    if (g_copyDestCount >= kMaxCopyDest) {
        return;
    }

    g_copyDest[g_copyDestCount++] = slot;
    g_swapAt.store(GetTickCount64() + kSwapDelayMs);
}

constexpr std::ptrdiff_t kControlName = 0x20;

constexpr size_t kMaxNumBags = 128;
constexpr size_t kNumBagViaBytes = 16;

constexpr std::ptrdiff_t kBagInnerOffsets[] = {0x50, 0x48, 0x58, 0xC8};

struct NumBagCandidate {
    void* bag;
    char via[kNumBagViaBytes];
};

NumBagCandidate g_numBags[kMaxNumBags]{};
std::atomic<size_t> g_numBagCount{0};

std::atomic<std::uintptr_t> g_editControl{0};

constexpr size_t kMaxEditCands = 4;
std::uintptr_t g_editCands[kMaxEditCands]{};
std::atomic<size_t> g_editCandCount{0};
std::string g_editCandStart[kMaxEditCands];
bool g_editStartCaptured = false;

void addEditCandidate(std::uintptr_t control)
{
    if (control == 0) {
        return;
    }
    const size_t n = g_editCandCount.load();
    for (size_t i = 0; i < n; ++i) {
        if (g_editCands[i] == control) {
            return;
        }
    }
    if (n >= kMaxEditCands) {
        return;
    }
    g_editCands[n] = control;
    g_editCandCount.store(n + 1);
}

void addNumBag(void* bag, const char* via, std::ptrdiff_t off)
{
    const size_t n = g_numBagCount.load();
    if (n >= kMaxNumBags) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (g_numBags[i].bag == bag) {
            return;
        }
    }
    g_numBags[n].bag = bag;
    std::snprintf(g_numBags[n].via, kNumBagViaBytes, "+%llx %.8s",
                  static_cast<unsigned long long>(off), via);
    g_numBagCount.store(n + 1);
}

void rememberNumBag(void* owner, const char* via)
{
    if (owner == nullptr) {
        return;
    }
    for (const std::ptrdiff_t off : kBagInnerOffsets) {
        const auto at = reinterpret_cast<const char*>(owner) + off;
        if (!memory::isReadable(at, sizeof(void*))) {
            continue;
        }
        void* inner = nullptr;
        std::memcpy(&inner, at, sizeof(inner));
        if (inner == nullptr || !memory::isReadable(inner, 16)) {
            continue;
        }
        unsigned char tag = 0;
        std::memcpy(&tag, reinterpret_cast<const char*>(inner) + 8, 1);
        if (tag != 7) {
            continue;
        }
        addNumBag(inner, via, off);
    }
}

void forgetNumBags() { g_numBagCount.store(0); }

void* controlBag(std::uintptr_t control)
{
    if (control == 0) {
        return nullptr;
    }
    const auto at = reinterpret_cast<const char*>(control) + 8;
    if (!memory::isReadable(at, sizeof(void*))) {
        return nullptr;
    }
    void* bag = nullptr;
    std::memcpy(&bag, at, sizeof(bag));
    return bag;
}

constexpr std::ptrdiff_t kControlChildren = 0x098;

constexpr std::ptrdiff_t kControlPos = 0x10;
constexpr std::ptrdiff_t kControlRect = 0x40;

bool controlHitBy(std::uintptr_t control, float x, float y, float* out)
{
    if (control == 0) {
        return false;
    }
    const auto* posAt = reinterpret_cast<const char*>(control) + kControlPos;
    const auto* rectAt = reinterpret_cast<const char*>(control) + kControlRect;
    if (!memory::isReadable(posAt, sizeof(float) * 2)
        || !memory::isReadable(rectAt, sizeof(float) * 4)) {
        return false;
    }
    float pos[2]{};
    float rect[4]{};
    std::memcpy(pos, posAt, sizeof(pos));
    std::memcpy(rect, rectAt, sizeof(rect));
    const float x0 = pos[0];
    const float y0 = pos[1];
    if (out != nullptr) {
        out[0] = x0;
        out[1] = y0;
        out[2] = rect[2];
        out[3] = rect[3];
        out[4] = rect[0];
        out[5] = rect[1];
    }
    if (!(rect[2] > 0.0f) || !(rect[3] > 0.0f)) {
        return false;
    }
    return x >= x0 && x < x0 + rect[2] && y >= y0 && y < y0 + rect[3];
}

std::uintptr_t childByName(std::uintptr_t control, const wchar_t* want)
{
    if (control == 0) {
        return 0;
    }
    const auto at = reinterpret_cast<const char*>(control) + kControlChildren;
    if (!memory::isReadable(at, 24)) {
        return 0;
    }
    std::uintptr_t vec[3]{};
    std::memcpy(vec, at, sizeof(vec));
    if (vec[0] == 0 || vec[1] <= vec[0] || vec[2] < vec[1]) {
        return 0;
    }
    const std::uintptr_t bytes = vec[1] - vec[0];
    if (bytes % sizeof(void*) != 0) {
        return 0;
    }
    const std::uintptr_t count = bytes / sizeof(void*);
    if (count == 0 || count > 48
        || !memory::isReadable(reinterpret_cast<const void*>(vec[0]),
                               static_cast<size_t>(bytes))) {
        return 0;
    }
    for (std::uintptr_t i = 0; i < count; ++i) {
        std::uintptr_t child = 0;
        std::memcpy(&child, reinterpret_cast<const char*>(vec[0]) + i * sizeof(void*),
                    sizeof(child));
        if (child == 0) {
            continue;
        }
        const auto* nameAt = reinterpret_cast<const char*>(child) + kControlName;
        if (!memory::isReadable(nameAt, 0x20)) {
            continue;
        }
        if (readString(nameAt) == want) {
            return child;
        }
    }
    return 0;
}

bool readStdString(const char* at, std::string& out, size_t cap = 64)
{
    if (at == nullptr || !memory::isReadable(at, 0x20)) {
        return false;
    }
    std::uintptr_t len = 0;
    std::uintptr_t room = 0;
    std::memcpy(&len, at + 0x10, sizeof(len));
    std::memcpy(&room, at + 0x18, sizeof(room));
    if (len == 0 || room < 15 || room > 0x400 || len > room || len >= cap) {
        return false;
    }
    const char* text = at;
    if (room > 15) {
        std::memcpy(&text, at, sizeof(text));
        if (text == nullptr || !memory::isReadable(text, static_cast<size_t>(len) + 1)) {
            return false;
        }
    }
    char buf[80]{};
    std::memcpy(buf, text, static_cast<size_t>(len));
    buf[sizeof(buf) - 1] = '\0';
    out.assign(buf, static_cast<size_t>(len));
    return true;
}

bool readEditBoxText(std::uintptr_t box, std::string& out)
{
    std::uintptr_t at = box;
    for (const wchar_t* step : {L"centering_panel", L"clipper_panel", L"display_text"}) {
        at = childByName(at, step);
        if (at == 0) {
            return false;
        }
    }

    constexpr std::ptrdiff_t kBindSpan = 0x400;
    for (std::ptrdiff_t off = 0; off + 24 <= 0x160; off += sizeof(void*)) {
        if (off == kControlChildren) {
            continue;
        }
        const auto* head = reinterpret_cast<const char*>(at) + off;
        if (!memory::isReadable(head, 24)) {
            continue;
        }
        std::uintptr_t vec[3]{};
        std::memcpy(vec, head, sizeof(vec));
        if (vec[0] == 0 || vec[1] <= vec[0] || vec[2] < vec[1]) {
            continue;
        }
        const std::uintptr_t bytes = vec[1] - vec[0];
        if (bytes % sizeof(void*) != 0 || bytes > 0x200) {
            continue;
        }
        const std::uintptr_t count = bytes / sizeof(void*);
        if (count == 0 || count > 16
            || !memory::isReadable(reinterpret_cast<const void*>(vec[0]),
                                   static_cast<size_t>(bytes))) {
            continue;
        }
        for (std::uintptr_t i = 0; i < count; ++i) {
            std::uintptr_t elem = 0;
            std::memcpy(&elem, reinterpret_cast<const char*>(vec[0]) + i * sizeof(void*),
                        sizeof(elem));
            if (elem == 0 || !memory::isReadable(reinterpret_cast<const void*>(elem), 0x40)) {
                continue;
            }
            std::ptrdiff_t nameAt = -1;
            for (std::ptrdiff_t s = 0; s + 0x20 <= kBindSpan; s += sizeof(void*)) {
                std::string name;
                if (readStdString(reinterpret_cast<const char*>(elem + s), name)
                    && name == "#item_name") {
                    nameAt = s;
                    break;
                }
            }
            if (nameAt < 0) {
                continue;
            }
            for (std::ptrdiff_t s = nameAt + 0x20; s + 0x20 <= kBindSpan; s += sizeof(void*)) {
                std::string text;
                if (!readStdString(reinterpret_cast<const char*>(elem + s), text)) {
                    continue;
                }
                if (text.empty() || text.size() > 32) {
                    continue;
                }
                bool numeric = true;
                for (const char c : text) {
                    if ((c < '0' || c > '9') && c != '.' && c != '-' && c != '+') {
                        numeric = false;
                        break;
                    }
                }
                if (!numeric) {
                    continue;
                }
                out = text;
                return true;
            }
        }
    }
    return false;
}

void dumpStringsIn(std::uintptr_t obj, std::ptrdiff_t span = 0x160)
{
    int shown = 0;
    for (std::ptrdiff_t off = 0; off + 0x20 <= span && shown < 16; off += sizeof(void*)) {
        const auto* head = reinterpret_cast<const char*>(obj) + off;
        if (!memory::isReadable(head, 0x20)) {
            continue;
        }
        std::uintptr_t len = 0;
        std::uintptr_t cap = 0;
        std::memcpy(&len, head + 0x10, sizeof(len));
        std::memcpy(&cap, head + 0x18, sizeof(cap));
        if (len == 0 || cap < 15 || cap > 0x400 || len > cap) {
            continue;
        }
        const char* text = head;
        if (cap > 15) {
            std::memcpy(&text, head, sizeof(text));
            if (text == nullptr || !memory::isReadable(text, static_cast<size_t>(len) + 1)) {
                continue;
            }
        }
        char buf[80]{};
        std::memcpy(buf, text, (len > 79) ? 79 : static_cast<size_t>(len));
        const std::string s = buf;
        ++shown;
    }
}

void dumpVectorsIn(std::uintptr_t obj)
{
    int vecs = 0;
    for (std::ptrdiff_t off = 0; off + 24 <= 0x160 && vecs < 4; off += sizeof(void*)) {
        if (off == kControlChildren) {
            continue;
        }
        const auto* head = reinterpret_cast<const char*>(obj) + off;
        if (!memory::isReadable(head, 24)) {
            continue;
        }
        std::uintptr_t vec[3]{};
        std::memcpy(vec, head, sizeof(vec));
        if (vec[0] == 0 || vec[1] <= vec[0] || vec[2] < vec[1]) {
            continue;
        }
        const std::uintptr_t bytes = vec[1] - vec[0];
        if (bytes % sizeof(void*) != 0 || bytes > 0x200) {
            continue;
        }
        const std::uintptr_t count = bytes / sizeof(void*);
        if (count == 0 || count > 16
            || !memory::isReadable(reinterpret_cast<const void*>(vec[0]),
                                   static_cast<size_t>(bytes))) {
            continue;
        }
        ++vecs;
        for (std::uintptr_t i = 0; i < count && i < 6; ++i) {
            std::uintptr_t elem = 0;
            std::memcpy(&elem, reinterpret_cast<const char*>(vec[0]) + i * sizeof(void*),
                        sizeof(elem));
            if (elem == 0 || !memory::isReadable(reinterpret_cast<const void*>(elem), 0x40)) {
                continue;
            }
            dumpStringsIn(elem, 0x400);
        }
    }
}

void probeEditControl()
{
    const std::uintptr_t box = g_editControl.load();
    if (box == 0) {
        return;
    }
    const std::wstring boxName =
        memory::isReadable(reinterpret_cast<const char*>(box) + kControlName, 0x20)
            ? readString(reinterpret_cast<const char*>(box) + kControlName)
            : L"<unreadable>";

    dumpStringsIn(box);
    dumpVectorsIn(box);

    std::uintptr_t at = box;
    for (const wchar_t* step : {L"centering_panel", L"clipper_panel", L"display_text"}) {
        const std::uintptr_t next = childByName(at, step);
        if (next == 0) {
            at = 0;
            break;
        }
        at = next;
    }
    if (at == 0 || at == box) {
        return;
    }

    dumpStringsIn(at);

    dumpVectorsIn(at);
}

int dumpBagKeys(void* bag, int maxKeys, bool onlyOwn)
{
    void* node = nullptr;
    std::memcpy(&node, bag, sizeof(node));
    if (node == nullptr || !memory::isReadable(node, 16)) {
        return 0;
    }
    void* head = nullptr;
    std::memcpy(&head, node, sizeof(head));
    if (head == nullptr || !memory::isReadable(head, sizeof(TreeNode))) {
        return 0;
    }
    TreeNode headNode{};
    std::memcpy(&headNode, head, sizeof(headNode));

    constexpr size_t kStackMax = 64;
    std::uintptr_t stack[kStackMax]{};
    size_t top = 0;
    stack[top++] = headNode.parent;

    int shown = 0;
    int visited = 0;
    while (top > 0 && shown < maxKeys && visited < 400) {
        const std::uintptr_t at = stack[--top];
        const auto* raw = reinterpret_cast<const void*>(at);
        if (at == 0 || !memory::isReadable(raw, sizeof(TreeNode))) {
            continue;
        }
        TreeNode n{};
        std::memcpy(&n, raw, sizeof(n));
        if (n.isnil != 0) {
            continue;
        }
        ++visited;
        if (top + 2 <= kStackMax) {
            stack[top++] = n.left;
            stack[top++] = n.right;
        }

        const auto* key = reinterpret_cast<const char*>(n.key);
        size_t span = 64;
        while (span > 8 && !memory::isReadable(key, span)) {
            span /= 2;
        }
        if (key == nullptr || span <= 8) {
            continue;
        }
        char buf[64]{};
        std::memcpy(buf, key, (span > 63) ? 63 : span);
        buf[63] = '\0';
        if (onlyOwn && std::strstr(buf, "tk_n_") == nullptr) {
            continue;
        }
        const std::string keyStr = buf;
        ++shown;
    }
    return shown;
}

void dumpAllBagKeys()
{
    const size_t n = g_numBagCount.load();
    int found = 0;
    for (size_t i = 0; i < n; ++i) {
        void* const bag = g_numBags[i].bag;
        if (!memory::isReadable(bag, 16)) {
            continue;
        }
        unsigned char tag = 0;
        std::memcpy(&tag, reinterpret_cast<const char*>(bag) + 8, 1);
        if (tag != 7) {
            continue;
        }
        if (dumpBagKeys(bag, 4, true) > 0) {
            const std::string viaStr = g_numBags[i].via;
            ++found;
        }
    }
    if (found > 0) {
        return;
    }

    int shown = 0;
    for (size_t i = 0; i < n && shown < 3; ++i) {
        void* const bag = g_numBags[i].bag;
        if (!memory::isReadable(bag, 16)) {
            continue;
        }
        unsigned char tag = 0;
        std::memcpy(&tag, reinterpret_cast<const char*>(bag) + 8, 1);
        if (tag != 7) {
            continue;
        }
        const std::string viaStr = g_numBags[i].via;
        if (dumpBagKeys(bag, 8, false) > 0) {
            ++shown;
        }
    }
}

void probeNumBags(const char* key)
{
    if (key == nullptr) {
        return;
    }
    const std::string keyStr = key;
    const std::wstring keyText(keyStr.begin(), keyStr.end());

    const size_t n = g_numBagCount.load();
    int hits = 0;
    int usable = 0;
    for (size_t i = 0; i < n; ++i) {
        void* const bag = g_numBags[i].bag;

        if (!memory::isReadable(bag, 16)) {
            continue;
        }
        unsigned char bagTag = 0;
        std::memcpy(&bagTag, reinterpret_cast<const char*>(bag) + 8, 1);
        if (bagTag != 7) {
            continue;
        }
        ++usable;

        void* const slot = hooks::callUiBagFind(bag, key);
        if (slot == nullptr || !memory::isReadable(slot, 16)) {
            continue;
        }
        unsigned char tag = 0;
        std::memcpy(&tag, reinterpret_cast<const char*>(slot) + 8, 1);
        if (tag == 0) {
            continue;
        }
        ++hits;
        const std::string viaStr = g_numBags[i].via;
        const std::wstring viaText(viaStr.begin(), viaStr.end());

        void* rec = nullptr;
        std::memcpy(&rec, slot, sizeof(rec));
    }

}

void* onResolveVarBegin(void* self, const void* name)
{
    void* const saved = t_varBag;
    t_varBag = nullptr;
    if (!kSubstituteEnabled || self == nullptr) {
        return saved;
    }
    const auto at = reinterpret_cast<const char*>(self) + 8;
    if (!memory::isReadable(at, sizeof(void*))) {
        return saved;
    }
    void* bag = nullptr;
    std::memcpy(&bag, at, sizeof(bag));
    t_varBag = bag;

    if (name == nullptr || !memory::isReadable(name, 0x20)) {
        return saved;
    }
    std::size_t cap = 0;
    std::memcpy(&cap, reinterpret_cast<const char*>(name) + 0x18, sizeof(cap));
    const char* text = reinterpret_cast<const char*>(name);
    if (cap > 15) {
        std::memcpy(&text, name, sizeof(text));
    }
    if (text == nullptr || !memory::isReadable(text, 24)) {
        return saved;
    }

    char via[kNumBagViaBytes]{};
    std::snprintf(via, sizeof(via), "%.15s", text);
    rememberNumBag(bag, via);
    return saved;
}

void onBindingRead(void* bag)
{
    if (!kSubstituteEnabled || bag == nullptr || !memory::isReadable(bag, 16)) {
        return;
    }
    unsigned char tag = 0;
    std::memcpy(&tag, reinterpret_cast<const char*>(bag) + 8, 1);
    if (tag != 7) {
        return;
    }
    addNumBag(bag, "bind", 0);
}

void onControlsBindingName(void* self, void* arg3)
{
    if (self == nullptr) {
        return;
    }
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > 6) {
        return;
    }

    if (!memory::isReadable(self, 0x10)) {
        return;
    }
    std::uintptr_t arr = 0;
    std::uintptr_t idxPtr = 0;
    std::memcpy(&arr, self, sizeof(arr));
    std::memcpy(&idxPtr, reinterpret_cast<const char*>(self) + 8, sizeof(idxPtr));
    int idx = -1;
    if (idxPtr != 0 && memory::isReadable(reinterpret_cast<const void*>(idxPtr), 4)) {
        std::memcpy(&idx, reinterpret_cast<const void*>(idxPtr), sizeof(idx));
    }
    if (arr == 0 || !memory::isReadable(reinterpret_cast<const void*>(arr), 8)) {
        return;
    }
    std::uintptr_t base = 0;
    std::memcpy(&base, reinterpret_cast<const void*>(arr), sizeof(base));
    if (base == 0 || idx < 0) {
        return;
    }
    const std::uintptr_t elem = base + static_cast<std::uintptr_t>(idx) * 0x60;
    if (!memory::isReadable(reinterpret_cast<const void*>(elem), 0x60)) {
        return;
    }
}

void onControlsRowBindings(void* rcx, void* rdx, void* ctx)
{
    constexpr int kMaxLogs = 8;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxLogs) {
        return;
    }

    std::string name;

}

bool readFacetName(const void* at, std::string& out)
{
    if (at == nullptr || !memory::isReadable(at, sizeof(void*) * 2)) {
        return false;
    }
    std::uintptr_t ptr = 0;
    std::uintptr_t len = 0;
    std::memcpy(&ptr, at, sizeof(ptr));
    std::memcpy(&len, reinterpret_cast<const char*>(at) + sizeof(ptr), sizeof(len));
    if (ptr == 0 || len == 0 || len > 128) {
        return false;
    }
    if (!memory::isReadable(reinterpret_cast<const void*>(ptr), len)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(ptr), len);
    return true;
}

void onOreFacetBind(void* out, void* rdx, void* name, unsigned flag)
{
    std::string text;
    if (!readFacetName(name, text)) {
        static std::atomic<bool> said{false};
        return;
    }

    constexpr size_t kMaxFacetNames = 120;
    static std::mutex mutex;
    static std::set<std::string> seen;
    static bool capped = false;
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (capped) {
            return;
        }
        if (!seen.insert(text).second) {
            return;
        }
        if (seen.size() > kMaxFacetNames) {
            capped = true;
            log().warn(L"UiProbe: stopped listing facet names at {} kinds", kMaxFacetNames);
            return;
        }
    }
    const std::wstring wide(text.begin(), text.end());

    if (text.rfind("keyboardAndMouse.inputGroup", 0) == 0) {
        static std::atomic<bool> traced{false};
    }
    (void)rdx;
}

void onOreKeyboardInputGroup(void* self, void* out)
{
    constexpr int kMaxLogs = 4;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxLogs) {
        return;
    }

}

std::uintptr_t g_oreKeyRowsWrapAddr = 0;
std::atomic<int> g_rowNameCalls{0};

int bumpKeyRowLimit(void* container, int delta)
{
    if (container == nullptr || !memory::isReadable(container, 0x18)) {
        return 0;
    }
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, reinterpret_cast<const char*>(container) + 0x08, sizeof(begin));
    std::memcpy(&end, reinterpret_cast<const char*>(container) + 0x10, sizeof(end));
    if (begin == 0 || end <= begin || (end - begin) % 0x40 != 0) {
        return 0;
    }
    const std::uintptr_t at = end - 0x40 + 0x38;
    if (!memory::isWritable(reinterpret_cast<void*>(at), 1)) {
        return 0;
    }
    unsigned char count = 0;
    std::memcpy(&count, reinterpret_cast<const void*>(at), 1);
    const int next = static_cast<int>(count) + delta;
    if (next < 0 || next > 255) {
        return 0;
    }
    const auto value = static_cast<unsigned char>(next);
    std::memcpy(reinterpret_cast<void*>(at), &value, 1);
    return delta;
}

void dumpKeyRowContainer(void* container, void* arg3)
{
    constexpr int kMaxCalls = 6;
    constexpr size_t kMaxElements = 64;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxCalls) {
        return;
    }
    if (container == nullptr || !memory::isReadable(container, 0x50)) {
        return;
    }

    const auto* const base = reinterpret_cast<const char*>(container);

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, base + 0x08, sizeof(begin));
    std::memcpy(&end, base + 0x10, sizeof(end));
    if (begin == 0 || end < begin || (end - begin) % 0x40 != 0) {
        return;
    }
    const auto count = static_cast<size_t>((end - begin) / 0x40);

    std::uintptr_t alias = 0;
    std::uintptr_t begin2 = 0;
    std::uintptr_t end2 = 0;
    std::memcpy(&alias, base + 0x48, sizeof(alias));
    if (alias != 0 && memory::isReadable(reinterpret_cast<const void*>(alias), 0x10)) {
        std::memcpy(&begin2, reinterpret_cast<const void*>(alias), sizeof(begin2));
        std::memcpy(&end2, reinterpret_cast<const char*>(alias) + 8, sizeof(end2));
    }

    size_t sum = 0;
    for (size_t k = 0; k < count; ++k) {
        const auto* const elem = reinterpret_cast<const char*>(begin + k * 0x40);
        if (!memory::isReadable(elem, 0x40)) {
            continue;
        }
        unsigned char cnt = 0;
        std::memcpy(&cnt, elem + 0x38, 1);
        sum += cnt;
        if (k >= kMaxElements) {
            continue;
        }
        std::uintptr_t len = 0;
        std::uintptr_t room = 0;
        std::memcpy(&len, elem + 0x10, sizeof(len));
        std::memcpy(&room, elem + 0x18, sizeof(room));
        std::string id;
        std::wstring keys;
        std::uintptr_t keysBegin = 0;
        std::uintptr_t keysEnd = 0;
        std::memcpy(&keysBegin, elem + 0x20, sizeof(keysBegin));
        std::memcpy(&keysEnd, elem + 0x28, sizeof(keysEnd));
        if (keysBegin != 0 && keysEnd > keysBegin && (keysEnd - keysBegin) % sizeof(int) == 0
            && memory::isReadable(reinterpret_cast<const void*>(keysBegin),
                                  static_cast<size_t>(keysEnd - keysBegin))) {
            const size_t count = static_cast<size_t>(keysEnd - keysBegin) / sizeof(int);
            for (size_t i = 0; i < count && i < 8; ++i) {
                int value = 0;
                std::memcpy(&value, reinterpret_cast<const char*>(keysBegin) + i * sizeof(int),
                            sizeof(value));
                keys += std::to_wstring(value);
                keys += L' ';
            }
        } else {
            keys = L"<unreadable>";
        }
    }
}

constexpr size_t kRowEntrySize = 0x40;
constexpr size_t kMaxRowEntries = 128;

constexpr size_t kMaxOwnKeyRows = 40;

struct OwnKeyRow {
    char id[16]{};
    char label[64]{};
    int keyCode = 0;
    int lastSeen = 0;
    int defaultKey = 0;
    int module = -1;
    int child = -1;
};

OwnKeyRow g_ownKeyRows[kMaxOwnKeyRows];
size_t g_ownKeyRowCount = 0;

std::atomic<int> g_keyRowFilterModule{-1};

std::vector<MenuItem> sortedMenuItems()
{
    std::vector<MenuItem> tree = ModuleManager::instance().buildMenuItems();
    std::sort(tree.begin(), tree.end(), [](const MenuItem& a, const MenuItem& b) {
        const std::wstring left = a.labelText();
        const std::wstring right = b.labelText();
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](wchar_t x, wchar_t y) { return towlower(x) < towlower(y); });
    });
    return tree;
}

size_t ensureOwnKeyRows()
{
    if (g_ownKeyRowCount != 0) {
        return g_ownKeyRowCount;
    }
    const std::vector<MenuItem> tree = sortedMenuItems();
    for (size_t i = 0; i < tree.size() && g_ownKeyRowCount < kMaxOwnKeyRows; ++i) {
        const MenuItem& tab = tree[i];
        for (size_t c = 0; c < tab.children.size() && g_ownKeyRowCount < kMaxOwnKeyRows; ++c) {
            const MenuItem& one = tab.children[c];
            if (one.kind != MenuItemKind::Keybind || !one.setKeys || !one.getKeys) {
                continue;
            }
            OwnKeyRow& row = g_ownKeyRows[g_ownKeyRowCount];
            std::snprintf(row.id, sizeof(row.id), "key.tk.%02zu", g_ownKeyRowCount);
            copyAscii(row.label, sizeof(row.label),
                      (tab.labelText() + L" " + one.labelText()).c_str());
            row.module = static_cast<int>(i);
            row.child = static_cast<int>(c);
            const std::vector<int> keys = one.getKeys();
            row.keyCode = keys.empty() ? 0 : keys.front();
            row.lastSeen = row.keyCode;
            row.defaultKey = one.defaultKeys.empty() ? 0 : one.defaultKeys.front();
            ++g_ownKeyRowCount;
        }
    }
    return g_ownKeyRowCount;
}

int ownKeyRowIndexFromBindingIndex(const void* self, std::uintptr_t index) noexcept
{
    if (self == nullptr || !memory::isReadable(self, 16)) {
        return -1;
    }
    std::uintptr_t holder = 0;
    std::memcpy(&holder, static_cast<const char*>(self) + 8, sizeof(holder));
    if (holder == 0 || !memory::isReadable(reinterpret_cast<const void*>(holder), 16)) {
        return -1;
    }
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, reinterpret_cast<const void*>(holder), sizeof(begin));
    std::memcpy(&end, reinterpret_cast<const char*>(holder) + 8, sizeof(end));
    if (begin == 0 || end <= begin) {
        return -1;
    }
    const size_t count = static_cast<size_t>(end - begin) / 0x40;
    if (index >= count) {
        return -1;
    }
    const auto* const elem = reinterpret_cast<const char*>(begin + index * 0x40);
    if (!memory::isReadable(elem, 0x20)) {
        return -1;
    }
    std::string id;
    if (!readStdString(elem, id)) {
        return -1;
    }
    if (id.rfind("key.tk.", 0) != 0) {
        return -1;
    }
    size_t at = 7;
    size_t number = 0;
    if (at >= id.size() || id[at] < '0' || id[at] > '9') {
        return -1;
    }
    while (at < id.size() && id[at] >= '0' && id[at] <= '9') {
        number = number * 10 + static_cast<size_t>(id[at] - '0');
        ++at;
    }
    if (at != id.size() || number >= g_ownKeyRowCount) {
        return -1;
    }
    return static_cast<int>(number);
}

int ownKeyRowIndexFromCompId(const char* id) noexcept
{
    if (id == nullptr) {
        return -1;
    }
    const char* const at = std::strstr(id, "key.tk.");
    if (at == nullptr) {
        return -1;
    }
    const char* p = at + 7;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    size_t index = 0;
    while (*p >= '0' && *p <= '9') {
        index = index * 10 + static_cast<size_t>(*p - '0');
        ++p;
    }
    if (*p != 0) {
        return -1;
    }
    return (index < g_ownKeyRowCount) ? static_cast<int>(index) : -1;
}

const char* ownKeyRowLabel(const char* key) noexcept
{
    constexpr size_t kPrefix = 7;
    if (key == nullptr || std::strncmp(key, "key.tk.", kPrefix) != 0) {
        return nullptr;
    }
    const char* p = key + kPrefix;
    if (*p < '0' || *p > '9') {
        return nullptr;
    }
    size_t index = 0;
    while (*p >= '0' && *p <= '9') {
        index = index * 10 + static_cast<size_t>(*p - '0');
        ++p;
    }
    if (index >= g_ownKeyRowCount) {
        return nullptr;
    }
    if (*p != 0 && std::strcmp(p, ".name") != 0 && std::strcmp(p, ".description") != 0) {
        return nullptr;
    }
    return g_ownKeyRows[index].label;
}

constexpr size_t kMaxRowSpans = 8;

alignas(16) unsigned char g_rowEntryBuffer[kMaxRowSpans][kMaxRowEntries * kRowEntrySize];

alignas(16) unsigned char g_rowDefaultBuffer[kMaxRowSpans][kMaxRowEntries * kRowEntrySize];

struct SavedRowSpan {
    void* container = nullptr;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t defBegin = 0;
    std::uintptr_t defEnd = 0;
};

SavedRowSpan g_savedRowSpans[kMaxRowSpans];
size_t g_savedRowSpanCount = 0;

void restoreKeyRowsFor(void* container);

bool purgeOwnKeyRows(void* container)
{
    if (g_savedRowSpanCount >= kMaxRowSpans || container == nullptr
        || !memory::isReadable(container, 0x50)) {
        return false;
    }
    auto* const base = reinterpret_cast<char*>(container);
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t alias = 0;
    std::memcpy(&begin, base + 0x08, sizeof(begin));
    std::memcpy(&end, base + 0x10, sizeof(end));
    std::memcpy(&alias, base + 0x48, sizeof(alias));
    if (alias != reinterpret_cast<std::uintptr_t>(base) + 0x08 || begin == 0 || end <= begin
        || (end - begin) % kRowEntrySize != 0) {
        return false;
    }
    for (size_t i = 0; i < g_savedRowSpanCount; ++i) {
        if (g_savedRowSpans[i].container == container) {
            return false;
        }
    }
    const auto count = static_cast<size_t>((end - begin) / kRowEntrySize);
    if (count == 0 || count > kMaxRowEntries
        || !memory::isReadable(reinterpret_cast<const void*>(begin), count * kRowEntrySize)
        || !memory::isWritable(base + 0x08, 0x10)) {
        return false;
    }

    unsigned char* const buffer = g_rowEntryBuffer[g_savedRowSpanCount];
    size_t kept = 0;
    size_t dropped = 0;
    for (size_t k = 0; k < count; ++k) {
        const auto* const elem = reinterpret_cast<const char*>(begin + k * kRowEntrySize);
        std::string id;
        if (readStdString(elem, id) && id.rfind("key.tk.", 0) == 0) {
            ++dropped;
            continue;
        }
        std::memcpy(buffer + kept * kRowEntrySize, elem, kRowEntrySize);
        ++kept;
    }
    if (dropped == 0) {
        return false;
    }

    const auto newBegin = reinterpret_cast<std::uintptr_t>(buffer);
    const std::uintptr_t newEnd = newBegin + kept * kRowEntrySize;
    std::memcpy(base + 0x08, &newBegin, sizeof(newBegin));
    std::memcpy(base + 0x10, &newEnd, sizeof(newEnd));
    constexpr bool kPurgeIsPermanent = true;
    g_savedRowSpans[g_savedRowSpanCount] =
        kPurgeIsPermanent ? SavedRowSpan{} : SavedRowSpan{container, begin, end};
    ++g_savedRowSpanCount;

    static std::atomic<int> said{0};
    return true;
}

bool substituteKeyRows(void* container, bool refresh)
{
    if (g_savedRowSpanCount >= kMaxRowSpans) {
        return false;
    }
    if (container == nullptr || !memory::isReadable(container, 0x50)) {
        return false;
    }
    for (size_t i = 0; i < g_savedRowSpanCount; ++i) {
        if (g_savedRowSpans[i].container == container) {
            if (!refresh) {
                return false;
            }
            restoreKeyRowsFor(container);
            break;
        }
    }
    auto* const base = reinterpret_cast<char*>(container);
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t alias = 0;
    std::memcpy(&begin, base + 0x08, sizeof(begin));
    std::memcpy(&end, base + 0x10, sizeof(end));
    std::memcpy(&alias, base + 0x48, sizeof(alias));

    if (alias != reinterpret_cast<std::uintptr_t>(base) + 0x08) {
        return false;
    }
    if (begin == 0 || end <= begin || (end - begin) % kRowEntrySize != 0) {
        return false;
    }
    const auto count = static_cast<size_t>((end - begin) / kRowEntrySize);
    if (count == 0 || count + 1 > kMaxRowEntries) {
        return false;
    }
    if (!memory::isReadable(reinterpret_cast<const void*>(begin), count * kRowEntrySize)
        || !memory::isWritable(base + 0x08, 0x10)) {
        return false;
    }
    std::string first;
    if (!readStdString(reinterpret_cast<const char*>(begin), first)
        || first.rfind("key.", 0) != 0) {
        return false;
    }

    const size_t extras = ensureOwnKeyRows();
    size_t picked[kMaxOwnKeyRows]{};
    size_t pickCount = 0;
    for (size_t r = 0; r < extras && pickCount < kMaxOwnKeyRows; ++r) {
        picked[pickCount++] = r;
    }
    const size_t keep = count;
    if (pickCount == 0 || keep + pickCount > kMaxRowEntries) {
        return false;
    }
    unsigned char* const buffer = g_rowEntryBuffer[g_savedRowSpanCount];
    std::memcpy(buffer, reinterpret_cast<const void*>(begin), keep * kRowEntrySize);
    for (size_t k = 0; k < pickCount; ++k) {
        const size_t r = picked[k];
        unsigned char* const extra = buffer + (keep + k) * kRowEntrySize;
        std::memset(extra, 0, kRowEntrySize);
        const size_t idLength = std::strlen(g_ownKeyRows[r].id);
        std::memcpy(extra, g_ownKeyRows[r].id, idLength);
        std::uintptr_t value = idLength;
        std::memcpy(extra + kStringSize, &value, sizeof(value));
        value = kSsoCapacity;
        std::memcpy(extra + kStringCapacity, &value, sizeof(value));
        const auto keysBegin = reinterpret_cast<std::uintptr_t>(&g_ownKeyRows[r].keyCode);
        const std::uintptr_t keysEnd = keysBegin + sizeof(int);
        std::memcpy(extra + 0x20, &keysBegin, sizeof(keysBegin));
        std::memcpy(extra + 0x28, &keysEnd, sizeof(keysEnd));
        std::memcpy(extra + 0x30, &keysEnd, sizeof(keysEnd));
        value = 1;
        std::memcpy(extra + 0x38, &value, sizeof(value));
    }

    const auto newBegin = reinterpret_cast<std::uintptr_t>(buffer);
    const std::uintptr_t newEnd = newBegin + (keep + pickCount) * kRowEntrySize;

    std::uintptr_t defBegin = 0;
    std::uintptr_t defEnd = 0;
    bool defDone = false;
    if (memory::isReadable(base + 0x20, 0x10) && memory::isWritable(base + 0x20, 0x10)) {
        std::memcpy(&defBegin, base + 0x20, sizeof(defBegin));
        std::memcpy(&defEnd, base + 0x28, sizeof(defEnd));
        const size_t defCount =
            (defEnd > defBegin) ? static_cast<size_t>(defEnd - defBegin) / kRowEntrySize : 0;
        if (defCount > 0 && defCount + pickCount <= kMaxRowEntries
            && memory::isReadable(reinterpret_cast<const void*>(defBegin),
                                  defCount * kRowEntrySize)) {
            unsigned char* const defBuf = g_rowDefaultBuffer[g_savedRowSpanCount];
            std::memcpy(defBuf, reinterpret_cast<const void*>(defBegin),
                        defCount * kRowEntrySize);
            for (size_t k = 0; k < pickCount; ++k) {
                const size_t r = picked[k];
                unsigned char* const extra = defBuf + (defCount + k) * kRowEntrySize;
                std::memset(extra, 0, kRowEntrySize);
                const size_t idLength = std::strlen(g_ownKeyRows[r].id);
                std::memcpy(extra, g_ownKeyRows[r].id, idLength);
                std::uintptr_t value = idLength;
                std::memcpy(extra + kStringSize, &value, sizeof(value));
                value = kSsoCapacity;
                std::memcpy(extra + kStringCapacity, &value, sizeof(value));
                const auto keysBegin =
                    reinterpret_cast<std::uintptr_t>(&g_ownKeyRows[r].defaultKey);
                const std::uintptr_t keysEnd = keysBegin + sizeof(int);
                std::memcpy(extra + 0x20, &keysBegin, sizeof(keysBegin));
                std::memcpy(extra + 0x28, &keysEnd, sizeof(keysEnd));
                std::memcpy(extra + 0x30, &keysEnd, sizeof(keysEnd));
                value = 1;
                std::memcpy(extra + 0x38, &value, sizeof(value));
            }
            const auto newDefBegin = reinterpret_cast<std::uintptr_t>(defBuf);
            const std::uintptr_t newDefEnd =
                newDefBegin + (defCount + pickCount) * kRowEntrySize;
            std::memcpy(base + 0x20, &newDefBegin, sizeof(newDefBegin));
            std::memcpy(base + 0x28, &newDefEnd, sizeof(newDefEnd));
            defDone = true;
        }
    }

    std::memcpy(base + 0x08, &newBegin, sizeof(newBegin));
    std::memcpy(base + 0x10, &newEnd, sizeof(newEnd));
    g_savedRowSpans[g_savedRowSpanCount] =
        SavedRowSpan{container, begin, end, defDone ? defBegin : 0, defDone ? defEnd : 0};
    ++g_savedRowSpanCount;

    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 3) {
        std::uintptr_t defBegin = 0;
        std::uintptr_t defEnd = 0;
        if (memory::isReadable(base + 0x20, 0x10)) {
            std::memcpy(&defBegin, base + 0x20, sizeof(defBegin));
            std::memcpy(&defEnd, base + 0x28, sizeof(defEnd));
        }
        const size_t defCount =
            (defEnd > defBegin) ? static_cast<size_t>(defEnd - defBegin) / kRowEntrySize : 0;
        std::string defLast;
        if (defCount > 0
            && memory::isReadable(reinterpret_cast<const void*>(defBegin),
                                  defCount * kRowEntrySize)) {
            readStdString(reinterpret_cast<const char*>(defBegin + (defCount - 1) * kRowEntrySize),
                          defLast);
        }
    }
    return true;
}

void pumpControlsKeybind()
{
    if (g_ownKeyRowCount == 0) {
        return;
    }
    std::vector<MenuItem> tree;
    for (size_t r = 0; r < g_ownKeyRowCount; ++r) {
        OwnKeyRow& row = g_ownKeyRows[r];
        const int current = row.keyCode;
        if (current == row.lastSeen) {
            continue;
        }
        row.lastSeen = current;
        if (tree.empty()) {
            tree = sortedMenuItems();
        }
        if (row.module < 0 || static_cast<size_t>(row.module) >= tree.size()) {
            continue;
        }
        const MenuItem& tab = tree[static_cast<size_t>(row.module)];
        if (row.child < 0 || static_cast<size_t>(row.child) >= tab.children.size()) {
            continue;
        }
        const MenuItem& child = tab.children[static_cast<size_t>(row.child)];
        if (child.kind != MenuItemKind::Keybind || !child.setKeys) {
            continue;
        }
        if (current > 0) {
            child.setKeys({current});
        } else {
            child.setKeys({});
        }
    }
}

void restoreKeyRowsFor(void* container)
{
    for (size_t i = 0; i < g_savedRowSpanCount; ++i) {
        SavedRowSpan& span = g_savedRowSpans[i];
        if (span.container != container) {
            continue;
        }
        auto* const base = reinterpret_cast<char*>(span.container);
        if (memory::isWritable(base + 0x08, 0x10)) {
            std::memcpy(base + 0x08, &span.begin, sizeof(span.begin));
            std::memcpy(base + 0x10, &span.end, sizeof(span.end));
        }
        if (span.defBegin != 0 && memory::isWritable(base + 0x20, 0x10)) {
            std::memcpy(base + 0x20, &span.defBegin, sizeof(span.defBegin));
            std::memcpy(base + 0x28, &span.defEnd, sizeof(span.defEnd));
        }
        for (size_t k = i + 1; k < g_savedRowSpanCount; ++k) {
            g_savedRowSpans[k - 1] = g_savedRowSpans[k];
        }
        --g_savedRowSpanCount;
        g_savedRowSpans[g_savedRowSpanCount] = SavedRowSpan{};
        return;
    }
}

void popKeyRowSubstitution()
{
    if (g_savedRowSpanCount == 0) {
        return;
    }
    --g_savedRowSpanCount;
    SavedRowSpan& span = g_savedRowSpans[g_savedRowSpanCount];
    if (span.container != nullptr) {
        auto* const base = reinterpret_cast<char*>(span.container);
        if (memory::isWritable(base + 0x08, 0x10)) {
            std::memcpy(base + 0x08, &span.begin, sizeof(span.begin));
            std::memcpy(base + 0x10, &span.end, sizeof(span.end));
        }
        if (span.defBegin != 0 && memory::isWritable(base + 0x20, 0x10)) {
            std::memcpy(base + 0x20, &span.defBegin, sizeof(span.defBegin));
            std::memcpy(base + 0x28, &span.defEnd, sizeof(span.defEnd));
        }
    }
    span = SavedRowSpan{};
}

void restoreKeyRows()
{
    while (g_savedRowSpanCount > 0) {
        popKeyRowSubstitution();
    }
}

void onOreKeyRowData(void* container, std::uintptr_t index, bool substituted)
{
    constexpr int kMaxHeadLogs = 3;
    constexpr int kMaxTailLogs = 10;
    constexpr std::uintptr_t kTailFrom = 40;
    static std::atomic<int> head{0};
    static std::atomic<int> tail{0};
    int n = 0;
    if (index >= kTailFrom) {
        n = tail.fetch_add(1) + 1;
        if (n > kMaxTailLogs) {
            return;
        }
    } else {
        n = head.fetch_add(1) + 1;
        if (n > kMaxHeadLogs) {
            return;
        }
    }
    size_t count = 0;
    if (container != nullptr && memory::isReadable(container, 0x18)) {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::memcpy(&begin, reinterpret_cast<const char*>(container) + 0x08, sizeof(begin));
        std::memcpy(&end, reinterpret_cast<const char*>(container) + 0x10, sizeof(end));
        if (begin != 0 && end > begin && (end - begin) % kRowEntrySize == 0) {
            count = static_cast<size_t>((end - begin) / kRowEntrySize);
        }
    }
}

void onOreKeyRowDataResult(void* out, std::uintptr_t index)
{
    constexpr int kMaxLogs = 10;
    constexpr std::uintptr_t kTailFrom = 40;
    if (index < kTailFrom) {
        return;
    }
    static std::atomic<int> seen{0};
    if (seen.fetch_add(1) >= kMaxLogs) {
        return;
    }
    std::string text;
    if (!readStdString(reinterpret_cast<const char*>(out), text)) {
        return;
    }
}

bool copyStdStringFast(const void* at, char* out, size_t cap) noexcept;

constexpr char kOwnSettingsNameKey[] = "tk.tsukuyomi";
constexpr char kOwnSettingsNameText[] = "Tsukuyomi";

const char* ownSettingsText(const char* key);

void flushOwnPublishes();

bool overrideTranslation(const void* key, void* out) noexcept
{
    if (key == nullptr || out == nullptr) {
        return false;
    }
    char buf[80]{};
    if (!copyStdStringFast(key, buf, sizeof(buf))) {
        return false;
    }
    flushOwnPublishes();
    pumpControlsKeybind();
    const char* text = nullptr;
    if (const int leftRow = pageLeftRowFromKey(buf); leftRow >= 0) {
        text = pageLeftLabel(static_cast<size_t>(leftRow));
    } else if (std::strcmp(buf, kOwnSettingsNameKey) == 0) {
        text = kOwnSettingsNameText;
    } else if (const char* const own = ownSettingsText(buf); own != nullptr) {
        text = own;
    } else if (const char* const rowLabel = ownKeyRowLabel(buf); rowLabel != nullptr) {
        text = rowLabel;
    } else {
        constexpr size_t kIdLength = 7;
        if (std::strncmp(buf, "key.tk.", kIdLength) != 0) {
            if (buf[0] == 't' && buf[1] == 'k' && buf[2] == '.') {
                static std::atomic<int> missed{0};
                if (missed.fetch_add(1) < 30) {
                    log().warn(L"UiProbe: could not answer for key \"{}\"",
                               std::wstring(buf, buf + std::strlen(buf)));
                }
            }
            return false;
        }
        return false;
    }
    const size_t len = std::strlen(text);

    auto* const head = static_cast<char*>(out);
    if (!memory::isWritable(head, 0x20)) {
        return false;
    }
    std::uintptr_t room = 0;
    std::memcpy(&room, head + kStringCapacity, sizeof(room));
    if (room > 0x400) {
        return false;
    }
    char* dest = head;
    if (room < len) {
        void* const buf = hooks::callGameAllocate(len + 1);
        if (buf == nullptr) {
            return false;
        }
        dest = static_cast<char*>(buf);
        const auto ptr = reinterpret_cast<std::uintptr_t>(buf);
        std::memcpy(head, &ptr, sizeof(ptr));
        const std::uintptr_t newRoom = len;
        std::memcpy(head + kStringCapacity, &newRoom, sizeof(newRoom));
    } else if (room > kSsoCapacity) {
        std::memcpy(&dest, head, sizeof(dest));
        if (dest == nullptr || !memory::isWritable(dest, len + 1)) {
            return false;
        }
    }
    std::memcpy(dest, text, len + 1);
    const std::uintptr_t size = len;
    std::memcpy(head + kStringSize, &size, sizeof(size));

    static std::atomic<int> said{0};
    return true;
}

void onOreKeyRowsBegin()
{
    g_rowNameCalls.store(0);
}

constexpr bool kAddTestRow = false;

constexpr bool kOverwriteOnly = true;
constexpr std::uintptr_t kTestRowIndex = 50;
constexpr std::uintptr_t kMinRowsToTouch = 20;

void onOreKeyRowsEnd(void* out)
{

    constexpr int kMaxLogs = 4;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxLogs) {
        return;
    }
    if (out == nullptr || !memory::isReadable(out, 24)) {
        return;
    }
    std::uintptr_t vec[3]{};
    std::memcpy(vec, out, sizeof(vec));
    const std::uintptr_t bytes = (vec[1] >= vec[0]) ? (vec[1] - vec[0]) : 0;

    if (kAddTestRow && vec[0] != 0 && bytes >= kMinRowsToTouch * 8
        && vec[1] + sizeof(std::uintptr_t) <= vec[2]
        && memory::isWritable(reinterpret_cast<void*>(vec[1]), sizeof(std::uintptr_t))
        && memory::isWritable(out, sizeof(vec))) {
        if (kOverwriteOnly) {
            std::memcpy(reinterpret_cast<void*>(vec[0]), &kTestRowIndex, sizeof(kTestRowIndex));
            return;
        }

        std::memmove(reinterpret_cast<void*>(vec[0] + sizeof(std::uintptr_t)),
                     reinterpret_cast<const void*>(vec[0]), bytes);
        std::memcpy(reinterpret_cast<void*>(vec[0]), &kTestRowIndex, sizeof(kTestRowIndex));
        const std::uintptr_t newEnd = vec[1] + sizeof(std::uintptr_t);
        std::memcpy(reinterpret_cast<char*>(out) + sizeof(std::uintptr_t), &newEnd,
                    sizeof(newEnd));
    }

    if (vec[0] != 0 && bytes >= 8
        && memory::isReadable(reinterpret_cast<const void*>(vec[0]), bytes)) {
        const size_t count = bytes / 8;
        std::wstring line;
        for (size_t i = 0; i < count && i < 64; ++i) {
            std::uintptr_t v = 0;
            std::memcpy(&v, reinterpret_cast<const char*>(vec[0]) + i * 8, sizeof(v));
            line += std::to_wstring(v);
            line += L' ';
        }
    }
}

constexpr int kOverrideIndex = -1;
constexpr const char* kOverrideName = "key.inventory";

bool overrideKeyActionName(void* out, int index)
{
    if (index != kOverrideIndex || out == nullptr || !memory::isWritable(out, 0x20)) {
        return false;
    }
    std::uintptr_t cap = 0;
    std::memcpy(&cap, reinterpret_cast<const char*>(out) + 0x18, sizeof(cap));
    const size_t len = std::strlen(kOverrideName);
    if (cap > 15 || len > 15) {
        return false;
    }
    std::memcpy(out, kOverrideName, len + 1);
    const std::uintptr_t size = len;
    std::memcpy(reinterpret_cast<char*>(out) + 0x10, &size, sizeof(size));
    return true;
}

void onOreKeyRowsConsume(void* rcx, void* rdx, bool after)
{
    constexpr int kMaxLogs = 6;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxLogs) {
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto inModule = [base](std::uintptr_t p) {
        return base != 0 && p > base && p < base + 0x13000000;
    };
    int found = 0;
    for (void* obj : {rcx, rdx}) {
        if (obj == nullptr || !memory::isReadable(obj, 0x200)) {
            continue;
        }
        for (std::ptrdiff_t off = 0; off < 0x200 && found < 6; off += 8) {
            std::uintptr_t p = 0;
            std::memcpy(&p, reinterpret_cast<const char*>(obj) + off, sizeof(p));
            if (p == 0 || !memory::isReadable(reinterpret_cast<const void*>(p), 0x80)) {
                continue;
            }
            std::uintptr_t q = 0;
            std::memcpy(&q, reinterpret_cast<const char*>(p) + 0x78, sizeof(q));
            if (q == 0 || !memory::isReadable(reinterpret_cast<const void*>(q), 8)) {
                continue;
            }
            std::uintptr_t vft = 0;
            std::memcpy(&vft, reinterpret_cast<const void*>(q), sizeof(vft));
            if (!inModule(vft) || !memory::isReadable(reinterpret_cast<const void*>(vft), 0x20)) {
                continue;
            }
            std::uintptr_t fn = 0;
            std::memcpy(&fn, reinterpret_cast<const char*>(vft) + 0x10, sizeof(fn));
            if (!inModule(fn)) {
                continue;
            }
            ++found;
        }
    }
}

bool copyStdStringFast(const void* at, char* out, size_t cap) noexcept
{
    __try {
        const auto* p = static_cast<const char*>(at);
        const size_t size = *reinterpret_cast<const size_t*>(p + 0x10);
        const size_t capacity = *reinterpret_cast<const size_t*>(p + 0x18);
        if (size == 0 || size > 250 || capacity < size) {
            return false;
        }
        const char* body = (capacity <= 15) ? p : *reinterpret_cast<const char* const*>(p);
        if (body == nullptr) {
            return false;
        }
        const size_t n = (size < cap - 1) ? size : cap - 1;
        for (size_t i = 0; i < n; ++i) {
            out[i] = body[i];
        }
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void onTranslate(const void* key)
{
    if (key == nullptr) {
        return;
    }
    char buf[256]{};
    if (!copyStdStringFast(key, buf, sizeof(buf))) {
        return;
    }
    if (buf[0] != 'k' || buf[1] != 'e' || buf[2] != 'y' || buf[3] != '.') {
        return;
    }
    std::string text(buf);
    if (text.rfind("key.tk", 0) == 0) {
        static std::atomic<int> ownSeen{0};
        return;
    }
    constexpr size_t kMaxNames = 90;
    static std::mutex mutex;
    static std::set<std::string> seen;
    static bool capped = false;
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (capped || !seen.insert(text).second) {
            return;
        }
        if (seen.size() > kMaxNames) {
            capped = true;
            log().warn(L"UiProbe: stopped listing keys at {} kinds", kMaxNames);
            return;
        }
    }
}

void onOreKeyNameToIndex(void* arg1)
{
    std::string text;
    if (!readFacetName(arg1, text) && !readStdString(reinterpret_cast<const char*>(arg1), text)) {
        static std::atomic<bool> said{false};
        return;
    }
    if (text.empty()) {
        return;
    }
    constexpr size_t kMaxNames = 100;
    static std::mutex mutex;
    static std::set<std::string> seen;
    static bool capped = false;
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (capped || !seen.insert(text).second) {
            return;
        }
        if (seen.size() > kMaxNames) {
            capped = true;
            log().warn(L"UiProbe: stopped listing names at {} kinds", kMaxNames);
            return;
        }
    }
}

void onRowDataCandidate(int which, void* arg4)
{
    static std::atomic<int> counts[2]{};
    const int n = counts[which & 1].fetch_add(1) + 1;
    if (n > 3 && n != 48 && n != 50 && n != 96 && n != 100) {
        return;
    }
    std::uintptr_t idx = 0;
    if (arg4 != nullptr && memory::isReadable(arg4, 8)) {
        std::memcpy(&idx, arg4, sizeof(idx));
    }
}

void setOreKeyRowsWrapAddr(void* address)
{
    g_oreKeyRowsWrapAddr = reinterpret_cast<std::uintptr_t>(address);
}

void onOreKeyRowsWrap(void* out, void* container)
{
    constexpr int kMaxLogs = 4;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxLogs) {
        return;
    }
    if (n == 1) {
        const std::uintptr_t want = g_oreKeyRowsWrapAddr;
        std::uintptr_t probe = reinterpret_cast<std::uintptr_t>(&want);
        bool found = false;
        for (std::ptrdiff_t off = 0; off < 0x4000 && !found; off += 8) {
            const auto slot = reinterpret_cast<const void*>(probe + off);
            if (!memory::isReadable(slot, 8)) {
                continue;
            }
            std::uintptr_t obj = 0;
            std::memcpy(&obj, slot, sizeof(obj));
            if (obj == 0 || !memory::isReadable(reinterpret_cast<const void*>(obj), 0x80)) {
                continue;
            }
            std::uintptr_t inner38 = 0;
            std::memcpy(&inner38, reinterpret_cast<const char*>(obj) + 0x38, sizeof(inner38));
            if (inner38 == 0 || !memory::isReadable(reinterpret_cast<const void*>(inner38), 8)) {
                continue;
            }
            std::uintptr_t vft38 = 0;
            std::memcpy(&vft38, reinterpret_cast<const void*>(inner38), sizeof(vft38));
            if (vft38 == 0 || !memory::isReadable(reinterpret_cast<const void*>(vft38), 0x20)) {
                continue;
            }
            std::ptrdiff_t hitSlot = -1;
            for (std::ptrdiff_t slot = 0; slot < 0x40; slot += 8) {
                std::uintptr_t fn38 = 0;
                std::memcpy(&fn38, reinterpret_cast<const char*>(vft38) + slot, sizeof(fn38));
                if (fn38 == want) {
                    hitSlot = slot;
                    break;
                }
            }
            if (hitSlot < 0) {
                continue;
            }
            found = true;
            std::uintptr_t inner78 = 0;
            std::memcpy(&inner78, reinterpret_cast<const char*>(obj) + 0x78, sizeof(inner78));
            if (inner78 == 0 || !memory::isReadable(reinterpret_cast<const void*>(inner78), 8)) {
                break;
            }
            std::uintptr_t vft78 = 0;
            std::memcpy(&vft78, reinterpret_cast<const void*>(inner78), sizeof(vft78));
            std::uintptr_t fn78 = 0;
            if (memory::isReadable(reinterpret_cast<const void*>(vft78), 0x20)) {
                std::memcpy(&fn78, reinterpret_cast<const char*>(vft78) + 0x10, sizeof(fn78));
            }
        }
    }
}

void onKeyBindingLookup(const void* name)
{
    std::string text;
    if (!readStdString(reinterpret_cast<const char*>(name), text) || text.empty()) {
        return;
    }
    constexpr size_t kMaxNames = 120;
    static std::mutex mutex;
    static std::set<std::string> seen;
    static bool capped = false;
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (capped || !seen.insert(text).second) {
            return;
        }
        if (seen.size() > kMaxNames) {
            capped = true;
            log().warn(L"UiProbe: stopped listing names at {} kinds", kMaxNames);
            return;
        }
    }
}

void onControlsSectionSetup(void* self, void* arg2)
{
    constexpr int kMaxLogs = 4;
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > kMaxLogs) {
        return;
    }

    if (self != nullptr && memory::isReadable(self, sizeof(void*))) {
        std::uintptr_t vft = 0;
        std::memcpy(&vft, self, sizeof(vft));
    }

}

void onKeybindListBuilt(void* self)
{
    if (self == nullptr) {
        return;
    }
    static std::atomic<int> seen{0};
    const int n = seen.fetch_add(1) + 1;
    if (n > 4) {
        return;
    }

    const auto at = reinterpret_cast<const char*>(self) + 0xd8;
    if (!memory::isReadable(at, 24)) {
        return;
    }
    std::uintptr_t vec[3]{};
    std::memcpy(vec, at, sizeof(vec));

    int hits = 0;
    for (std::ptrdiff_t off = 0; off + 0x20 <= 0x600 && hits < 12; off += sizeof(void*)) {
        const auto* head = reinterpret_cast<const char*>(self) + off;
        if (!memory::isReadable(head, 0x20)) {
            continue;
        }
        std::uintptr_t len = 0;
        std::uintptr_t room = 0;
        std::memcpy(&len, head + 0x10, sizeof(len));
        std::memcpy(&room, head + 0x18, sizeof(room));
        if (len == 0 || room < 15 || room > 0x400 || len > room || len > 60) {
            continue;
        }
        const char* text = head;
        if (room > 15) {
            std::memcpy(&text, head, sizeof(text));
            if (text == nullptr || !memory::isReadable(text, static_cast<size_t>(len) + 1)) {
                continue;
            }
        }
        char buf[72]{};
        std::memcpy(buf, text, static_cast<size_t>(len));
        const std::string s = buf;
        ++hits;
    }

    {
        for (const std::ptrdiff_t base : {0x18, 0x58, 0x98, 0xd8, 0x118}) {
            std::uintptr_t h = 0;
            std::uintptr_t c = 0;
            const auto* p = reinterpret_cast<const char*>(self) + base;
            if (!memory::isReadable(p, 0x18)) {
                continue;
            }
            std::memcpy(&h, p + 8, sizeof(h));
            std::memcpy(&c, p + 0x10, sizeof(c));

            if (h == 0 || !memory::isReadable(reinterpret_cast<const void*>(h), 0x18)) {
                continue;
            }
            std::uintptr_t nd = 0;
            std::memcpy(&nd, reinterpret_cast<const void*>(h), sizeof(nd));
            for (int n = 0; n < 2 && nd != 0 && nd != h; ++n) {
                if (!memory::isReadable(reinterpret_cast<const void*>(nd), 0x50)) {
                    break;
                }
                std::memcpy(&nd, reinterpret_cast<const void*>(nd), sizeof(nd));
            }
        }

        std::uintptr_t head = 0;
        std::uintptr_t count = 0;
        std::memcpy(&head, reinterpret_cast<const char*>(self) + 0xe0, sizeof(head));
        std::memcpy(&count, reinterpret_cast<const char*>(self) + 0xe8, sizeof(count));
        if (head == 0 || !memory::isReadable(reinterpret_cast<const void*>(head), 0x18)) {
            return;
        }
        std::uintptr_t node = 0;
        std::memcpy(&node, reinterpret_cast<const void*>(head), sizeof(node));
        for (int n = 0; n < 6 && node != 0 && node != head; ++n) {
            if (!memory::isReadable(reinterpret_cast<const void*>(node), 0x50)) {
                break;
            }
            std::memcpy(&node, reinterpret_cast<const void*>(node), sizeof(node));
        }
    }
}

void onResolveVarEnd(void* saved)
{
    t_varBag = saved;
}

constexpr float kMenuFirstY = 38.0f;
constexpr float kMenuPitchY = 28.0f;
constexpr float kMenuMaxX = 190.0f;

constexpr unsigned kMouseEventType = 0xb447f8e7u;
constexpr unsigned kKeyEventType = 0xf9fbc001u;

int menuIndexFromName(const std::wstring& name, const wchar_t* suffix)
{
    const size_t tail = std::wcslen(suffix);
    if (name.size() < 4 + tail) {
        return -1;
    }
    const size_t at = name.size() - (4 + tail);
    if (name.compare(at, 2, L"tk") != 0 || name.compare(at + 4, tail, suffix) != 0) {
        return -1;
    }
    const wchar_t hi = name[at + 2];
    const wchar_t lo = name[at + 3];
    if (hi < L'0' || hi > L'9' || lo < L'0' || lo > L'9') {
        return -1;
    }
    if (at != 0 && name[at - 1] != L'.') {
        return -1;
    }
    const int idx = (hi - L'0') * 10 + (lo - L'0');
    return (idx >= 0 && static_cast<size_t>(idx) < g_menuCount) ? idx : -1;
}

constexpr bool kProbeControlNames = false;

constexpr std::ptrdiff_t kNameScanSpan = 0x1400;
constexpr int kMaxNameHits = 48;

std::wstring readWideString(const void* text)
{
    constexpr size_t kWideSso = 7;
    if (!memory::isReadable(text, static_cast<size_t>(kStringCapacity) + sizeof(size_t))) {
        return {};
    }
    const auto* const base = static_cast<const std::byte*>(text);
    size_t length = 0;
    size_t capacity = 0;
    std::memcpy(&length, base + kStringSize, sizeof(length));
    std::memcpy(&capacity, base + kStringCapacity, sizeof(capacity));
    if (length == 0 || length > kMaxNameLength || capacity < length) {
        return {};
    }
    const wchar_t* chars = nullptr;
    if (capacity <= kWideSso) {
        chars = reinterpret_cast<const wchar_t*>(base);
    } else {
        const wchar_t* heap = nullptr;
        std::memcpy(&heap, base, sizeof(heap));
        if (!memory::isReadable(heap, length * sizeof(wchar_t))) {
            return {};
        }
        chars = heap;
    }
    std::wstring out;
    for (size_t i = 0; i < length; ++i) {
        const wchar_t ch = chars[i];
        if (ch < 0x20 || ch > 0x7E) {
            return {};
        }
        out.push_back(ch);
    }
    return out;
}

bool looksNumeric(const std::wstring& text)
{
    if (text.empty() || text.size() > 12) {
        return false;
    }
    for (const wchar_t ch : text) {
        if ((ch < L'0' || ch > L'9') && ch != L'.' && ch != L'-') {
            return false;
        }
    }
    return true;
}

void dumpNumericStrings(std::uintptr_t base)
{
    constexpr std::ptrdiff_t kNodeSpan = 0x160;
    constexpr std::ptrdiff_t kSubSpan = 0x140;
    constexpr int kMaxTries = 40000;
    constexpr int kMaxHits = 24;
    constexpr auto kStep = static_cast<std::ptrdiff_t>(sizeof(void*));

    if (base == 0 || !memory::isReadable(reinterpret_cast<const void*>(base), kNodeSpan)) {
        return;
    }
    int tries = 0;
    int hits = 0;
    const auto check = [&](std::uintptr_t at, int depth, std::uintptr_t via1,
                           std::uintptr_t via2) {
        if (hits >= kMaxHits || ++tries > kMaxTries) {
            return;
        }
        const auto* const from = reinterpret_cast<const char*>(at);
        std::wstring text = readString(from);
        const wchar_t* how = L"8bit";
        if (!looksNumeric(text)) {
            text = readWideString(from);
            how = L"16bit";
        }
        if (!looksNumeric(text)) {
            return;
        }
        ++hits;
    };

    for (std::ptrdiff_t a = 0; a < kNodeSpan && tries < kMaxTries; a += kStep) {
        check(base + static_cast<std::uintptr_t>(a), 0, base, 0);
    }
    for (std::ptrdiff_t a = 0; a < kNodeSpan && tries < kMaxTries; a += kStep) {
        std::uintptr_t w1 = 0;
        std::memcpy(&w1, reinterpret_cast<const char*>(base) + a, sizeof(w1));
        if (w1 == 0 || !memory::isReadable(reinterpret_cast<const void*>(w1), kSubSpan)) {
            continue;
        }
        for (std::ptrdiff_t b = 0; b < kSubSpan && tries < kMaxTries; b += kStep) {
            check(w1 + static_cast<std::uintptr_t>(b), 1, w1, 0);
        }
        for (std::ptrdiff_t b = 0; b < kSubSpan && tries < kMaxTries; b += kStep) {
            std::uintptr_t w2 = 0;
            std::memcpy(&w2, reinterpret_cast<const char*>(w1) + b, sizeof(w2));
            if (w2 == 0 || !memory::isReadable(reinterpret_cast<const void*>(w2), kSubSpan)) {
                continue;
            }
            for (std::ptrdiff_t c = 0; c < kSubSpan && tries < kMaxTries; c += kStep) {
                check(w2 + static_cast<std::uintptr_t>(c), 2, w1, w2);
            }
        }
    }
}

void dumpDeepStrings(std::uintptr_t base)
{
    constexpr std::ptrdiff_t kNodeSpan = 0x160;
    constexpr std::ptrdiff_t kFollowSpan = 0x120;
    if (base == 0 || !memory::isReadable(reinterpret_cast<const void*>(base), kNodeSpan)) {
        return;
    }
    int hits = 0;
    for (std::ptrdiff_t at = 0; at < kNodeSpan; at += static_cast<std::ptrdiff_t>(sizeof(void*))) {
        std::uintptr_t word = 0;
        std::memcpy(&word, reinterpret_cast<const char*>(base) + at, sizeof(word));
        if (word == 0 || !memory::isReadable(reinterpret_cast<const void*>(word), kFollowSpan)) {
            continue;
        }
        for (std::ptrdiff_t sub = 0; sub < kFollowSpan && hits < kMaxNameHits;
             sub += static_cast<std::ptrdiff_t>(sizeof(void*))) {
            const std::wstring text =
                readString(reinterpret_cast<const char*>(word) + sub);
            if (text.empty()) {
                continue;
            }
            ++hits;
        }
    }
}

void dumpStringsAround(const wchar_t* label, std::uintptr_t base)
{
    if (base == 0) {
        return;
    }
    int hits = 0;
    for (std::ptrdiff_t at = 0; at < kNameScanSpan && hits < kMaxNameHits;
         at += static_cast<std::ptrdiff_t>(sizeof(void*))) {
        const auto* const from = reinterpret_cast<const char*>(base) + at;
        const std::wstring text = readString(from);
        if (text.empty()) {
            continue;
        }
        ++hits;
    }
}

int menuIndexFromPoint(float x, float y)
{
    if (x < 0.0f || x > kMenuMaxX) {
        return -1;
    }
    const float rel = (y - kMenuFirstY) / kMenuPitchY;
    const int idx = static_cast<int>(rel + 0.5f);
    if (idx < 0 || idx >= static_cast<int>(g_menuCount)) {
        return -1;
    }
    const float gap = rel - static_cast<float>(idx);
    if (gap > 0.5f || gap < -0.5f) {
        return -1;
    }
    return idx;
}

void onUiEvent(void* self, const void* event)
{
    if (event == nullptr || !memory::isReadable(event, 0x40)) {
        return;
    }

    constexpr size_t kMaxKinds = 24;
    static std::atomic<unsigned> g_kinds[kMaxKinds]{};
    static std::atomic<size_t> g_kindCount{0};

    unsigned kind = 0;
    std::memcpy(&kind, event, sizeof(kind));

    int seen = 0;
    bool verbose = true;
    if (kind == 1) {
        unsigned char act = 0;
        std::memcpy(&act, reinterpret_cast<const char*>(event) + 0x10, sizeof(act));
        if (act != 2) {
            return;
        }
        constexpr int kMaxInputLogs = 40;
        static std::atomic<int> g_inputSeen{0};
        const int n = g_inputSeen.fetch_add(1);
        verbose = (n < kMaxInputLogs);
        seen = n + 1;

        float x = 0.0f;
        float y = 0.0f;
        std::memcpy(&x, reinterpret_cast<const char*>(event) + 0x20, sizeof(x));
        std::memcpy(&y, reinterpret_cast<const char*>(event) + 0x24, sizeof(y));

        std::uintptr_t p18 = 0;
        std::uintptr_t p30 = 0;
        std::uintptr_t p38 = 0;
        std::memcpy(&p18, reinterpret_cast<const char*>(event) + 0x18, sizeof(p18));
        std::memcpy(&p30, reinterpret_cast<const char*>(event) + 0x30, sizeof(p30));
        std::memcpy(&p38, reinterpret_cast<const char*>(event) + 0x38, sizeof(p38));

        unsigned evType = 0;
        std::memcpy(&evType, reinterpret_cast<const char*>(event) + 0x0C, sizeof(evType));
        if (evType != kMouseEventType) {
            if (evType == kKeyEventType) {
                noteScreenClosed();
            }
            return;
        }

        constexpr unsigned long long kSameClickMs = 120;
        static std::atomic<unsigned long long> g_lastPressAt{0};
        static std::atomic<int> g_lastPressKey{-1};
        const unsigned long long now = GetTickCount64();
        const int posKey = static_cast<int>(y * 4.0f) * 4096 + static_cast<int>(x * 4.0f);
        const bool duplicate =
            (now - g_lastPressAt.load() < kSameClickMs) && (g_lastPressKey.load() == posKey);
        g_lastPressAt.store(now);
        g_lastPressKey.store(posKey);
        const std::wstring anyName =
            (p38 != 0) ? readString(reinterpret_cast<const char*>(p38) + kControlName)
                       : std::wstring();
        const wchar_t anyKind = (anyName.size() >= 2) ? anyName[anyName.size() - 2] : L'\0';
        const bool isEditRow =
            (anyKind == L'n' || anyKind == L'k') && anyName[anyName.size() - 1] >= L'0'
            && anyName[anyName.size() - 1] <= L'9'
            && menuIndexFromName(anyName.substr(0, anyName.size() - 2), L"_") >= 0;
        if (isEditRow && anyKind == L'n') {
            addEditCandidate(p38);
        }
        const bool isPageRow = (anyName.find(L"tk_pg") != std::wstring::npos)
                               || (anyName.find(L"tk_pt") != std::wstring::npos);
        if (duplicate && !isEditRow && !isPageRow) {
            return;
        }

        {
            constexpr bool kProbeAnyControl = false;
            if (kProbeAnyControl && p38 != 0) {
                g_toggleCtl.store(p38);
            }
        }

        const std::wstring hitName =
            (p38 != 0) ? readString(reinterpret_cast<const char*>(p38) + kControlName)
                       : std::wstring();
        if (verbose) {
            const std::wstring hitName18 =
                (p18 != 0) ? readString(reinterpret_cast<const char*>(p18) + kControlName)
                           : std::wstring();
        }

        {
            {
                const size_t tabAt = hitName.rfind(L"tk_pt");
                if (tabAt != std::wstring::npos && tabAt + 5 < hitName.size()
                    && hitName[tabAt + 5] >= L'0' && hitName[tabAt + 5] <= L'9') {
                    int tab = 0;
                    for (size_t d = tabAt + 5;
                         d < hitName.size() && hitName[d] >= L'0' && hitName[d] <= L'9'; ++d) {
                        tab = tab * 10 + (hitName[d] - L'0');
                    }
                    if (static_cast<size_t>(tab) < kMaxLeftRows) {
                        float box[6]{};
                        const bool inside = controlHitBy(p38, x, y, box);
                        static unsigned long long lastTabAt[kMaxLeftRows]{};
                        const unsigned long long now = GetTickCount64();
                        if (now - lastTabAt[tab] < 300) {
                            return;
                        }
                        lastTabAt[tab] = now;
                        if (inside) {
                            g_ptPressed.store(tab);
                        } else {
                            int none = -1;
                            g_ptPressed.compare_exchange_strong(none, tab);
                        }
                    }
                    return;
                }
            }
            const size_t at = hitName.rfind(L"tk_pg");
            if (at != std::wstring::npos && at + 5 < hitName.size() && hitName[at + 5] >= L'0'
                && hitName[at + 5] <= L'9') {
                int row = 0;
                for (size_t d = at + 5;
                     d < hitName.size() && hitName[d] >= L'0' && hitName[d] <= L'9'; ++d) {
                    row = row * 10 + (hitName[d] - L'0');
                }
                float box[6]{};
                const bool inside = controlHitBy(p38, x, y, box);
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 12) {
                    log().info(
                        L"UiProbe: page hit {} (row {}, inside={}, at {},{}, box {},{} {}x{})",
                        hitName, row, inside ? 1 : 0, x, y, box[0], box[1], box[2], box[3]);
                }
                if (static_cast<size_t>(row) < kMaxPageRows) {
                    g_pgCtl[row].store(p38);
                }
                const bool isEditRow =
                    (static_cast<size_t>(row) < kMaxPageRows) && g_pgIsEdit[row];
                if (!isEditRow) {
                    static unsigned long long lastAt[kMaxPageRows]{};
                    const unsigned long long now = GetTickCount64();
                    if (now - lastAt[row] < 300) {
                        return;
                    }
                    lastAt[row] = now;
                    if (inside) {
                        g_pgPressed.store(row);
                    } else {
                        int none = -1;
                        g_pgPressed.compare_exchange_strong(none, row);
                    }
                }
                return;
            }
        }

        if (kProbeControlNames && hitName.find(L"_n") != std::wstring::npos) {
            constexpr int kMaxNameProbes = 4;
            static std::atomic<int> g_nameProbes{0};
            if (g_nameProbes.fetch_add(1) < kMaxNameProbes) {
                dumpNumericStrings(p38);
            }
        }

        if (kNavPaneOwnItems) {
            const int navIdx = menuIndexFromName(hitName, L"_button_toggle");
            const int btnIdx = menuIndexFromName(hitName, L"_tgl");

            std::wstring rowName = hitName;
            bool resetHit = false;
            if (rowName.size() > 2 && rowName[rowName.size() - 2] == L'_'
                && (rowName.back() == L'b' || rowName.back() == L'r')) {
                resetHit = (rowName.back() == L'r');
                rowName.resize(rowName.size() - 2);
            }
            const wchar_t rowKind = (rowName.size() >= 2) ? rowName[rowName.size() - 2] : L'\0';
            if ((rowKind == L'n' || rowKind == L'k') && rowName[rowName.size() - 1] >= L'0'
                && rowName[rowName.size() - 1] <= L'9') {
                const int owner = menuIndexFromName(rowName.substr(0, rowName.size() - 2), L"_");
                const int slot = rowName[rowName.size() - 1] - L'0';
                const size_t slotMax =
                    (rowKind == L'k') ? kMaxKeysPerModule : kMaxNumbersPerModule;
                if (owner >= 0 && slot >= 0 && static_cast<size_t>(slot) < slotMax) {
                    float box[6]{};
                    const bool inside = controlHitBy(p38, x, y, box);
                    {
                        static std::atomic<int> hitLogs{0};
                    }
                    if (!inside) {
                        return;
                    }

                    if (resetHit && rowKind == L'k') {
                        const int want = owner * 16 + slot;
                        int none = -1;
                        g_resetRequest.compare_exchange_strong(none, want);
                        return;
                    }

                    const int req = owner * 16 + slot + ((rowKind == L'k') ? kEditKindKey : 0);
                    int expected = -1;
                    if (g_editRequest.compare_exchange_strong(expected, req)) {
                        g_editControl.store(p38);
                    }
                    return;
                }
            }

            if (navIdx >= 0) {
                g_selectedMenu.store(navIdx);
                const std::string text = g_menuTextBuf[navIdx];
            } else if (btnIdx >= 0) {
                const std::string text = g_menuTextBuf[btnIdx];

                g_pendingMenu.store(btnIdx);
                g_toggleCtl.store(p38);
            }
        } else {
            const int menuIdx = menuIndexFromPoint(x, y);
            if (menuIdx >= 0) {
                const std::string text = g_menuTextBuf[menuIdx];
                g_pendingMenu.store(menuIdx);
            }
        }
    } else {
        const size_t have = g_kindCount.load();
        for (size_t i = 0; i < have && i < kMaxKinds; ++i) {
            if (g_kinds[i].load() == kind) {
                return;
            }
        }
        const size_t slot = g_kindCount.fetch_add(1);
        if (slot >= kMaxKinds) {
            return;
        }
        g_kinds[slot].store(kind);
        seen = static_cast<int>(slot) + 1;
    }

    if (!verbose) {
        return;
    }

    std::uintptr_t first = 0;
    std::uintptr_t last = 0;
    const auto at = reinterpret_cast<const char*>(self);
    if (self != nullptr && memory::isReadable(at + 0x9a8, sizeof(first) * 2)) {
        std::memcpy(&first, at + 0x9a8, sizeof(first));
        std::memcpy(&last, at + 0x9b0, sizeof(last));
    }
    const std::uintptr_t count = (last > first) ? (last - first) / 0x10 : 0;

    for (int i = 0; i < 4; ++i) {
        const auto from = reinterpret_cast<const char*>(event) + i * 0x20;
        if (!memory::isReadable(from, 0x20)) {
            break;
        }
    }

    for (std::uintptr_t i = 0; i < count && i < 4; ++i) {
        const auto slotAt = first + i * 0x10;
        std::uintptr_t handler = 0;
        if (!memory::isReadable(reinterpret_cast<const void*>(slotAt), sizeof(handler))) {
            break;
        }
        std::memcpy(&handler, reinterpret_cast<const void*>(slotAt), sizeof(handler));
        std::uintptr_t vft = 0;
        if (handler != 0 && memory::isReadable(reinterpret_cast<const void*>(handler), sizeof(vft))) {
            std::memcpy(&vft, reinterpret_cast<const void*>(handler), sizeof(vft));
        }
    }
}

void pumpNumberEntry()
{
    static bool down[256]{};
    const auto pressedOnce = [](int vk) {
        const SHORT state = GetAsyncKeyState(vk);
        const bool now = (state & 0x8000) != 0;
        const bool sincePrev = (state & 0x0001) != 0;
        const bool hit = (now && !down[vk]) || sincePrev;
        down[vk] = now;
        return hit;
    };

    if (const int reset = g_resetRequest.exchange(-1); reset >= 0) {
        const size_t i = static_cast<size_t>(reset) / 16;
        const size_t k = static_cast<size_t>(reset) % 16;
        if (i < g_menuCount && k < g_keybindCount[i] && g_keybinds[i][k].set) {
            if (g_editIsKey && g_editTarget == reset) {
                g_editTarget = -1;
                g_editIsKey = false;
            }
            g_keybinds[i][k].set(g_keybinds[i][k].defaults);
            const std::wstring shown = gameComboName(g_keybinds[i][k].defaults);
            size_t s = 0;
            for (; s < shown.size() && s + 1 < kNumTextBytes; ++s) {
                g_keybinds[i][k].shown[s] = (shown[s] < 0x80) ? static_cast<char>(shown[s]) : '?';
            }
            g_keybinds[i][k].shown[s] = '\0';

            const std::string label = g_keybinds[i][k].label;
            const std::string owner = g_menuTextBuf[i];
        }
    }

    const int req = g_editRequest.exchange(-1);
    if (req >= 0) {
        g_editIsKey = (req & kEditKindKey) != 0;
        g_editTarget = req & 0xFFFF;
        const size_t i = static_cast<size_t>(g_editTarget) / 16;
        const size_t k = static_cast<size_t>(g_editTarget) % 16;
        if (g_editIsKey) {
            const std::string label = g_keybinds[i][k].label;
            g_keyWaitFrom = GetTickCount64();
            g_keyWaitTicks = 0;
            g_keyWaitLogs = 0;

            int held = 0;
            std::wstring heldNames;
            for (int vk = 0; vk < 256; ++vk) {
                const bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
                g_keyWaitHeld[vk] = isDown;
                down[vk] = isDown;
                if (isDown) {
                    ++held;
                    if (held <= 6) {
                        if (!heldNames.empty()) {
                            heldNames += L" ";
                        }
                        heldNames += keys::name(vk);
                    }
                }
            }
        } else {
            const std::string label = g_numbers[i][k].label;
        }
        g_editStartCaptured = false;
    }
    if (g_editTarget < 0) {
        pressedOnce(VK_RETURN);
        pressedOnce(VK_ESCAPE);
        return;
    }

    if (g_editIsKey) {
        ++g_keyWaitTicks;
        if (pressedOnce(VK_ESCAPE)) {
            g_editTarget = -1;
            g_editIsKey = false;
            return;
        }

        std::vector<int> combo;
        bool hasReal = false;
        int stillHeld = 0;
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
                continue;
            }
            const SHORT state = GetAsyncKeyState(vk);
            const bool nowDown = (state & 0x8000) != 0;
            const bool sincePrev = (state & 0x0001) != 0;
            if (!nowDown && !sincePrev) {
                g_keyWaitHeld[vk] = false;
                continue;
            }
            if (g_keyWaitHeld[vk] && !sincePrev) {
                ++stillHeld;
                continue;
            }
            g_keyWaitHeld[vk] = false;
            const int norm = keys::normalize(vk);
            if (std::find(combo.begin(), combo.end(), norm) == combo.end()) {
                combo.push_back(norm);
            }
            if (!keys::isModifier(norm)) {
                hasReal = true;
            }
        }

        {
            const bool shout = (g_keyWaitTicks <= 3) || (g_keyWaitTicks % 60 == 0)
                               || !combo.empty();
            if (shout && g_keyWaitLogs < 12) {
                ++g_keyWaitLogs;
            }
        }

        if (!hasReal) {
            return;
        }

        const size_t i = static_cast<size_t>(g_editTarget) / 16;
        const size_t k = static_cast<size_t>(g_editTarget) % 16;
        g_editTarget = -1;
        g_editIsKey = false;
        if (i >= g_menuCount || k >= g_keybindCount[i] || !g_keybinds[i][k].set) {
            return;
        }
        g_keybinds[i][k].set(combo);

        const std::string label = g_keybinds[i][k].label;
        const std::string owner = g_menuTextBuf[i];
        const std::wstring shown = gameComboName(combo);
        return;
    }

    if (!g_editStartCaptured) {
        const size_t cands = g_editCandCount.load();
        for (size_t c = 0; c < cands && c < kMaxEditCands; ++c) {
            g_editCandStart[c].clear();
            readEditBoxText(g_editCands[c], g_editCandStart[c]);
        }
        g_editStartCaptured = true;
    }

    if (pressedOnce(VK_ESCAPE)) {
        g_editTarget = -1;
        return;
    }
    if (!pressedOnce(VK_RETURN)) {
        return;
    }
    g_editTarget = -1;

    std::uintptr_t ctl = 0;
    std::string typed;
    {
        const size_t cands = g_editCandCount.load();
        for (size_t c = 0; c < cands && c < kMaxEditCands; ++c) {
            std::string now;
            if (!readEditBoxText(g_editCands[c], now)) {
                continue;
            }
            if (now != g_editCandStart[c]) {
                ctl = g_editCands[c];
                typed = now;
                break;
            }
        }
        g_editCandCount.store(0);
        g_editStartCaptured = false;
    }
    if (ctl == 0) {
        return;
    }

    const auto* ctlNameAt = reinterpret_cast<const char*>(ctl) + kControlName;
    if (!memory::isReadable(ctlNameAt, 0x20)) {
        return;
    }
    const std::wstring ctlName = readString(ctlNameAt);
    if (ctlName.size() < 2 || ctlName[ctlName.size() - 2] != L'n'
        || ctlName[ctlName.size() - 1] < L'0' || ctlName[ctlName.size() - 1] > L'9') {
        return;
    }
    const int ownerIdx = menuIndexFromName(ctlName.substr(0, ctlName.size() - 2), L"_");
    const int slot = ctlName[ctlName.size() - 1] - L'0';
    if (ownerIdx < 0 || slot < 0 || static_cast<size_t>(slot) >= kMaxNumbersPerModule) {
        return;
    }
    const size_t i = static_cast<size_t>(ownerIdx);
    const size_t k = static_cast<size_t>(slot);

    if (typed.empty() || i >= g_menuCount || k >= g_numberCount[i] || !g_numbers[i][k].set) {
        return;
    }
    const float value = std::strtof(typed.c_str(), nullptr);
    g_numbers[i][k].set(value);

    const std::string label = g_numbers[i][k].label;
    const std::string owner = g_menuTextBuf[i];
}

void dumpBindingNames(std::uintptr_t obj, const wchar_t* label, int n)
{
    constexpr std::ptrdiff_t kBindSpan = 0x400;
    int total = 0;
    for (const std::ptrdiff_t voff : {std::ptrdiff_t{0x0b8}, std::ptrdiff_t{0x108},
                                      std::ptrdiff_t{0x118}}) {
        const auto* const head = reinterpret_cast<const char*>(obj) + voff;
        if (!memory::isReadable(head, 24)) {
            continue;
        }
        std::uintptr_t v[3]{};
        std::memcpy(v, head, sizeof(v));
        if (v[0] == 0 || v[1] <= v[0]) {
            continue;
        }
        const std::uintptr_t cnt = (v[1] - v[0]) / sizeof(void*);
        if (cnt == 0 || cnt > 32
            || !memory::isReadable(reinterpret_cast<const void*>(v[0]),
                                   static_cast<size_t>(v[1] - v[0]))) {
            continue;
        }
        for (std::uintptr_t i = 0; i < cnt; ++i) {
            std::uintptr_t elem = 0;
            std::memcpy(&elem, reinterpret_cast<const char*>(v[0]) + i * sizeof(void*),
                        sizeof(elem));
            if (elem == 0 || !memory::isReadable(reinterpret_cast<const void*>(elem), 0x40)) {
                continue;
            }
            std::wstring names;
            int shown = 0;
            for (std::ptrdiff_t s = 0; s + 0x20 <= kBindSpan && shown < 8; s += sizeof(void*)) {
                std::string t;
                if (!readStdString(reinterpret_cast<const char*>(elem + s), t)) {
                    continue;
                }
                if (t.empty() || t.size() > 40) {
                    continue;
                }
                if (!names.empty()) {
                    names += L" | ";
                }
                names += std::wstring(t.begin(), t.end());
                ++shown;
                ++total;
            }
        }
    }
}

void dumpToggleControlPeriodically()
{
    constexpr bool kDumpToggleControl = false;
    if (!kDumpToggleControl) {
        return;
    }
    const std::uintptr_t ctl = g_toggleCtl.load();
    if (ctl == 0) {
        return;
    }
    constexpr unsigned long long kEveryMs = 4000;
    constexpr int kMaxDumps = 4;
    static std::atomic<unsigned long long> lastAt{0};
    static std::atomic<int> dumps{0};
    static std::atomic<std::uintptr_t> lastCtl{0};
    if (lastCtl.exchange(ctl) != ctl) {
        dumps.store(0);
    }
    const unsigned long long now = GetTickCount64();
    if (now - lastAt.load() < kEveryMs || dumps.load() >= kMaxDumps) {
        return;
    }
    lastAt.store(now);
    const int n = dumps.fetch_add(1) + 1;

    const auto at = reinterpret_cast<const char*>(ctl) + kControlChildren;
    if (!memory::isReadable(at, 24)) {
        return;
    }
    std::uintptr_t vec[3]{};
    std::memcpy(vec, at, sizeof(vec));
    if (vec[0] == 0 || vec[1] <= vec[0]) {
        return;
    }
    (void)vec;

    dumpBindingNames(ctl, L"toggle", n);
    for (const wchar_t* which : {L"checked", L"unchecked"}) {
        const std::uintptr_t child = childByName(ctl, which);
        if (child == 0) {
            continue;
        }
        dumpBindingNames(child, which, n);
    }

    constexpr std::ptrdiff_t kBindSpan = 0x400;
    const auto* const head = reinterpret_cast<const char*>(ctl) + 0x0b8;
    if (!memory::isReadable(head, 24)) {
        return;
    }
    std::uintptr_t bvec[3]{};
    std::memcpy(bvec, head, sizeof(bvec));
    if (bvec[0] == 0 || bvec[1] <= bvec[0]) {
        return;
    }
    const std::uintptr_t bcount = (bvec[1] - bvec[0]) / sizeof(void*);
    int found = 0;
    for (std::uintptr_t i = 0; i < bcount && i < 16; ++i) {
        std::uintptr_t elem = 0;
        std::memcpy(&elem, reinterpret_cast<const char*>(bvec[0]) + i * sizeof(void*),
                    sizeof(elem));
        if (elem == 0 || !memory::isReadable(reinterpret_cast<const void*>(elem), 0x40)) {
            continue;
        }
        for (std::ptrdiff_t s = 0; s + 0x20 <= kBindSpan; s += sizeof(void*)) {
            std::string name;
            if (!readStdString(reinterpret_cast<const char*>(elem + s), name)) {
                continue;
            }
            if (name.rfind("#tk_tgl_", 0) != 0) {
                continue;
            }
            ++found;
            for (int c = -2; c < 4; ++c) {
                const std::ptrdiff_t at = s + c * 0x20;
                if (at < 0) {
                    continue;
                }
            }
            break;
        }
    }
}

std::atomic<void*> g_settingsRegistry{nullptr};
std::atomic<void*> g_tabsGroupProvider{nullptr};
std::atomic<int> g_settingsGroupCount{0};

std::string readStringView(const void* view)
{
    if (view == nullptr || !memory::isReadable(view, sizeof(void*) * 2)) {
        return {};
    }
    const char* text = nullptr;
    size_t length = 0;
    std::memcpy(&text, view, sizeof(text));
    std::memcpy(&length, static_cast<const char*>(view) + sizeof(void*), sizeof(length));
    if (text == nullptr || length == 0 || length > 200 || !memory::isReadable(text, length)) {
        return {};
    }
    std::string out(text, length);
    for (const char ch : out) {
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E) {
            return {};
        }
    }
    return out;
}

constexpr char kTabsGroupId[] = "settings-tabs-groups";

constexpr char kOwnGroupIdBytes[] = "tsukuyomi";
constexpr const char* kOwnGroupId = kOwnGroupIdBytes;
constexpr char kOwnTabIdBytes[] = "tsukuyomi-menu";

constexpr bool kAddOwnSettingsTab = true;

std::atomic<std::uintptr_t> g_tabsCapture0{0};
std::atomic<std::uintptr_t> g_tabsCapture1{0};

struct GroupCapture {
    std::uintptr_t cap0;
    std::uintptr_t cap1;
    char id[64];
};
constexpr size_t kMaxGroupCaptures = 192;
GroupCapture g_groupCaptures[kMaxGroupCaptures]{};
std::atomic<size_t> g_groupCaptureCount{0};

const char* findGroupIdByCapture(std::uintptr_t cap0, std::uintptr_t cap1)
{
    const size_t count = g_groupCaptureCount.load();
    for (size_t i = 0; i < count && i < kMaxGroupCaptures; ++i) {
        if (g_groupCaptures[i].cap0 == cap0 && g_groupCaptures[i].cap1 == cap1) {
            return g_groupCaptures[i].id;
        }
    }
    return nullptr;
}

constexpr size_t kMaxOwnItems = 256;
constexpr size_t kOwnItemIdMax = 48;
constexpr size_t kOwnItemTextMax = 48;
constexpr char kKeyValueSuffix = 'v';
constexpr bool kVanillaKeyRows = true;
constexpr char kBorrowedKeyGroupId[] = "keyboardAndMouse.inputGroup.full.chord";

bool useOwnKeyGroups();
int ownKeyGroupIndex(const char* id);
constexpr char kKeyRowIdFormat[] = "tk.g%d";
constexpr char kChoiceSuffix = 'o';
extern void* g_ownActionVtable[6];
bool prepareOwnActionVtable(const void* donorCallable);
bool prepareOwnEnabledVtable(const void* donorCallable);
bool prepareOwnVisibleVtable(const void* donorCallable, bool force = false);
int findOwnItemByComp(const void* comp);
bool ownKeyRowIsDefault(int index);
extern void* g_ownVisibleVtable[6];
extern void* g_ownEnabledVtable[6];

constexpr std::ptrdiff_t kCompId = 0x08;
constexpr std::ptrdiff_t kCompNameKey = 0x28;
constexpr std::ptrdiff_t kCompDescKey = 0x50;
constexpr std::ptrdiff_t kCompHasDesc = 0x70;
constexpr std::ptrdiff_t kCompNameFn = 0x78;
constexpr std::ptrdiff_t kCompNameFnPtr = 0xB0;
constexpr std::ptrdiff_t kCompPubList = 0x148;
constexpr std::ptrdiff_t kCompPubCount = 0x158;
constexpr std::ptrdiff_t kCompPubPending = 0x1B0;
constexpr std::ptrdiff_t kCompPublisher = 0x1B8;
constexpr std::ptrdiff_t kCompNameMode = 0x1D9;
constexpr std::ptrdiff_t kCompBoolProvider = 0x1E0;
constexpr std::ptrdiff_t kCompActionLabel = 0x1E0;
constexpr std::ptrdiff_t kCompActionConfirm = 0x288;
constexpr std::ptrdiff_t kCompActionEnabledFn = 0x348;
constexpr std::ptrdiff_t kCompVisibleFn = 0x0F8;
constexpr std::ptrdiff_t kCompVisibleFnPtr = 0x130;
constexpr std::ptrdiff_t kCompActionEnabledFnPtr = 0x380;
constexpr std::ptrdiff_t kCompActionFn = 0x388;
constexpr std::ptrdiff_t kCompActionFnPtr = 0x3C0;
constexpr std::ptrdiff_t kTextProviderNotify = 0x40;
constexpr std::ptrdiff_t kTextProviderSource = 0x58;
constexpr std::ptrdiff_t kTextProviderDraft = 0x78;
constexpr std::ptrdiff_t kTextProviderHasDraft = 0x98;
constexpr std::ptrdiff_t kCompType = 0x3C8;
constexpr size_t kCompSize = 0x3E0;
constexpr unsigned char kCompTypeGroupInfo = 7;

std::atomic<void*> g_ownSectionComp{nullptr};
std::atomic<void*> g_ownTabComp{nullptr};
std::atomic<bool> g_ownCompsReady{false};

void* __fastcall ownPublisherSubscribe(void* self, void* out, void* fn, unsigned flags, void* a,
                                       void* b)
{
    (void)self;
    (void)fn;
    (void)flags;
    (void)a;
    (void)b;
    if (out != nullptr) {
        std::memset(out, 0, 16);
    }
    return out;
}

void* __fastcall ownPublisherNoop(void*, void*, void*, void*) { return nullptr; }

void* g_ownPublisherVtable[8] = {
    reinterpret_cast<void*>(&ownPublisherNoop), reinterpret_cast<void*>(&ownPublisherSubscribe),
    reinterpret_cast<void*>(&ownPublisherNoop), reinterpret_cast<void*>(&ownPublisherNoop),
    reinterpret_cast<void*>(&ownPublisherNoop), reinterpret_cast<void*>(&ownPublisherNoop),
    reinterpret_cast<void*>(&ownPublisherNoop), reinterpret_cast<void*>(&ownPublisherNoop),
};

constexpr bool kOwnPublisherReal = true;

constexpr std::ptrdiff_t kSubCallback = 0x50;
constexpr std::ptrdiff_t kSubCallbackPtr = 0x88;

std::atomic<bool> g_inPublish{false};

void publishOwnComponent(const unsigned char* comp)
{
    if (comp == nullptr || !memory::isReadable(comp, kCompSize)) {
        return;
    }
    const auto head = reinterpret_cast<std::uintptr_t>(comp) + kCompPubList;
    std::uintptr_t node = 0;
    std::memcpy(&node, comp + kCompPubList, sizeof(node));

    constexpr int kMaxSubscribers = 8;
    std::uintptr_t subs[kMaxSubscribers]{};
    int count = 0;
    for (; count < kMaxSubscribers && node != 0 && node != head; ++count) {
        if (!memory::isReadable(reinterpret_cast<const void*>(node), 0x20)) {
            break;
        }
        std::uintptr_t sub = 0;
        std::memcpy(&sub, reinterpret_cast<const char*>(node) + 0x18, sizeof(sub));
        subs[count] = sub;
        std::memcpy(&node, reinterpret_cast<const void*>(node), sizeof(node));
    }

    const bool wasPublishing = g_inPublish.exchange(true);
    for (int i = 0; i < count; ++i) {
        const std::uintptr_t sub = subs[i];
        if (sub == 0 || !memory::isReadable(reinterpret_cast<const void*>(sub), 0x90)) {
            continue;
        }
        const auto callable = sub + static_cast<std::uintptr_t>(kSubCallback);
        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<const void*>(callable), sizeof(vtable));
        if (vtable == 0 || !memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 3)) {
            continue;
        }
        std::uintptr_t doCall = 0;
        std::memcpy(&doCall, reinterpret_cast<const char*>(vtable) + 0x10, sizeof(doCall));
        if (doCall == 0) {
            continue;
        }
        using DoCall = void(__fastcall*)(void*);
        reinterpret_cast<DoCall>(doCall)(reinterpret_cast<void*>(callable));
    }
    g_inPublish.store(wasPublishing);
    static std::atomic<int> said{0};
}

constexpr size_t kPublishWords = (kMaxOwnItems + 63) / 64;
std::atomic<unsigned long long> g_publishMask[kPublishWords]{};

bool anyPublishPending()
{
    for (size_t w = 0; w < kPublishWords; ++w) {
        if (g_publishMask[w].load(std::memory_order_relaxed) != 0) {
            return true;
        }
    }
    return false;
}
std::atomic<bool> g_publishing{false};
const unsigned char* ownItemComponent(int index);

void requestOwnPublish(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < kMaxOwnItems) {
        g_publishMask[static_cast<size_t>(index) / 64].fetch_or(
            1ull << (static_cast<size_t>(index) % 64));
    }
}

int ownTabOf(int index);

int ownKeysTabIndex();
int ownHolderIndex();
bool ownItemIsKeyRow(int index);

void requestOwnPublishSiblings(int index);
void requestOwnPublishEverything();

void requestOwnPublishWithTab(int index)
{
    requestOwnPublish(index);
    requestOwnPublish(ownTabOf(index));
    requestOwnPublishSiblings(index);
    if (ownItemIsKeyRow(index)) {
        requestOwnPublish(ownHolderIndex());
        requestOwnPublish(ownKeysTabIndex());
    }
}

std::atomic<unsigned long> g_gameThreadId{0};

void markGameThread()
{
}

std::atomic<void*> g_keyResetDonor{nullptr};

void flushOwnPublishes()
{
    if (!anyPublishPending()) {
        return;
    }
    bool expected = false;
    if (!g_publishing.compare_exchange_strong(expected, true)) {
        return;
    }
    for (size_t w = 0; w < kPublishWords; ++w) {
        unsigned long long mask = g_publishMask[w].exchange(0);
        while (mask != 0) {
            unsigned long bit = 0;
            _BitScanForward64(&bit, mask);
            mask &= mask - 1;
            publishOwnComponent(ownItemComponent(static_cast<int>(w * 64 + bit)));
        }
    }
    g_publishing.store(false);
}

using PeekMessageFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageFn g_peekMessageW = nullptr;

BOOL WINAPI detourPeekMessageW(LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax, UINT remove)
{
    const BOOL result = g_peekMessageW(msg, hwnd, filterMin, filterMax, remove);
    if (!anyPublishPending()) {
        return result;
    }
    const unsigned long here = GetCurrentThreadId();
    const unsigned long want = g_gameThreadId.load(std::memory_order_relaxed);
    if (here == want) {
        flushOwnPublishes();
        return result;
    }
    static std::atomic<int> said{0};
    return result;
}

void logOwnSubscribers(const unsigned char* comp, int index)
{
    const auto head = reinterpret_cast<std::uintptr_t>(comp) + kCompPubList;
    std::uintptr_t node = 0;
    std::memcpy(&node, comp + kCompPubList, sizeof(node));
    std::uintptr_t count = 0;
    std::memcpy(&count, comp + kCompPubCount, sizeof(count));
    for (int n = 0; n < 4 && node != 0 && node != head; ++n) {
        if (!memory::isReadable(reinterpret_cast<const void*>(node), 0x20)) {
            break;
        }
        std::uintptr_t sub = 0;
        std::memcpy(&sub, reinterpret_cast<const char*>(node) + 0x18, sizeof(sub));
        std::uintptr_t call = 0;
        std::uintptr_t vtable = 0;
        std::uintptr_t doCall = 0;
        if (sub != 0 && memory::isReadable(reinterpret_cast<const void*>(sub), 0x90)) {
            std::memcpy(&call, reinterpret_cast<const char*>(sub) + kSubCallbackPtr, sizeof(call));
            if (call != 0 && memory::isReadable(reinterpret_cast<const void*>(call), 8)) {
                std::memcpy(&vtable, reinterpret_cast<const void*>(call), sizeof(vtable));
                if (vtable != 0
                    && memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 4)) {
                    std::memcpy(&doCall, reinterpret_cast<const char*>(vtable) + 0x10,
                                sizeof(doCall));
                }
            }
        }
        std::memcpy(&node, reinterpret_cast<const void*>(node), sizeof(node));
    }
}

void writeSsoString(void* slot, const char* text, size_t length);

void writeOwnString(void* slot, const char* text, size_t length)
{
    if (length <= kSsoCapacity) {
        writeSsoString(slot, text, length);
        return;
    }
    auto* const p = static_cast<char*>(slot);
    void* const buf = hooks::callGameAllocate(length + 1);
    if (buf == nullptr) {
        writeSsoString(slot, text, kSsoCapacity);
        return;
    }
    std::memcpy(buf, text, length);
    static_cast<char*>(buf)[length] = '\0';
    std::memset(p, 0, 0x20);
    const auto ptr = reinterpret_cast<std::uintptr_t>(buf);
    std::memcpy(p, &ptr, sizeof(ptr));
    const std::uintptr_t size = length;
    const std::uintptr_t room = length;
    std::memcpy(p + kStringSize, &size, sizeof(size));
    std::memcpy(p + kStringCapacity, &room, sizeof(room));
}

void writeSsoString(void* slot, const char* text, size_t length)
{
    auto* const p = static_cast<char*>(slot);
    std::memset(p, 0, 0x20);
    if (length > kSsoCapacity) {
        length = kSsoCapacity;
    }
    std::memcpy(p, text, length);
    const std::uintptr_t size = length;
    const std::uintptr_t room = kSsoCapacity;
    std::memcpy(p + kStringSize, &size, sizeof(size));
    std::memcpy(p + kStringCapacity, &room, sizeof(room));
}

size_t neutralizeHeapStrings(unsigned char* dest, std::ptrdiff_t from, std::ptrdiff_t to,
                             std::ptrdiff_t skip)
{
    size_t hit = 0;
    for (std::ptrdiff_t off = from; off + 0x20 <= to; off += 8) {
        if (off == skip) {
            continue;
        }
        std::uintptr_t ptr = 0;
        std::uintptr_t size = 0;
        std::uintptr_t room = 0;
        std::memcpy(&ptr, dest + off, sizeof(ptr));
        std::memcpy(&size, dest + off + kStringSize, sizeof(size));
        std::memcpy(&room, dest + off + kStringCapacity, sizeof(room));
        if (room <= kSsoCapacity || room > 0x4000 || size > room || size == 0) {
            continue;
        }
        if (ptr == 0 || !memory::isReadable(reinterpret_cast<const void*>(ptr), size + 1)) {
            continue;
        }
        const auto* const text = reinterpret_cast<const char*>(ptr);
        bool printable = true;
        for (size_t i = 0; i < size && printable; ++i) {
            const auto ch = static_cast<unsigned char>(text[i]);
            printable = (ch >= 0x20 && ch <= 0x7E);
        }
        if (!printable) {
            continue;
        }
        writeSsoString(dest + off, "", 0);
        ++hit;
    }
    return hit;
}

bool buildOwnComponent(unsigned char* dest, const void* donor, const char* id, size_t idLength,
                       const char* nameKey, size_t nameKeyLength,
                       unsigned char type = kCompTypeGroupInfo, void* boolProvider = nullptr,
                       std::uintptr_t originalBase = 0)
{
    if (donor == nullptr || !memory::isReadable(donor, kCompSize)) {
        return false;
    }
    std::memcpy(dest, donor, kCompSize);

    const auto donorBase =
        (originalBase != 0) ? originalBase : reinterpret_cast<std::uintptr_t>(donor);
    const auto ownBase = reinterpret_cast<std::uintptr_t>(dest);
    for (size_t off = 0; off + sizeof(std::uintptr_t) <= kCompSize; off += sizeof(std::uintptr_t)) {
        std::uintptr_t word = 0;
        std::memcpy(&word, dest + off, sizeof(word));
        if (word >= donorBase && word < donorBase + kCompSize) {
            const std::uintptr_t rebased = ownBase + (word - donorBase);
            std::memcpy(dest + off, &rebased, sizeof(rebased));
        }
    }

    if (type == 5) {
        std::memset(dest + 0x1D0, 0, 0x388 - 0x1D0);
    } else if (type == 2) {
        std::memset(dest + 0x1D0, 0, kCompType - 0x1D0);
        const float scale = 1.0f;
        std::memcpy(dest + 0x1E8, &scale, sizeof(scale));
    } else if (type == 4) {
        const size_t dropped =
            neutralizeHeapStrings(dest, 0x1D0, kCompType, kCompBoolProvider);
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            log().info(L"UiProbe: text component - emptied {} heap string(s) in the copy",
                       dropped);
        }
        {
            const auto* const donorVisible = static_cast<const char*>(donor) + kCompVisibleFn;
            if (prepareOwnVisibleVtable(donorVisible)) {
                std::memset(dest + kCompVisibleFn, 0, 0x40);
                const auto vtable = reinterpret_cast<std::uintptr_t>(&g_ownVisibleVtable[0]);
                std::memcpy(dest + kCompVisibleFn, &vtable, sizeof(vtable));
                const auto self = reinterpret_cast<std::uintptr_t>(dest) + kCompVisibleFn;
                std::memcpy(dest + kCompVisibleFnPtr, &self, sizeof(self));
            }
        }
    }

    const std::uintptr_t head = ownBase + kCompPubList;
    std::memcpy(dest + kCompPubList, &head, sizeof(head));
    std::memcpy(dest + kCompPubList + 8, &head, sizeof(head));
    const std::uintptr_t zero = 0;
    std::memcpy(dest + kCompPubCount, &zero, sizeof(zero));
    std::memcpy(dest + kCompPubPending, &zero, sizeof(zero));

    writeOwnString(dest + kCompId, id, idLength);
    writeOwnString(dest + kCompNameKey, nameKey, nameKeyLength);
    writeSsoString(dest + kCompDescKey, "", 0);
    dest[kCompHasDesc] = 0;
    dest[kCompType] = type;
    dest[kCompNameMode] = 0;

    if (boolProvider != nullptr) {
        const auto value = reinterpret_cast<std::uintptr_t>(boolProvider);
        std::memcpy(dest + kCompBoolProvider, &value, sizeof(value));
    }

    if (type == 5) {
        if (!prepareOwnActionVtable(dest + kCompActionFn)) {
            return false;
        }
        std::memset(dest + kCompActionFn, 0, 0x40);
        const auto vtable = reinterpret_cast<std::uintptr_t>(&g_ownActionVtable[0]);
        std::memcpy(dest + kCompActionFn, &vtable, sizeof(vtable));
        const auto self = reinterpret_cast<std::uintptr_t>(dest) + kCompActionFn;
        std::memcpy(dest + kCompActionFnPtr, &self, sizeof(self));
        char valueKey[kOwnItemIdMax]{};
        const size_t n = (std::min)(nameKeyLength, sizeof(valueKey) - 2);
        std::memcpy(valueKey, nameKey, n);
        valueKey[n] = kKeyValueSuffix;
        valueKey[n + 1] = '\0';
        writeSsoString(dest + kCompActionLabel, valueKey, n + 1);
        {
            const auto* const donorEnabled = static_cast<const char*>(donor) + kCompActionEnabledFn;
            std::uintptr_t donorPtr = 0;
            std::memcpy(&donorPtr, donorEnabled + 0x38, sizeof(donorPtr));
            const void* const donorCallable =
                (donorPtr != 0) ? reinterpret_cast<const void*>(donorPtr) : donorEnabled;
            if (prepareOwnEnabledVtable(donorCallable)) {
                std::memset(dest + kCompActionEnabledFn, 0, 0x40);
                const auto vtable = reinterpret_cast<std::uintptr_t>(&g_ownEnabledVtable[0]);
                std::memcpy(dest + kCompActionEnabledFn, &vtable, sizeof(vtable));
                const auto self =
                    reinterpret_cast<std::uintptr_t>(dest) + kCompActionEnabledFn;
                std::memcpy(dest + kCompActionEnabledFnPtr, &self, sizeof(self));
            }
        }

        {
            const auto* const donorVisible = static_cast<const char*>(donor) + kCompVisibleFn;
            const bool ready = prepareOwnVisibleVtable(donorVisible);
            if (ready) {
                std::memset(dest + kCompVisibleFn, 0, 0x40);
                const auto vtable = reinterpret_cast<std::uintptr_t>(&g_ownVisibleVtable[0]);
                std::memcpy(dest + kCompVisibleFn, &vtable, sizeof(vtable));
                const auto self = reinterpret_cast<std::uintptr_t>(dest) + kCompVisibleFn;
                std::memcpy(dest + kCompVisibleFnPtr, &self, sizeof(self));
            }
            constexpr size_t kTail = sizeof(".reset") - 1;
            if (idLength > kTail && std::strcmp(id + idLength - kTail, ".reset") == 0) {
                const std::string_view text(id, idLength);
            }
        }
        dest[kCompActionConfirm] = 0;
        static std::atomic<int> said{0};
    }

    if (!kOwnPublisherReal) {
        const auto vtable = reinterpret_cast<std::uintptr_t>(&g_ownPublisherVtable[0]);
        std::memcpy(dest + kCompPublisher, &vtable, sizeof(vtable));
    }
    return true;
}

bool copyStdFunction(const void* source, void* dest)
{
    if (source == nullptr || dest == nullptr || !memory::isReadable(source, 0x40)) {
        return false;
    }
    std::memset(dest, 0, 0x40);
    std::uintptr_t impl = 0;
    std::memcpy(&impl, static_cast<const char*>(source) + 0x38, sizeof(impl));
    if (impl == 0 || !memory::isReadable(reinterpret_cast<const void*>(impl), sizeof(void*))) {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<const void*>(impl), sizeof(vtable));
    if (vtable == 0 || !memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 5)) {
        return false;
    }
    std::uintptr_t copyFn = 0;
    std::memcpy(&copyFn, reinterpret_cast<const void*>(vtable), sizeof(copyFn));
    if (copyFn == 0) {
        return false;
    }
    using CopyFn = void*(__fastcall*)(void*, void*);
    void* made = nullptr;
    __try {
        made = reinterpret_cast<CopyFn>(copyFn)(reinterpret_cast<void*>(impl), dest);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (made == nullptr) {
        return false;
    }
    std::memcpy(static_cast<char*>(dest) + 0x38, &made, sizeof(made));
    return true;
}

constexpr size_t kBoolProviderSize = 0x100;
constexpr size_t kBoolVtableSlots = 16;
void* g_ownBoolVtable[kBoolVtableSlots]{};
std::atomic<bool> g_ownBoolVtableReady{false};

struct ProviderLink {
    void* provider;
    int index;
};
ProviderLink g_providerLinks[kMaxOwnItems]{};
std::atomic<size_t> g_providerLinkCount{0};

int indexOfProvider(const void* self)
{
    const size_t count = g_providerLinkCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_providerLinks[i].provider == self) {
            return g_providerLinks[i].index;
        }
    }
    return -1;
}

std::atomic<int> g_ownToggleRequest{-1};

struct OwnPending {
    std::atomic<bool> has{false};
    std::atomic<float> value{0.0f};
};
OwnPending g_ownPending[kMaxOwnItems];

void setOwnPending(int index, float value)
{
    if (index < 0 || index >= static_cast<int>(kMaxOwnItems)) {
        return;
    }
    g_ownPending[index].value.store(value);
    g_ownPending[index].has.store(true);
}

void clearOwnPending(int index)
{
    if (index >= 0 && index < static_cast<int>(kMaxOwnItems)) {
        g_ownPending[index].has.store(false);
    }
}

bool ownPending(int index, float& out)
{
    if (index < 0 || index >= static_cast<int>(kMaxOwnItems) || !g_ownPending[index].has.load()) {
        return false;
    }
    out = g_ownPending[index].value.load();
    return true;
}

void flushOwnPublishes();
void requestOwnPublish(int index);

bool ownItemValue(int index);
void ownItemRequestToggle(int index);
void buildOwnItems();

unsigned char __fastcall ownBoolGet(void* self)
{
    flushOwnPublishes();
    const int index = indexOfProvider(self);
    float pending = 0.0f;
    const bool value = ownPending(index, pending) ? (pending != 0.0f) : ownItemValue(index);
    static std::atomic<int> said{0};
    return value ? 1u : 0u;
}

void __fastcall ownBoolSet(void* self, unsigned value)
{
    const int index = indexOfProvider(self);
    if (index < 0) {
        return;
    }
    const bool want = (value & 1u) != 0;
    static std::atomic<int> said{0};
    if (want != ownItemValue(index)) {
        setOwnPending(index, want ? 1.0f : 0.0f);
        ownItemRequestToggle(index);
        publishOwnComponent(ownItemComponent(index));
    }
}

void* __fastcall ownProviderNoop(void*, void*, void*, void*) { return nullptr; }

bool ownItemAvailable(int index);
int indexOfProvider(const void* self);

void logOwnSubscribers(const unsigned char* comp, int index);
const unsigned char* ownItemComponent(int index);

unsigned char __fastcall ownProviderAvailable(void* self)
{
    markGameThread();
    flushOwnPublishes();
    const int index = indexOfProvider(self);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 3) {
        if (const unsigned char* const comp = ownItemComponent(index); comp != nullptr) {
            logOwnSubscribers(comp, index);
        }
    }
    return ownItemAvailable(index) ? 1u : 0u;
}

float ownItemNumber(int index);
void ownItemSetNumber(int index, float value);
int indexOfProvider(const void* self);

bool ownItemNumberRange(int index, float& min, float& max);

float __fastcall ownNumberGet(void* self)
{
    flushOwnPublishes();
    const int index = indexOfProvider(self);
    float pending = 0.0f;
    const float value = ownPending(index, pending) ? pending : ownItemNumber(index);
    static std::atomic<int> said{0};
    return value;
}

float __fastcall ownNumberMin(void* self)
{
    float min = 0.0f;
    float max = 0.0f;
    return ownItemNumberRange(indexOfProvider(self), min, max) ? min : 0.0f;
}

float __fastcall ownNumberMax(void* self)
{
    float min = 0.0f;
    float max = 0.0f;
    return ownItemNumberRange(indexOfProvider(self), min, max) ? max : 1.0f;
}

void __fastcall ownNumberSet(void* self, float value)
{
    const int index = indexOfProvider(self);
    if (index < 0) {
        return;
    }
    static std::atomic<int> said{0};
    setOwnPending(index, value);
    ownItemSetNumber(index, value);
}

constexpr std::ptrdiff_t kOptionProviderList = 0xD8;
constexpr bool kProbeOptionLabels = false;

constexpr size_t kOptionStride = 0xA8;
constexpr size_t kOptionLabelCopy = 0x100;
constexpr std::ptrdiff_t kOptionValue = 0x00;
constexpr std::ptrdiff_t kOptionLabelPtr = 0x40;
constexpr size_t kMaxChoices = 8;
constexpr size_t kMaxOptionSets = 8;

struct OptionDonorTemplate {
    unsigned char element[kOptionStride];
    unsigned char label[kOptionLabelCopy];
    std::uintptr_t labelBase;
    bool ready;
};
OptionDonorTemplate g_optionDonor{};

struct OwnOptionSet {
    alignas(16) unsigned char elements[kMaxChoices][kOptionStride];
    alignas(16) unsigned char labels[kMaxChoices][kOptionLabelCopy];
    std::uintptr_t labelObject[kMaxChoices];
    size_t count;
    int item;
};
OwnOptionSet g_ownOptionSets[kMaxOptionSets]{};
std::atomic<size_t> g_ownOptionSetCount{0};

struct OptionSpan {
    std::uintptr_t count;
    const void* first;
};

int ownItemChoice(int index);
void ownItemSetChoice(int index, int at);
const OwnOptionSet* ownOptionSetFor(int index);
MenuItem* ownMenuItem(int index);
void copyAscii(char* dest, size_t cap, const wchar_t* text);

void* g_ownOptionLabelVtable[24]{};
std::atomic<bool> g_ownOptionLabelVtableReady{false};

const wchar_t* ownOptionLabelTextFor(const void* self)
{
    const size_t count = g_ownOptionSetCount.load();
    for (size_t i = 0; i < count && i < kMaxOptionSets; ++i) {
        const OwnOptionSet& set = g_ownOptionSets[i];
        for (size_t n = 0; n < set.count && n < kMaxChoices; ++n) {
            const bool mine = (static_cast<const void*>(set.labels[n]) == self)
                              || (set.labelObject[n] != 0
                                  && reinterpret_cast<const void*>(set.labelObject[n]) == self);
            if (!mine) {
                continue;
            }
            const MenuItem* const item = ownMenuItem(set.item);
            if (item == nullptr || n >= item->choices.size()) {
                return nullptr;
            }
            return item->choices[n].c_str();
        }
    }
    return nullptr;
}

void* __fastcall ownOptionLabelText(void* self, void* out)
{
    if (out == nullptr) {
        return out;
    }
    std::memset(out, 0, 0x20);
    const wchar_t* const wide = ownOptionLabelTextFor(self);
    char text[kOwnItemTextMax]{};
    if (wide != nullptr) {
        copyAscii(text, sizeof(text), wide);
    }
    size_t length = std::strlen(text);
    if (length > kSsoCapacity) {
        length = kSsoCapacity;
        text[length] = '\0';
    }
    writeSsoString(static_cast<char*>(out), text, length);
    static std::atomic<int> said{0};
    return out;
}

bool prepareOwnOptionLabelVtable(const void* label)
{
    if (g_ownOptionLabelVtableReady.load()) {
        return true;
    }
    if (label == nullptr || !memory::isReadable(label, sizeof(void*))) {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, label, sizeof(vtable));
    if (vtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(vtable), sizeof(g_ownOptionLabelVtable))) {
        return false;
    }
    std::memcpy(g_ownOptionLabelVtable, reinterpret_cast<const void*>(vtable),
                sizeof(g_ownOptionLabelVtable));
    g_ownOptionLabelVtable[2] = reinterpret_cast<void*>(&ownOptionLabelText);
    g_ownOptionLabelVtableReady.store(true);
    return true;
}

OptionSpan* __fastcall ownOptionList(void* self, OptionSpan* out)
{
    flushOwnPublishes();
    if (out == nullptr) {
        return out;
    }
    out->count = 0;
    out->first = nullptr;
    const OwnOptionSet* const set = ownOptionSetFor(indexOfProvider(self));
    if (set != nullptr) {
        out->count = set->count;
        out->first = set->elements[0];
    }
    static std::atomic<int> said{0};
    return out;
}

int __fastcall ownOptionGet(void* self)
{
    flushOwnPublishes();
    const int index = indexOfProvider(self);
    float pending = 0.0f;
    const int at = ownPending(index, pending) ? static_cast<int>(pending) : ownItemChoice(index);
    static std::atomic<int> said{0};
    return at;
}

void __fastcall ownOptionSet(void* self, int at)
{
    const int index = indexOfProvider(self);
    if (index < 0) {
        return;
    }
    setOwnPending(index, static_cast<float>(at));
    ownItemSetChoice(index, at);
    publishOwnComponent(ownItemComponent(index));
}

void* g_ownOptionVtable[kBoolVtableSlots]{};
std::atomic<bool> g_ownOptionVtableReady{false};

bool prepareOwnOptionVtable(const void* donorProvider)
{
    if (g_ownOptionVtableReady.load()) {
        return true;
    }
    if (donorProvider == nullptr || !memory::isReadable(donorProvider, sizeof(void*))) {
        return false;
    }
    std::uintptr_t donorVtable = 0;
    std::memcpy(&donorVtable, donorProvider, sizeof(donorVtable));
    if (donorVtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(donorVtable),
                               sizeof(void*) * kBoolVtableSlots)) {
        return false;
    }
    std::memcpy(g_ownOptionVtable, reinterpret_cast<const void*>(donorVtable),
                sizeof(g_ownOptionVtable));
    g_ownOptionVtable[0] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownOptionVtable[1] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownOptionVtable[2] = reinterpret_cast<void*>(&ownProviderAvailable);
    g_ownOptionVtable[3] = reinterpret_cast<void*>(&ownOptionList);
    g_ownOptionVtable[4] = reinterpret_cast<void*>(&ownOptionGet);
    g_ownOptionVtable[5] = reinterpret_cast<void*>(&ownOptionSet);
    g_ownOptionVtableReady.store(true);
    return true;
}

void* g_ownNumberVtable[kBoolVtableSlots]{};
std::atomic<bool> g_ownNumberVtableReady{false};

bool prepareOwnNumberVtable(const void* donorProvider)
{
    if (g_ownNumberVtableReady.load()) {
        return true;
    }
    if (donorProvider == nullptr || !memory::isReadable(donorProvider, sizeof(void*))) {
        return false;
    }
    std::uintptr_t donorVtable = 0;
    std::memcpy(&donorVtable, donorProvider, sizeof(donorVtable));
    if (donorVtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(donorVtable),
                               sizeof(void*) * kBoolVtableSlots)) {
        return false;
    }
    std::memcpy(g_ownNumberVtable, reinterpret_cast<const void*>(donorVtable),
                sizeof(g_ownNumberVtable));
    g_ownNumberVtable[0] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownNumberVtable[1] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownNumberVtable[2] = reinterpret_cast<void*>(&ownProviderAvailable);
    g_ownNumberVtable[3] = reinterpret_cast<void*>(&ownNumberGet);
    g_ownNumberVtable[4] = reinterpret_cast<void*>(&ownNumberSet);
    g_ownNumberVtable[5] = reinterpret_cast<void*>(&ownNumberMin);
    g_ownNumberVtable[6] = reinterpret_cast<void*>(&ownNumberMax);
    g_ownNumberVtableReady.store(true);
    return true;
}

constexpr size_t kOwnTextMax = 64;

struct OwnPendingText {
    bool has = false;
    char text[kOwnTextMax]{};
};
OwnPendingText g_ownPendingText[kMaxOwnItems]{};
std::mutex g_ownPendingTextMutex;

void setOwnPendingText(int index, const char* text, size_t length)
{
    if (index < 0 || index >= static_cast<int>(kMaxOwnItems)) {
        return;
    }
    if (length > kOwnTextMax - 1) {
        length = kOwnTextMax - 1;
    }
    const std::lock_guard<std::mutex> lock(g_ownPendingTextMutex);
    OwnPendingText& slot = g_ownPendingText[index];
    std::memcpy(slot.text, text, length);
    slot.text[length] = '\0';
    slot.has = true;
}

void clearOwnPendingText(int index)
{
    if (index < 0 || index >= static_cast<int>(kMaxOwnItems)) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_ownPendingTextMutex);
    g_ownPendingText[index].has = false;
}

bool ownPendingText(int index, char* out, size_t cap)
{
    if (index < 0 || index >= static_cast<int>(kMaxOwnItems) || out == nullptr || cap == 0) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(g_ownPendingTextMutex);
    const OwnPendingText& slot = g_ownPendingText[index];
    if (!slot.has) {
        return false;
    }
    std::snprintf(out, cap, "%s", slot.text);
    return true;
}

bool ownItemText(int index, char* out, size_t cap);
void ownItemSetText(int index, const char* text, size_t length);
void noteTextSlot(int slot);

void* __fastcall ownTextGet(void* self, void* out)
{
    flushOwnPublishes();
    if (out == nullptr) {
        return out;
    }
    const int index = indexOfProvider(self);
    char text[kOwnTextMax]{};
    if (!ownPendingText(index, text, sizeof(text))) {
        ownItemText(index, text, sizeof(text));
    }
    {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 2) {
            log().info(L"UiProbe: text get for item {} -> [{}]", index, toUtf16(text));
        }
    }
    writeOwnString(out, text, std::strlen(text));
    return out;
}

void __fastcall ownTextSet(void* self, const std::uintptr_t* view)
{
    const int index = indexOfProvider(self);
    if (index < 0 || view == nullptr
        || !memory::isReadable(view, sizeof(std::uintptr_t) * 2)) {
        return;
    }
    const auto* const chars = reinterpret_cast<const char*>(view[0]);
    size_t length = view[1];
    if (length > kOwnTextMax - 1) {
        length = kOwnTextMax - 1;
    }
    char text[kOwnTextMax]{};
    if (length > 0) {
        if (chars == nullptr || !memory::isReadable(chars, length)) {
            return;
        }
        std::memcpy(text, chars, length);
    }
    text[length] = '\0';
    {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 2) {
            log().info(L"UiProbe: text set for item {} -> [{}]", index, toUtf16(text));
        }
    }
    setOwnPendingText(index, text, length);
    ownItemSetText(index, text, length);
}

void __fastcall ownTextCommit(void* self)
{
    (void)self;
    noteTextSlot(5);
    flushOwnPublishes();
}

void noteTextSlot(int slot)
{
    static std::atomic<unsigned> seen{0};
    const unsigned bit = 1u << slot;
    if ((seen.fetch_or(bit) & bit) == 0) {
        log().info(L"UiProbe: text provider slot [{}] was called", slot);
    }
}

unsigned int __fastcall ownTextZero(void*)
{
    noteTextSlot(6);
    return 0u;
}

unsigned int __fastcall ownTextAvailable(void* self)
{
    noteTextSlot(2);
    markGameThread();
    flushOwnPublishes();
    return ownItemAvailable(indexOfProvider(self)) ? 1u : 0u;
}

void* __fastcall ownTextSpare7(void*, void*, void*, void*) { noteTextSlot(7); return nullptr; }
void* __fastcall ownTextSpare8(void*, void*, void*, void*) { noteTextSlot(8); return nullptr; }
void* __fastcall ownTextSpare9(void*, void*, void*, void*) { noteTextSlot(9); return nullptr; }

void* g_ownTextVtable[kBoolVtableSlots]{};
std::atomic<bool> g_ownTextVtableReady{false};

bool prepareOwnTextVtable()
{
    if (g_ownTextVtableReady.load()) {
        return true;
    }
    std::memset(g_ownTextVtable, 0, sizeof(g_ownTextVtable));
    g_ownTextVtable[0] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownTextVtable[1] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownTextVtable[2] = reinterpret_cast<void*>(&ownTextAvailable);
    g_ownTextVtable[3] = reinterpret_cast<void*>(&ownTextGet);
    g_ownTextVtable[4] = reinterpret_cast<void*>(&ownTextSet);
    g_ownTextVtable[5] = reinterpret_cast<void*>(&ownTextCommit);
    g_ownTextVtable[6] = reinterpret_cast<void*>(&ownTextZero);
    g_ownTextVtable[7] = reinterpret_cast<void*>(&ownTextSpare7);
    g_ownTextVtable[8] = reinterpret_cast<void*>(&ownTextSpare8);
    g_ownTextVtable[9] = reinterpret_cast<void*>(&ownTextSpare9);
    g_ownTextVtableReady.store(true);
    return true;
}

std::atomic<int> g_ownCaptureRequest{-1};

void ownItemActivated(const void* callable);

struct ActionCopyLink {
    const void* callable;
    int index;
};
constexpr size_t kMaxActionCopies = 32;
ActionCopyLink g_actionCopies[kMaxActionCopies]{};
std::atomic<size_t> g_actionCopyAt{0};

int indexOfActionCallable(const void* callable);

void rememberActionCopy(const void* callable, int index)
{
    if (callable == nullptr || index < 0) {
        return;
    }
    for (auto& one : g_actionCopies) {
        if (one.callable == callable) {
            one.index = index;
            return;
        }
    }
    const size_t at = g_actionCopyAt.fetch_add(1) % kMaxActionCopies;
    g_actionCopies[at].callable = callable;
    g_actionCopies[at].index = index;
}

void* __fastcall ownActionCopy(void* self, void* where)
{
    if (self == nullptr || where == nullptr) {
        return nullptr;
    }
    std::memcpy(where, self, 0x40);
    const auto ptr = reinterpret_cast<std::uintptr_t>(where);
    std::memcpy(static_cast<char*>(where) + 0x38, &ptr, sizeof(ptr));
    rememberActionCopy(where, indexOfActionCallable(self));
    return where;
}

void __fastcall ownActionCall(void* self)
{
    flushOwnPublishes();
    ownItemActivated(self);
}

void __fastcall ownActionDelete(void*, unsigned) {}

void* g_ownActionVtable[6]{};
std::atomic<bool> g_ownActionVtableReady{false};

int findOwnItemByComp(const void* comp);
bool ownKeyRowIsDefault(int index);

unsigned int __fastcall ownEnabledCall(void* self)
{
    if (self == nullptr) {
        return 0u;
    }
    return 1u;
}

void* g_ownEnabledVtable[6]{};
std::atomic<bool> g_ownEnabledVtableReady{false};

unsigned int __fastcall ownVisibleCall(void* self)
{
    if (self == nullptr) {
        return 0u;
    }
    const auto* const comp = static_cast<const unsigned char*>(self) - kCompVisibleFn;
    const int index = findOwnItemByComp(comp);
    const unsigned int state = (index >= 0 && ownKeyRowIsDefault(index)) ? 2u : 0u;
    static std::atomic<int> said{0};
    return state;
}

void* g_ownVisibleVtable[6]{};
std::atomic<bool> g_ownVisibleVtableReady{false};

bool prepareOwnEnabledVtable(const void* donorCallable)
{
    if (g_ownEnabledVtableReady.load()) {
        return true;
    }
    if (donorCallable == nullptr || !memory::isReadable(donorCallable, sizeof(void*))) {
        return false;
    }
    std::uintptr_t donorVtable = 0;
    std::memcpy(&donorVtable, donorCallable, sizeof(donorVtable));
    if (donorVtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(donorVtable), sizeof(void*) * 6)) {
        return false;
    }
    void* slots[6]{};
    std::memcpy(slots, reinterpret_cast<const void*>(donorVtable), sizeof(slots));
    g_ownEnabledVtable[0] = reinterpret_cast<void*>(&ownActionCopy);
    g_ownEnabledVtable[1] = reinterpret_cast<void*>(&ownActionCopy);
    g_ownEnabledVtable[2] = reinterpret_cast<void*>(&ownEnabledCall);
    g_ownEnabledVtable[3] = slots[3];
    g_ownEnabledVtable[4] = reinterpret_cast<void*>(&ownActionDelete);
    g_ownEnabledVtable[5] = slots[5];
    g_ownEnabledVtableReady.store(true);
    return true;
}

constexpr std::size_t kKeyResetVisibleVtableDisp = 3;

bool prepareOwnVisibleVtable(const void* donorCallable, bool force)
{
    if (g_ownVisibleVtableReady.load() && !force) {
        return true;
    }
    std::uintptr_t donorVtable = 0;
    if (std::byte* const ref = Scanner::instance().address(Target::KeyResetVisibleVtableRef);
        ref != nullptr) {
        const auto candidate = reinterpret_cast<std::uintptr_t>(
            memory::ripTarget(ref, kKeyResetVisibleVtableDisp));
        if (memory::isReadable(reinterpret_cast<const void*>(candidate), sizeof(void*) * 6)) {
            donorVtable = candidate;
        }
    }
    if (donorVtable == 0) {
        if (donorCallable == nullptr || !memory::isReadable(donorCallable, sizeof(void*))) {
            log().warn(L"UiProbe: cannot read the visibility donor");
            return false;
        }
        std::memcpy(&donorVtable, donorCallable, sizeof(donorVtable));
    }
    if (donorVtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(donorVtable), sizeof(void*) * 6)) {
        log().warn(L"UiProbe: cannot read the vtable of the visibility donor ({:#x})", donorVtable);
        return false;
    }
    void* slots[6]{};
    std::memcpy(slots, reinterpret_cast<const void*>(donorVtable), sizeof(slots));
    g_ownVisibleVtable[0] = reinterpret_cast<void*>(&ownActionCopy);
    g_ownVisibleVtable[1] = reinterpret_cast<void*>(&ownActionCopy);
    g_ownVisibleVtable[2] = reinterpret_cast<void*>(&ownVisibleCall);
    g_ownVisibleVtable[3] = slots[3];
    g_ownVisibleVtable[4] = reinterpret_cast<void*>(&ownActionDelete);
    g_ownVisibleVtable[5] = slots[5];
    g_ownVisibleVtableReady.store(true);
    return true;
}

bool prepareOwnActionVtable(const void* donorCallable)
{
    if (g_ownActionVtableReady.load()) {
        return true;
    }
    if (donorCallable == nullptr || !memory::isReadable(donorCallable, sizeof(void*))) {
        return false;
    }
    std::uintptr_t donorVtable = 0;
    std::memcpy(&donorVtable, donorCallable, sizeof(donorVtable));
    if (donorVtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(donorVtable), sizeof(void*) * 6)) {
        return false;
    }
    void* slots[6]{};
    std::memcpy(slots, reinterpret_cast<const void*>(donorVtable), sizeof(slots));
    g_ownActionVtable[0] = reinterpret_cast<void*>(&ownActionCopy);
    g_ownActionVtable[1] = reinterpret_cast<void*>(&ownActionCopy);
    g_ownActionVtable[2] = reinterpret_cast<void*>(&ownActionCall);
    g_ownActionVtable[3] = slots[3];
    g_ownActionVtable[4] = reinterpret_cast<void*>(&ownActionDelete);
    g_ownActionVtable[5] = slots[5];
    g_ownActionVtableReady.store(true);
    return true;
}

bool prepareOwnBoolVtable(const void* donorProvider)
{
    if (g_ownBoolVtableReady.load()) {
        return true;
    }
    if (donorProvider == nullptr || !memory::isReadable(donorProvider, sizeof(void*))) {
        return false;
    }
    std::uintptr_t donorVtable = 0;
    std::memcpy(&donorVtable, donorProvider, sizeof(donorVtable));
    if (donorVtable == 0
        || !memory::isReadable(reinterpret_cast<const void*>(donorVtable),
                               sizeof(void*) * kBoolVtableSlots)) {
        return false;
    }
    std::memcpy(g_ownBoolVtable, reinterpret_cast<const void*>(donorVtable),
                sizeof(g_ownBoolVtable));
    g_ownBoolVtable[0] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownBoolVtable[1] = reinterpret_cast<void*>(&ownProviderNoop);
    g_ownBoolVtable[2] = reinterpret_cast<void*>(&ownProviderAvailable);
    g_ownBoolVtable[3] = reinterpret_cast<void*>(&ownBoolGet);
    g_ownBoolVtable[4] = reinterpret_cast<void*>(&ownBoolSet);
    g_ownBoolVtableReady.store(true);
    return true;
}

struct OwnItem {
    char id[kOwnItemIdMax];
    char nameKey[kOwnItemIdMax];
    char text[kOwnItemTextMax];
    unsigned char type;
    int module;
    int child;
    void* comp;
    bool weakBody;
};

std::vector<MenuItem> g_ownMenuTree;

OwnItem g_ownItems[kMaxOwnItems]{};
std::atomic<size_t> g_ownItemCount{0};

MenuItem* ownMenuItem(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= g_ownItemCount.load()) {
        return nullptr;
    }
    const OwnItem& item = g_ownItems[static_cast<size_t>(index)];
    if (item.module < 0 || static_cast<size_t>(item.module) >= g_ownMenuTree.size()) {
        return nullptr;
    }
    MenuItem& tab = g_ownMenuTree[static_cast<size_t>(item.module)];
    if (item.child < 0 || static_cast<size_t>(item.child) >= tab.children.size()) {
        return nullptr;
    }
    return &tab.children[static_cast<size_t>(item.child)];
}

void copyAscii(char* dest, size_t cap, const wchar_t* text)
{
    size_t n = 0;
    if (text != nullptr) {
        for (; text[n] != L'\0' && n + 1 < cap; ++n) {
            const wchar_t ch = text[n];
            dest[n] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
        }
    }
    dest[n] = '\0';
}

std::atomic<int> g_captureItem{-1};

std::string ownRowPrefix(const char* id)
{
    const char* const dot = std::strrchr(id, '.');
    return (dot == nullptr) ? std::string(id) : std::string(id, dot);
}

int ownCaptureStateOf(int index)
{
    const size_t count = g_ownItemCount.load();
    if (index < 0 || static_cast<size_t>(index) >= count) {
        return -1;
    }
    const std::string want = ownRowPrefix(g_ownItems[index].id) + ".captureState";
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (want == g_ownItems[i].id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ownItemValue(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < kMaxOwnItems) {
        const char* const id = g_ownItems[index].id;
        const size_t len = std::strlen(id);
        constexpr size_t kTail = sizeof(".captureState") - 1;
        if (len > kTail && std::strcmp(id + len - kTail, ".captureState") == 0) {
            const int capturing = g_captureItem.load();
            if (capturing < 0 || static_cast<size_t>(capturing) >= kMaxOwnItems) {
                return false;
            }
            return ownRowPrefix(g_ownItems[capturing].id) == ownRowPrefix(id);
        }
    }
    const MenuItem* const item = ownMenuItem(index);
    return (item != nullptr && item->isOn) ? item->isOn() : false;
}

int ownTabOf(int index)
{
    const size_t count = g_ownItemCount.load();
    if (index < 0 || static_cast<size_t>(index) >= count || static_cast<size_t>(index) >= kMaxOwnItems) {
        return -1;
    }
    const int module = g_ownItems[index].module;
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].module == module && g_ownItems[i].child == -1) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const unsigned char* ownItemComponent(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= g_ownItemCount.load()
        || static_cast<size_t>(index) >= kMaxOwnItems) {
        return nullptr;
    }
    return static_cast<const unsigned char*>(g_ownItems[index].comp);
}

void requestOwnPublishAllTabs()
{
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].type == kCompTypeGroupInfo && g_ownItems[i].child < 0) {
            requestOwnPublish(static_cast<int>(i));
        }
    }
}

void requestOwnPublishEverything()
{
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        requestOwnPublish(static_cast<int>(i));
    }
}

void requestOwnPublishSiblings(int index)
{
    const size_t count = g_ownItemCount.load();
    if (index < 0 || static_cast<size_t>(index) >= count) {
        return;
    }
    const int module = g_ownItems[index].module;
    if (module < 0) {
        return;
    }
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].module == module && static_cast<int>(i) != index) {
            requestOwnPublish(static_cast<int>(i));
        }
    }
}

int findOwnItemByComp(const void* comp)
{
    if (comp == nullptr) {
        return -1;
    }
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].comp == comp) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ownKeyRowIsDefault(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= g_ownItemCount.load()) {
        return false;
    }
    const char* const id = g_ownItems[index].id;
    const size_t length = std::strlen(id);
    constexpr size_t kTail = sizeof(".reset") - 1;
    if (length <= kTail || std::strcmp(id + length - kTail, ".reset") != 0) {
        return false;
    }
    const MenuItem* const item = ownMenuItem(index);
    if (item == nullptr || !item->getKeys) {
        return false;
    }
    std::vector<int> now = item->getKeys();
    std::vector<int> want = item->defaultKeys;
    std::sort(now.begin(), now.end());
    std::sort(want.begin(), want.end());
    return now == want;
}

void* findRegistryComponent(const char* id);

bool refreshOwnPage();

MenuItem* schematicaRowOf(int want, size_t index)
{
    buildOwnItems();
    MenuItem* tab = nullptr;
    for (MenuItem& one : g_ownMenuTree) {
        if (one.labelText() == L"Schematica") {
            tab = &one;
            break;
        }
    }
    if (tab == nullptr) {
        return nullptr;
    }
    size_t seen = 0;
    for (MenuItem& child : tab->children) {
        if (!child.onPage) {
            continue;
        }
        if (child.pageTab != want) {
            continue;
        }
        if (seen++ == index) {
            return &child;
        }
    }
    return nullptr;
}

MenuItem* schematicaRow(size_t index)
{
    if (index >= kMaxPageRows) {
        return nullptr;
    }
    const int tab = g_pgRowMenuTab[index];
    const int at = g_pgRowMenuIdx[index];
    if (tab < 0 || at < 0) {
        return nullptr;
    }
    return schematicaRowOf(tab, static_cast<size_t>(at));
}

bool schematicaRowTextOf(int tab, size_t index, char* out, size_t cap)
{
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = 0;
    const MenuItem* const child = schematicaRowOf(tab, index);
    if (child == nullptr) {
        return false;
    }
    std::wstring text = child->labelText();
    const std::wstring value = child->valueText();
    if (!value.empty()) {
        text += L": ";
        text += value;
    } else if (child->kind == MenuItemKind::Toggle) {
        text += child->toggleState() ? L": ON" : L": OFF";
    }
    copyAscii(out, cap, text.c_str());
    return true;
}

bool schematicaRowText(size_t index, char* out, size_t cap)
{
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = '\0';
    const MenuItem* const child = schematicaRow(index);
    if (child == nullptr) {
        return false;
    }
    std::wstring text = child->labelText();
    const std::wstring value = child->valueText();
    if (!value.empty()) {
        text += L": ";
        text += value;
    } else if (child->kind == MenuItemKind::Toggle) {
        text += child->toggleState() ? L": ON" : L": OFF";
    }
    copyAscii(out, cap, text.c_str());
    return true;
}

void requestPageRebuild(bool closeItOurselves)
{
    if (g_pgCloseOnPress.load()) {
        const unsigned long long want = GetTickCount64() + kPageReopenDelayMs;
        unsigned long long have = g_pgReopenAt.load();
        bool alreadyBooked = false;
        while (true) {
            if (have != 0 && have <= want) {
                alreadyBooked = true;
                break;
            }
            if (g_pgReopenAt.compare_exchange_weak(have, want)) {
                break;
            }
        }
        if (closeItOurselves && !alreadyBooked) {
            g_pgReopenAt.store(0);
            g_pgCloseDeadline.store(GetTickCount64() + kPageCloseWaitMs);
            sendEscapeToGame();
        }
        return;
    }
    openOwnPage(true);
}

void pumpPagePress()
{
    if (const int row = g_ptPressed.exchange(-1); row >= 0) {
        if (static_cast<size_t>(row) < g_ptCount) {
            const int wasLast = g_ptLastPressed.exchange(row);
            bool changed = (wasLast != row);
            if (g_ptKind[row] == kLeftTab) {
                const int tab = g_ptIndex[row];
                const int before = g_pgTab.exchange(tab);
                changed = changed || (before != tab);
                if (before != tab) {
                    log().info(L"UiProbe: page tab {} -> {} ({})", before, tab,
                               toUtf16(kPageTabNames[tab]));
                }
            } else {
                Schematica& mod = Schematica::instance();
                const int beforeAt = mod.editingIndex();
                mod.selectBlueprint(static_cast<size_t>(g_ptIndex[row]));
                changed = changed || (beforeAt != mod.editingIndex());
                if (g_pgTab.load() == kEmptyTab) {
                    g_pgTab.store(1);
                    changed = true;
                }
                if (beforeAt != mod.editingIndex()) {
                    log().info(L"UiProbe: selected blueprint {}",
                               mod.blueprintName(static_cast<size_t>(g_ptIndex[row])));
                }
            }
            (void)changed;
        }
        return;
    }

    const int idx = g_pgPressed.exchange(-1);
    if (idx < 0 || static_cast<size_t>(idx) >= kMaxPageRows) {
        return;
    }
    if (g_pgRowAct[idx] == kActGoto) {
        const int want = g_pgRowArg[idx];
        if (want < 0 || want >= kScreenCount) {
            return;
        }
        bool changed = false;
        if (g_pgRowBlueprint[idx] >= 0
            && Schematica::instance().editingIndex() != g_pgRowBlueprint[idx]) {
            Schematica::instance().selectBlueprint(static_cast<size_t>(g_pgRowBlueprint[idx]));
            changed = true;
        }
        const int before = g_pgScreen.exchange(want);
        changed = changed || (before != want);
        if (!changed) {
            return;
        }
        (void)pageHeaderTitle();
        requestPageRebuild(false);
        return;
    }
    if (g_pgRowAct[idx] == kActNone) {
        return;
    }
    if (g_pgRowAct[idx] == kActShow) {
        const int which = g_pgRowBlueprint[idx];
        if (which < 0 || static_cast<size_t>(which) >= Schematica::instance().blueprintCount()) {
            return;
        }
        const bool was = Schematica::instance().blueprintVisible(static_cast<size_t>(which));
        Schematica::instance().setBlueprintVisible(static_cast<size_t>(which), !was);
        log().info(L"UiProbe: {} display set to {}",
                   Schematica::instance().blueprintName(static_cast<size_t>(which)),
                   was ? L"OFF" : L"ON");
        requestPageRebuild(false);
        return;
    }
    MenuItem* const row = schematicaRow(static_cast<size_t>(idx));
    if (row == nullptr) {
        log().warn(L"UiProbe: page row {} has no item", idx);
        return;
    }
    if (g_pgIsEdit[idx]) {
        return;
    }
    if (g_pgRowKind[idx] == kRowSlider) {
        return;
    }
    if (g_pgRowBlueprint[idx] >= 0
        && Schematica::instance().editingIndex() != g_pgRowBlueprint[idx]) {
        Schematica::instance().selectBlueprint(static_cast<size_t>(g_pgRowBlueprint[idx]));
    }
    const std::wstring before = row->labelText() + L" = " + row->valueText();
    const size_t countBefore = Schematica::instance().blueprintCount();
    if (row->activate) {
        row->activate();
    }
    if (Schematica::instance().blueprintCount() < countBefore) {
        g_pgTab.store(kEmptyTab);
        g_ptLastPressed.store(-1);
        g_pgScreen.store(kScreenBlueprints);
    }
    schematicaRowText(static_cast<size_t>(idx), g_pgTexts[idx], kPageTextBytes);
    log().info(L"UiProbe: page row {} pressed ({} -> {})", idx, before,
               toUtf16(g_pgTexts[idx]));

    const std::wstring after = row->labelText() + L" = " + row->valueText();
    const bool layerRow = (g_pgRowMenuTab[idx] == 2);
    if (after == before && g_pgRowKind[idx] != kRowToggle && !layerRow) {
        return;
    }
    const bool needsSelfClose = (g_pgRowKind[idx] != kRowButton);
    requestPageRebuild(needsSelfClose);
}

void pumpPageReopen()
{
    if (const unsigned long long limit = g_pgCloseDeadline.load(); limit != 0) {
        if (GetTickCount64() < limit) {
            return;
        }
        g_pgCloseDeadline.store(0);
        log().warn(L"UiProbe: no close signal after {} ms, reopening the page",
                   kPageCloseWaitMs);
        openOwnPage(true);
        g_pgReopenAt.store(0);
        return;
    }
    const unsigned long long at = g_pgReopenAt.load();
    if (at == 0 || GetTickCount64() < at) {
        return;
    }
    g_pgReopenAt.store(0);
    openOwnPage(true);
}

void pumpPageEntry()
{
    Schematica& mod = Schematica::instance();
    const int keepEditing = mod.editingIndex();
    int nowEditing = keepEditing;
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        if (!g_pgIsEdit[i]) {
            continue;
        }
        const std::uintptr_t box = g_pgCtl[i].load();
        if (box == 0) {
            continue;
        }
        wchar_t want[24]{};
        std::swprintf(want, 24, L"tk_pg%u", static_cast<unsigned>(i));
        const size_t wantLen = std::wcslen(want);
        const std::wstring name = readString(reinterpret_cast<const char*>(box) + kControlName);
        if (name.size() < wantLen || name.compare(name.size() - wantLen, wantLen, want) != 0) {
            g_pgCtl[i].store(0);
            continue;
        }
        std::string text;
        if (!readEditBoxText(box, text) || text.empty()) {
            continue;
        }
        if (std::strncmp(text.c_str(), g_pgLastText[i], kPageTextBytes) == 0) {
            continue;
        }
        std::snprintf(g_pgLastText[i], kPageTextBytes, "%s", text.c_str());
        MenuItem* const row = schematicaRow(i);
        if (row == nullptr || !row->setText) {
            continue;
        }
        if (g_pgRowBlueprint[i] >= 0 && nowEditing != g_pgRowBlueprint[i]) {
            mod.selectBlueprintQuiet(static_cast<size_t>(g_pgRowBlueprint[i]));
            nowEditing = g_pgRowBlueprint[i];
        }
        row->setText(toUtf16(text));
        log().info(L"UiProbe: page row {} typed [{}] -> {}", i, toUtf16(text), row->valueText());
    }
    if (nowEditing != keepEditing && keepEditing >= 0) {
        mod.selectBlueprintQuiet(static_cast<size_t>(keepEditing));
    }
}

constexpr std::uintptr_t kSliderRaw = 0x38;
constexpr std::uintptr_t kSliderStep = 0x3C;
constexpr std::uintptr_t kSliderSteps = 0x40;
constexpr std::uintptr_t kSliderFlags = 0x58;
float g_pgLastSlider[kMaxPageRows]{};

constexpr std::ptrdiff_t kSliderOwner = 0x88;

int pageSliderRowOf(void* self)
{
    if (self == nullptr || !memory::isReadable(reinterpret_cast<const char*>(self) + 8, 8)) {
        return -1;
    }
    const void* const ctl =
        *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(self) + 8);
    if (ctl == nullptr || !memory::isReadable(reinterpret_cast<const char*>(ctl) + kSliderOwner, 8)) {
        return -1;
    }
    const void* const owner =
        *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(ctl) + kSliderOwner);
    if (owner == nullptr
        || !memory::isReadable(reinterpret_cast<const char*>(owner) + kControlName, 0x20)) {
        return -1;
    }
    const std::wstring name = readString(reinterpret_cast<const char*>(owner) + kControlName);
    const size_t at = name.rfind(L"tk_pg");
    if (at == std::wstring::npos) {
        return -1;
    }
    int row = -1;
    for (size_t d = at + 5; d < name.size() && name[d] >= L'0' && name[d] <= L'9'; ++d) {
        row = (row < 0 ? 0 : row) * 10 + (name[d] - L'0');
    }
    return (row >= 0 && static_cast<size_t>(row) < kMaxPageRows) ? row : -1;
}

void writeSliderRatio(void* self, float ratio)
{
    if (!memory::isReadable(reinterpret_cast<const char*>(self) + kSliderFlags, 8)) {
        return;
    }
    unsigned char flags = 0;
    std::memcpy(&flags, reinterpret_cast<const char*>(self) + kSliderFlags, sizeof(flags));
    std::memcpy(reinterpret_cast<char*>(self) + kSliderRaw, &ratio, sizeof(ratio));
    if ((flags & 1u) != 0) {
        std::int32_t steps = 0;
        std::memcpy(&steps, reinterpret_cast<const char*>(self) + kSliderSteps, sizeof(steps));
        if (steps > 1) {
            const std::int32_t step = static_cast<std::int32_t>(
                std::lround(static_cast<double>(ratio) * (steps - 1)));
            const std::int32_t clamped = std::clamp(step, 0, steps - 1);
            std::memcpy(reinterpret_cast<char*>(self) + kSliderStep, &clamped, sizeof(clamped));
        }
    }
}

bool g_pgSliderSeeded[kMaxPageRows]{};

void forgetPageSliders()
{
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        g_pgSliderSeeded[i] = false;
        g_pgLastSlider[i] = -1.0F;
    }
}

bool onSliderPublishImpl(void* self, float value, float& reseed)
{
    const int row = pageSliderRowOf(self);
    if (row < 0 || g_pgRowKind[row] != kRowSlider) {
        return false;
    }
    MenuItem* const item = schematicaRow(static_cast<size_t>(row));
    if (item == nullptr || !item->setNumber || !item->getNumber) {
        return false;
    }
    const float low = item->numberMin;
    const float high = item->numberMax;
    if (!(high > low)) {
        return false;
    }

    const float want = std::clamp((item->getNumber() - low) / (high - low), 0.0F, 1.0F);
    if (!g_pgSliderSeeded[row]) {
        g_pgSliderSeeded[row] = true;
        g_pgLastSlider[row] = want;
        if (std::fabs(value - want) < 0.0005F) {
            return false;
        }
        writeSliderRatio(self, want);
        reseed = want;
        return true;
    }

    if (!(value >= 0.0F) || value > 1.0F) {
        return false;
    }
    if (std::fabs(value - g_pgLastSlider[row]) < 0.0005F) {
        return false;
    }
    g_pgLastSlider[row] = value;
    const float made = low + value * (high - low);
    item->setNumber(made);
    log().info(L"UiProbe: {} set to {}", item->labelText(), made);
    return false;
}

void onPageBag(void* bag)
{
    g_pgBag.store(bag);
}

float g_pgToldToggle[kMaxPageRows]{};
bool g_pgToldOnce[kMaxPageRows]{};
float g_ptTold[kMaxLeftRows]{};
bool g_ptToldOnce[kMaxLeftRows]{};

void pumpToggleState()
{
    void* const holder = g_pgBag.load();
    if (holder == nullptr || !memory::isReadable(holder, 32)) {
        return;
    }
    Schematica& mod = Schematica::instance();
    const int keepEditing = mod.editingIndex();
    int nowEditing = keepEditing;
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        if (g_pgRowKind[i] != kRowToggle) {
            continue;
        }
        const MenuItem* const item = schematicaRow(i);
        if (item == nullptr) {
            continue;
        }
        if (g_pgRowBlueprint[i] >= 0 && nowEditing != g_pgRowBlueprint[i]) {
            mod.selectBlueprintQuiet(static_cast<size_t>(g_pgRowBlueprint[i]));
            nowEditing = g_pgRowBlueprint[i];
        }
        const float want = item->toggleState() ? 1.0F : 0.0F;
        if (g_pgToldOnce[i] && g_pgToldToggle[i] == want) {
            continue;
        }
        char name[24]{};
        const int len = std::snprintf(name, sizeof(name), "#tk_pg_%u",
                                      static_cast<unsigned>(i));
        if (len <= 0) {
            continue;
        }
        if (!hooks::setUiBagNumber(holder, name, static_cast<size_t>(len), want)) {
            break;
        }
        g_pgToldToggle[i] = want;
        g_pgToldOnce[i] = true;
    }
    if (nowEditing != keepEditing && keepEditing >= 0) {
        mod.selectBlueprintQuiet(static_cast<size_t>(keepEditing));
    }

    for (size_t t = 0; t < g_ptCount; ++t) {
        const float want = pageLeftRowSelected(t) ? 1.0F : 0.0F;
        if (g_ptToldOnce[t] && g_ptTold[t] == want) {
            continue;
        }
        const int len = std::snprintf(g_ptBindNames[t], sizeof(g_ptBindNames[t]), "#tk_pt_%u",
                                      static_cast<unsigned>(t));
        if (len <= 0) {
            continue;
        }
        if (!hooks::setUiBagNumber(holder, g_ptBindNames[t], static_cast<size_t>(len), want)) {
            return;
        }
        g_ptTold[t] = want;
        g_ptToldOnce[t] = true;
    }
}

void forgetToggleState()
{
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        g_pgToldOnce[i] = false;
    }
    for (size_t t = 0; t < kMaxLeftRows; ++t) {
        g_ptToldOnce[t] = false;
    }
}

bool onSliderPublish(void* self, float value, float& reseed)
{
    if (self == nullptr) {
        return false;
    }
    return onSliderPublishImpl(self, value, reseed);
}

bool ownItemAvailable(int index)
{
    const MenuItem* const item = ownMenuItem(index);
    return (item == nullptr) ? true : item->isAvailable();
}

float ownItemNumber(int index)
{
    const MenuItem* const item = ownMenuItem(index);
    return (item != nullptr && item->getNumber) ? item->getNumber() : 0.0f;
}

bool ownItemNumberRange(int index, float& min, float& max)
{
    const MenuItem* const item = ownMenuItem(index);
    if (item == nullptr || item->numberMin >= item->numberMax) {
        return false;
    }
    min = item->numberMin;
    max = item->numberMax;
    return true;
}

float ownItemNumberScale(int index)
{
    float min = 0.0f;
    float max = 0.0f;
    if (!ownItemNumberRange(index, min, max) || max <= min) {
        return 1.0f;
    }
    if (const MenuItem* const item = ownMenuItem(index);
        item != nullptr && item->numberIsInteger) {
        return 1.0f;
    }
    constexpr int kWantSteps = 24;
    constexpr float kCandidates[] = {1.0f, 10.0f, 100.0f, 1000.0f, 2.0f,  4.0f,
                                     8.0f, 16.0f, 32.0f,  64.0f,   128.0f};
    const auto isWhole = [](float value) {
        return std::fabs(value - std::round(value)) < 1e-4f;
    };
    for (const float scale : kCandidates) {
        if (!isWhole(min * scale) || !isWhole(max * scale)) {
            continue;
        }
        if ((max - min) * scale >= static_cast<float>(kWantSteps)) {
            return scale;
        }
    }
    return 100.0f;
}

std::atomic<int> g_ownNumberRequest{-1};
std::atomic<float> g_ownNumberValue{0.0f};

void ownItemSetNumber(int index, float value)
{
    g_ownNumberValue.store(value);
    g_ownNumberRequest.store(index);
}

void ownItemRequestToggle(int index) { g_ownToggleRequest.store(index); }

bool ownItemText(int index, char* out, size_t cap)
{
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = '\0';
    const MenuItem* const item = ownMenuItem(index);
    if (item == nullptr || !item->getText) {
        return false;
    }
    copyAscii(out, cap, item->getText().c_str());
    return true;
}

std::atomic<int> g_ownTextRequest{-1};
char g_ownTextValue[kOwnTextMax]{};
std::mutex g_ownTextValueMutex;

void ownItemSetText(int index, const char* text, size_t length)
{
    if (length > kOwnTextMax - 1) {
        length = kOwnTextMax - 1;
    }
    {
        const std::lock_guard<std::mutex> lock(g_ownTextValueMutex);
        std::memcpy(g_ownTextValue, text, length);
        g_ownTextValue[length] = '\0';
    }
    g_ownTextRequest.store(index);
}

int ownItemChoice(int index)
{
    const MenuItem* const item = ownMenuItem(index);
    return (item != nullptr && item->getChoice) ? item->getChoice() : 0;
}

std::atomic<int> g_ownChoiceRequest{-1};
std::atomic<int> g_ownChoiceValue{0};

void ownItemSetChoice(int index, int at)
{
    g_ownChoiceValue.store(at);
    g_ownChoiceRequest.store(index);
}

const OwnOptionSet* ownOptionSetFor(int index)
{
    const size_t count = g_ownOptionSetCount.load();
    for (size_t i = 0; i < count && i < kMaxOptionSets; ++i) {
        if (g_ownOptionSets[i].item == index) {
            return &g_ownOptionSets[i];
        }
    }
    return nullptr;
}

using MakeOptionElementFn = void(__fastcall*)(void* out, int value, const void* twoStrings,
                                              const void* oneString, const unsigned char* flag);

void writeArgString(unsigned char* dest, const char* text)
{
    std::memset(dest, 0, 0x20);
    size_t length = std::strlen(text);
    if (length > kSsoCapacity) {
        length = kSsoCapacity;
    }
    std::memcpy(dest, text, length);
    const std::uintptr_t size = length;
    const std::uintptr_t room = kSsoCapacity;
    std::memcpy(dest + 0x10, &size, sizeof(size));
    std::memcpy(dest + 0x18, &room, sizeof(room));
}

bool buildOwnOptionSetByGame(OwnOptionSet& set, const MenuItem& item)
{
    const auto make = Scanner::instance().addressAs<MakeOptionElementFn>(
        Target::MakeOptionElement);
    if (make == nullptr) {
        return false;
    }
    for (size_t n = 0; n < set.count; ++n) {
        char text[kOwnItemTextMax]{};
        copyAscii(text, sizeof(text), item.choices[n].c_str());
        alignas(16) unsigned char two[0x40]{};
        alignas(16) unsigned char one[0x20]{};
        writeArgString(two, text);
        writeArgString(two + 0x20, text);
        writeArgString(one, text);
        const unsigned char flag = 0;
        std::memset(set.elements[n], 0, kOptionStride);
        make(set.elements[n], static_cast<int>(n), two, one, &flag);

        std::uintptr_t label = 0;
        std::memcpy(&label, set.elements[n] + kOptionLabelPtr, sizeof(label));
        set.labelObject[n] = label;
        if (label != 0 && memory::isReadable(reinterpret_cast<const void*>(label), 0x48)
            && prepareOwnOptionLabelVtable(reinterpret_cast<const void*>(label))) {
            const auto own = reinterpret_cast<std::uintptr_t>(&g_ownOptionLabelVtable[0]);
            std::memcpy(reinterpret_cast<void*>(label), &own, sizeof(own));
        }
        static std::atomic<int> said{0};
    }
    return true;
}

bool buildOwnOptionSet(int index, const MenuItem& item, const char* id)
{
    if (item.choices.empty()) {
        return false;
    }
    const size_t slot = g_ownOptionSetCount.load();
    if (slot >= kMaxOptionSets) {
        log().warn(L"UiProbe: not enough option containers (limit {})", kMaxOptionSets);
        return false;
    }
    OwnOptionSet& set = g_ownOptionSets[slot];
    set.item = index;
    set.count = (std::min)(item.choices.size(), kMaxChoices);

    if (buildOwnOptionSetByGame(set, item)) {
        g_ownOptionSetCount.store(slot + 1);
        return true;
    }
    if (!g_optionDonor.ready) {
        return false;
    }

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto dropForeignPointers = [&](unsigned char* buffer, size_t size,
                                         const bool* keep) {
        for (size_t off = 0; off + sizeof(std::uintptr_t) <= size; off += sizeof(std::uintptr_t)) {
            if (keep != nullptr && keep[off / sizeof(std::uintptr_t)]) {
                continue;
            }
            std::uintptr_t word = 0;
            std::memcpy(&word, buffer + off, sizeof(word));
            if (word == 0) {
                continue;
            }
            const auto self = reinterpret_cast<std::uintptr_t>(buffer);
            const bool inSelf = (word >= self && word < self + size);
            const bool inModule =
                (moduleBase != 0 && word > moduleBase && word < moduleBase + 0x13000000);
            if (inSelf || inModule) {
                continue;
            }
            if (!memory::isReadable(reinterpret_cast<const void*>(word), 8)) {
                continue;
            }
            const std::uintptr_t none = 0;
            std::memcpy(buffer + off, &none, sizeof(none));
        }
    };

    for (size_t n = 0; n < set.count; ++n) {
        std::memcpy(set.elements[n], g_optionDonor.element, kOptionStride);
        dropForeignPointers(set.elements[n], kOptionStride, nullptr);
        const std::uintptr_t value = n;
        std::memcpy(set.elements[n] + kOptionValue, &value, sizeof(value));

        std::memcpy(set.labels[n], g_optionDonor.label, kOptionLabelCopy);
        const auto ownBase = reinterpret_cast<std::uintptr_t>(set.labels[n]);
        for (size_t off = 0; off + sizeof(std::uintptr_t) <= kOptionLabelCopy;
             off += sizeof(std::uintptr_t)) {
            std::uintptr_t word = 0;
            std::memcpy(&word, set.labels[n] + off, sizeof(word));
            if (word >= g_optionDonor.labelBase
                && word < g_optionDonor.labelBase + kOptionLabelCopy) {
                const std::uintptr_t rebased = ownBase + (word - g_optionDonor.labelBase);
                std::memcpy(set.labels[n] + off, &rebased, sizeof(rebased));
            }
        }

        char key[kOwnItemIdMax + 8]{};
        const int written = std::snprintf(key, sizeof(key), "%s%c%zu", id, kChoiceSuffix, n);
        const size_t keyLength = (written > 0) ? static_cast<size_t>(written) : 0;
        if (keyLength >= kSsoCapacity) {
            log().warn(L"UiProbe: option key is too long \"{}\" ({} chars)",
                       std::wstring(key, key + keyLength), keyLength);
        }
        int rewritten = 0;
        bool isText[kOptionLabelCopy / sizeof(std::uintptr_t)]{};
        for (std::ptrdiff_t off = 0; off + 0x20 <= static_cast<std::ptrdiff_t>(kOptionLabelCopy);
             off += 8) {
            std::uintptr_t size = 0;
            std::uintptr_t room = 0;
            std::memcpy(&size, set.labels[n] + off + 0x10, sizeof(size));
            std::memcpy(&room, set.labels[n] + off + 0x18, sizeof(room));
            if (size == 0 || room < 15 || room > 0x400 || size > room) {
                continue;
            }
            if (keyLength >= kSsoCapacity) {
                continue;
            }
            writeSsoString(set.labels[n] + off, key, keyLength);
            for (std::ptrdiff_t at = off; at < off + 0x10; at += 8) {
                isText[at / 8] = true;
            }
            ++rewritten;
        }
        dropForeignPointers(set.labels[n], kOptionLabelCopy, isText);

        if (prepareOwnOptionLabelVtable(set.labels[n])) {
            const auto own = reinterpret_cast<std::uintptr_t>(&g_ownOptionLabelVtable[0]);
            std::memcpy(set.labels[n], &own, sizeof(own));
        }

        const auto labelPtr = reinterpret_cast<std::uintptr_t>(set.labels[n]);
        std::memcpy(set.elements[n] + kOptionLabelPtr, &labelPtr, sizeof(labelPtr));

    }
    g_ownOptionSetCount.store(slot + 1);
    return true;
}

unsigned long long g_nameResolvedAt[kMaxOwnItems]{};

struct ResolveMark {
    int item;
    unsigned long long tick;
};
constexpr size_t kResolveMarks = 24;
ResolveMark g_resolveMarks[kResolveMarks]{};
std::atomic<size_t> g_resolveMarkAt{0};

void markItemResolved(int item)
{
    const size_t at = g_resolveMarkAt.fetch_add(1) % kResolveMarks;
    g_resolveMarks[at].item = item;
    g_resolveMarks[at].tick = GetTickCount64();
}

std::atomic<int> g_maybePressed{-1};
std::atomic<unsigned long long> g_maybePressedAt{0};

int indexOfActionCallable(const void* callable)
{
    if (callable == nullptr) {
        return -1;
    }
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].comp == nullptr) {
            continue;
        }
        if (static_cast<const char*>(g_ownItems[i].comp) + kCompActionFn == callable) {
            return static_cast<int>(i);
        }
    }
    for (const auto& one : g_actionCopies) {
        if (one.callable == callable) {
            return one.index;
        }
    }
    return -1;
}

void ownItemActivated(const void* callable)
{
    if (g_inPublish.load()) {
        static std::atomic<int> said{0};
        return;
    }
    const int index = indexOfActionCallable(callable);
    if (index < 0) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 6) {
            log().warn(L"UiProbe: could not tell what was pressed ({:#x})",
                       reinterpret_cast<std::uintptr_t>(callable));
        }
        return;
    }
    const unsigned long long since = GetTickCount64() - g_nameResolvedAt[index];
    const bool isCopy = (g_ownItems[index].comp == nullptr)
                        || (static_cast<const char*>(g_ownItems[index].comp) + kCompActionFn
                            != callable);
    (void)since;
    (void)isCopy;
    g_maybePressedAt.store(GetTickCount64());
    g_maybePressed.store(index);
}

constexpr char kResetHiddenMark[] = "tk-hide";

void formatOwnValueText(char* dest, size_t cap, int index)
{
    const MenuItem* const item = ownMenuItem(index);
    if (item == nullptr) {
        copyAscii(dest, cap, L"-");
        return;
    }
    if (index >= 0 && static_cast<size_t>(index) < kMaxOwnItems) {
        const char* const id = g_ownItems[static_cast<size_t>(index)].id;
        const size_t length = std::strlen(id);
        constexpr size_t kTail = sizeof(".reset") - 1;
        if (length > kTail && std::strcmp(id + length - kTail, ".reset") == 0) {
            copyAscii(dest, cap, ownKeyRowIsDefault(index) ? L"tk-hide" : L"tk-show");
            return;
        }
    }
    if (item->kind == MenuItemKind::Keybind && item->getKeys) {
        copyAscii(dest, cap, keys::comboName(item->getKeys()).c_str());
        return;
    }
    const std::wstring text = item->valueText();
    copyAscii(dest, cap, text.empty() ? L"-" : text.c_str());
}

const char* ownSettingsText(const char* key)
{
    if (key == nullptr || key[0] != 't' || key[1] != 'k' || key[2] != '.') {
        return nullptr;
    }
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (std::strcmp(g_ownItems[i].nameKey, key) != 0) {
            continue;
        }
        g_nameResolvedAt[i] = GetTickCount64();
        markItemResolved(static_cast<int>(i));
        if (g_ownItems[i].type == 4) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1) < 2) {
                log().info(L"UiProbe: the name of the text row {} was asked for", toUtf16(key));
            }
        }
        return g_ownItems[i].text;
    }
    const size_t length = std::strlen(key);

    for (size_t at = length; at >= 2; --at) {
        if (key[at - 1] != kChoiceSuffix) {
            continue;
        }
        bool digits = (at < length);
        for (size_t d = at; d < length && digits; ++d) {
            digits = (key[d] >= '0' && key[d] <= '9');
        }
        if (!digits) {
            continue;
        }
        char base[kOwnItemIdMax]{};
        if (at - 1 >= sizeof(base)) {
            break;
        }
        std::memcpy(base, key, at - 1);
        base[at - 1] = '\0';
        const int which = std::atoi(key + at);
        for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
            if (g_ownItems[i].type != 3 || std::strcmp(g_ownItems[i].id, base) != 0) {
                continue;
            }
            const MenuItem* const source = ownMenuItem(static_cast<int>(i));
            if (source == nullptr || which < 0
                || static_cast<size_t>(which) >= source->choices.size()) {
                return nullptr;
            }
            static char text[kOwnItemTextMax]{};
            copyAscii(text, sizeof(text), source->choices[static_cast<size_t>(which)].c_str());
            static std::atomic<int> said{0};
            return text;
        }
    }

    {
        char bare[kOwnItemTextMax]{};
        size_t bareLength = (std::min)(length, sizeof(bare) - 1);
        std::memcpy(bare, key, bareLength);
        bare[bareLength] = '\0';
        constexpr size_t kNameTail = sizeof(".name") - 1;
        if (bareLength > kNameTail
            && std::strcmp(bare + bareLength - kNameTail, ".name") == 0) {
            bareLength -= kNameTail;
            bare[bareLength] = '\0';
        }
        static char text[kOwnItemTextMax]{};
        for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
            if (g_ownItems[i].type != 3) {
                continue;
            }
            const MenuItem* const source = ownMenuItem(static_cast<int>(i));
            if (source == nullptr) {
                continue;
            }
            for (const std::wstring& choice : source->choices) {
                copyAscii(text, sizeof(text), choice.c_str());
                if (std::strcmp(text, bare) == 0) {
                    return text;
                }
            }
        }
    }

    if (length < 2 || key[length - 1] != kKeyValueSuffix) {
        return nullptr;
    }
    char base[kOwnItemIdMax]{};
    if (length - 1 >= sizeof(base)) {
        return nullptr;
    }
    std::memcpy(base, key, length - 1);
    base[length - 1] = '\0';
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].type == 5 && std::strcmp(g_ownItems[i].nameKey, base) == 0) {
            static char text[kOwnItemTextMax]{};
            formatOwnValueText(text, sizeof(text), static_cast<int>(i));
            return text;
        }
    }
    return nullptr;
}

constexpr size_t kMaxOwnProviders = kMaxOwnItems + 1;
std::atomic<bool> g_ownItemsIncomplete{false};

std::atomic<bool> g_resetUsedFallback{false};

alignas(16) unsigned char g_ownProviderPool[kMaxOwnProviders][0x40]{};
alignas(16) unsigned char g_swapProviderPool[0x40]{};
std::atomic<bool> g_swapProviderReady{false};
std::atomic<size_t> g_ownProviderCount{0};
alignas(16) unsigned char g_ownProviderSection[0x40]{};
alignas(16) unsigned char g_ownProviderTab[0x40]{};
std::atomic<bool> g_ownProvidersReady{false};

alignas(16) unsigned char g_vanillaBorrowedProvider[0x40]{};
std::atomic<bool> g_vanillaBorrowedReady{false};

bool callStdFunctionInto(void* fn, void* out)
{
    if (fn == nullptr || out == nullptr) {
        return false;
    }
    std::uintptr_t impl = 0;
    std::memcpy(&impl, static_cast<const char*>(fn) + 0x38, sizeof(impl));
    if (impl == 0 || !memory::isReadable(reinterpret_cast<const void*>(impl), sizeof(void*))) {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<const void*>(impl), sizeof(vtable));
    if (vtable == 0 || !memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 5)) {
        return false;
    }
    std::uintptr_t doCall = 0;
    std::memcpy(&doCall, reinterpret_cast<const void*>(vtable + 0x10), sizeof(doCall));
    if (doCall == 0) {
        return false;
    }
    using CallFn = void(__fastcall*)(void*, void*);
    __try {
        reinterpret_cast<CallFn>(doCall)(reinterpret_cast<void*>(impl), out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void* findRegistryNode(void* registry, const std::wstring& id)
{
    if (registry == nullptr || !memory::isReadable(registry, 0x40 * 8)) {
        return nullptr;
    }
    std::uintptr_t words[0x40]{};
    std::memcpy(words, registry, sizeof(words));
    for (size_t i = 0; i < 0x40; ++i) {
        const auto head = reinterpret_cast<void*>(words[i]);
        if (head == nullptr || !memory::isReadable(head, 0x70)) {
            continue;
        }
        void* node = head;
        for (size_t step = 0; step < 4096; ++step) {
            std::uintptr_t next = 0;
            std::memcpy(&next, node, sizeof(next));
            if (next == 0 || !memory::isReadable(reinterpret_cast<const void*>(next), 0x70)) {
                break;
            }
            node = reinterpret_cast<void*>(next);
            if (node == head) {
                break;
            }
            if (readString(static_cast<const char*>(node) + 0x10) == id) {
                return node;
            }
        }
    }
    return nullptr;
}

void swapBorrowedGroupProvider(void* registry)
{
    static std::atomic<void*> done{nullptr};
    const std::wstring want(kBorrowedKeyGroupId,
                            kBorrowedKeyGroupId + sizeof(kBorrowedKeyGroupId) - 1);
    void* const node = findRegistryNode(registry, want);
    if (node == nullptr) {
        log().warn(L"UiProbe: could not find the node of the group we borrow ({:#x})",
                   reinterpret_cast<std::uintptr_t>(registry));
        return;
    }
    void* const slot = static_cast<char*>(node) + 0x30;
    if (done.load() == slot) {
        return;
    }
    if (!g_vanillaBorrowedReady.load()) {
        g_vanillaBorrowedReady.store(copyStdFunction(slot, g_vanillaBorrowedProvider));
    }
    if (!copyStdFunction(g_swapProviderPool, slot)) {
        log().warn(L"UiProbe: could not rewrite the node of the group we borrow ({:#x})",
                   reinterpret_cast<std::uintptr_t>(node));
        return;
    }
    done.store(slot);
}

bool captureKeyGroupProvider(void* registry)
{
    if (g_vanillaBorrowedReady.load()) {
        return true;
    }
    constexpr char kSourceId[] = "keyboardAndMouse.inputGroup.standard";
    const std::wstring want(kSourceId, kSourceId + sizeof(kSourceId) - 1);
    void* const node = findRegistryNode(registry, want);
    if (node == nullptr) {
        return false;
    }
    const bool ok = copyStdFunction(static_cast<char*>(node) + 0x30, g_vanillaBorrowedProvider);
    g_vanillaBorrowedReady.store(ok);
    static std::atomic<bool> told{false};
    return ok;
}

void onSettingsGroupRegister(void* registry, const void* idView, void* provider)
{
    const std::string id = readStringView(idView);
    g_settingsGroupCount.fetch_add(1);
    if (registry != nullptr) {
        g_settingsRegistry.store(registry);
    }
    {
        static std::atomic<int> said{0};
        const bool interesting = (id.rfind("json-navigation-", 0) == 0)
                                 || (id.size() > 7 && id.compare(id.size() - 7, 7, "-jsonui") == 0);
        const int at = said.fetch_add(1);
        if (interesting || at < 3) {
            std::uintptr_t vtable = 0;
            if (provider != nullptr && memory::isReadable(provider, sizeof(vtable))) {
                std::memcpy(&vtable, provider, sizeof(vtable));
            }
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            log().info(L"UiProbe: settings group [{}] \"{}\" provider {} (vtable RVA {:#x}){}",
                       at, toUtf16(id), provider,
                       (vtable > base) ? (vtable - base) : vtable,
                       interesting ? L"  <<< json navigation" : L"");
        }
    }
    std::uintptr_t words[3]{};
    const bool readable = (provider != nullptr && memory::isReadable(provider, sizeof(words)));
    if (readable) {
        std::memcpy(words, provider, sizeof(words));
    }
    if (id == kTabsGroupId) {
        g_tabsGroupProvider.store(provider);
        g_tabsCapture0.store(words[1]);
        g_tabsCapture1.store(words[2]);
        if (kAddOwnSettingsTab) {
            buildOwnItems();
            size_t need = kVanillaKeyRows ? 2 : 1;
            const size_t items = g_ownItemCount.load();
            for (size_t i = 0; i < items; ++i) {
                if (g_ownItems[i].type == kCompTypeGroupInfo) {
                    ++need;
                }
            }
            if (need > kMaxOwnProviders) {
                need = kMaxOwnProviders;
            }
            size_t made = 0;
            for (; made < need; ++made) {
                if (!copyStdFunction(provider, g_ownProviderPool[made])) {
                    break;
                }
            }
            if (kVanillaKeyRows && !g_swapProviderReady.load()) {
                g_swapProviderReady.store(copyStdFunction(provider, g_swapProviderPool));
            }
            if (kVanillaKeyRows && !useOwnKeyGroups() && g_swapProviderReady.load()) {
                swapBorrowedGroupProvider(registry);
            }
            if (kVanillaKeyRows && useOwnKeyGroups()) {
                captureKeyGroupProvider(registry);
            }
            g_ownProviderCount.store(made);
            g_ownProvidersReady.store(made == need && made > 0);
        }
    }

    if (readable && !id.empty()) {
        const size_t slot = g_groupCaptureCount.load();
        if (slot < kMaxGroupCaptures) {
            g_groupCaptures[slot].cap0 = words[1];
            g_groupCaptures[slot].cap1 = words[2];
            const size_t n = (std::min)(id.size(), sizeof(g_groupCaptures[slot].id) - 1);
            std::memcpy(g_groupCaptures[slot].id, id.c_str(), n);
            g_groupCaptures[slot].id[n] = '\0';
            g_groupCaptureCount.store(slot + 1);
        }
    }

    static std::atomic<int> said{0};
}

bool g_keyRowMade = false;
int g_keyRowCount = 0;

constexpr int kKeysModule = -2;

constexpr int kJsonTabModule = -3;
constexpr char kOwnTabId[] = "tkschem-jsonui";
constexpr char kOwnTabNameKey[] = "tk.schematica";
constexpr bool kOpenPageFromKey = false;

constexpr bool kOwnJsonTabEntry = false;

bool useOwnKeyGroups()
{
    return kVanillaKeyRows && oreui::patchReady() && oreui::ownGroupIdCount() > 0;
}

int ownKeyGroupIndex(const char* id)
{
    if (id == nullptr || id[0] == '\0') {
        return -1;
    }
    const int count = oreui::ownGroupIdCount();
    for (int i = 0; i < count; ++i) {
        const char* const mine = oreui::ownGroupId(i);
        if (mine != nullptr && std::strcmp(mine, id) == 0) {
            return i;
        }
    }
    return -1;
}

constexpr char kBorrowedKeyNameKey[] = "tk.keys";

std::atomic<int> g_lastRenderedModule{-1};

std::atomic<unsigned long long> g_ownRowTouchedAt{0};

bool ownTabIsRendering()
{
    constexpr unsigned long long kFreshMs = 300;
    const unsigned long long at = g_ownRowTouchedAt.load();
    return at != 0 && (GetTickCount64() - at) <= kFreshMs;
}

bool ownItemIsKeyRow(int index)
{
    return index >= 0 && static_cast<size_t>(index) < g_ownItemCount.load()
           && std::strncmp(g_ownItems[index].id, "tk.g", 4) == 0;
}

int ownKeysTabIndex()
{
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].module == kKeysModule && g_ownItems[i].child == -1) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ownHolderIndex()
{
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (g_ownItems[i].child == 9999) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t keybindsIn(const MenuItem& tab)
{
    size_t n = 0;
    for (const MenuItem& child : tab.children) {
        if (child.kind == MenuItemKind::Keybind) {
            ++n;
        }
    }
    return n;
}

void buildOwnItems()
{
    if (g_ownItemCount.load() != 0) {
        return;
    }
    g_ownMenuTree = ModuleManager::instance().buildMenuItems();
    std::sort(g_ownMenuTree.begin(), g_ownMenuTree.end(),
              [](const MenuItem& a, const MenuItem& b) {
                  const std::wstring left = a.labelText();
                  const std::wstring right = b.labelText();
                  return std::lexicographical_compare(
                      left.begin(), left.end(), right.begin(), right.end(),
                      [](wchar_t x, wchar_t y) { return towlower(x) < towlower(y); });
              });
    size_t count = 0;
    for (size_t i = 0; i < g_ownMenuTree.size() && count < kMaxOwnItems; ++i) {
        MenuItem& tabSource = g_ownMenuTree[i];
        OwnItem& tab = g_ownItems[count];
        std::snprintf(tab.id, sizeof(tab.id), "tk.m%zu", i);
        std::snprintf(tab.nameKey, sizeof(tab.nameKey), "%s", tab.id);
        copyAscii(tab.text, sizeof(tab.text), tabSource.labelText().c_str());
        tab.type = kCompTypeGroupInfo;
        tab.module = static_cast<int>(i);
        tab.child = -1;
        tab.comp = nullptr;
        ++count;
        for (size_t c = 0; c < tabSource.children.size() && count < kMaxOwnItems; ++c) {
            const MenuItem& child = tabSource.children[c];
            if (child.onPage) {
                continue;
            }
            unsigned char type = 0xFF;
            switch (child.kind) {
            case MenuItemKind::Toggle:
                type = 0;
                break;
            case MenuItemKind::Keybind:
                type = 5;
                break;
            case MenuItemKind::Cycle:
                type = (child.choices.size() >= 2 && child.getChoice && child.setChoice) ? 3 : 5;
                if (child.choices.size() == 1) {
                    static std::atomic<bool> told{false};
                    if (!told.exchange(true)) {
                        log().warn(L"UiProbe: a choice has only one option - showing it as a "
                                   L"button instead");
                    }
                }
                break;
            case MenuItemKind::Number:
                type = 2;
                break;
            case MenuItemKind::Text:
                if (!child.getText || !child.setText) {
                    continue;
                }
                type = 4;
                break;
            case MenuItemKind::Action:
                if (!child.opensPage) {
                    continue;
                }
                type = 5;
                break;
            default:
                continue;
            }
            if (kVanillaKeyRows && child.kind == MenuItemKind::Keybind
                && count + 6 <= kMaxOwnItems) {
                const int rowNo = g_keyRowCount++;
                char rowId[kOwnItemIdMax]{};
                std::snprintf(rowId, sizeof(rowId), kKeyRowIdFormat, rowNo);

                if (useOwnKeyGroups()) {
                    bool already = false;
                    for (size_t k = 0; k < count && !already; ++k) {
                        already = (g_ownItems[k].child == 9999
                                   && g_ownItems[k].module == static_cast<int>(i));
                    }
                    const char* const groupId = oreui::ownGroupId(static_cast<int>(i));
                    if (groupId == nullptr) {
                        static std::atomic<bool> told{false};
                        if (!told.exchange(true)) {
                            log().warn(L"UiProbe: not enough keys for the key-row container "
                                       L"({} available, {} modules). Raise kOwnIds in "
                                       L"OreUiPatch.cpp",
                                       oreui::ownGroupIdCount(),
                                       g_ownMenuTree.size());
                        }
                    }
                    if (!already && groupId != nullptr) {
                        OwnItem& holder = g_ownItems[count++];
                        std::snprintf(holder.id, sizeof(holder.id), "%s", groupId);
                        std::snprintf(holder.nameKey, sizeof(holder.nameKey), "tk.kh%zu", i);
                        copyAscii(holder.text, sizeof(holder.text), L"Keys");
                        holder.type = kCompTypeGroupInfo;
                        holder.module = static_cast<int>(i);
                        holder.child = 9999;
                        holder.comp = nullptr;
                    }
                }

                std::wstring rowName;
                if (useOwnKeyGroups()) {
                    rowName = child.labelText();
                } else {
                    rowName = tabSource.labelText();
                    if (keybindsIn(tabSource) > 1) {
                        rowName += L" ";
                        rowName += child.labelText();
                    }
                }
                OwnItem& keyRow = g_ownItems[count++];
                std::snprintf(keyRow.id, sizeof(keyRow.id), "%s", rowId);
                std::snprintf(keyRow.nameKey, sizeof(keyRow.nameKey), "%s", rowId);
                copyAscii(keyRow.text, sizeof(keyRow.text), rowName.c_str());
                keyRow.type = kCompTypeGroupInfo;
                keyRow.module = static_cast<int>(i);
                keyRow.child = static_cast<int>(c);
                keyRow.comp = nullptr;

                const struct {
                    const char* suffix;
                    unsigned char type;
                } kParts[] = {{".bind", 5}, {".reset", 5}, {".captureState", 0}};
                for (const auto& part : kParts) {
                    OwnItem& one = g_ownItems[count++];
                    std::snprintf(one.id, sizeof(one.id), "%s%s", rowId, part.suffix);
                    std::snprintf(one.nameKey, sizeof(one.nameKey), "%s", one.id);
                    copyAscii(one.text, sizeof(one.text), rowName.c_str());
                    one.type = part.type;
                    one.module = static_cast<int>(i);
                    one.child = static_cast<int>(c);
                    one.comp = nullptr;
                }
                continue;
            }
            OwnItem& row = g_ownItems[count];
            std::snprintf(row.id, sizeof(row.id), "tk.m%zu.%zu", i, c);
            std::snprintf(row.nameKey, sizeof(row.nameKey), "tk.m%zu.%zu", i, c);
            copyAscii(row.text, sizeof(row.text), child.labelText().c_str());
            row.type = type;
            row.module = static_cast<int>(i);
            row.child = static_cast<int>(c);
            row.comp = nullptr;
            ++count;
        }
    }
    if (kVanillaKeyRows && !useOwnKeyGroups() && g_keyRowCount > 0
        && count + 2 <= kMaxOwnItems) {
        g_keyRowMade = true;
        OwnItem& tab = g_ownItems[count++];
        std::snprintf(tab.id, sizeof(tab.id), "tk.k");
        std::snprintf(tab.nameKey, sizeof(tab.nameKey), "%s", tab.id);
        copyAscii(tab.text, sizeof(tab.text), L"Keys");
        tab.type = kCompTypeGroupInfo;
        tab.module = kKeysModule;
        tab.child = -1;
        tab.comp = nullptr;

        OwnItem& holder = g_ownItems[count++];
        std::snprintf(holder.id, sizeof(holder.id), "%s", kBorrowedKeyGroupId);
        std::snprintf(holder.nameKey, sizeof(holder.nameKey), "%s", kBorrowedKeyNameKey);
        copyAscii(holder.text, sizeof(holder.text), L"Key bindings");
        holder.type = kCompTypeGroupInfo;
        holder.module = kKeysModule;
        holder.child = 9999;
        holder.comp = nullptr;
    }
    if (kOwnJsonTabEntry && count + 2 <= kMaxOwnItems) {
        OwnItem& tab = g_ownItems[count++];
        std::snprintf(tab.id, sizeof(tab.id), "%s", kOwnTabId);
        std::snprintf(tab.nameKey, sizeof(tab.nameKey), "%s", kOwnTabNameKey);
        copyAscii(tab.text, sizeof(tab.text), L"Schematica");
        tab.type = kCompTypeGroupInfo;
        tab.module = kJsonTabModule;
        tab.child = -1;
        tab.comp = nullptr;

        OwnItem& nav = g_ownItems[count++];
        std::snprintf(nav.id, sizeof(nav.id), "%s", kOwnNavId);
        std::snprintf(nav.nameKey, sizeof(nav.nameKey), "%s", kOwnTabNameKey);
        copyAscii(nav.text, sizeof(nav.text), L"Schematica");
        nav.type = 5;
        nav.module = kJsonTabModule;
        nav.child = 0;
        nav.comp = nullptr;
    }
    g_ownItemCount.store(count);

    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        log().info(L"UiProbe: built {} own item(s) (limit {})", count, kMaxOwnItems);
    }
    if (count >= kMaxOwnItems) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            log().warn(L"UiProbe: own items hit the limit ({}) - the tree is cut short",
                       kMaxOwnItems);
        }
    }
}

std::uintptr_t g_ownViewPool[kMaxOwnProviders][2]{};

int findOwnItem(const char* id)
{
    const size_t count = g_ownItemCount.load();
    for (size_t i = 0; i < count && i < kMaxOwnItems; ++i) {
        if (std::strcmp(g_ownItems[i].id, id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::atomic<void*> g_donorByType[16]{};

struct DonorTemplate {
    unsigned char bytes[kCompSize];
    std::uintptr_t base;
    bool ready;
};
DonorTemplate g_donorTemplate[16]{};

void dumpComponentCallables(const unsigned char* comp, std::uintptr_t base, int kind)
{
    for (std::ptrdiff_t off = 0; off + 0x40 <= static_cast<std::ptrdiff_t>(kCompSize); off += 8) {
        std::uintptr_t ptr = 0;
        std::memcpy(&ptr, comp + off + 0x38, sizeof(ptr));
        if (ptr == 0 || ptr != base + static_cast<std::uintptr_t>(off)) {
            continue;
        }
        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, comp + off, sizeof(vtable));
        std::uintptr_t doCall = 0;
        if (vtable != 0 && memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 5)) {
            std::memcpy(&doCall, reinterpret_cast<const void*>(vtable + 0x10), sizeof(doCall));
        }
    }
}

void dumpComponentStrings(const unsigned char* comp, int kind)
{
    for (std::ptrdiff_t off = 0; off + 0x20 <= kCompSize; off += 8) {
        std::uintptr_t size = 0;
        std::uintptr_t room = 0;
        std::memcpy(&size, comp + off + 0x10, sizeof(size));
        std::memcpy(&room, comp + off + 0x18, sizeof(room));
        if (room < 15 || room > 0x400 || size > room) {
            continue;
        }
        const char* text = nullptr;
        if (room == 15) {
            text = reinterpret_cast<const char*>(comp + off);
        } else {
            std::uintptr_t ptr = 0;
            std::memcpy(&ptr, comp + off, sizeof(ptr));
            if (ptr == 0 || !memory::isReadable(reinterpret_cast<const void*>(ptr), size + 1)) {
                continue;
            }
            text = reinterpret_cast<const char*>(ptr);
        }
        if (size == 0) {
            continue;
        }
        bool printable = true;
        for (size_t i = 0; i < size; ++i) {
            const auto ch = static_cast<unsigned char>(text[i]);
            if (ch < 0x20 || ch > 0x7E) {
                printable = false;
                break;
            }
        }
        if (!printable) {
            continue;
        }
        const std::string body(text, size);
    }
}

void dumpTextDonor(const unsigned char* comp)
{
    if (comp == nullptr || !memory::isReadable(comp, kCompSize)) {
        return;
    }
    log().info(L"UiProbe: --- text (kind 4) donor at {} ---", static_cast<const void*>(comp));
    for (std::ptrdiff_t off = 0x1D0; off + 8 <= kCompType; off += 8) {
        std::uintptr_t word = 0;
        std::memcpy(&word, comp + off, sizeof(word));
        if (word == 0) {
            continue;
        }
        std::wstring note;
        if (off + 0x20 <= kCompType) {
            std::uintptr_t size = 0;
            std::uintptr_t room = 0;
            std::memcpy(&size, comp + off + kStringSize, sizeof(size));
            std::memcpy(&room, comp + off + kStringCapacity, sizeof(room));
            if (room >= kSsoCapacity && room <= 0x4000 && size <= room && size > 0) {
                const char* text = (room == kSsoCapacity)
                                       ? reinterpret_cast<const char*>(comp + off)
                                       : reinterpret_cast<const char*>(word);
                if (text != nullptr && memory::isReadable(text, size + 1)) {
                    bool printable = true;
                    for (size_t i = 0; i < size && printable; ++i) {
                        const auto ch = static_cast<unsigned char>(text[i]);
                        printable = (ch >= 0x20 && ch <= 0x7E);
                    }
                    if (printable) {
                        const std::string body(text, size);
                        note = std::format(L" string({}) \"{}\"",
                                           (room == kSsoCapacity) ? L"sso" : L"heap",
                                           std::wstring(body.begin(), body.end()));
                    }
                }
            }
        }
        float asFloat = 0.0f;
        std::memcpy(&asFloat, comp + off, sizeof(asFloat));
        log().info(L"UiProbe:   +{:#05x} = {:#x} (float {}){}", static_cast<size_t>(off), word,
                   asFloat, note);
    }
    std::uintptr_t provider = 0;
    std::memcpy(&provider, comp + kCompBoolProvider, sizeof(provider));
    if (provider == 0 || !memory::isReadable(reinterpret_cast<const void*>(provider), 0xA0)) {
        log().warn(L"UiProbe:   the provider at +0x1E0 is not readable ({:#x})", provider);
        return;
    }
    const auto* const p = reinterpret_cast<const unsigned char*>(provider);
    std::uintptr_t notify = 0;
    std::uintptr_t source = 0;
    std::uintptr_t length = 0;
    std::uintptr_t room = 0;
    std::memcpy(&notify, p + kTextProviderNotify, sizeof(notify));
    std::memcpy(&source, p + kTextProviderSource, sizeof(source));
    std::memcpy(&length, p + kTextProviderDraft + kStringSize, sizeof(length));
    std::memcpy(&room, p + kTextProviderDraft + kStringCapacity, sizeof(room));
    log().info(L"UiProbe:   provider {:#x} notify={:#x} source={:#x} draft(len {} cap {}) has={}",
               provider, notify, source, length, room,
               static_cast<int>(p[kTextProviderHasDraft]));
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, p, sizeof(vtable));
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (vtable != 0 && memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 10)) {
        for (int i = 0; i < 10; ++i) {
            std::uintptr_t slot = 0;
            std::memcpy(&slot, reinterpret_cast<const char*>(vtable) + i * 8, sizeof(slot));
            log().info(L"UiProbe:   vtable[{}] = RVA {:#x}", i,
                       (slot > base) ? (slot - base) : slot);
        }
    }
}

void dumpNavComponent(const unsigned char* comp, const std::string& id)
{
    if (comp == nullptr || !memory::isReadable(comp, kCompSize)) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    log().info(L"UiProbe: === json-navigation component \"{}\" at {} (kind {}) ===",
               toUtf16(id), static_cast<const void*>(comp), comp[kCompType]);
    for (std::ptrdiff_t off = 0; off + 0x20 <= kCompSize; off += 8) {
        std::uintptr_t size = 0;
        std::uintptr_t room = 0;
        std::memcpy(&size, comp + off + kStringSize, sizeof(size));
        std::memcpy(&room, comp + off + kStringCapacity, sizeof(room));
        if (room < kSsoCapacity || room > 0x400 || size == 0 || size > room) {
            continue;
        }
        const char* text = reinterpret_cast<const char*>(comp + off);
        if (room > kSsoCapacity) {
            std::uintptr_t ptr = 0;
            std::memcpy(&ptr, comp + off, sizeof(ptr));
            if (ptr == 0 || !memory::isReadable(reinterpret_cast<const void*>(ptr), size + 1)) {
                continue;
            }
            text = reinterpret_cast<const char*>(ptr);
        }
        bool printable = true;
        for (size_t i = 0; i < size && printable; ++i) {
            const auto ch = static_cast<unsigned char>(text[i]);
            printable = (ch >= 0x20 && ch <= 0x7E);
        }
        if (!printable) {
            continue;
        }
        const std::string body(text, size);
        log().info(L"UiProbe:   +{:#05x} string \"{}\"", static_cast<size_t>(off),
                   toUtf16(body));
    }
    const auto self = reinterpret_cast<std::uintptr_t>(comp);
    for (std::ptrdiff_t off = 0; off + 0x40 <= static_cast<std::ptrdiff_t>(kCompSize); off += 8) {
        std::uintptr_t ptr = 0;
        std::memcpy(&ptr, comp + off + 0x38, sizeof(ptr));
        if (ptr == 0 || ptr != self + static_cast<std::uintptr_t>(off)) {
            continue;
        }
        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, comp + off, sizeof(vtable));
        std::uintptr_t doCall = 0;
        if (vtable != 0 && memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 5)) {
            std::memcpy(&doCall, reinterpret_cast<const char*>(vtable) + 0x10, sizeof(doCall));
        }
        std::uintptr_t cap0 = 0;
        std::uintptr_t cap1 = 0;
        std::uintptr_t cap2 = 0;
        std::memcpy(&cap0, comp + off + 0x08, sizeof(cap0));
        std::memcpy(&cap1, comp + off + 0x10, sizeof(cap1));
        std::memcpy(&cap2, comp + off + 0x18, sizeof(cap2));
        log().info(L"UiProbe:   +{:#05x} function _Do_call RVA {:#x} caps {:#x} {:#x} {:#x}",
                   static_cast<size_t>(off), (doCall > base) ? (doCall - base) : doCall, cap0,
                   cap1, cap2);
    }
    for (std::ptrdiff_t off = 0x1D0; off + 8 <= kCompSize; off += 8) {
        std::uintptr_t word = 0;
        std::memcpy(&word, comp + off, sizeof(word));
        if (word == 0) {
            continue;
        }
        log().info(L"UiProbe:   +{:#05x} = {:#x}{}", static_cast<size_t>(off), word,
                   (word > base && word < base + 0x13000000)
                       ? std::format(L" (module RVA {:#x})", word - base)
                       : std::wstring{});
    }
}

void dumpComponentVtable(const void* comp, int kind)
{
    if (comp == nullptr || !memory::isReadable(comp, 8)) {
        return;
    }
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, comp, sizeof(vtable));
    if (vtable == 0 || !memory::isReadable(reinterpret_cast<const void*>(vtable), 8 * 32)) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::wstring line;
    for (int i = 0; i < 32; ++i) {
        std::uintptr_t slot = 0;
        std::memcpy(&slot, reinterpret_cast<const char*>(vtable) + i * 8, sizeof(slot));
        line += std::format(L"{:#x} ", (slot > base) ? (slot - base) : slot);
    }
    log().info(L"UiProbe: component vtable for kind {} ({:#x}): {}", kind,
               (vtable > base) ? (vtable - base) : vtable, line);
}

void dumpOptionList(const unsigned char* comp)
{
    std::uintptr_t provider = 0;
    std::memcpy(&provider, comp + kCompBoolProvider, sizeof(provider));
    if (provider == 0 || !memory::isReadable(reinterpret_cast<const void*>(provider), 0xE8)) {
        return;
    }
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, reinterpret_cast<const char*>(provider) + 0xD8, sizeof(begin));
    std::memcpy(&end, reinterpret_cast<const char*>(provider) + 0xE0, sizeof(end));
    if (begin == 0 || end < begin || (end - begin) % kOptionStride != 0) {
        log().warn(L"UiProbe: cannot read the option list (begin {:#x} end {:#x})", begin, end);
        return;
    }
    const size_t count = (end - begin) / kOptionStride;

    if (!g_optionDonor.ready && count > 0
        && memory::isReadable(reinterpret_cast<const void*>(begin), kOptionStride)) {
        std::memcpy(g_optionDonor.element, reinterpret_cast<const void*>(begin), kOptionStride);
        std::uintptr_t labelPtr = 0;
        std::memcpy(&labelPtr, g_optionDonor.element + kOptionLabelPtr, sizeof(labelPtr));
        if (labelPtr != 0
            && memory::isReadable(reinterpret_cast<const void*>(labelPtr), kOptionLabelCopy)) {
            std::memcpy(g_optionDonor.label, reinterpret_cast<const void*>(labelPtr),
                        kOptionLabelCopy);
            g_optionDonor.labelBase = labelPtr;
            g_optionDonor.ready = true;
            std::uintptr_t labelVtable = 0;
            std::memcpy(&labelVtable, g_optionDonor.label, sizeof(labelVtable));
        }
    }
    for (size_t i = 0; i < count && i < 3; ++i) {
        const auto* const one = reinterpret_cast<const unsigned char*>(begin + i * kOptionStride);
        if (!memory::isReadable(one, kOptionStride)) {
            break;
        }
        for (std::ptrdiff_t off = 0; off + 8 <= static_cast<std::ptrdiff_t>(kOptionStride);
             off += 8) {
            std::uintptr_t word = 0;
            std::memcpy(&word, one + off, sizeof(word));
            std::string note;
            if (off + 0x20 <= static_cast<std::ptrdiff_t>(kOptionStride)) {
                std::uintptr_t size = 0;
                std::uintptr_t room = 0;
                std::memcpy(&size, one + off + 0x10, sizeof(size));
                std::memcpy(&room, one + off + 0x18, sizeof(room));
                if (size > 0 && room >= 15 && room <= 0x400 && size <= room) {
                    const char* text = (room == 15) ? reinterpret_cast<const char*>(one + off)
                                                    : reinterpret_cast<const char*>(word);
                    if (room == 15 || (word != 0 && memory::isReadable(text, size + 1))) {
                        bool printable = true;
                        for (size_t c = 0; c < size; ++c) {
                            const auto ch = static_cast<unsigned char>(text[c]);
                            if (ch < 0x20 || ch > 0x7E) {
                                printable = false;
                                break;
                            }
                        }
                        if (printable) {
                            note = " <= \"" + std::string(text, size) + "\"";
                        }
                    }
                }
            }
            const std::wstring wide(note.begin(), note.end());
        }
    }
}

void dumpComponentPointers(const unsigned char* comp, std::uintptr_t base, int kind)
{
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (std::ptrdiff_t off = 0; off + 8 <= kCompSize; off += 8) {
        std::uintptr_t ptr = 0;
        std::memcpy(&ptr, comp + off, sizeof(ptr));
        if (ptr == 0 || ptr == base + static_cast<std::uintptr_t>(off)) {
            continue;
        }
        if (!memory::isReadable(reinterpret_cast<const void*>(ptr), sizeof(std::uintptr_t) * 8)) {
            continue;
        }
        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<const void*>(ptr), sizeof(vtable));
        if (moduleBase == 0 || vtable <= moduleBase || vtable >= moduleBase + 0x13000000) {
            continue;
        }
        std::uintptr_t slot[6]{};
        if (memory::isReadable(reinterpret_cast<const void*>(vtable), sizeof(slot))) {
            std::memcpy(slot, reinterpret_cast<const void*>(vtable), sizeof(slot));
        }
    }
}

void* findDonorByType(unsigned char type, const char* const* candidates, size_t count)
{
    void* const registry = g_settingsRegistry.load();
    if (registry == nullptr) {
        return (type < 16) ? g_donorByType[type].load() : nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        std::uintptr_t view[2] = {reinterpret_cast<std::uintptr_t>(candidates[i]),
                                  std::strlen(candidates[i])};
        void* const found = hooks::callSettingsFindComponent(registry, view);
        if (found != nullptr && memory::isReadable(found, kCompType + 1)
            && static_cast<const unsigned char*>(found)[kCompType] == type) {
            return found;
        }
    }
    void* const warm = (type < 16) ? g_donorByType[type].load() : nullptr;
    if (warm == nullptr) {
        return nullptr;
    }
    if (!memory::isReadable(warm, kCompType + 1)
        || static_cast<const unsigned char*>(warm)[kCompType] != type) {
        g_donorByType[type].store(nullptr);
        return nullptr;
    }
    return warm;
}

void* findBooleanDonor()
{
    static const char* const kCandidates[] = {
        "accessibility.tts_enabled",
        "accessibility.tts_enable_ui",
        "accessibility.open_chat_message",
        "accessibility.gameplay.subtitles.enable",
    };
    return findDonorByType(0, kCandidates, std::size(kCandidates));
}

void* findOptionDonor()
{
    static const char* const kCandidates[] = {
        "accessibility.chat_message_duration",
        "accessibility.toast_notification_duration",
        "accessibility.ui_scale_modifier",
    };
    return findDonorByType(3, kCandidates, std::size(kCandidates));
}

void* findNumberDonor()
{
    static const char* const kCandidates[] = {
        "accessibility.tts_volume",
        "accessibility.darkness_effect_strength",
        "accessibility.screen_distortion",
        "accessibility.glint_strength",
    };
    return findDonorByType(2, kCandidates, std::size(kCandidates));
}

std::string readCompId(const void* comp)
{
    if (comp == nullptr || !memory::isReadable(comp, kCompId + 0x20)) {
        return {};
    }
    const auto* const slot = static_cast<const char*>(comp) + kCompId;
    std::uintptr_t size = 0;
    std::uintptr_t room = 0;
    std::memcpy(&size, slot + kStringSize, sizeof(size));
    std::memcpy(&room, slot + kStringCapacity, sizeof(room));
    if (size == 0 || size > 200 || room < size) {
        return {};
    }
    const char* text = slot;
    if (room > kSsoCapacity) {
        std::uintptr_t ptr = 0;
        std::memcpy(&ptr, slot, sizeof(ptr));
        if (ptr == 0 || !memory::isReadable(reinterpret_cast<const void*>(ptr), size)) {
            return {};
        }
        text = reinterpret_cast<const char*>(ptr);
    }
    std::string out(text, size);
    for (const char ch : out) {
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E) {
            return {};
        }
    }
    return out;
}

void dumpNavComponent(const unsigned char* comp, const std::string& id);

void dumpSettingsRegistry(const wchar_t* why)
{
    void* const registry = g_settingsRegistry.load();
    if (registry == nullptr || !memory::isReadable(registry, 0x58)) {
        log().warn(L"UiProbe: registry census ({}) - no registry", why);
        return;
    }
    std::uintptr_t head = 0;
    std::memcpy(&head, static_cast<const char*>(registry) + 0x50, sizeof(head));
    if (head == 0 || !memory::isReadable(reinterpret_cast<const void*>(head), 0x40)) {
        log().warn(L"UiProbe: registry census ({}) - the list head is not readable", why);
        return;
    }
    size_t counts[16]{};
    size_t nodes = 0;
    size_t comps = 0;
    std::string textIds;
    std::string navIds;
    std::uintptr_t node = 0;
    std::memcpy(&node, reinterpret_cast<const void*>(head), sizeof(node));
    while (node != 0 && node != head && nodes < 4096
           && memory::isReadable(reinterpret_cast<const void*>(node), 0x40)) {
        ++nodes;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::memcpy(&begin, reinterpret_cast<const char*>(node) + 0x30, sizeof(begin));
        std::memcpy(&end, reinterpret_cast<const char*>(node) + 0x38, sizeof(end));
        for (std::uintptr_t at = begin; at + 8 <= end && at - begin < 0x8000; at += 8) {
            if (!memory::isReadable(reinterpret_cast<const void*>(at), 8)) {
                break;
            }
            std::uintptr_t comp = 0;
            std::memcpy(&comp, reinterpret_cast<const void*>(at), sizeof(comp));
            if (comp == 0 || !memory::isReadable(reinterpret_cast<const void*>(comp), kCompSize)) {
                continue;
            }
            ++comps;
            const auto kind = reinterpret_cast<const unsigned char*>(comp)[kCompType];
            if (kind < 16) {
                ++counts[kind];
            }
            if (kind == 4 && textIds.size() < 600) {
                textIds += readCompId(reinterpret_cast<const void*>(comp));
                textIds += ' ';
            }
            {
                const std::string one = readCompId(reinterpret_cast<const void*>(comp));
                if (one.rfind("json-navigation-", 0) == 0) {
                    if (navIds.size() < 600) {
                        navIds += one;
                        navIds += ' ';
                    }
                    static std::atomic<int> shown{0};
                    if (shown.fetch_add(1) < 2) {
                        dumpNavComponent(reinterpret_cast<const unsigned char*>(comp), one);
                    }
                }
            }
        }
        std::memcpy(&node, reinterpret_cast<const void*>(node), sizeof(node));
    }
    std::wstring tally;
    for (size_t k = 0; k < 16; ++k) {
        if (counts[k] != 0) {
            tally += std::format(L"{}:{} ", k, counts[k]);
        }
    }
    log().info(L"UiProbe: registry census ({}) - {} node(s), {} component(s), {}", why, nodes,
               comps, tally);
    if (!navIds.empty()) {
        log().info(L"UiProbe:   json-navigation ids: {}", toUtf16(navIds));
    }
    if (!textIds.empty()) {
        log().info(L"UiProbe:   kind 4 ids: {}", toUtf16(textIds));
    }
}

void* findTextDonorPreferred()
{
    static const char* const kCandidates[] = {
        "game.general.worldName",
        "game.worldPreferences.worldSeed.string",
        "game.cheats.randomTickSpeed.string",
        "game.worldOptions.respawnRadius",
    };
    void* const registry = g_settingsRegistry.load();
    if (registry == nullptr) {
        return nullptr;
    }
    for (const char* const candidate : kCandidates) {
        std::uintptr_t view[2] = {reinterpret_cast<std::uintptr_t>(candidate),
                                  std::strlen(candidate)};
        void* const found = hooks::callSettingsFindComponent(registry, view);
        if (found != nullptr && memory::isReadable(found, kCompType + 1)
            && static_cast<const unsigned char*>(found)[kCompType] == 4) {
            return found;
        }
    }
    return nullptr;
}

void* findTextDonor()
{
    if (void* const good = findTextDonorPreferred(); good != nullptr) {
        return good;
    }
    static const char* const kCandidates[] = {
        "account.change_name",
    };
    return findDonorByType(4, kCandidates, std::size(kCandidates));
}

std::atomic<void*> g_ownGroupDonor{nullptr};

void* findActionDonor();
void* findTextDonorPreferred();
void* findKeyResetDonor();
void* findRegistryNode(void* registry, const std::wstring& id);
bool callStdFunctionInto(void* fn, void* out);

bool ensureOwnItem(size_t index)
{
    if (index >= g_ownItemCount.load() || index >= kMaxOwnItems) {
        return false;
    }
    OwnItem& item = g_ownItems[index];
    if (item.comp != nullptr) {
        if (item.type == 4 && item.weakBody && findTextDonorPreferred() != nullptr) {
            item.comp = nullptr;
            item.weakBody = false;
            log().info(L"UiProbe: rebuilding the text row {} with a better body",
                       toUtf16(item.id));
        } else {
            return true;
        }
    }
    const bool isGroup = (item.type == kCompTypeGroupInfo);
    void* donor = nullptr;
    void* providerDonor = nullptr;
    std::uintptr_t donorBase = 0;
    if (isGroup) {
        donor = g_ownGroupDonor.load();
    } else if (item.type == 2) {
        donor = findNumberDonor();
        providerDonor = donor;
    } else if (item.type == 5) {
        donor = findActionDonor();
        providerDonor = donor;
    } else if (item.type == 4) {
        donor = findTextDonorPreferred();
        item.weakBody = (donor == nullptr);
        if (donor == nullptr) {
            donor = findTextDonor();
        }
        providerDonor = nullptr;
        if (donor == nullptr) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) {
                log().warn(L"UiProbe: no text donor for {} - the row will not appear",
                           toUtf16(item.id));
                dumpSettingsRegistry(L"no text donor");
            }
        }
    } else if (item.type == 3) {
        donor = findOptionDonor();
        providerDonor = donor;
    } else {
        donor = findBooleanDonor();
        providerDonor = donor;
    }
    if (donor == nullptr && item.type == 5 && g_donorTemplate[5].ready) {
        donor = g_donorTemplate[5].bytes;
        providerDonor = donor;
        donorBase = g_donorTemplate[5].base;
    }
    if (donor == nullptr && item.type == 4 && g_donorTemplate[4].ready) {
        donor = g_donorTemplate[4].bytes;
        donorBase = g_donorTemplate[4].base;
    }
    if (donor == nullptr) {
        g_ownItemsIncomplete.store(true);
        return false;
    }
    auto* const comp = static_cast<unsigned char*>(hooks::callGameAllocate(kCompSize));
    if (comp == nullptr) {
        return false;
    }
    void* provider = nullptr;
    if (item.type == 4) {
        if (!prepareOwnTextVtable()) {
            return false;
        }
        void* const own = hooks::callGameAllocate(kBoolProviderSize);
        if (own == nullptr) {
            return false;
        }
        std::memset(own, 0, kBoolProviderSize);
        const auto vtable = reinterpret_cast<std::uintptr_t>(&g_ownTextVtable[0]);
        std::memcpy(own, &vtable, sizeof(vtable));
        const size_t slot = g_providerLinkCount.load();
        if (slot >= kMaxOwnItems) {
            return false;
        }
        g_providerLinks[slot].provider = own;
        g_providerLinks[slot].index = static_cast<int>(index);
        g_providerLinkCount.store(slot + 1);
        provider = own;
    } else if (item.type == 0 || item.type == 2 || item.type == 3) {
        std::uintptr_t donorProvider = 0;
        std::memcpy(&donorProvider, static_cast<const char*>(providerDonor) + kCompBoolProvider,
                    sizeof(donorProvider));
        const bool isNumber = (item.type == 2);
        const bool isOption = (item.type == 3);
        const size_t providerSize = kBoolProviderSize;
        if (donorProvider == 0
            || !memory::isReadable(reinterpret_cast<const void*>(donorProvider), providerSize)) {
            return false;
        }
        bool vtableOk = false;
        if (isOption) {
            vtableOk = prepareOwnOptionVtable(reinterpret_cast<const void*>(donorProvider));
        } else if (isNumber) {
            vtableOk = prepareOwnNumberVtable(reinterpret_cast<const void*>(donorProvider));
        } else {
            vtableOk = prepareOwnBoolVtable(reinterpret_cast<const void*>(donorProvider));
        }
        if (!vtableOk) {
            return false;
        }
        void* const own = hooks::callGameAllocate(providerSize);
        if (own == nullptr) {
            return false;
        }
        std::memcpy(own, reinterpret_cast<const void*>(donorProvider), providerSize);
        const auto vtable = reinterpret_cast<std::uintptr_t>(
            isOption ? &g_ownOptionVtable[0]
                     : (isNumber ? &g_ownNumberVtable[0] : &g_ownBoolVtable[0]));
        std::memcpy(own, &vtable, sizeof(vtable));
        if (isOption) {
            std::memset(static_cast<char*>(own) + kOptionProviderList, 0,
                        sizeof(std::uintptr_t) * 3);
        }
        const size_t slot = g_providerLinkCount.load();
        if (slot >= kMaxOwnItems) {
            return false;
        }
        g_providerLinks[slot].provider = own;
        g_providerLinks[slot].index = static_cast<int>(index);
        g_providerLinkCount.store(slot + 1);
        provider = own;
    }
    const size_t idLength = std::strlen(item.id);
    if (!buildOwnComponent(comp, donor, item.id, idLength, item.nameKey,
                           std::strlen(item.nameKey), item.type, provider, donorBase)) {
        return false;
    }
    if (item.type == 2) {
        const float scale = ownItemNumberScale(static_cast<int>(index));
        std::memcpy(comp + 0x1E8, &scale, sizeof(scale));
    }
    if (item.type == 3) {
        if (!g_optionDonor.ready) {
            dumpOptionList(static_cast<const unsigned char*>(donor));
        }
        const MenuItem* const source = ownMenuItem(static_cast<int>(index));
        if (source == nullptr || !buildOwnOptionSet(static_cast<int>(index), *source, item.id)) {
            log().warn(L"UiProbe: could not build the option list for item {} (mold {})", index,
                       g_optionDonor.ready ? L"yes" : L"no");
            return false;
        }
    }
    item.comp = comp;
    {
        static std::atomic<int> said{0};
    }
    return true;
}

void* findKeyResetDonor()
{
    void* const registry = g_settingsRegistry.load();
    if (registry == nullptr) {
        return nullptr;
    }
    static const char* const kCandidates[] = {
        "keyboardAndMouse.inputGroup.standard.key.attack.reset",
        "keyboardAndMouse.inputGroup.full.key.attack.reset",
        "keyboardAndMouse.inputGroup.standard.key.jump.reset",
    };
    if (void* const warm = g_keyResetDonor.load();
        warm != nullptr && memory::isReadable(warm, kCompSize)
        && static_cast<const unsigned char*>(warm)[kCompType] == 5) {
        return warm;
    }
    const auto lookup = [&](const char* candidate) -> void* {
        std::uintptr_t view[2] = {reinterpret_cast<std::uintptr_t>(candidate),
                                  std::strlen(candidate)};
        void* const found = hooks::callSettingsFindComponent(registry, view);
        if (found != nullptr && memory::isReadable(found, kCompSize)
            && static_cast<const unsigned char*>(found)[kCompType] == 5) {
            return found;
        }
        return nullptr;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const char* candidate : kCandidates) {
            if (void* const found = lookup(candidate); found != nullptr) {
                static std::atomic<bool> said{false};
                return found;
            }
        }
        if (pass != 0) {
            break;
        }
        static const wchar_t* const kGroups[] = {
            L"keyboardAndMouse.inputGroup.standard.key.attack",
            L"keyboardAndMouse.inputGroup.full.key.attack",
            L"keyboardAndMouse.inputGroup.standard.key.jump",
        };
        break;
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        log().warn(L"UiProbe: could not capture the reset donor for key rows");
        if (void* const head = findRegistryNode(registry, L"settings-tabs-groups");
            head != nullptr) {
            void* node = head;
            int seen = 0;
            int hits = 0;
            for (int step = 0; step < 4096; ++step) {
                std::uintptr_t next = 0;
                std::memcpy(&next, node, sizeof(next));
                if (next == 0
                    || !memory::isReadable(reinterpret_cast<const void*>(next), 0x70)) {
                    break;
                }
                node = reinterpret_cast<void*>(next);
                if (node == head) {
                    break;
                }
                ++seen;
                const std::wstring name = readString(static_cast<const char*>(node) + 0x10);
                if (name.rfind(L"keyboardAndMouse", 0) == 0) {
                    ++hits;
                }
            }
        } else {
            log().warn(L"UiProbe: cannot reach the registry node");
        }
    }
    return nullptr;
}

void* findActionDonor()
{
    void* const registry = g_settingsRegistry.load();
    if (registry == nullptr) {
        return nullptr;
    }
    static const char* const kCandidates[] = {
        "accessibility.reset",
        "general.restartTutorial",
        "controller.reset",
    };
    for (const char* candidate : kCandidates) {
        std::uintptr_t view[2] = {reinterpret_cast<std::uintptr_t>(candidate),
                                  std::strlen(candidate)};
        void* const found = hooks::callSettingsFindComponent(registry, view);
        if (found != nullptr && memory::isReadable(found, kCompSize)
            && static_cast<const unsigned char*>(found)[kCompType] == 5) {
            return found;
        }
    }
    return nullptr;
}

void rebuildOwnComponents(void* groupDonor)
{
    buildOwnItems();
    g_ownCompsReady.store(false);
    g_ownSectionComp.store(nullptr);
    g_ownTabComp.store(nullptr);
    g_ownGroupDonor.store(groupDonor);
    g_providerLinkCount.store(0);
    g_ownOptionSetCount.store(0);
    for (size_t w = 0; w < kPublishWords; ++w) {
        g_publishMask[w].store(0);
    }
    for (auto& one : g_ownPending) {
        one.has.store(false);
    }
    {
        const std::lock_guard<std::mutex> lock(g_ownPendingTextMutex);
        for (auto& one : g_ownPendingText) {
            one.has = false;
        }
    }
    for (auto& slot : g_donorByType) {
        slot.store(nullptr);
    }

    auto* const section = static_cast<unsigned char*>(hooks::callGameAllocate(kCompSize));
    const bool sectionOk =
        (section != nullptr)
        && buildOwnComponent(section, groupDonor, kOwnGroupIdBytes, sizeof(kOwnGroupIdBytes) - 1,
                             kOwnSettingsNameKey, sizeof(kOwnSettingsNameKey) - 1);
    g_ownSectionComp.store(sectionOk ? section : nullptr);

    const size_t items = g_ownItemCount.load();
    int madeTabs = 0;
    for (size_t i = 0; i < items; ++i) {
        g_ownItems[i].comp = nullptr;
        g_ownItems[i].weakBody = false;
    }
    for (size_t i = 0; i < items; ++i) {
        if (g_ownItems[i].type == kCompTypeGroupInfo && ensureOwnItem(i)) {
            ++madeTabs;
        }
    }
    g_ownCompsReady.store(sectionOk && madeTabs > 0);
}

void fillOwnItemsByPrefix(void* out, const std::uintptr_t (&vec)[3], size_t count,
                          const std::wstring& prefix, bool exact, int onlyModule = -1)
{
    (void)count;
    void* picked[kMaxOwnItems]{};
    size_t picks = 0;
    const size_t items = g_ownItemCount.load();
    std::string want(prefix.begin(), prefix.end());
    for (size_t i = 0; i < items && picks < kMaxOwnItems; ++i) {
        const char* const id = g_ownItems[i].id;
        if (std::strncmp(id, want.c_str(), want.size()) != 0) {
            continue;
        }
        if (!exact && std::strchr(id + want.size(), '.') != nullptr) {
            continue;
        }
        if (onlyModule >= 0 && g_ownItems[i].module != onlyModule) {
            continue;
        }
        if (ensureOwnItem(i) && g_ownItems[i].comp != nullptr) {
            picked[picks++] = g_ownItems[i].comp;
        }
    }
    if (picks == 0) {
        std::memcpy(static_cast<char*>(out) + 0x08, &vec[0], sizeof(vec[0]));
        return;
    }
    const size_t room = static_cast<size_t>((vec[2] - vec[0]) / 8);
    std::uintptr_t begin = vec[0];
    if (picks > room) {
        void* const buf = hooks::callGameAllocate(picks * 8);
        if (buf == nullptr) {
            return;
        }
        begin = reinterpret_cast<std::uintptr_t>(buf);
        const std::uintptr_t cap = begin + picks * 8;
        std::memcpy(static_cast<char*>(out) + 0x00, &begin, sizeof(begin));
        std::memcpy(static_cast<char*>(out) + 0x10, &cap, sizeof(cap));
    }
    for (size_t i = 0; i < picks; ++i) {
        const auto value = reinterpret_cast<std::uintptr_t>(picked[i]);
        std::memcpy(reinterpret_cast<void*>(begin + i * 8), &value, sizeof(value));
    }
    const std::uintptr_t end = begin + picks * 8;
    std::memcpy(static_cast<char*>(out) + 0x08, &end, sizeof(end));
}

void fillOwnGroup(void* out, const std::uintptr_t (&vec)[3], size_t count, int owner)
{
    (void)count;
    void* picked[kMaxOwnItems]{};
    size_t picks = 0;
    const size_t items = g_ownItemCount.load();
    if (owner < 0) {
        for (size_t i = 0; i < items && picks < kMaxOwnItems; ++i) {
            if (g_ownItems[i].module == kJsonTabModule) {
                continue;
            }
            if (g_ownItems[i].type == kCompTypeGroupInfo && g_ownItems[i].child < 0
                && g_ownItems[i].comp != nullptr) {
                picked[picks++] = g_ownItems[i].comp;
            }
        }
    } else {
        const int module = g_ownItems[static_cast<size_t>(owner)].module;
        for (size_t i = 0; i < items && picks < kMaxOwnItems; ++i) {
            const bool isTabItself = (g_ownItems[i].child < 0);
            const bool isOwnRow =
                (module == kKeysModule)
                    ? (g_ownItems[i].child == 9999)
                    : (std::strncmp(g_ownItems[i].id, "tk.m", 4) == 0
                       || std::strcmp(g_ownItems[i].id, kOwnNavId) == 0);
            if (g_ownItems[i].module == module && !isTabItself && isOwnRow) {
                if (ensureOwnItem(i) && g_ownItems[i].comp != nullptr) {
                    picked[picks++] = g_ownItems[i].comp;
                }
            }
        }
        if (useOwnKeyGroups()) {
            for (size_t i = 0; i < items && picks < kMaxOwnItems; ++i) {
                if (g_ownItems[i].child == 9999 && g_ownItems[i].module == module
                    && ensureOwnItem(i) && g_ownItems[i].comp != nullptr) {
                    picked[picks++] = g_ownItems[i].comp;
                    break;
                }
            }
        }
    }
    if (picks == 0) {
        std::memcpy(static_cast<char*>(out) + 0x08, &vec[0], sizeof(vec[0]));
        return;
    }
    const size_t room = static_cast<size_t>((vec[2] - vec[0]) / 8);
    std::uintptr_t begin = vec[0];
    if (picks > room) {
        void* const buf = hooks::callGameAllocate(picks * 8);
        if (buf == nullptr) {
            return;
        }
        begin = reinterpret_cast<std::uintptr_t>(buf);
        const std::uintptr_t cap = begin + picks * 8;
        std::memcpy(static_cast<char*>(out) + 0x00, &begin, sizeof(begin));
        std::memcpy(static_cast<char*>(out) + 0x10, &cap, sizeof(cap));
    }
    for (size_t i = 0; i < picks; ++i) {
        const auto value = reinterpret_cast<std::uintptr_t>(picked[i]);
        std::memcpy(reinterpret_cast<void*>(begin + i * 8), &value, sizeof(value));
    }
    const std::uintptr_t end = begin + picks * 8;
    std::memcpy(static_cast<char*>(out) + 0x08, &end, sizeof(end));
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 20) {
        std::string names;
        for (size_t i = 0; i < items && i < kMaxOwnItems; ++i) {
            for (size_t k = 0; k < picks; ++k) {
                if (g_ownItems[i].comp == picked[k]) {
                    names += g_ownItems[i].id;
                    names += ' ';
                }
            }
        }
    }
}

constexpr unsigned kOwnTabEnum = 0x27;

constexpr char kOwnScreenName[] = "tsukuyomi.schematica";
constexpr char kOwnTabDescKey[] = "tk.schematica.tab";

constexpr bool kAddOwnJsonTab = kOwnJsonTabEntry;

void afterSettingsTabList(void* out)
{
    if (!kAddOwnSettingsTab || out == nullptr || !memory::isWritable(out, 0x18)) {
        return;
    }
    std::uintptr_t vec[3]{};
    std::memcpy(vec, out, sizeof(vec));
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        const size_t count = (vec[1] > vec[0]) ? (vec[1] - vec[0]) / 8 : 0;
        log().info(L"UiProbe: settings tab list has {} entr(ies)", count);
        for (size_t i = 0; i < count && i < 64; ++i) {
            std::uintptr_t entry = 0;
            std::memcpy(&entry, reinterpret_cast<const void*>(vec[0] + i * 8), sizeof(entry));
            if (entry == 0 || !memory::isReadable(reinterpret_cast<const void*>(entry), 0x60)) {
                log().info(L"UiProbe:   tab[{}] = {:#x}", i, entry);
                continue;
            }
            std::wstring texts;
            for (std::ptrdiff_t off = 0; off + 0x20 <= 0x60; off += 8) {
                const auto* const slot = reinterpret_cast<const char*>(entry) + off;
                std::uintptr_t size = 0;
                std::uintptr_t room = 0;
                std::memcpy(&size, slot + kStringSize, sizeof(size));
                std::memcpy(&room, slot + kStringCapacity, sizeof(room));
                if (size == 0 || size > 200 || room < size || room > 0x400) {
                    continue;
                }
                const char* text = slot;
                if (room > kSsoCapacity) {
                    std::uintptr_t ptr = 0;
                    std::memcpy(&ptr, slot, sizeof(ptr));
                    if (ptr == 0
                        || !memory::isReadable(reinterpret_cast<const void*>(ptr), size + 1)) {
                        continue;
                    }
                    text = reinterpret_cast<const char*>(ptr);
                }
                bool printable = true;
                for (size_t k = 0; k < size && printable; ++k) {
                    const auto ch = static_cast<unsigned char>(text[k]);
                    printable = (ch >= 0x20 && ch <= 0x7E);
                }
                if (!printable) {
                    continue;
                }
                texts += std::format(L" +{:#x} [{}]", static_cast<size_t>(off),
                                     toUtf16(std::string(text, size)));
            }
            unsigned tag = 0;
            std::memcpy(&tag, reinterpret_cast<const void*>(entry), sizeof(tag));
            log().info(L"UiProbe:   tab[{}] {:#x} first-dword {:#x}{}", i, entry, tag, texts);
        }
    }
    if (!kAddOwnJsonTab) {
        return;
    }
    static std::uintptr_t screenView[2] = {
        reinterpret_cast<std::uintptr_t>(kOwnScreenName), sizeof(kOwnScreenName) - 1};
    static std::uintptr_t descView[2] = {reinterpret_cast<std::uintptr_t>(kOwnTabDescKey),
                                         sizeof(kOwnTabDescKey) - 1};
    const bool ok = hooks::callSettingsAddTab(out, kOwnTabEnum, screenView, descView);
    std::uintptr_t now[3]{};
    std::memcpy(now, out, sizeof(now));
    std::wstring was;
    if (ok && now[1] > now[0]) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry, reinterpret_cast<const void*>(now[1] - 8), sizeof(entry));
        if (entry != 0 && memory::isWritable(reinterpret_cast<void*>(entry), 0x50)) {
            auto* const p = reinterpret_cast<char*>(entry);
            std::uintptr_t size = 0;
            std::uintptr_t room = 0;
            std::memcpy(&size, p + 0x08 + kStringSize, sizeof(size));
            std::memcpy(&room, p + 0x08 + kStringCapacity, sizeof(room));
            const char* text = p + 0x08;
            if (room > kSsoCapacity) {
                std::uintptr_t ptr = 0;
                std::memcpy(&ptr, p + 0x08, sizeof(ptr));
                text = reinterpret_cast<const char*>(ptr);
            }
            if (text != nullptr && size > 0 && size < 100
                && memory::isReadable(text, size + 1)) {
                was = toUtf16(std::string(text, size));
            }
            writeSsoString(p + 0x08, kOwnTabId, sizeof(kOwnTabId) - 1);
            writeSsoString(p + 0x28, kOwnTabNameKey, sizeof(kOwnTabNameKey) - 1);
        }
    }
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 3) {
        log().info(L"UiProbe: added our settings tab ({}); id [{}] -> [{}]",
                   ok ? L"ok" : L"failed", was, toUtf16(kOwnTabId));
    }
}

void afterSettingsGroupRegister(void* registry, const void* idView, void* provider)
{
    (void)provider;
    if (!kAddOwnSettingsTab || !g_ownProvidersReady.load()) {
        return;
    }
    const std::string id = readStringView(idView);
    if (id != kTabsGroupId) {
        return;
    }
    buildOwnItems();

    size_t used = 0;
    int ok = 0;
    int failed = 0;
    const auto registerOne = [&](const char* groupId, size_t length) {
        if (used >= g_ownProviderCount.load() || used >= kMaxOwnProviders) {
            ++failed;
            return;
        }
        g_ownViewPool[used][0] = reinterpret_cast<std::uintptr_t>(groupId);
        g_ownViewPool[used][1] = length;
        if (hooks::callSettingsGroupRegister(registry, g_ownViewPool[used],
                                             g_ownProviderPool[used])) {
            ++ok;
        } else {
            ++failed;
        }
        ++used;
    };

    const size_t items = g_ownItemCount.load();
    registerOne(kOwnGroupIdBytes, sizeof(kOwnGroupIdBytes) - 1);
    if (kVanillaKeyRows) {
        const bool registerHolders = useOwnKeyGroups();
        for (size_t i = 0; i < items; ++i) {
            if (g_ownItems[i].type != kCompTypeGroupInfo || g_ownItems[i].child < 0) {
                continue;
            }
            if (g_ownItems[i].child == 9999 && !registerHolders) {
                continue;
            }
            registerOne(g_ownItems[i].id, std::strlen(g_ownItems[i].id));
        }
    }
    for (size_t i = 0; i < items; ++i) {
        if (g_ownItems[i].type == kCompTypeGroupInfo && g_ownItems[i].child < 0) {
            registerOne(g_ownItems[i].id, std::strlen(g_ownItems[i].id));
        }
    }
    g_ownProvidersReady.store(false);
    g_keyResetDonor.store(nullptr);
}

void onSettingsProviderCall(void* self, void* out)
{
    if (self == nullptr || out == nullptr || !memory::isReadable(out, 0x18)) {
        return;
    }
    std::uintptr_t vec[3]{};
    std::memcpy(vec, out, sizeof(vec));
    if (vec[0] == 0 || vec[1] <= vec[0]) {
        return;
    }
    const std::uintptr_t bytes = vec[1] - vec[0];

    std::uintptr_t caps[3]{};
    if (memory::isReadable(self, sizeof(caps))) {
        std::memcpy(caps, self, sizeof(caps));
    }

    const std::wstring group = readString(static_cast<const char*>(self) - 0x20);
    const bool named = !group.empty() && group.size() < 64;

    static std::set<std::wstring> told;
    static std::mutex toldMutex;
    bool tell = false;
    if (named) {
        const std::lock_guard<std::mutex> lock(toldMutex);
        tell = told.insert(group).second;
    }
    if (tell) {
        const size_t count = static_cast<size_t>(bytes / 8);
        for (size_t i = 0; i < count && i < 12; ++i) {
            std::uintptr_t item = 0;
            std::memcpy(&item, reinterpret_cast<const void*>(vec[0] + i * 8), sizeof(item));
            if (item == 0 || !memory::isReadable(reinterpret_cast<const void*>(item), 0x3D0)) {
                continue;
            }
            const auto* const bytesOf = reinterpret_cast<const unsigned char*>(item);
            const std::wstring itemId = readString(bytesOf + 0x08);
            const std::wstring second = readString(bytesOf + 0x28);
            std::uintptr_t nameProvider = 0;
            std::memcpy(&nameProvider, bytesOf + 0xB0, sizeof(nameProvider));
        }
    }

    if (!kAddOwnSettingsTab) {
        return;
    }

    const size_t count = static_cast<size_t>(bytes / 8);

    if (group == L"settings-tabs-groups") {
        if (count >= 1) {
            std::uintptr_t donor = 0;
            std::memcpy(&donor, reinterpret_cast<const void*>(vec[0]), sizeof(donor));
            rebuildOwnComponents(reinterpret_cast<void*>(donor));
        }
        if (g_ownCompsReady.load()) {
            void* const buf = hooks::callGameAllocate((count + 1) * 8);
            if (buf == nullptr) {
                log().warn(L"UiProbe: could not grow the list (allocation failed)");
                return;
            }
            std::memcpy(buf, reinterpret_cast<const void*>(vec[0]), count * 8);
            const auto own = reinterpret_cast<std::uintptr_t>(g_ownSectionComp.load());
            std::memcpy(static_cast<char*>(buf) + count * 8, &own, sizeof(own));
            const auto begin = reinterpret_cast<std::uintptr_t>(buf);
            const std::uintptr_t end = begin + (count + 1) * 8;
            std::memcpy(static_cast<char*>(out) + 0x00, &begin, sizeof(begin));
            std::memcpy(static_cast<char*>(out) + 0x08, &end, sizeof(end));
            std::memcpy(static_cast<char*>(out) + 0x10, &end, sizeof(end));
            static std::atomic<int> said{0};
        }
        return;
    }

    if (!g_ownCompsReady.load()) {
        return;
    }

    if (group == L"tsukuyomi") {
        fillOwnGroup(out, vec, count, -1);
        return;
    }

    if (group == std::wstring(kOwnTabId, kOwnTabId + sizeof(kOwnTabId) - 1)) {
        const int index = findOwnItem(kOwnNavId);
        if (index >= 0 && ensureOwnItem(static_cast<size_t>(index))
            && g_ownItems[static_cast<size_t>(index)].comp != nullptr) {
            void* const one[1] = {g_ownItems[static_cast<size_t>(index)].comp};
            const size_t room = static_cast<size_t>((vec[2] - vec[0]) / 8);
            std::uintptr_t begin = vec[0];
            if (room < 1) {
                void* const buf = hooks::callGameAllocate(8);
                if (buf == nullptr) {
                    return;
                }
                begin = reinterpret_cast<std::uintptr_t>(buf);
                const std::uintptr_t cap = begin + 8;
                std::memcpy(static_cast<char*>(out) + 0x00, &begin, sizeof(begin));
                std::memcpy(static_cast<char*>(out) + 0x10, &cap, sizeof(cap));
            }
            const auto value = reinterpret_cast<std::uintptr_t>(one[0]);
            std::memcpy(reinterpret_cast<void*>(begin), &value, sizeof(value));
            const std::uintptr_t end = begin + 8;
            std::memcpy(static_cast<char*>(out) + 0x08, &end, sizeof(end));
            static std::atomic<int> said{0};
            if (said.fetch_add(1) < 2) {
                log().info(L"UiProbe: served the json tab with 1 action ({})",
                           toUtf16(kOwnNavId));
            }
        } else {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                log().warn(L"UiProbe: could not build the json navigation action");
            }
        }
        return;
    }

    if (group == std::wstring(kBorrowedKeyGroupId, kBorrowedKeyGroupId
                                                      + sizeof(kBorrowedKeyGroupId) - 1)) {
        if (!ownTabIsRendering()) {
            if (!callStdFunctionInto(g_vanillaBorrowedProvider, out)) {
                std::memcpy(static_cast<char*>(out) + 0x08, &vec[0], sizeof(vec[0]));
            }
            return;
        }
        fillOwnItemsByPrefix(out, vec, count, L"tk.g", false);
        return;
    }
    if (group.size() > 4 && group.compare(0, 4, L"tk.g") == 0) {
        fillOwnItemsByPrefix(out, vec, count, group + L".", true);
        return;
    }
    if (useOwnKeyGroups() && group.size() > 4 && group.compare(0, 4, L"tk.k") == 0) {
        char idBytes[kOwnItemIdMax]{};
        size_t n = 0;
        for (; n + 1 < sizeof(idBytes) && n < group.size(); ++n) {
            idBytes[n] = static_cast<char>(group[n]);
        }
        idBytes[n] = '\0';
        const int which = ownKeyGroupIndex(idBytes);
        if (which >= 0) {
            if (g_vanillaBorrowedReady.load()) {
                const bool ok = callStdFunctionInto(g_vanillaBorrowedProvider, out);
                if (ok) {
                    std::uintptr_t made[3]{};
                    std::memcpy(made, out, sizeof(made));
                    const size_t n = (made[1] >= made[0]) ? (made[1] - made[0]) / 8 : 0;
                    size_t kept = 0;
                    for (size_t i = 0; i < n; ++i) {
                        std::uintptr_t comp = 0;
                        std::memcpy(&comp, reinterpret_cast<const void*>(made[0] + i * 8),
                                    sizeof(comp));
                        if (comp == 0
                            || !memory::isReadable(reinterpret_cast<const void*>(comp), 0x30)) {
                            continue;
                        }
                        std::string cid;
                        if (!readStdString(reinterpret_cast<const char*>(comp) + 0x08, cid)) {
                            continue;
                        }
                        const int row = ownKeyRowIndexFromCompId(cid.c_str());
                        if (row < 0 || g_ownKeyRows[row].module != which) {
                            continue;
                        }
                        std::memcpy(reinterpret_cast<void*>(made[0] + kept * 8), &comp,
                                    sizeof(comp));
                        ++kept;
                    }
                    const std::uintptr_t end = made[0] + kept * 8;
                    std::memcpy(static_cast<char*>(out) + 0x08, &end, sizeof(end));
                }
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 6) {
                    std::uintptr_t made[3]{};
                    std::memcpy(made, out, sizeof(made));
                    const size_t n = (made[1] >= made[0]) ? (made[1] - made[0]) / 8 : 0;
                    for (size_t i = 0; i < n && i < 4; ++i) {
                        std::uintptr_t comp = 0;
                        std::memcpy(&comp, reinterpret_cast<const void*>(made[0] + i * 8),
                                    sizeof(comp));
                        std::string id;
                    }
                }
                if (ok) {
                    return;
                }
            }
            fillOwnItemsByPrefix(out, vec, count, L"tk.g", false, which);
            return;
        }
    }
    if (group == L"tk.k") {
        const int keys = ownKeysTabIndex();
        if (keys >= 0) {
            fillOwnGroup(out, vec, count, keys);
        }
        return;
    }
    if (group.size() > 4 && group.compare(0, 4, L"tk.m") == 0) {
        char idBytes[kOwnItemIdMax]{};
        size_t n = 0;
        for (; n + 1 < sizeof(idBytes) && n < group.size(); ++n) {
            idBytes[n] = static_cast<char>(group[n]);
        }
        idBytes[n] = '\0';
        const int owner = findOwnItem(idBytes);
        if (owner >= 0) {
            fillOwnGroup(out, vec, count, owner);
        }
        return;
    }
}

constexpr std::ptrdiff_t kGroupInfoFacetId = 0x170;

constexpr char kGroupInfoDonorId[] = "settings-addons-group";

bool beforeSettingsGroupInfoUpdate(void* self)
{
    if (self == nullptr
        || !memory::isReadable(static_cast<const char*>(self) + kGroupInfoFacetId,
                               static_cast<size_t>(kStringCapacity) + sizeof(size_t))) {
        return false;
    }
    const std::wstring id = readString(static_cast<const char*>(self) + kGroupInfoFacetId);
    {
        static std::mutex mutex;
        static std::set<std::wstring> seen;
        if (id.find(L"key.tk.") != std::wstring::npos) {
            std::lock_guard<std::mutex> guard(mutex);
        }
    }
    static std::atomic<int> said{0};
    if (!kAddOwnSettingsTab) {
        return false;
    }
    (void)kGroupInfoDonorId;
    return false;
}

void afterSettingsGroupInfoUpdate(void* self, bool swapped)
{
    (void)self;
    (void)swapped;
}

std::atomic<std::uintptr_t> g_navComp{0};

void* __fastcall pgDoneCopy(void* self, void* dest)
{
    if (self != nullptr && dest != nullptr) {
        std::memcpy(dest, self, 0x38);
    }
    return dest;
}
void* __fastcall pgDoneCall(void*, void*, void*, void*) { return nullptr; }
void* __fastcall pgDoneType(void*) { return nullptr; }
void __fastcall pgDoneDelete(void*, unsigned) {}

void* g_pgDoneVtable[8]{};
alignas(16) unsigned char g_pgDone[0x40]{};

alignas(16) unsigned char g_pgAction[0x40]{};
std::atomic<std::uintptr_t> g_pgActionImpl{0};

bool copyNavAction(std::uintptr_t comp)
{
    if (g_pgActionImpl.load() != 0) {
        return true;
    }
    if (comp == 0 || !memory::isReadable(reinterpret_cast<const void*>(comp), kCompSize)) {
        return false;
    }
    std::uintptr_t impl = 0;
    std::memcpy(&impl, reinterpret_cast<const char*>(comp) + kCompActionEnabledFnPtr,
                sizeof(impl));
    if (impl == 0 || !memory::isReadable(reinterpret_cast<const void*>(impl), 8)) {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<const void*>(impl), sizeof(vtable));
    if (vtable == 0 || !memory::isReadable(reinterpret_cast<const void*>(vtable), 0x28)) {
        return false;
    }
    std::uintptr_t copyFn = 0;
    std::memcpy(&copyFn, reinterpret_cast<const char*>(vtable), sizeof(copyFn));
    if (copyFn == 0) {
        return false;
    }
    std::memset(g_pgAction, 0, sizeof(g_pgAction));
    using CopyFn = void*(__fastcall*)(void*, void*);
    void* fresh = nullptr;
    __try {
        fresh = reinterpret_cast<CopyFn>(copyFn)(reinterpret_cast<void*>(impl), &g_pgAction[0]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log().warn(L"UiProbe: could not copy the navigation action");
        return false;
    }
    if (fresh == nullptr) {
        return false;
    }
    const auto value = reinterpret_cast<std::uintptr_t>(fresh);
    std::memcpy(g_pgAction + 0x38, &value, sizeof(value));
    g_pgActionImpl.store(value);
    log().info(L"UiProbe: copied the navigation action ({:#x} -> {:#x})", impl, value);
    return true;
}

bool callCopiedNavAction()
{
    const std::uintptr_t impl = g_pgActionImpl.load();
    if (impl == 0 || !memory::isReadable(reinterpret_cast<const void*>(impl), 8)) {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<const void*>(impl), sizeof(vtable));
    if (vtable == 0 || !memory::isReadable(reinterpret_cast<const void*>(vtable), 0x28)) {
        return false;
    }
    std::uintptr_t doCall = 0;
    std::memcpy(&doCall, reinterpret_cast<const char*>(vtable) + 0x10, sizeof(doCall));
    if (doCall == 0) {
        return false;
    }
    using DoCall = void*(__fastcall*)(void*, void*);
    __try {
        reinterpret_cast<DoCall>(doCall)(reinterpret_cast<void*>(impl), &g_pgDone[0]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log().warn(L"UiProbe: the copied navigation action threw");
        return false;
    }
}

void* findRegistryComponent(const char* id)
{
    void* const registry = g_settingsRegistry.load();
    if (id == nullptr || registry == nullptr || !memory::isReadable(registry, 0x58)) {
        return nullptr;
    }
    std::uintptr_t head = 0;
    std::memcpy(&head, static_cast<const char*>(registry) + 0x50, sizeof(head));
    if (head == 0 || !memory::isReadable(reinterpret_cast<const void*>(head), 0x40)) {
        return nullptr;
    }
    std::uintptr_t node = 0;
    std::memcpy(&node, reinterpret_cast<const void*>(head), sizeof(node));
    size_t nodes = 0;
    while (node != 0 && node != head && nodes < 4096
           && memory::isReadable(reinterpret_cast<const void*>(node), 0x40)) {
        ++nodes;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::memcpy(&begin, reinterpret_cast<const char*>(node) + 0x30, sizeof(begin));
        std::memcpy(&end, reinterpret_cast<const char*>(node) + 0x38, sizeof(end));
        for (std::uintptr_t at = begin; at + 8 <= end && at - begin < 0x8000; at += 8) {
            if (!memory::isReadable(reinterpret_cast<const void*>(at), 8)) {
                break;
            }
            std::uintptr_t comp = 0;
            std::memcpy(&comp, reinterpret_cast<const void*>(at), sizeof(comp));
            if (comp == 0 || !memory::isReadable(reinterpret_cast<const void*>(comp), kCompSize)) {
                continue;
            }
            if (readCompId(reinterpret_cast<const void*>(comp)) == id) {
                return reinterpret_cast<void*>(comp);
            }
        }
        std::memcpy(&node, reinterpret_cast<const void*>(node), sizeof(node));
    }
    return nullptr;
}

void renameNavComponent(const std::string& id, void* out)
{
    if (id != kOwnNavId) {
        return;
    }
    if (out == nullptr || !memory::isReadable(out, 16)) {
        return;
    }
    std::uintptr_t comp = 0;
    std::memcpy(&comp, out, sizeof(comp));
    if (comp == 0 || !memory::isWritable(reinterpret_cast<void*>(comp), kCompSize)) {
        return;
    }
    const int index = findOwnItem(kOwnNavId);
    if (index < 0) {
        return;
    }
    const OwnItem& item = g_ownItems[static_cast<size_t>(index)];
    const size_t keyLength = std::strlen(item.nameKey);
    if (keyLength == 0 || keyLength + 2 > kOwnItemIdMax) {
        return;
    }
    char valueKey[kOwnItemIdMax]{};
    std::memcpy(valueKey, item.nameKey, keyLength);
    valueKey[keyLength] = kKeyValueSuffix;
    auto* const at = reinterpret_cast<unsigned char*>(comp);
    if (readString(at + kCompNameKey) == toUtf16(item.nameKey)) {
        g_navComp.store(comp);
        copyNavAction(comp);
        return;
    }
    writeSsoString(at + kCompNameKey, item.nameKey, keyLength);
    writeSsoString(at + kCompActionLabel, valueKey, keyLength + 1);
    g_navComp.store(comp);
    copyNavAction(comp);
    log().info(L"UiProbe: renamed the borrowed navigation component at {:#x} to {}", comp,
               toUtf16(item.nameKey));
}

bool openOwnPage(bool force)
{
    static std::atomic<unsigned long long> lastAt{0};
    const unsigned long long now = GetTickCount64();
    if (!force && now - lastAt.load() < 500) {
        return false;
    }
    lastAt.store(now);
    if (!force) {
        Schematica& mod = Schematica::instance();
        const int beforeAt = mod.editingIndex();
        const std::wstring beforeName =
            (beforeAt >= 0) ? mod.blueprintName(static_cast<size_t>(beforeAt)) : std::wstring{};
        mod.refreshFiles();
        const int afterAt = mod.editingIndex();
        const std::wstring afterName =
            (afterAt >= 0) ? mod.blueprintName(static_cast<size_t>(afterAt)) : std::wstring{};
        int screen = g_pgScreen.load();
        const int was = screen;
        const bool perBlueprint = screen == kScreenBlueprint || screen == kScreenMaterials
                                  || screen == kScreenVerifier;
        if (screen < 0 || screen >= kScreenCount) {
            screen = kScreenMenu;
        } else if (perBlueprint && (afterName.empty() || afterName != beforeName)) {
            screen = kScreenBlueprints;
        }
        if (screen != was) {
            g_pgScreen.store(screen);
        }
        (void)pageHeaderTitle();
    }
    g_ownNavInvokedAt.store(now);

    g_afterInvokeLogs.store(0);

    g_pgDoneVtable[0] = reinterpret_cast<void*>(&pgDoneCopy);
    g_pgDoneVtable[1] = reinterpret_cast<void*>(&pgDoneCopy);
    g_pgDoneVtable[2] = reinterpret_cast<void*>(&pgDoneCall);
    g_pgDoneVtable[3] = reinterpret_cast<void*>(&pgDoneType);
    g_pgDoneVtable[4] = reinterpret_cast<void*>(&pgDoneDelete);
    std::memset(g_pgDone, 0, sizeof(g_pgDone));
    {
        const auto vtable = reinterpret_cast<std::uintptr_t>(&g_pgDoneVtable[0]);
        std::memcpy(g_pgDone, &vtable, sizeof(vtable));
        const auto self = reinterpret_cast<std::uintptr_t>(&g_pgDone[0]);
        std::memcpy(g_pgDone + 0x38, &self, sizeof(self));
    }

    constexpr bool kAlwaysBorrowScreen = true;
    const auto comp = kAlwaysBorrowScreen
                          ? 0
                          : reinterpret_cast<std::uintptr_t>(findRegistryComponent(kOwnNavId));
    if (comp == 0) {
        g_ownKeyOpenAt.store(GetTickCount64());
        g_ownSettingsRoute.store(false);
        g_afterInvokeLogs.store(0);
        log().info(L"UiProbe: opening a screen from the key (lookups so far: {})",
                   g_lookupCount.load());
        if (hooks::callOpenHowToPlayScreen()) {
            log().info(L"UiProbe: opened the borrowed How to Play screen for the page");
            return true;
        }
        g_ownKeyOpenAt.store(0);
        log().warn(L"UiProbe: could not open a screen (no client instance yet?)");
        return false;
    }
    g_navComp.store(comp);

    if (!memory::isReadable(reinterpret_cast<const void*>(comp), kCompSize)) {
        log().warn(L"UiProbe: the navigation component is not readable ({:#x})", comp);
        return false;
    }
    const auto kind = reinterpret_cast<const unsigned char*>(comp)[kCompType];
    if (kind != 5) {
        log().warn(L"UiProbe: the navigation component is kind {} (expected 5)", kind);
        return false;
    }
    std::uintptr_t action = 0;
    std::memcpy(&action, reinterpret_cast<const char*>(comp) + kCompActionEnabledFnPtr,
                sizeof(action));
    if (action == 0 || !memory::isReadable(reinterpret_cast<const void*>(action), 8)) {
        log().warn(L"UiProbe: the navigation component has no action at +0x380");
        return false;
    }
    {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            if (g_exeSize == 0) {
                noteModule(GetModuleHandleW(nullptr), g_exeBase, g_exeSize);
            }
            std::uintptr_t vtable = 0;
            std::memcpy(&vtable, reinterpret_cast<const void*>(action), sizeof(vtable));
            std::wstring slots;
            if (vtable != 0 && memory::isReadable(reinterpret_cast<const void*>(vtable), 0x28)) {
                for (int i = 0; i < 5; ++i) {
                    std::uintptr_t fn = 0;
                    std::memcpy(&fn, reinterpret_cast<const char*>(vtable) + i * 8, sizeof(fn));
                    slots += std::format(L"[{}] {} ", i, codeName(fn));
                }
            }
            log().info(L"UiProbe: navigation action {:#x} vtable {:#x} {}", action, vtable,
                       slots);
        }
    }
    g_ownSettingsRoute.store(true);
    const bool ok =
        hooks::callSettingsInvokeAction(reinterpret_cast<void*>(comp), &g_pgDone[0]);
    log().info(L"UiProbe: asked the game to open the Schematica page ({})",
               ok ? L"ok" : L"the game said no");
    return ok;
}

bool refreshOwnPage()
{
    if (findRegistryComponent(kOwnNavId) == nullptr) {
        return false;
    }
    return openOwnPage();
}

void onSettingsFindComponent(void* , void* out, const void* idView)
{
    if (kAddOwnSettingsTab && g_ownCompsReady.load() && out != nullptr
        && memory::isWritable(out, 16)) {
        const std::string want = readStringView(idView);
        void* own = nullptr;
        if (want == kOwnGroupIdBytes) {
            own = g_ownSectionComp.load();
        } else if (kVanillaKeyRows && want == kBorrowedKeyGroupId) {
            g_ownRowTouchedAt.store(GetTickCount64());
            const int index = findOwnItem(want.c_str());
            if (index >= 0 && ensureOwnItem(static_cast<size_t>(index))) {
                own = g_ownItems[static_cast<size_t>(index)].comp;
            }
        } else if (useOwnKeyGroups() && ownKeyGroupIndex(want.c_str()) >= 0) {
            const int index = findOwnItem(want.c_str());
            if (index >= 0 && ensureOwnItem(static_cast<size_t>(index))) {
                own = g_ownItems[static_cast<size_t>(index)].comp;
            }
        } else if (want.rfind("json-navigation-", 0) == 0) {
            renameNavComponent(want, out);
        } else if (want.size() > 3 && want.compare(0, 3, "tk.") == 0) {
            const int index = findOwnItem(want.c_str());
            {
                static std::mutex mutex;
                static std::set<std::string> seen;
                static bool capped = false;
                std::lock_guard<std::mutex> guard(mutex);
                if (!capped && seen.insert(want).second) {
                    if (seen.size() > 64) {
                        capped = true;
                        log().info(L"UiProbe: stopped listing rows at 64");
                    } else {
                        const int type = index >= 0
                                             ? g_ownItems[static_cast<size_t>(index)].type
                                             : -1;
                        log().info(L"UiProbe: row {} (type {})", toUtf16(want), type);
                    }
                }
            }
            {
                static std::atomic<bool> told{false};
                if (!told.exchange(true)) {
                    dumpSettingsRegistry(L"looking for json-navigation");
                }
            }
            if (index >= 0 && ensureOwnItem(static_cast<size_t>(index))) {
                own = g_ownItems[static_cast<size_t>(index)].comp;

            }
        }
        if (own != nullptr) {
            const auto value = reinterpret_cast<std::uintptr_t>(own);
            std::memcpy(out, &value, sizeof(value));
            *(static_cast<unsigned char*>(out) + 8) = 1;
            static std::atomic<int> said{0};
            return;
        }
    }

    if (kAddOwnSettingsTab && out != nullptr && memory::isReadable(out, 16)
        && *(static_cast<const unsigned char*>(out) + 8) == 1) {
        std::uintptr_t comp = 0;
        std::memcpy(&comp, out, sizeof(comp));
        if (comp != 0 && memory::isReadable(reinterpret_cast<const void*>(comp), kCompType + 1)) {
            const auto kind = reinterpret_cast<const unsigned char*>(comp)[kCompType];
            if (kind < 16) {
                g_donorByType[kind].store(reinterpret_cast<void*>(comp));
                if (!g_donorTemplate[kind].ready
                    && memory::isReadable(reinterpret_cast<const void*>(comp), kCompSize)) {
                    std::memcpy(g_donorTemplate[kind].bytes, reinterpret_cast<const void*>(comp),
                                kCompSize);
                    g_donorTemplate[kind].base = comp;
                    g_donorTemplate[kind].ready = true;
                    if (kind == 4) {
                        dumpTextDonor(reinterpret_cast<const unsigned char*>(comp));
                    }
                    if (kind == 0 || kind == 4) {
                        dumpComponentVtable(reinterpret_cast<const void*>(comp),
                                            static_cast<int>(kind));
                    }
                    if (kind == 3 || kind == 5) {
                        dumpComponentStrings(g_donorTemplate[kind].bytes, static_cast<int>(kind));
                        dumpComponentCallables(g_donorTemplate[kind].bytes, comp,
                                               static_cast<int>(kind));
                    }
                }
                if (kind == 5) {
                    const std::string seen = readStringView(idView);
                    if (seen.rfind("json-navigation-", 0) == 0) {
                        static std::atomic<int> said{0};
                        if (said.fetch_add(1) < 2) {
                            dumpNavComponent(reinterpret_cast<const unsigned char*>(comp), seen);
                        }
                    }
                }
                if (g_ownItemsIncomplete.exchange(false)) {
                    requestOwnPublishAllTabs();
                }
            }
        }
    }

    static std::atomic<bool> full{false};
    if (full.load(std::memory_order_relaxed)) {
        return;
    }
    const std::string id = readStringView(idView);
    if (id.empty()) {
        return;
    }
    static std::set<std::string> told;
    static std::mutex toldMutex;
    {
        const std::lock_guard<std::mutex> lock(toldMutex);
        if (told.size() >= 80) {
            full.store(true, std::memory_order_relaxed);
            return;
        }
        if (!told.insert(id).second) {
            return;
        }
    }
    std::uintptr_t found = 0;
    int has = -1;
    if (out != nullptr && memory::isReadable(out, 16)) {
        std::memcpy(&found, out, sizeof(found));
        has = static_cast<int>(*(static_cast<const unsigned char*>(out) + 8));
    }
    int type = -1;
    std::uintptr_t nameProvider = 0;
    std::wstring second;
    if (has == 1 && found != 0 && memory::isReadable(reinterpret_cast<const void*>(found), 0x3D0)) {
        const auto* const bytesOf = reinterpret_cast<const unsigned char*>(found);
        type = static_cast<int>(bytesOf[0x3C8]);
        std::memcpy(&nameProvider, bytesOf + 0xB0, sizeof(nameProvider));
        second = readString(bytesOf + 0x28);
    }
}

void reportSettingsGroups()
{
    static unsigned ticks = 0;
    static int said = 0;
    ++ticks;
    if (said >= 3 || ticks % 600 != 0) {
        return;
    }
    ++said;
}

std::atomic<bool> g_settingsDirty{false};

void resolveMaybePressed()
{
    const int index = g_maybePressed.load();
    if (index < 0) {
        return;
    }
    constexpr unsigned long long kDecideAfterMs = 60;
    constexpr unsigned long long kAroundMs = 200;
    const unsigned long long at = g_maybePressedAt.load();
    const unsigned long long now = GetTickCount64();
    if (now - at < kDecideAfterMs) {
        return;
    }
    if (g_maybePressed.exchange(-1) != index) {
        return;
    }
    int others = 0;
    for (const auto& mark : g_resolveMarks) {
        if (mark.tick == 0 || mark.item == index) {
            continue;
        }
        const unsigned long long diff = (mark.tick > at) ? (mark.tick - at) : (at - mark.tick);
        if (diff <= kAroundMs) {
            ++others;
        }
    }
    if (others > 0) {
        static std::atomic<int> said{0};
        return;
    }
    g_ownCaptureRequest.store(index);
}

void pumpSettingsKeybind()
{
    static int capturing = -1;
    static bool waitingRelease = false;
    resolveMaybePressed();
    const int requested = g_ownCaptureRequest.exchange(-1);
    if (requested >= 0) {
        MenuItem* const source = ownMenuItem(requested);
        const char* const pressedId =
            (requested >= 0 && static_cast<size_t>(requested) < kMaxOwnItems)
                ? g_ownItems[static_cast<size_t>(requested)].id
                : "";
        const size_t pressedLen = std::strlen(pressedId);
        const bool isResetButton =
            (pressedLen > 6 && std::strcmp(pressedId + pressedLen - 6, ".reset") == 0);
        if (isResetButton && source != nullptr && source->setKeys) {
            source->setKeys(source->defaultKeys);
            g_settingsDirty.store(true);
            requestOwnPublishWithTab(requested);
        } else if (source != nullptr && source->opensPage) {
            openOwnPage(true);
        } else if (source != nullptr && source->kind == MenuItemKind::Cycle) {
            if (source->activate) {
                source->activate();
            }
        } else {
            capturing = requested;
            waitingRelease = true;
            g_captureItem.store(requested);
            requestOwnPublish(ownCaptureStateOf(requested));
        }
    }
    if (capturing < 0) {
        return;
    }
    std::vector<int> pressed;
    for (int vk = 0x08; vk <= 0xDF; ++vk) {
        if (vk >= 0x15 && vk <= 0x1A) {
            continue;
        }
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
            pressed.push_back(keys::normalize(vk));
        }
    }
    if (waitingRelease) {
        if (pressed.empty()) {
            waitingRelease = false;
        } else {
            static std::atomic<int> said{0};
        }
        return;
    }
    if (!pressed.empty()) {
        static std::atomic<int> said{0};
    }
    if (pressed.empty()) {
        return;
    }
    if (std::find(pressed.begin(), pressed.end(), VK_ESCAPE) != pressed.end()) {
        requestOwnPublish(ownCaptureStateOf(capturing));
        capturing = -1;
        g_captureItem.store(-1);
        return;
    }
    const bool onlyModifiers =
        std::all_of(pressed.begin(), pressed.end(), [](int vk) { return keys::isModifier(vk); });
    if (onlyModifiers) {
        return;
    }
    std::sort(pressed.begin(), pressed.end());
    pressed.erase(std::unique(pressed.begin(), pressed.end()), pressed.end());
    const int target = capturing;
    capturing = -1;
    g_captureItem.store(-1);
    requestOwnPublish(ownCaptureStateOf(target));
    MenuItem* const source = ownMenuItem(target);
    if (source == nullptr || !source->setKeys) {
        return;
    }
    source->setKeys(pressed);
    g_settingsDirty.store(true);
    requestOwnPublishWithTab(target);
}

void pumpSettingsNumber()
{
    const int index = g_ownNumberRequest.exchange(-1);
    if (index < 0) {
        return;
    }
    MenuItem* const source = ownMenuItem(index);
    if (source == nullptr || !source->setNumber) {
        return;
    }
    const float value = g_ownNumberValue.load();
    source->setNumber(value);
    g_settingsDirty.store(true);
    clearOwnPending(index);
    requestOwnPublishWithTab(index);
    static std::atomic<int> said{0};
}

void pumpSettingsChoice()
{
    const int index = g_ownChoiceRequest.exchange(-1);
    if (index < 0) {
        return;
    }
    MenuItem* const source = ownMenuItem(index);
    if (source == nullptr || !source->setChoice) {
        return;
    }
    const int at = g_ownChoiceValue.load();
    if (at < 0 || static_cast<size_t>(at) >= source->choices.size()) {
        return;
    }
    source->setChoice(at);
    g_settingsDirty.store(true);
    clearOwnPending(index);
    requestOwnPublishWithTab(index);
}

void pumpSettingsText()
{
    const int index = g_ownTextRequest.exchange(-1);
    if (index < 0) {
        return;
    }
    MenuItem* const source = ownMenuItem(index);
    if (source == nullptr || !source->setText) {
        return;
    }
    char text[kOwnTextMax]{};
    {
        const std::lock_guard<std::mutex> lock(g_ownTextValueMutex);
        std::snprintf(text, sizeof(text), "%s", g_ownTextValue);
    }
    source->setText(toUtf16(text));
    g_settingsDirty.store(true);
}

LONG CALLBACK crashWatch(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION
        && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO
        && code != 0xC0000374 ) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    constexpr std::size_t kCrashSeenMax = 32;
    static std::atomic<std::uintptr_t> crashSeen[kCrashSeenMax] = {};
    static std::atomic<std::size_t> crashSeenCount{0};
    bool fresh = false;
    {
        const std::size_t used =
            std::min(crashSeenCount.load(std::memory_order_acquire), kCrashSeenMax);
        std::size_t i = 0;
        for (; i < used; ++i) {
            if (crashSeen[i].load(std::memory_order_relaxed) == at) {
                break;
            }
        }
        if (i == used && used < kCrashSeenMax) {
            crashSeen[used].store(at, std::memory_order_relaxed);
            crashSeenCount.store(used + 1, std::memory_order_release);
            fresh = true;
        }
    }
    if (fresh) {
        const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&crashWatch), &self);
        const auto mine = reinterpret_cast<std::uintptr_t>(self);
        const bool inSelf = (mine != 0 && at >= mine && at < mine + 0x800000);
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(at), &owner);
        wchar_t ownerName[MAX_PATH] = L"";
        if (owner != nullptr) {
            GetModuleFileNameW(owner, ownerName, MAX_PATH);
        }
        const wchar_t* const slash = std::wcsrchr(ownerName, 0x5C );
        const wchar_t* const leaf = slash != nullptr ? slash + 1 : ownerName;
        if (owner != nullptr) {
            log().warn(L"UiProbe: crashed. exception {:#x} at {:#x} ({} + {:#x}) thread {}",
                       code, at, leaf, at - reinterpret_cast<std::uintptr_t>(owner),
                       GetCurrentThreadId());
        } else {
            MEMORY_BASIC_INFORMATION mbi = {};
            const SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi));
            log().warn(L"UiProbe: crashed. exception {:#x} at {:#x} (outside the module, base "
                       L"{:#x} state {:#x} protect {:#x} type {:#x}) thread {}",
                       code,
                       at,
                       got != 0 ? reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) : 0,
                       got != 0 ? mbi.State : 0,
                       got != 0 ? mbi.Protect : 0,
                       got != 0 ? mbi.Type : 0,
                       GetCurrentThreadId());
        }
        log().warn(L"UiProbe: exe {:#x} / self {:#x} ({})",
                   exe,
                   mine,
                   inSelf ? L"inside us" : L"outside us");
        if (info->ExceptionRecord->NumberParameters >= 2) {
            log().warn(L"UiProbe:   touched address {:#x}{} ({})",
                       info->ExceptionRecord->ExceptionInformation[1],
                       ownName(info->ExceptionRecord->ExceptionInformation[1]),
                       info->ExceptionRecord->ExceptionInformation[0] == 0 ? L"read" : L"write");
        }
        if (g_exeSize == 0) {
            noteModule(GetModuleHandleW(nullptr), g_exeBase, g_exeSize);
        }
        if (g_selfSize == 0 && self != nullptr) {
            noteModule(self, g_selfBase, g_selfSize);
        }
        void* frames[24]{};
        const USHORT got = CaptureStackBackTrace(0, 24, frames, nullptr);
        std::wstring chain;
        for (USHORT i = 0; i < got && i < 14; ++i) {
            chain += std::format(L"[{}] {} ", i,
                                 codeName(reinterpret_cast<std::uintptr_t>(frames[i])));
        }
        log().warn(L"UiProbe: call chain {}", chain);
        if (const CONTEXT* const ctx = info->ContextRecord;
            ctx != nullptr && memory::isReadable(ctx, sizeof(CONTEXT))) {
            log().warn(L"UiProbe:   rax={:#x}{} rcx={:#x}{} rdx={:#x}{} r8={:#x}{} r9={:#x}{}",
                       ctx->Rax, ownName(ctx->Rax), ctx->Rcx, ownName(ctx->Rcx), ctx->Rdx,
                       ownName(ctx->Rdx), ctx->R8, ownName(ctx->R8), ctx->R9,
                       ownName(ctx->R9));
            constexpr std::size_t kProbeSlots = 192;
            const auto* const sp = reinterpret_cast<const std::uintptr_t*>(ctx->Rsp);
            if (memory::isReadable(sp, kProbeSlots * sizeof(std::uintptr_t))) {
                std::wstring raw;
                int found = 0;
                for (std::size_t i = 0; i < kProbeSlots && found < 12; ++i) {
                    const std::uintptr_t value = sp[i];
                    const bool mine = (g_selfSize != 0 && value >= g_selfBase
                                       && value < g_selfBase + g_selfSize);
                    const bool game = (g_exeSize != 0 && value >= g_exeBase
                                       && value < g_exeBase + g_exeSize);
                    if (mine || game) {
                        raw += std::format(L"(+{:#x}) {} ", i * sizeof(std::uintptr_t),
                                           codeName(value));
                        ++found;
                    }
                }
                log().warn(L"UiProbe: raw stack {}", raw);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool installPublishPump()
{
    if (!kAddOwnSettingsTab) {
        return true;
    }
    static std::atomic<bool> watched{false};
    if (!watched.exchange(true)) {
        AddVectoredExceptionHandler(1, &crashWatch);
    }
    HMODULE const user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        log().warn(L"UiProbe: user32 is not loaded (cannot install the publish pump)");
        return false;
    }
    void* const target = reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageW"));
    if (target == nullptr) {
        log().warn(L"UiProbe: PeekMessageW not found (cannot install the publish pump)");
        return false;
    }
    return HookManager::instance().create(target, reinterpret_cast<void*>(&detourPeekMessageW),
                                          reinterpret_cast<void**>(&g_peekMessageW),
                                          L"PeekMessageW");
}

bool takeSettingsDirty() { return g_settingsDirty.exchange(false); }

void pumpSettingsToggle()
{
    pumpSettingsKeybind();
    pumpSettingsNumber();
    pumpSettingsChoice();
    pumpSettingsText();
    const int index = g_ownToggleRequest.exchange(-1);
    if (index < 0) {
        return;
    }
    MenuItem* const source = ownMenuItem(index);
    if (source == nullptr || !source->isAvailable()) {
        return;
    }
    switch (source->kind) {
    case MenuItemKind::Keybind:
        g_ownCaptureRequest.store(index);
        return;
    case MenuItemKind::Cycle:
        if (source->activate) {
            source->activate();
            g_settingsDirty.store(true);
        }
        return;
    default:
        break;
    }
    if (source->activate) {
        source->activate();
        g_settingsDirty.store(true);
        clearOwnPending(index);
        requestOwnPublishWithTab(index);
    }
}

void pumpMenuSelection()
{
    {
        static bool escDown = false;
        const bool now = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (now && !escDown) {
            if (!selfEscapeInFlight()) {
                cancelPageReopen();
            }
            g_pgBag.store(nullptr);
        }
        escDown = now;
    }
    if (kNavPaneOwnItems) {
        pumpNumberEntry();
    }
    pumpPagePress();
    pumpToggleState();
    pumpPageReopen();
    pumpPageEntry();
    dumpToggleControlPeriodically();
    reportSettingsGroups();

    const int idx = g_pendingMenu.exchange(-1);
    if (idx <= 0 || static_cast<size_t>(idx) >= g_menuCount) {
        return;
    }
    Module* const target = g_menuModules[idx];
    if (target == nullptr || !target->available()) {
        return;
    }
    target->toggle();

    const std::string text = g_menuTextBuf[idx];
}

void onButtonMappingsBegin(void* out)
{
    t_mappingOut = out;
}

void onButtonMappingsEnd(void* out)
{
    (void)out;
    t_mappingOut = nullptr;

    void* const target = g_idMappingOut.load();
    if (target == nullptr || g_mappingDumped.exchange(true)) {
        return;
    }
}

void onOptionRegister(void* self, int id, const void* name)
{
    const unsigned long long index = g_options.fetch_add(1) + 1;

    void* known = g_optionSelf.load();
    if (known != self) {
        if (!g_optionSelf.compare_exchange_strong(known, self) && known != self) {
            log().warn(L"UiProbe: option registry self changed to {} at #{}",
                       static_cast<const void*>(self), index);
        }
    }

    if (index > kMaxOptions) {
        if (!g_optionsCapped.exchange(true)) {
            log().warn(L"UiProbe: stopped listing options at {}", kMaxOptions);
        }
        return;
    }

    const std::wstring text = readString(name);
    if (text.empty()) {
        return;
    }
}

}
