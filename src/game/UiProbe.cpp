#include "game/UiProbe.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "game/GameVersion.h"
#include "game/OreUiPatch.h"
#include "game/StackCount.h"
#include "game/UiTree.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "hooks/HookCount.h"
#include "input/Keys.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/Module.h"
#include "modules/ModuleManager.h"
#include "modules/Schematica.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

#include <nlohmann/json.hpp>

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

std::mutex g_mutex;

constexpr std::ptrdiff_t kNodeLeft = 0x00;
constexpr std::ptrdiff_t kNodeParent = 0x08;
constexpr std::ptrdiff_t kNodeIsNil = 0x19;
constexpr std::ptrdiff_t kNodeValue = 0x20;
constexpr std::ptrdiff_t kNodeRight = 0x10;
constexpr unsigned kTagString = 4;
constexpr unsigned kTagObject = 7;

constexpr int kMaxNodes = 256;
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

constexpr int kMaxNavKeys = 32;
constexpr char kOwnTextVar[] = "$button_text";

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

constexpr char kSecCtlNameVar[] = "$control_name";
constexpr char kSecBindNameVar[] = "$option_binding_name";
constexpr char kSecLabelVar[] = "$option_label";
constexpr char kNumHintVar[] = "$option_place_holder_text";
constexpr char kTgTypeVar[] = "$toggle_binding_type";
constexpr char kTgTypeNone[] = "none";
constexpr char kTgTypeGlobal[] = "global";
constexpr char kTgDefaultVar[] = "$toggle_default_state";
constexpr char kTgBindVar[] = "$toggle_state_binding_name";
constexpr char kTgNoBind[] = "#not_data_bound";
constexpr char kTgTrueKey[] = "focus_magnet_enabled";
constexpr char kTgFalseKey[] = "$toggle_default_state|default";

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

constexpr size_t kArenaNodes = 24576;
constexpr size_t kArenaWords = 98304;
constexpr size_t kArenaRecords = 24576;
constexpr size_t kArenaText = 262144;
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
    }
    out.ok = true;
    return true;
}

PageDonors g_pgDonors;

struct Over {
    enum class Kind { Text, Pair, Raw, Int, Bag, Objects };
    const char* key = nullptr;
    Kind kind = Kind::Text;
    std::string text;
    std::string second;
    std::uintptr_t value = 0;
    std::uintptr_t tag = 0;
    std::uintptr_t seq = 0;
    std::int64_t number = 0;
    std::vector<Over> items;
    std::vector<std::vector<Over>> rows;
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

Over overRawValue(const char* key, std::uintptr_t value, std::uintptr_t tag,
                  std::uintptr_t seq)
{
    Over over;
    over.key = key;
    over.kind = Over::Kind::Raw;
    over.value = value;
    over.tag = tag;
    over.seq = seq;
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
    case Over::Kind::Objects: {
        std::vector<const std::uintptr_t*> elements;
        elements.reserve(over.rows.size());
        for (const std::vector<Over>& row : over.rows) {
            std::vector<uitree::KeyValue> inner;
            inner.reserve(row.size());
            for (const Over& one : row) {
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
            const std::uintptr_t* const element = arena.makeValue(donors.elemVal, node);
            if (element == nullptr) {
                return false;
            }
            elements.push_back(element);
        }
        const std::uintptr_t* const vec = arena.makeVector(
            donors.vecSrc, elements.empty() ? nullptr : elements.data(), elements.size());
        if (vec == nullptr) {
            return false;
        }
        out.value = reinterpret_cast<std::uintptr_t>(vec);
        out.tag = donors.arrTag;
        out.seq = donors.arrSeq;
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

}

void onLookup(const void* space, const void* name)
{
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
        }
    }

}

constexpr size_t kMaxPageRows = 256;
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
constexpr int kRowTab = 8;
constexpr char kTabRowDef[] = "how_to_play_common.section_toggle_button";
constexpr char kTabRowHeight[] = "30px";
constexpr int kRowPick = 9;

constexpr char kColGreen[] = "§a";
constexpr char kColAqua[] = "§b";
constexpr char kColRed[] = "§c";
constexpr char kColGold[] = "§6";
constexpr char kColPurple[] = "§d";
constexpr char kColGray[] = "§7";
constexpr char kColDark[] = "§8";
constexpr char kColWhite[] = "§f";
int g_pgRowKind[kMaxPageRows]{};
std::int32_t g_pgRowIcon[kMaxPageRows]{};
bool g_pgRowHasIcon[kMaxPageRows]{};
bool g_pgRowIconCol[kMaxPageRows]{};
int g_pgRowNameW[kMaxPageRows]{};
char g_pgCols[kMaxPageRows][2][kPageTextBytes]{};
char g_pgColColor[kMaxPageRows][2][4]{};

constexpr int kActItem = 0;
constexpr int kActGoto = 1;
constexpr int kActNone = 2;
constexpr int kActShow = 3;
constexpr int kActLayerMode = 4;
constexpr int kActLayerAxis = 5;
constexpr int kActMatList = 6;
constexpr int kActMatRefresh = 7;
constexpr int kActMatWrite = 8;
constexpr int kActMatClear = 9;
constexpr int kActMatIgnore = 10;
int g_pgRowAct[kMaxPageRows]{};
int g_pgRowArg[kMaxPageRows]{};

bool g_pgRowChild[kMaxPageRows]{};
int g_pgRowKids[kMaxPageRows]{};
char g_pgRowWidth[kMaxPageRows][12]{};
constexpr int kSlotBody = 0;
constexpr int kSlotTop = 1;
constexpr int kSlotBottom = 2;
int g_pgRowSlot[kMaxPageRows]{};
char g_pgRowHeight[kMaxPageRows][12]{};
int g_pgRowTabIndex[kMaxPageRows]{};
bool g_pgRowSelected[kMaxPageRows]{};
size_t g_pgRowsDropped = 0;
int g_pgTabSelected = 0;
std::uintptr_t g_pgTopElems[kMaxPageRows]{};
size_t g_pgTopCount = 0;
int g_pgRowMenuTab[kMaxPageRows]{};
int g_pgRowMenuIdx[kMaxPageRows]{};
int g_pgRowTab[kMaxPageRows]{};
int g_pgRowBlueprint[kMaxPageRows]{};
constexpr size_t kMaxTabsForRows = 20;
size_t g_pgTabStart[kMaxTabsForRows]{};
size_t g_pgTabRows[kMaxTabsForRows]{};

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
constexpr char kPageBodyHeight[] = "100% - 42px";
char g_pgBodyW[16] = "58%";
char g_pgBodyX[16] = "0px";
constexpr char kBodyWideW[] = "78%";
constexpr char kBodyNarrowW[] = "58%";
std::uintptr_t* g_pgRoot = nullptr;
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

std::atomic<int> g_pgTab{0};
std::atomic<int> g_ptPressed{-1};

char g_ptTexts[kMaxLeftRows][kPageTextBytes]{};
char g_ptBindNames[kMaxLeftRows][32]{};
alignas(8) unsigned char g_ptSizeRecs[kMaxLeftRows][2][16]{};
std::uintptr_t g_ptSizeElems[kMaxLeftRows][2][4]{};
std::uintptr_t g_ptSizeElemPtrs[kMaxLeftRows][2]{};
std::uintptr_t g_ptSizeVec[kMaxLeftRows][4]{};

constexpr char kPageExitId[] = "button.menu_exit";
constexpr char kPageExitVar[] = "$pressed_button_name";

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
    g_pgRowsDropped = 0;
    for (size_t i = 0; i < kMaxPageRows; ++i) {
        g_pgRowIcon[i] = 0;
        g_pgRowHasIcon[i] = false;
        g_pgRowIconCol[i] = false;
        g_pgRowNameW[i] = 0;
        g_pgRowSlot[i] = kSlotBody;
        g_pgRowHeight[i][0] = 0;
        g_pgRowTabIndex[i] = -1;
        g_pgRowSelected[i] = false;
    }
    g_pgTabSelected = 0;

    int slot = kSlotBody;
    int group = -1;

    const auto newRow = [&](const char* width) -> long {
        if (rows >= kMaxPageRows) {
            ++g_pgRowsDropped;
            return -1;
        }
        const size_t at = rows++;
        g_pgTexts[at][0] = 0;
        g_pgLabels[at][0] = 0;
        g_pgHints[at][0] = 0;
        g_pgRowAct[at] = kActNone;
        g_pgRowArg[at] = 0;
        g_pgRowMenuTab[at] = -1;
        g_pgRowMenuIdx[at] = -1;
        g_pgRowTab[at] = 0;
        g_pgRowBlueprint[at] = -1;
        g_pgRowKind[at] = kRowButton;
        g_pgIsEdit[at] = false;
        g_pgRowChild[at] = (group >= 0);
        g_pgRowKids[at] = 0;
        g_pgRowSlot[at] = slot;
        g_pgRowHeight[at][0] = 0;
        g_pgRowTabIndex[at] = -1;
        g_pgRowSelected[at] = false;
        g_pgRowWidth[at][0] = 0;
        if (width != nullptr) {
            std::snprintf(g_pgRowWidth[at], sizeof(g_pgRowWidth[at]), "%s", width);
        }
        return static_cast<long>(at);
    };

    const auto beginRow = [&](const char* height) {
        const long at = newRow(nullptr);
        if (at < 0) {
            return;
        }
        if (height != nullptr) {
            std::snprintf(g_pgRowHeight[at], sizeof(g_pgRowHeight[at]), "%s", height);
        }
        group = static_cast<int>(at);
    };
    const auto endRow = [&]() {
        if (group >= 0) {
            g_pgRowKids[group] = static_cast<int>(rows) - group - 1;
            group = -1;
        }
    };

    const auto addItemAs = [&](int tab, size_t index, const char* width,
                               bool labelOnly) -> bool {
        const MenuItem* const item = schematicaRowOf(tab, index);
        if (item == nullptr) {
            return false;
        }
        const long at = newRow(width);
        if (at < 0) {
            return false;
        }
        if (labelOnly) {
            copyAscii(g_pgTexts[at], kPageTextBytes, item->labelText().c_str());
        } else if (!schematicaRowTextOf(tab, index, g_pgTexts[at], kPageTextBytes)) {
            --rows;
            return false;
        }
        copyAscii(g_pgLabels[at], kPageTextBytes, item->labelText().c_str());
        copyAscii(g_pgHints[at], kPageTextBytes, item->valueText().c_str());
        g_pgRowAct[at] = kActItem;
        g_pgRowMenuTab[at] = tab;
        g_pgRowMenuIdx[at] = static_cast<int>(index);
        g_pgRowBlueprint[at] = (tab == 1) ? mod.editingIndex() : -1;
        if (item->kind == MenuItemKind::Text && item->getText && item->setText) {
            g_pgRowKind[at] = kRowEdit;
        } else if (item->kind == MenuItemKind::Number && item->getNumber && item->setNumber) {
            g_pgRowKind[at] = kRowSlider;
        } else if (item->kind == MenuItemKind::Toggle) {
            g_pgRowKind[at] = kRowToggle;
        } else {
            g_pgRowKind[at] = kRowButton;
        }
        g_pgIsEdit[at] = (g_pgRowKind[at] == kRowEdit);
        return true;
    };
    const auto addItem = [&](int tab, size_t index, const char* width = nullptr) -> bool {
        return addItemAs(tab, index, width, false);
    };

    const auto addPlain = [&](int kind, const char* text, const char* width = nullptr) {
        const long at = newRow(width);
        if (at < 0) {
            return;
        }
        std::snprintf(g_pgTexts[at], kPageTextBytes, "%s", (text != nullptr) ? text : "");
        g_pgRowKind[at] = kind;
    };

    const auto addGoto = [&](const char* text, int screen, int blueprint,
                             const char* width = nullptr) {
        const long at = newRow(width);
        if (at < 0) {
            return;
        }
        std::snprintf(g_pgTexts[at], kPageTextBytes, "%s", text);
        g_pgRowAct[at] = kActGoto;
        g_pgRowArg[at] = screen;
        g_pgRowBlueprint[at] = blueprint;
        g_pgRowKind[at] = kRowButton;
    };

    struct TabDef {
        const char* text;
        int screen;
    };
    static const TabDef kMainTabs[] = {{"Placements", kScreenBlueprints},
                                       {"Configuration", kScreenSettings},
                                       {"Render Layers", kScreenLayers}};
    static const TabDef kPlaceTabs[] = {{"Configure", kScreenBlueprint},
                                        {"Material List", kScreenMaterials},
                                        {"Verifier", kScreenVerifier}};

    const auto addTabStrip = [&](const TabDef* defs, size_t count, int screen) {
        const int before = slot;
        slot = kSlotTop;
        char width[12]{};
        std::snprintf(width, sizeof(width), "%u%%",
                      static_cast<unsigned>(100 / (count > 0 ? count : 1)));
        for (size_t t = 0; t < count; ++t) {
            const long at = newRow(width);
            if (at < 0) {
                break;
            }
            std::snprintf(g_pgTexts[at], kPageTextBytes, "%s", defs[t].text);
            g_pgRowKind[at] = kRowTab;
            g_pgRowAct[at] = kActGoto;
            g_pgRowArg[at] = defs[t].screen;
            g_pgRowTabIndex[at] = static_cast<int>(t);
            g_pgRowSelected[at] = (defs[t].screen == screen);
            if (g_pgRowSelected[at]) {
                g_pgTabSelected = static_cast<int>(t);
            }
        }
        slot = before;
    };

    const auto addBottomNav = [&](const char* backText, int backScreen) {
        const int before = slot;
        slot = kSlotBottom;
        if (backScreen != kScreenMenu) {
            addGoto(backText, backScreen, -1, "40%");
            addPlain(kRowDivider, nullptr, "20%");
        } else {
            addPlain(kRowDivider, nullptr, "60%");
        }
        addGoto("Schematica menu", kScreenMenu, -1, "40%");
        slot = before;
    };

    const auto addAct = [&](const char* text, int act, int arg, const char* width) {
        const long at = newRow(width);
        if (at < 0) {
            return;
        }
        std::snprintf(g_pgTexts[at], kPageTextBytes, "%s", text);
        g_pgRowAct[at] = act;
        g_pgRowArg[at] = arg;
        g_pgRowKind[at] = kRowButton;
    };

    const auto addPick = [&](const char* text, int act, int arg, bool selected,
                             const char* width) {
        const long at = newRow(width);
        if (at < 0) {
            return;
        }
        std::snprintf(g_pgTexts[at], kPageTextBytes, "%s", text);
        g_pgRowAct[at] = act;
        g_pgRowArg[at] = arg;
        g_pgRowKind[at] = kRowPick;
        g_pgRowSelected[at] = selected;
    };

    const auto addColumns = [&](const char* name, const char* count, bool header, bool iconCol,
                                bool hasIcon, std::int32_t icon, int nameW,
                                const char* stacks = nullptr, const char* col3 = nullptr,
                                const char* col4 = nullptr, const char* width = nullptr) {
        const long at = newRow(width);
        if (at < 0) {
            return;
        }
        g_pgCols[at][0][0] = 0;
        g_pgCols[at][1][0] = 0;
        const auto putText = [](char* dest, const char* src) {
            std::snprintf(dest, kPageTextBytes, "%s", (src != nullptr) ? src : "");
            if (dest[0] == '(') {
                dest[0] = '[';
            }
        };
        putText(g_pgTexts[at], name);
        putText(g_pgHints[at], count);
        if (stacks != nullptr && stacks[0] != 0) {
            putText(g_pgLabels[at], stacks);
        }
        if (col3 != nullptr && col3[0] != 0) {
            putText(g_pgCols[at][0], col3);
        }
        if (col4 != nullptr && col4[0] != 0) {
            putText(g_pgCols[at][1], col4);
        }
        g_pgRowArg[at] = header ? 1 : 0;
        g_pgRowKind[at] = kRowColumns;
        g_pgRowIconCol[at] = iconCol;
        g_pgRowHasIcon[at] = iconCol && hasIcon && !header;
        g_pgRowIcon[at] = icon;
        g_pgRowNameW[at] = nameW;
    };
    const auto nameWidthFor = [](size_t longest) {
        const int want = static_cast<int>(longest) * 6 + 24;
        return std::clamp(want, 96, 420);
    };

    const auto addListRow = [&](size_t which) {
        char name[kPageTextBytes]{};
        copyAscii(name, sizeof(name), mod.blueprintName(which).c_str());
        const bool shown = mod.blueprintVisible(which);

        beginRow(nullptr);
        {
            char label[kPageTextBytes]{};
            std::snprintf(label, sizeof(label), "%s%s", shown ? kColGreen : kColGray, name);
            addGoto(label, kScreenBlueprint, static_cast<int>(which), "52%");
        }
        {
            const long at = newRow("26%");
            if (at >= 0) {
                std::snprintf(g_pgTexts[at], kPageTextBytes, "%s",
                              shown ? "Placement: ON" : "Placement: OFF");
                g_pgRowAct[at] = kActShow;
                g_pgRowBlueprint[at] = static_cast<int>(which);
                g_pgRowKind[at] = kRowButton;
            }
        }
        {
            const MenuItem* const del = schematicaRowOf(1, 4);
            const std::wstring armed = (del != nullptr) ? del->valueText() : std::wstring{};
            char armedText[kPageTextBytes]{};
            copyAscii(armedText, sizeof(armedText), armed.c_str());
            const bool armedHere = (armed.rfind(L"press again: ", 0) == 0)
                                   && (std::strstr(armedText, name) != nullptr)
                                   && (name[0] != 0);
            const long at = newRow("20%");
            if (at >= 0) {
                std::snprintf(g_pgTexts[at], kPageTextBytes, "%s",
                              armedHere ? "Remove: sure?" : "Remove");
                g_pgRowAct[at] = kActItem;
                g_pgRowMenuTab[at] = 1;
                g_pgRowMenuIdx[at] = 4;
                g_pgRowBlueprint[at] = static_cast<int>(which);
                g_pgRowKind[at] = kRowButton;
            }
        }
        endRow();
        {
            int sx = 0;
            int sy = 0;
            int sz = 0;
            std::size_t solid = 0;
            char text[kPageTextBytes]{};
            if (mod.blueprintInfo(which, sx, sy, sz, solid)) {
                std::snprintf(text, sizeof(text), "    %d, %d, %d   [%d x %d x %d]",
                              mod.blueprintPos(which, 0), mod.blueprintPos(which, 1),
                              mod.blueprintPos(which, 2), sx, sy, sz);
            } else {
                std::snprintf(text, sizeof(text), "    %d, %d, %d   [not loaded yet]",
                              mod.blueprintPos(which, 0), mod.blueprintPos(which, 1),
                              mod.blueprintPos(which, 2));
            }
            addPlain(kRowLabel, text);
        }
    };

    const int screen = g_pgScreen.load(std::memory_order_relaxed);
    switch (screen) {
    case kScreenBlueprints: {
        addTabStrip(kMainTabs, std::size(kMainTabs), kScreenBlueprints);
        const size_t count = mod.blueprintCount();
        for (size_t i = 0; i < count && rows + 6 < kMaxPageRows; ++i) {
            addListRow(i);
        }
        if (count == 0) {
            addPlain(kRowLabel, "No blueprints: use Import .mcstructure");
        }
        addPlain(kRowDivider, nullptr);
        addItem(0, 0);
        addBottomNav("< Schematica menu", kScreenMenu);
        break;
    }
    case kScreenBlueprint: {
        addTabStrip(kPlaceTabs, std::size(kPlaceTabs), kScreenBlueprint);
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
        addPlain(kRowHeader, "Placement");
        addItem(1, 0);
        beginRow(nullptr);
        addItem(1, 5, "50%");
        addItemAs(1, 6, "50%", true);
        endRow();
        addPlain(kRowHeader, "Placement origin");
        beginRow("100%c");
        addItem(1, 1, "33%");
        addItem(1, 2, "33%");
        addItem(1, 3, "33%");
        endRow();
        addPlain(kRowDivider, nullptr);
        addItem(1, 4);
        addBottomNav("< Placements", kScreenBlueprints);
        break;
    }
    case kScreenMaterials: {
        addTabStrip(kPlaceTabs, std::size(kPlaceTabs), kScreenMaterials);
        const int at = mod.editingIndex();
        {
            static constexpr const char* kListNames[Schematica::kMaterialListTypeCount] = {
                "List: All", "List: Missing"};
            char clear[kPageTextBytes]{};
            const std::size_t ignored = mod.ignoredMaterialCount();
            std::snprintf(clear, sizeof(clear), "Clear ignored (%u)",
                          static_cast<unsigned>(ignored));
            beginRow(nullptr);
            addAct(kListNames[std::clamp(mod.materialListType(), 0,
                                         Schematica::kMaterialListTypeCount - 1)],
                   kActMatList, 0, "26%");
            addAct("Refresh", kActMatRefresh, 0, "20%");
            addAct("Write to file", kActMatWrite, 0, "28%");
            addAct(clear, kActMatClear, 0, "26%");
            endRow();
        }
        std::size_t kinds = 0;
        const size_t left = (kMaxPageRows > rows + 10) ? (kMaxPageRows - rows - 10) : 0;
        const size_t room = left / 3;
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
            if (shown.size() > 30) {
                shown = shown.substr(0, 27) + "...";
            }
            longest = std::max(longest, shown.size());
            names.push_back(std::move(shown));
        }
        const int nameW = nameWidthFor(longest);
        {
            beginRow("19px");
            addColumns("Item", "Total", true, true, false, 0, nameW, "SB+stacks", "Missing",
                       "Avail", "88%");
            addPlain(kRowDivider, nullptr, "12%");
            endRow();
        }
        if (at < 0) {
            addPlain(kRowLabel, "- pick a schematic first");
        } else if (list.empty()) {
            addPlain(kRowLabel, (mod.materialListType() == Schematica::kMaterialMissing)
                                    ? "- nothing missing (or nothing counted yet)"
                                    : "- nothing loaded yet: turn Show on once");
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
            char missing[24]{};
            if (!list[i].missingKnown) {
                std::snprintf(missing, sizeof(missing), "%s-", kColGray);
            } else if (list[i].missing == 0) {
                std::snprintf(missing, sizeof(missing), "%s0", kColGreen);
            } else {
                std::snprintf(missing, sizeof(missing), "%s%u", kColAqua,
                              static_cast<unsigned>(list[i].missing));
            }
            char avail[24]{};
            if (!list[i].availableKnown) {
                std::snprintf(avail, sizeof(avail), "%s-", kColGray);
            } else {
                const bool enough =
                    !list[i].missingKnown || list[i].available >= list[i].missing;
                std::snprintf(avail, sizeof(avail), "%s%u", enough ? kColGreen : kColRed,
                              static_cast<unsigned>(list[i].available));
            }
            beginRow("19px");
            addColumns(names[i].c_str(), count, false, true, list[i].hasIcon,
                       list[i].iconIdAux, nameW, stacks.c_str(), missing, avail, "88%");
            addAct("X", kActMatIgnore, 0, "12%");
            if (rows > 0 && g_pgRowAct[rows - 1] == kActMatIgnore) {
                std::snprintf(g_pgHints[rows - 1], kPageTextBytes, "%s", rawName);
            }
            endRow();
        }
        if (kinds > list.size()) {
            char more[kPageTextBytes]{};
            std::snprintf(more, sizeof(more), "... and %u more (not shown)",
                          static_cast<unsigned>(kinds - list.size()));
            addPlain(kRowLabel, more);
        }
        addBottomNav("< Configure", kScreenBlueprint);
        break;
    }
    case kScreenVerifier: {
        addTabStrip(kPlaceTabs, std::size(kPlaceTabs), kScreenVerifier);
        std::size_t tally[blocks::kDiffKindCount]{};
        mod.diffTally(tally);
        std::size_t seen = 0;
        for (const std::size_t one : tally) {
            seen += one;
        }
        const struct {
            const char* label;
            std::size_t at;
            const char* color;
        } kRowsOf[] = {
            {"Correct", 0, kColGreen},      {"Missing", 1, kColAqua},
            {"Extra", 6, kColPurple},       {"Wrong block", 2, kColRed},
            {"Wrong state", 3, kColGold},   {"World not read", 4, kColGray},
            {"State unresolved", 5, kColGray},
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
            char label[kPageTextBytes]{};
            char count[24]{};
            std::snprintf(label, sizeof(label), "%s%s", one.color, one.label);
            std::snprintf(count, sizeof(count), "%s%u", one.color,
                          static_cast<unsigned>(tally[one.at]));
            addColumns(label, count, false, false, false, 0, nameW);
        }
        addBottomNav("< Configure", kScreenBlueprint);
        break;
    }
    case kScreenSettings: {
        addTabStrip(kMainTabs, std::size(kMainTabs), kScreenSettings);
        addPlain(kRowHeader, "Visuals");
        for (size_t i = 1; i < kMaxPageRows; ++i) {
            if (!addItem(0, i)) {
                break;
            }
        }
        addBottomNav("< Schematica menu", kScreenMenu);
        break;
    }
    case kScreenLayers: {
        addTabStrip(kMainTabs, std::size(kMainTabs), kScreenLayers);
        const int layerMode = mod.layerMode();
        addPlain(kRowHeader, "Render layer");
        {
            static constexpr const char* kModeText[Schematica::kLayerModeCount] = {
                "All", "Single", "Below", "Above", "Range"};
            beginRow(nullptr);
            for (int m = 0; m < Schematica::kLayerModeCount; ++m) {
                addPick(kModeText[m], kActLayerMode, m, m == layerMode, "20%");
            }
            endRow();
        }
        {
            static constexpr const char* kAxisText[3] = {"X", "Y", "Z"};
            const int axisNow = mod.layerAxis();
            beginRow(nullptr);
            addPlain(kRowLabel, "Axis", "40%");
            for (int a = 0; a < 3; ++a) {
                addPick(kAxisText[a], kActLayerAxis, a, a == axisNow, "20%");
            }
            endRow();
        }
        if (layerMode == Schematica::kLayerSingle || layerMode == Schematica::kLayerBelow
            || layerMode == Schematica::kLayerAbove) {
            addItem(2, 2);
        } else if (layerMode == Schematica::kLayerRange) {
            beginRow("100%c");
            addItem(2, 3, "50%");
            addItem(2, 4, "50%");
            endRow();
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
                if (value <= -kFarShown || value >= kFarShown) {
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
            beginRow(nullptr);
            addItemAs(2, 5, "34%", true);
            addItem(2, 6, "33%");
            addItem(2, 7, "33%");
            endRow();
        }
        addBottomNav("< Schematica menu", kScreenMenu);
        break;
    }
    default: {
        char placements[kPageTextBytes]{};
        std::snprintf(placements, sizeof(placements), "Schematic Placements (%u)",
                      static_cast<unsigned>(mod.blueprintCount()));
        beginRow(nullptr);
        addGoto(placements, kScreenBlueprints, -1, "50%");
        addItem(0, 0, "50%");
        endRow();
        static constexpr const char* kModeNames[Schematica::kLayerModeCount] = {
            "All", "Single layer", "All below", "All above", "Layer range"};
        char layers[kPageTextBytes]{};
        std::snprintf(layers, sizeof(layers), "Render Layers: %s",
                      kModeNames[std::clamp(mod.layerMode(), 0,
                                            Schematica::kLayerModeCount - 1)]);
        beginRow(nullptr);
        addGoto("Configuration menu", kScreenSettings, -1, "50%");
        addGoto(layers, kScreenLayers, -1, "50%");
        endRow();
        addPlain(kRowDivider, nullptr);
        {
            const size_t count = mod.blueprintCount();
            size_t shown = 0;
            for (size_t i = 0; i < count; ++i) {
                shown += mod.blueprintVisible(i) ? 1 : 0;
            }
            char left[kPageTextBytes]{};
            char right[kPageTextBytes]{};
            std::snprintf(left, sizeof(left), "Schematics: %u", static_cast<unsigned>(count));
            std::snprintf(right, sizeof(right), "Shown: %u", static_cast<unsigned>(shown));
            beginRow("14px");
            addPlain(kRowLabel, left, "50%");
            addPlain(kRowLabel, right, "50%");
            endRow();
        }
        break;
    }
    }
    endRow();

    if (g_pgRowsDropped > 0) {
        log().warn(L"UiProbe: the page ran out of row slots ({} row(s) dropped, cap {})",
                   g_pgRowsDropped, kMaxPageRows);
    }

    {
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

int pageTabRowFromKey(const char* key)
{
    constexpr char kPrefix[] = "howtoplay.tk_pg";
    constexpr size_t kLen = sizeof(kPrefix) - 1;
    if (key == nullptr || std::strncmp(key, kPrefix, kLen) != 0) {
        return -1;
    }
    int row = -1;
    for (size_t d = kLen; key[d] >= '0' && key[d] <= '9'; ++d) {
        row = (row < 0 ? 0 : row) * 10 + (key[d] - '0');
    }
    if (row < 0 || static_cast<size_t>(row) >= kMaxPageRows) {
        return -1;
    }
    return (g_pgRowKind[row] == kRowTab) ? row : -1;
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

constexpr char kCountColW[] = "72px";
constexpr char kStackColW[] = "108px";
constexpr char kRowLineTexture[] = "textures/ui/list_item_divider_line_light";
constexpr std::int64_t kIconLayer = 5;

const char* pageRowWidthOf(size_t i);

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
    if (g_pgCols[i][0][0] != 0) {
        row.kids.push_back(cell("c3", g_pgCols[i][0], kCountColW, true));
    }
    if (g_pgCols[i][1][0] != 0) {
        row.kids.push_back(cell("c4", g_pgCols[i][1], kCountColW, true));
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
    outer.overs.push_back(overPair("size", pageRowWidthOf(i), outerH));
    outer.kids.push_back(std::move(row));
    outer.kids.push_back(std::move(line));
    return outer;
}

Entry g_pgTgTrueRec{};
Entry g_pgTgFalseRec{};
bool g_pgTgRecsReady = false;

const char* pageRowWidthOf(size_t i)
{
    return (g_pgRowWidth[i][0] != 0) ? g_pgRowWidth[i] : kPageRowWidth;
}

std::string pageBorrowedDef(int kind)
{
    switch (kind) {
    case kRowSlider: return "settings_common.option_slider";
    case kRowToggle: return "settings_common.option_toggle";
    default:         return "settings_common.option_text_edit";
    }
}

Part buildPageRowPart(size_t i, size_t rows, bool iconsUsable)
{
    std::snprintf(g_pgKeys[i], kPageKeyBytes, "tk_pg%u", static_cast<unsigned>(i));
    const char* const width = pageRowWidthOf(i);
    const bool hasHeight = (g_pgRowHeight[i][0] != 0);

    if (g_pgRowKids[i] > 0) {
        Part row;
        row.name = g_pgKeys[i];
        row.def = "common.horizontal_stack_panel";
        row.overs.push_back(
            overPair("size", width, hasHeight ? g_pgRowHeight[i] : kPageRowHeight));
        for (int k = 1; k <= g_pgRowKids[i]; ++k) {
            const size_t at = i + static_cast<size_t>(k);
            if (at >= rows) {
                break;
            }
            row.kids.push_back(buildPageRowPart(at, rows, iconsUsable));
        }
        return row;
    }

    switch (g_pgRowKind[i]) {
    case kRowColumns:
        return buildColumnsRow(i, iconsUsable);

    case kRowTab: {
        Part p;
        p.name = g_pgKeys[i];
        p.def = kTabRowDef;
        p.overs.push_back(overText(kTabTopicVar, g_pgKeys[i]));
        const int forced = (g_pgRowTabIndex[i] == g_pgTabSelected)
                               ? 0
                               : (g_pgRowTabIndex[i] + 1);
        p.overs.push_back(overInt(kTabForcedVar, forced));
        p.overs.push_back(overPair("size", width, kTabRowHeight));
        return p;
    }

    case kRowHeader: {
        Part label;
        label.name = "t";
        label.def = "common.single_line_label";
        label.overs.push_back(overText("$single_line_label_text", g_pgTexts[i]));
        label.overs.push_back(overPair("$size", "100%", "13px"));

        Part line;
        line.name = "ln";
        line.def = "common.dialog_divider";
        line.overs.push_back(overPair("size", "100%", "1px"));
        line.overs.push_back(overText("texture", kRowLineTexture));
        line.overs.push_back(overInt("layer", kIconLayer));

        Part p;
        p.name = g_pgKeys[i];
        p.def = "common.vertical_stack_panel";
        p.overs.push_back(overPair("size", width, hasHeight ? g_pgRowHeight[i] : "18px"));
        p.kids.push_back(std::move(label));
        p.kids.push_back(std::move(line));
        return p;
    }

    case kRowDivider: {
        Part p;
        p.name = g_pgKeys[i];
        p.def = "common.empty_panel";
        p.overs.push_back(overPair("size", width, hasHeight ? g_pgRowHeight[i] : "8px"));
        return p;
    }

    case kRowLabel: {
        Part p;
        p.name = g_pgKeys[i];
        p.def = "common.single_line_label";
        p.overs.push_back(overText("$single_line_label_text", g_pgTexts[i]));
        p.overs.push_back(overPair("$size", width, hasHeight ? g_pgRowHeight[i] : "14px"));
        return p;
    }

    case kRowEdit:
    case kRowSlider:
    case kRowToggle: {
        std::snprintf(g_pgCtlNames[i], kPageKeyBytes, "settings_common.tk_pg%u",
                      static_cast<unsigned>(i));
        std::snprintf(g_pgBindNames[i], sizeof(g_pgBindNames[i]), "#tk_pg_%u",
                      static_cast<unsigned>(i));
        Part ctl;
        ctl.name = g_pgKeys[i];
        ctl.def = pageBorrowedDef(g_pgRowKind[i]);
        ctl.overs.push_back(overText(kSecCtlNameVar, g_pgCtlNames[i]));
        ctl.overs.push_back(overText(kSecBindNameVar, g_pgBindNames[i]));
        ctl.overs.push_back(overText(kSecLabelVar, g_pgLabels[i]));
        ctl.overs.push_back(overText(kNumHintVar, g_pgHints[i]));
        if (g_pgRowKind[i] == kRowToggle) {
            const MenuItem* const item = schematicaRow(i);
            const bool on = (item != nullptr) && item->toggleState();
            ctl.overs.push_back(overText(kTgTypeVar, on ? kTgTypeNone : kTgTypeGlobal));
            if (g_pgTgRecsReady) {
                ctl.overs.push_back(overRaw(kTgDefaultVar, on ? g_pgTgTrueRec : g_pgTgFalseRec));
            }
            ctl.overs.push_back(overText(kTgBindVar, kTgNoBind));
        }
        if (!g_pgRowChild[i]) {
            return ctl;
        }
        Part wrap;
        wrap.name = "hold";
        wrap.def = "common.empty_panel";
        wrap.overs.push_back(overPair("size", width, hasHeight ? g_pgRowHeight[i] : "100%c"));
        wrap.kids.push_back(std::move(ctl));
        return wrap;
    }

    default: {
        Part p;
        p.name = g_pgKeys[i];
        p.def = (g_pgRowKind[i] == kRowPick && !g_pgRowSelected[i])
                    ? "common_buttons.dark_text_button"
                    : "common_buttons.light_text_button";
        p.overs.push_back(overText(kOwnTextVar, g_pgTexts[i]));
        if (g_pgCloseOnPress.load()) {
            p.overs.push_back(overText(kPageExitVar, kPageExitId));
        }
        p.overs.push_back(
            overPair("size", width, hasHeight ? g_pgRowHeight[i] : kPageRowHeight));
        return p;
    }
    }
}

void* buildSchematicaPage(void* self)
{
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
        || !memory::isReadable(base, uitree::kValueWords * sizeof(std::uintptr_t))) {
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
    }
    g_pgTgRecsReady = false;
    if (tgTrue != nullptr && tgFalse != nullptr) {
        g_pgTgTrueRec = *tgTrue;
        g_pgTgFalseRec = *tgFalse;
        g_pgTgRecsReady = true;
    }

    size_t iconRows = 0;
    g_pgTopCount = 0;
    Part topBar;
    topBar.name = "tk_bar0";
    topBar.def = "common.horizontal_stack_panel";
    topBar.overs.push_back(overPair("size", "100%", kTabRowHeight));
    topBar.overs.push_back(overInt(kTabIndexVar, 0));
    Part bottomBar;
    bottomBar.name = "tk_bar1";
    bottomBar.def = "common.horizontal_stack_panel";
    bottomBar.overs.push_back(overPair("size", "100%", kPageRowHeight));
    for (size_t i = 0; i < rows; ++i) {
        if (g_pgRowChild[i]) {
            continue;
        }
        if (g_pgRowKind[i] == kRowColumns && g_pgRowHasIcon[i] && iconsUsable) {
            ++iconRows;
        }
        Part part = buildPageRowPart(i, rows, iconsUsable);
        if (g_pgRowSlot[i] == kSlotTop) {
            topBar.kids.push_back(std::move(part));
            continue;
        }
        if (g_pgRowSlot[i] == kSlotBottom) {
            bottomBar.kids.push_back(std::move(part));
            continue;
        }
        std::uintptr_t* const elem = buildPartElement(g_pageArena, g_pgDonors, part);
        if (elem == nullptr) {
            g_pgRowsDropped += rows - i;
            log().warn(L"UiProbe: the page ran out of room at row {} ({}) - {} row(s) dropped "
                       L"(arena {}/{} nodes, {}/{} words, {}/{} text)",
                       i, toUtf16(g_pgTexts[i]), rows - i, g_pageArena.usedNodes(),
                       g_pageArena.capacityNodes(), g_pageArena.usedWords(),
                       g_pageArena.capacityWords(), g_pageArena.usedText(),
                       g_pageArena.capacityText());
            break;
        }
        g_pgElems[i] = reinterpret_cast<std::uintptr_t>(elem);
        g_pgTopElems[g_pgTopCount++] = g_pgElems[i];
    }
    const size_t topRows = topBar.kids.size();
    const size_t bottomRows = bottomBar.kids.size();
    (void)iconRows;
    std::memcpy(g_pgVec, srcVec, sizeof(g_pgVec));
    g_pgVec[0] = reinterpret_cast<std::uintptr_t>(&g_pgTopElems[0]);
    g_pgVec[1] = reinterpret_cast<std::uintptr_t>(&g_pgTopElems[g_pgTopCount]);
    g_pgVec[2] = reinterpret_cast<std::uintptr_t>(&g_pgTopElems[g_pgTopCount]);

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

    std::vector<Part> stackKids;
    if (topRows > 0) {
        stackKids.push_back(std::move(topBar));
    }
    {
        Part scroll;
        scroll.name = "tk_col0";
        if (g_ptListReady) {
            scroll.def = "common.scrolling_panel";
            scroll.overs.push_back(overText(kPtListVar, kPtListName));
            scroll.overs.push_back(overPair(kPtScrollSizeVar, "5px", "100% - 4px"));
        } else {
            scroll.def = "common.vertical_stack_panel";
            scroll.overs.push_back(overRawValue("controls",
                                                reinterpret_cast<std::uintptr_t>(g_pgVec),
                                                controls->tag, controls->seq));
        }
        scroll.overs.push_back(overPair("size", "100%", "fill"));
        stackKids.push_back(std::move(scroll));
    }
    if (bottomRows > 0) {
        stackKids.push_back(std::move(bottomBar));
    }
    std::uintptr_t* const page = buildPartDefinition(
        self, g_pageArena, g_pgDonors, "common", "vertical_stack_panel",
        {overPair("offset", g_pgBodyX, kPageInsetY),
         overPair("size", g_pgBodyW, kPageBodyHeight)},
        stackKids);
    if (page == nullptr) {
        log().warn(L"UiProbe: could not build the page body (arena overflow = {})",
                   g_pageArena.overflowed() ? 1 : 0);
        return nullptr;
    }
    g_pgRoot = page;
    g_pgReady = true;
    log().info(L"UiProbe: built the Schematica page (screen {}, {} row(s): {} top / {} body / "
               L"{} bottom, arena {}/{} nodes, {}/{} words, {}/{} text)",
               g_pgScreen.load(), rows, topRows, g_pgTopCount, bottomRows,
               g_pageArena.usedNodes(), g_pageArena.capacityNodes(), g_pageArena.usedWords(),
               g_pageArena.capacityWords(), g_pageArena.usedText(),
               g_pageArena.capacityText());
    return page;
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
    return substituteOwnPage(self, space, name);
}

constexpr std::ptrdiff_t kControlName = 0x20;

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

constexpr size_t kRowEntrySize = 0x40;
constexpr size_t kMaxRowEntries = 256;

constexpr size_t kMaxOwnKeyRows = 128;

struct OwnKeyRow {
    char id[16]{};
    char label[64]{};
    int keyCode = 0;
    int lastSeen = 0;
    int defaultKey = 0;
    int module = -1;
    int child = -1;
    bool hidden = false;
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
            row.hidden = tab.hidden || one.hidden;
            const std::vector<int> keys = one.getKeys();
            row.keyCode = keys.empty() ? 0 : keys.front();
            row.lastSeen = row.keyCode;
            row.defaultKey = one.defaultKeys.empty() ? 0 : one.defaultKeys.front();
            ++g_ownKeyRowCount;
        }
    }
    if (g_ownKeyRowCount >= kMaxOwnKeyRows) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            log().warn(L"UiProbe: own key rows hit the limit ({}) - later modules lose their keys",
                       kMaxOwnKeyRows);
        }
    }
    return g_ownKeyRowCount;
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
        if (g_ownKeyRows[r].hidden) {
            continue;
        }
        picked[pickCount++] = r;
    }
    const size_t keep = count;
    if (pickCount == 0 || keep + pickCount > kMaxRowEntries) {
        if (pickCount != 0) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) {
                log().warn(L"UiProbe: {} vanilla + {} own key rows exceed the limit ({}) - no own "
                           L"key rows are shown", keep, pickCount, kMaxRowEntries);
            }
        }
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
        markSettingsDirty();
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
    if (const int tabRow = pageTabRowFromKey(buf); tabRow >= 0) {
        text = g_pgTexts[tabRow];
    } else if (const int leftRow = pageLeftRowFromKey(buf); leftRow >= 0) {
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

    return true;
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

constexpr unsigned kMouseEventType = 0xb447f8e7u;
constexpr unsigned kKeyEventType = 0xf9fbc001u;

void onUiEvent(void*, const void* event)
{
    if (event == nullptr || !memory::isReadable(event, 0x40)) {
        return;
    }

    unsigned kind = 0;
    std::memcpy(&kind, event, sizeof(kind));
    if (kind == 1) {
        unsigned char act = 0;
        std::memcpy(&act, reinterpret_cast<const char*>(event) + 0x10, sizeof(act));
        if (act != 2) {
            return;
        }
        float x = 0.0f;
        float y = 0.0f;
        std::memcpy(&x, reinterpret_cast<const char*>(event) + 0x20, sizeof(x));
        std::memcpy(&y, reinterpret_cast<const char*>(event) + 0x24, sizeof(y));

        std::uintptr_t p38 = 0;
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
        const std::wstring hitName =
            (p38 != 0) ? readString(reinterpret_cast<const char*>(p38) + kControlName)
                       : std::wstring();
        const bool isPageRow = (hitName.find(L"tk_pg") != std::wstring::npos)
                               || (hitName.find(L"tk_pt") != std::wstring::npos);
        if (duplicate && !isPageRow) {
            return;
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

constexpr size_t kMaxOwnItems = 1024;
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
constexpr std::ptrdiff_t kCompVisibleFnEngaged = 0x138;
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
    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
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
    TSUKUYOMI_HOOK_COUNT("PeekMessageW");
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
    return result;
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
        std::memset(dest + kCompVisibleFn, 0, kCompVisibleFnEngaged - kCompVisibleFn);
        dest[kCompVisibleFnEngaged] = 0;
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
        }
        dest[kCompActionConfirm] = 0;
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
    return value ? 1u : 0u;
}

void __fastcall ownBoolSet(void* self, unsigned value)
{
    const int index = indexOfProvider(self);
    if (index < 0) {
        return;
    }
    const bool want = (value & 1u) != 0;
    if (want != ownItemValue(index)) {
        setOwnPending(index, want ? 1.0f : 0.0f);
        ownItemRequestToggle(index);
        publishOwnComponent(ownItemComponent(index));
    }
}

void* __fastcall ownProviderNoop(void*, void*, void*, void*) { return nullptr; }

bool ownItemAvailable(int index);
int indexOfProvider(const void* self);

const unsigned char* ownItemComponent(int index);

unsigned char __fastcall ownProviderAvailable(void* self)
{
    markGameThread();
    flushOwnPublishes();
    const int index = indexOfProvider(self);
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
    setOwnPending(index, value);
    ownItemSetNumber(index, value);
}

constexpr std::ptrdiff_t kOptionProviderList = 0xD8;

constexpr size_t kOptionStride = 0xA8;
constexpr size_t kOptionLabelCopy = 0x100;
constexpr std::ptrdiff_t kOptionValue = 0x00;
constexpr std::ptrdiff_t kOptionLabelPtr = 0x40;
constexpr size_t kMaxChoices = 16;
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
    return out;
}

int __fastcall ownOptionGet(void* self)
{
    flushOwnPublishes();
    const int index = indexOfProvider(self);
    float pending = 0.0f;
    const int at = ownPending(index, pending) ? static_cast<int>(pending) : ownItemChoice(index);
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

struct TextSettle {
    std::atomic<int> index{-1};
    std::atomic<bool> read{false};
    std::atomic<unsigned long long> readAt{0};
    unsigned long long startedAt = 0;
    char typed[kOwnTextMax]{};
};
TextSettle g_textSettle;
constexpr unsigned long long kTextSettleGapMs = 150;
constexpr unsigned long long kTextSettleGiveUpMs = 1500;

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
    const bool pending = ownPendingText(index, text, sizeof(text));
    if (!pending) {
        ownItemText(index, text, sizeof(text));
    }
    if (pending && index >= 0 && index == g_textSettle.index.load() && !g_textSettle.read.load()) {
        g_textSettle.readAt.store(GetTickCount64());
        g_textSettle.read.store(true);
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

std::atomic<int> g_ownTextCommit{-1};

void __fastcall ownTextCommit(void* self)
{
    noteTextSlot(5);
    if (const int index = indexOfProvider(self); index >= 0) {
        g_ownTextCommit.store(index);
    }
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
        for (size_t at = 0; text[at] != L'\0' && n + 1 < cap; ++at) {
            const wchar_t ch = text[at];
            if (ch == static_cast<wchar_t>(0x00A7)) {
                if (n + 3 > cap) {
                    break;
                }
                dest[n++] = static_cast<char>(0xC2);
                dest[n++] = static_cast<char>(0xA7);
                continue;
            }
            dest[n++] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
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
        if (child.hidden) {
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
        requestPageRebuild(g_pgRowKind[idx] == kRowTab);
        return;
    }
    if (g_pgRowAct[idx] == kActNone) {
        return;
    }
    if (g_pgRowAct[idx] >= kActMatList && g_pgRowAct[idx] <= kActMatIgnore) {
        Schematica& mod = Schematica::instance();
        switch (g_pgRowAct[idx]) {
        case kActMatList:
            mod.cycleMaterialListType();
            break;
        case kActMatRefresh:
            mod.askMaterialRefresh();
            break;
        case kActMatWrite: {
            const int which = mod.editingIndex();
            std::wstring path;
            if (which >= 0 && mod.writeMaterialList(static_cast<size_t>(which), path)) {
                log().success(L"UiProbe: wrote the material list to {}", path);
            } else {
                log().warn(L"UiProbe: could not write the material list");
            }
            break;
        }
        case kActMatClear:
            mod.clearIgnoredMaterials();
            break;
        case kActMatIgnore: {
            if (g_pgHints[idx][0] != 0) {
                mod.toggleMaterialIgnored(toUtf16(g_pgHints[idx]));
            }
            break;
        }
        default:
            break;
        }
        requestPageRebuild(false);
        return;
    }
    if (g_pgRowAct[idx] == kActLayerMode || g_pgRowAct[idx] == kActLayerAxis) {
        Schematica& mod = Schematica::instance();
        const bool isMode = (g_pgRowAct[idx] == kActLayerMode);
        const int before = isMode ? mod.layerMode() : mod.layerAxis();
        if (before == g_pgRowArg[idx]) {
            return;
        }
        if (isMode) {
            mod.setLayerMode(g_pgRowArg[idx]);
        } else {
            mod.setLayerAxis(g_pgRowArg[idx]);
        }
        log().info(L"UiProbe: render layer {} {} -> {}", isMode ? L"mode" : L"axis", before,
                   g_pgRowArg[idx]);
        requestPageRebuild(false);
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
    if (item.choices.size() > kMaxChoices) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            log().warn(L"UiProbe: \"{}\" has {} options but only {} fit (raise kMaxChoices)",
                       item.labelText(), item.choices.size(), kMaxChoices);
        }
    }

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

}

bool g_keyRowMade = false;
int g_keyRowCount = 0;

constexpr int kKeysModule = -2;

constexpr int kJsonTabModule = -3;
constexpr char kOwnTabId[] = "tkschem-jsonui";
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
    const auto skipKeyRowNumber = [](const MenuItem& child) {
        if (kVanillaKeyRows && !child.onPage && child.kind == MenuItemKind::Keybind) {
            ++g_keyRowCount;
        }
    };
    for (size_t i = 0; i < g_ownMenuTree.size() && count < kMaxOwnItems; ++i) {
        MenuItem& tabSource = g_ownMenuTree[i];
        if (tabSource.hidden) {
            for (const MenuItem& child : tabSource.children) {
                skipKeyRowNumber(child);
            }
            continue;
        }
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
            if (child.hidden) {
                skipKeyRowNumber(child);
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
                if (!child.opensPage && !child.activate) {
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
    g_textSettle.index.store(-1);
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
    static const bool kContentsBlocked =
        !writes::allowed("SettingsProviderCall") || !writes::allowed("SettingsFindComponent");
    if (kContentsBlocked) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            log().warn(L"UiProbe: our settings tab is not added (SettingsProviderCall or "
                       L"SettingsFindComponent is turned off in hooks.json)");
        }
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
        return;
    }
    writeSsoString(at + kCompNameKey, item.nameKey, keyLength);
    writeSsoString(at + kCompActionLabel, valueKey, keyLength + 1);
    g_navComp.store(comp);
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
            if (index >= 0 && ensureOwnItem(static_cast<size_t>(index))) {
                own = g_ownItems[static_cast<size_t>(index)].comp;

            }
        }
        if (own != nullptr) {
            const auto value = reinterpret_cast<std::uintptr_t>(own);
            std::memcpy(out, &value, sizeof(value));
            *(static_cast<unsigned char*>(out) + 8) = 1;
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
        } else if (source != nullptr && source->kind == MenuItemKind::Action) {
            if (source->isAvailable() && source->activate) {
                source->activate();
                g_settingsDirty.store(true);
                requestOwnPublishWithTab(requested);
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
        }
        return;
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
    const int committed = g_ownTextCommit.exchange(-1);
    const int index = g_ownTextRequest.exchange(-1);
    if (index >= 0) {
        MenuItem* const source = ownMenuItem(index);
        if (source != nullptr && source->setText && !source->textOnCommit) {
            char text[kOwnTextMax]{};
            {
                const std::lock_guard<std::mutex> lock(g_ownTextValueMutex);
                std::snprintf(text, sizeof(text), "%s", g_ownTextValue);
            }
            source->setText(toUtf16(text));
            g_settingsDirty.store(true);
        }
    }
    if (const int settling = g_textSettle.index.load(); settling >= 0) {
        const unsigned long long now = GetTickCount64();
        const bool readEnough =
            g_textSettle.read.load() && now - g_textSettle.readAt.load() >= kTextSettleGapMs;
        if (readEnough || now - g_textSettle.startedAt >= kTextSettleGiveUpMs) {
            g_textSettle.index.store(-1);
            clearOwnPendingText(settling);
            requestOwnPublishWithTab(settling);
        }
    }
    if (committed >= 0) {
        char typed[kOwnTextMax]{};
        const bool hadTyped = ownPendingText(committed, typed, sizeof(typed));
        const bool alreadySettling = hadTyped && g_textSettle.index.load() == committed
                                     && std::strcmp(typed, g_textSettle.typed) == 0;
        MenuItem* const source = ownMenuItem(committed);
        if (hadTyped && !alreadySettling && source != nullptr && source->setText
            && source->textOnCommit) {
            source->setText(toUtf16(typed));
            g_settingsDirty.store(true);
        }
        char held[kOwnTextMax]{};
        ownItemText(committed, held, sizeof(held));
        if (alreadySettling) {
        } else if (hadTyped && std::strcmp(typed, held) != 0) {
            if (const int prev = g_textSettle.index.exchange(-1); prev >= 0 && prev != committed) {
                clearOwnPendingText(prev);
                requestOwnPublishWithTab(prev);
            }
            std::snprintf(g_textSettle.typed, sizeof(g_textSettle.typed), "%s", typed);
            g_textSettle.read.store(false);
            g_textSettle.startedAt = GetTickCount64();
            g_textSettle.index.store(committed);
        } else {
            clearOwnPendingText(committed);
        }
        requestOwnPublishWithTab(committed);
    }
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
            log().warn(L"UiProbe:   touched address {:#x} ({})",
                       info->ExceptionRecord->ExceptionInformation[1],
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
            log().warn(L"UiProbe:   rax={:#x} rcx={:#x} rdx={:#x} r8={:#x} r9={:#x}",
                       ctx->Rax, ctx->Rcx, ctx->Rdx,
                       ctx->R8, ctx->R9);
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

void markSettingsDirty() { g_settingsDirty.store(true); }

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
    pumpPagePress();
    pumpToggleState();
    pumpPageReopen();
    pumpPageEntry();
    reportSettingsGroups();

}

namespace {

struct DefExtension {
    std::string space;
    std::string name;
    nlohmann::json front;
    nlohmann::json back;
    std::vector<std::pair<std::string, nlohmann::json>> appends;
    const void* builtFor = nullptr;
    void* built = nullptr;
    int builds = 0;
};

std::mutex g_extMutex;
std::vector<DefExtension> g_extensions;
std::atomic<bool> g_extAny{false};

constexpr size_t kExtNodes = 524288;
constexpr size_t kExtWords = 2097152;
constexpr size_t kExtRecords = 524288;
constexpr size_t kExtText = 2097152;
uitree::TreeNode g_extNodes[kExtNodes]{};
std::uintptr_t g_extWords[kExtWords]{};
alignas(8) unsigned char g_extRecords[kExtRecords][uitree::kRecordBytes]{};
char g_extText[kExtText]{};
uitree::Arena g_extArena(g_extNodes, kExtNodes, g_extWords, kExtWords, &g_extRecords[0][0],
                         kExtRecords, g_extText, kExtText);
PageDonors g_extDonors;
Entry g_extTrue;
Entry g_extFalse;
bool g_extBools = false;

const char* internExtKey(const std::string& key)
{
    static std::set<std::string> keys;
    return keys.insert(key).first->c_str();
}

std::string pxText(const nlohmann::json& v)
{
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>()) + "px";
    }
    if (v.is_number_float()) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%gpx", v.get<double>());
        return buf;
    }
    return {};
}

bool jsonToOver(const char* key, const nlohmann::json& v, Over& out, std::string& err);

bool jsonToOvers(const nlohmann::json& obj, std::vector<Over>& out, std::string& err)
{
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        Over one;
        if (!jsonToOver(internExtKey(it.key()), it.value(), one, err)) {
            return false;
        }
        out.push_back(std::move(one));
    }
    return true;
}

bool jsonToOver(const char* key, const nlohmann::json& v, Over& out, std::string& err)
{
    if (v.is_string()) {
        out = overText(key, v.get<std::string>());
        return true;
    }
    if (v.is_boolean()) {
        if (!g_extBools) {
            err = std::string("no donor for a boolean: ") + key;
            return false;
        }
        out = overRaw(key, v.get<bool>() ? g_extTrue : g_extFalse);
        return true;
    }
    if (v.is_number_integer()) {
        out = overInt(key, v.get<long long>());
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (d == std::floor(d)) {
            out = overInt(key, static_cast<long long>(d));
            return true;
        }
        err = std::string("fractions are not supported: ") + key;
        return false;
    }
    if (v.is_array()) {
        const bool objects = !v.empty() && std::all_of(v.begin(), v.end(), [](const nlohmann::json& e) {
            return e.is_object();
        });
        if (objects) {
            Over o;
            o.key = key;
            o.kind = Over::Kind::Objects;
            for (const nlohmann::json& e : v) {
                std::vector<Over> row;
                if (!jsonToOvers(e, row, err)) {
                    return false;
                }
                o.rows.push_back(std::move(row));
            }
            out = std::move(o);
            return true;
        }
        if (v.size() == 2 && !v[0].is_object() && !v[1].is_object() && !v[0].is_array()
            && !v[1].is_array()) {
            out = overPair(key, pxText(v[0]), pxText(v[1]));
            return true;
        }
        err = std::string("unsupported array: ") + key;
        return false;
    }
    if (v.is_object()) {
        std::vector<Over> items;
        if (!jsonToOvers(v, items, err)) {
            return false;
        }
        out = overBag(key, std::move(items));
        return true;
    }
    err = std::string("unsupported value: ") + key;
    return false;
}

bool jsonToPart(const nlohmann::json& elem, Part& out, std::string& err)
{
    if (!elem.is_object() || elem.size() != 1) {
        err = "a control must be {\"name@def\": {...}}";
        return false;
    }
    const auto it = elem.begin();
    const std::string& head = it.key();
    const auto at = head.find('@');
    out.name = head.substr(0, at);
    out.def = (at == std::string::npos) ? std::string() : head.substr(at + 1);
    const nlohmann::json& props = it.value();
    if (!props.is_object()) {
        err = "the properties must be an object: " + head;
        return false;
    }
    for (auto p = props.begin(); p != props.end(); ++p) {
        if (p.key() == "controls") {
            if (!p.value().is_array()) {
                err = "controls must be an array: " + head;
                return false;
            }
            for (const nlohmann::json& kid : p.value()) {
                Part k;
                if (!jsonToPart(kid, k, err)) {
                    return false;
                }
                out.kids.push_back(std::move(k));
            }
            continue;
        }
        Over one;
        if (!jsonToOver(internExtKey(p.key()), p.value(), one, err)) {
            return false;
        }
        out.overs.push_back(std::move(one));
    }
    return true;
}

void collectExtBools(void* self)
{
    if (g_extBools) {
        return;
    }
    static const std::string kSpace = "common";
    static const std::string kName = "toggle";
    void* tg = nullptr;
    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!lookupGuarded(self, &kSpace, &kName, tg) || tg == nullptr
        || !collectEntries(tg, entries, count)) {
        return;
    }
    bool t = false;
    bool f = false;
    for (const Entry& one : entries) {
        if (one.keyText == toUtf16(kTgTrueKey)) {
            g_extTrue = one;
            t = true;
        } else if (one.keyText == toUtf16(kTgFalseKey)) {
            g_extFalse = one;
            f = true;
        }
    }
    g_extBools = t && f;
}

bool readVanillaVector(const Entry& entry, std::uintptr_t (&vecDonor)[uitree::kVectorWords],
                       std::vector<const std::uintptr_t*>& out, std::string& err)
{
    if (!memory::isReadable(reinterpret_cast<const void*>(entry.value), sizeof(vecDonor))) {
        err = "the vanilla array could not be read";
        return false;
    }
    std::memcpy(vecDonor, reinterpret_cast<const void*>(entry.value), sizeof(vecDonor));
    const auto* const first = reinterpret_cast<const std::uintptr_t*>(vecDonor[0]);
    const auto* const last = reinterpret_cast<const std::uintptr_t*>(vecDonor[1]);
    const std::ptrdiff_t n = last - first;
    if (n < 0 || n > 4096 || (n > 0 && !memory::isReadable(first, static_cast<size_t>(n) * 8))) {
        err = "the vanilla array looks wrong";
        return false;
    }
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        out.push_back(reinterpret_cast<const std::uintptr_t*>(first[i]));
    }
    return true;
}

const std::uintptr_t* buildObjectElement(const nlohmann::json& obj, std::string& err)
{
    std::vector<Over> row;
    if (!obj.is_object()) {
        err = "an appended element must be an object";
        return nullptr;
    }
    if (!jsonToOvers(obj, row, err)) {
        return nullptr;
    }
    std::vector<uitree::KeyValue> inner;
    inner.reserve(row.size());
    for (const Over& one : row) {
        uitree::KeyValue kv{};
        if (!fillOverride(g_extArena, g_extDonors, one, kv)) {
            err = "the arena ran out (appended element)";
            return nullptr;
        }
        inner.push_back(kv);
    }
    const std::uintptr_t* const node =
        g_extArena.makeMap(inner.empty() ? nullptr : inner.data(), inner.size());
    if (node == nullptr) {
        err = "the arena ran out (appended element)";
        return nullptr;
    }
    return g_extArena.makeValue(g_extDonors.elemVal, node);
}

void* buildExtension(void* self, const DefExtension& ext, void* vanilla, std::string& err)
{
    if (!collectPageDonors(self, g_extDonors)) {
        err = "the templates could not be collected";
        return nullptr;
    }
    collectExtBools(self);
    std::vector<Entry> entries;
    std::uintptr_t count = 0;
    if (!collectEntries(vanilla, entries, count) || entries.empty()) {
        err = "the vanilla definition could not be walked";
        return nullptr;
    }
    std::uintptr_t vanillaWords[uitree::kValueWords]{};
    if (!memory::isReadable(vanilla, sizeof(vanillaWords))) {
        err = "the vanilla value could not be read";
        return nullptr;
    }
    std::memcpy(vanillaWords, vanilla, sizeof(vanillaWords));

    std::vector<Part> front;
    std::vector<Part> back;
    for (const nlohmann::json& c : ext.front) {
        Part part;
        if (!jsonToPart(c, part, err)) {
            return nullptr;
        }
        front.push_back(std::move(part));
    }
    for (const nlohmann::json& c : ext.back) {
        Part part;
        if (!jsonToPart(c, part, err)) {
            return nullptr;
        }
        back.push_back(std::move(part));
    }

    const Entry* controls = nullptr;
    for (const Entry& one : entries) {
        if (one.keyText == L"controls") {
            controls = &one;
            break;
        }
    }
    const bool touchControls = !front.empty() || !back.empty();
    const std::uintptr_t* vec = nullptr;
    if (touchControls) {
        std::vector<const std::uintptr_t*> elements;
        for (const Part& part : front) {
            const std::uintptr_t* const e = buildPartElement(g_extArena, g_extDonors, part);
            if (e == nullptr) {
                err = "the arena ran out (front)";
                return nullptr;
            }
            elements.push_back(e);
        }
        std::uintptr_t vecDonor[uitree::kVectorWords]{};
        if (controls != nullptr) {
            if (!readVanillaVector(*controls, vecDonor, elements, err)) {
                return nullptr;
            }
        } else {
            std::memcpy(vecDonor, g_extDonors.vecSrc, sizeof(vecDonor));
        }
        for (const Part& part : back) {
            const std::uintptr_t* const e = buildPartElement(g_extArena, g_extDonors, part);
            if (e == nullptr) {
                err = "the arena ran out (back)";
                return nullptr;
            }
            elements.push_back(e);
        }
        vec = g_extArena.makeVector(vecDonor, elements.empty() ? nullptr : elements.data(),
                                    elements.size());
        if (vec == nullptr) {
            err = "the arena ran out (controls)";
            return nullptr;
        }
    }
    struct Appended {
        const Entry* entry = nullptr;
        const char* key = nullptr;
        const std::uintptr_t* vec = nullptr;
    };
    std::vector<Appended> appended;
    for (const auto& [key, list] : ext.appends) {
        Appended one;
        for (const Entry& e : entries) {
            if (e.keyText == toUtf16(key)) {
                one.entry = &e;
                break;
            }
        }
        std::vector<const std::uintptr_t*> elements;
        std::uintptr_t vecDonor[uitree::kVectorWords]{};
        if (one.entry != nullptr) {
            if (!readVanillaVector(*one.entry, vecDonor, elements, err)) {
                return nullptr;
            }
        } else {
            std::memcpy(vecDonor, g_extDonors.vecSrc, sizeof(vecDonor));
        }
        for (const nlohmann::json& obj : list) {
            const std::uintptr_t* const e = buildObjectElement(obj, err);
            if (e == nullptr) {
                return nullptr;
            }
            elements.push_back(e);
        }
        one.key = internExtKey(key);
        one.vec = g_extArena.makeVector(vecDonor, elements.empty() ? nullptr : elements.data(),
                                        elements.size());
        if (one.vec == nullptr) {
            err = "the arena ran out (appended array)";
            return nullptr;
        }
        appended.push_back(one);
    }
    std::vector<uitree::KeyValue> items;
    items.reserve(entries.size() + 1 + appended.size());
    for (const Entry& one : entries) {
        uitree::KeyValue kv{};
        kv.key = reinterpret_cast<const char*>(one.key);
        kv.value = (touchControls && &one == controls) ? reinterpret_cast<std::uintptr_t>(vec) : one.value;
        for (const Appended& a : appended) {
            if (a.entry == &one) {
                kv.value = reinterpret_cast<std::uintptr_t>(a.vec);
            }
        }
        kv.tag = one.tag;
        kv.seq = one.seq;
        items.push_back(kv);
    }
    if (touchControls && controls == nullptr) {
        uitree::KeyValue kv{};
        kv.key = "controls";
        kv.value = reinterpret_cast<std::uintptr_t>(vec);
        kv.tag = g_extDonors.arrTag;
        kv.seq = g_extDonors.arrSeq;
        items.push_back(kv);
    }
    for (const Appended& a : appended) {
        if (a.entry == nullptr) {
            uitree::KeyValue kv{};
            kv.key = a.key;
            kv.value = reinterpret_cast<std::uintptr_t>(a.vec);
            kv.tag = g_extDonors.arrTag;
            kv.seq = g_extDonors.arrSeq;
            items.push_back(kv);
        }
    }
    const std::uintptr_t* const node = g_extArena.makeMap(items.data(), items.size());
    if (node == nullptr) {
        err = "the arena ran out (definition)";
        return nullptr;
    }
    std::uintptr_t* const value = g_extArena.makeValue(vanillaWords, node);
    if (value == nullptr) {
        err = "the arena ran out (value)";
        return nullptr;
    }
    return value;
}

}

void registerDefExtension(const char* space, const char* name, const std::string& frontJson,
                          const std::string& backJson)
{
    DefExtension ext;
    ext.space = space;
    ext.name = name;
    try {
        ext.front = frontJson.empty() ? nlohmann::json::array() : nlohmann::json::parse(frontJson);
        ext.back = backJson.empty() ? nlohmann::json::array() : nlohmann::json::parse(backJson);
    } catch (const std::exception& e) {
        log().error(L"UiProbe: the extension of {}.{} is not valid JSON ({})", toUtf16(space),
                    toUtf16(name), toUtf16(e.what()));
        return;
    }
    if (!ext.front.is_array() || !ext.back.is_array()) {
        log().error(L"UiProbe: the extension of {}.{} must be arrays of controls", toUtf16(space),
                    toUtf16(name));
        return;
    }
    std::lock_guard<std::mutex> guard(g_extMutex);
    for (DefExtension& one : g_extensions) {
        if (one.space == ext.space && one.name == ext.name) {
            for (auto& part : ext.front) {
                one.front.push_back(std::move(part));
            }
            for (auto& part : ext.back) {
                one.back.push_back(std::move(part));
            }
            one.built = nullptr;
            one.builtFor = nullptr;
            return;
        }
    }
    g_extensions.push_back(std::move(ext));
    g_extAny.store(true, std::memory_order_release);
}

void registerDefAppend(const char* space, const char* name, const char* key,
                       const std::string& elementsJson)
{
    nlohmann::json list;
    try {
        list = nlohmann::json::parse(elementsJson);
    } catch (const std::exception& e) {
        log().error(L"UiProbe: the {} appended to {}.{} is not valid JSON ({})", toUtf16(key),
                    toUtf16(space), toUtf16(name), toUtf16(e.what()));
        return;
    }
    if (!list.is_array()) {
        log().error(L"UiProbe: the {} appended to {}.{} must be an array", toUtf16(key),
                    toUtf16(space), toUtf16(name));
        return;
    }
    std::lock_guard<std::mutex> guard(g_extMutex);
    DefExtension* target = nullptr;
    for (DefExtension& one : g_extensions) {
        if (one.space == space && one.name == name) {
            target = &one;
            break;
        }
    }
    if (target == nullptr) {
        DefExtension ext;
        ext.space = space;
        ext.name = name;
        ext.front = nlohmann::json::array();
        ext.back = nlohmann::json::array();
        g_extensions.push_back(std::move(ext));
        target = &g_extensions.back();
    }
    bool replaced = false;
    for (auto& [k, v] : target->appends) {
        if (k == key) {
            v = std::move(list);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        target->appends.emplace_back(key, std::move(list));
    }
    target->built = nullptr;
    target->builtFor = nullptr;
    g_extAny.store(true, std::memory_order_release);
}

void* extendDefinition(void* self, const void* space, const void* name, void* vanilla)
{
    if (!g_extAny.load(std::memory_order_acquire) || vanilla == nullptr || self == nullptr) {
        return nullptr;
    }
    char sp[96]{};
    char nm[128]{};
    if (!copyStdStringFast(space, sp, sizeof(sp)) || !copyStdStringFast(name, nm, sizeof(nm))) {
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(g_extMutex);
    for (DefExtension& ext : g_extensions) {
        if (ext.name != nm || ext.space != sp) {
            continue;
        }
        if (ext.built != nullptr && ext.builtFor == vanilla) {
            return ext.built;
        }
        if (ext.builds >= 128) {
            if (ext.builds++ == 128) {
                log().warn(L"UiProbe: {}.{} reached the build limit; the vanilla one is used from now on",
                           toUtf16(ext.space), toUtf16(ext.name));
            }
            return nullptr;
        }
        ++ext.builds;
        std::string err;
        void* const built = buildExtension(self, ext, vanilla, err);
        if (built == nullptr) {
            log().warn(L"UiProbe: could not extend {}.{} ({}); the vanilla one is used",
                       toUtf16(ext.space), toUtf16(ext.name), toUtf16(err));
            return nullptr;
        }
        ext.built = built;
        ext.builtFor = vanilla;
        log().info(L"UiProbe: extended {}.{} (+{} / +{} controls, {} array(s); arena {}/{} nodes)",
                   toUtf16(ext.space), toUtf16(ext.name), ext.front.size(), ext.back.size(),
                   ext.appends.size(), g_extArena.usedNodes(), g_extArena.capacityNodes());
        return built;
    }
    return nullptr;
}

}
