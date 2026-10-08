#pragma once

#include <Windows.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "input/Keys.h"

namespace tsukuyomi {

void __fastcall onGameButton(void*, int, const void*, void*);
void* __fastcall gameButtonActionName(void*, int);
void* __fastcall gameButtonFindKeymap(void*, const void*);
void __fastcall gameButtonBindAction(void*, void*, void*, const void*, int, bool);
void __fastcall gameButtonPadBind(void*, void*, const void*, int);
void __fastcall gameButtonInputUpdate(void*, void*, void*, void*, std::uint64_t);

namespace gamebuttonlogic {
constexpr int kMaxSlots = 96;
constexpr int kMaxWatchedButtons = 32;
constexpr int kMaxInputRows = 32;
constexpr int kMaxPadInputRows = 16;
constexpr int kMaxComboKeys = 4;
constexpr std::uint64_t kPressLifetimeMs = 100;
enum class CallbackKind : int { KeyDown, KeyUp, ButtonDown, ButtonUp, InputDown, InputUp, PadInputDown, PadInputUp };
constexpr int packCallback(CallbackKind kind, int index) { return (static_cast<int>(kind) << 12) | index; }
constexpr CallbackKind callbackKind(int value) { return static_cast<CallbackKind>(value >> 12); }
constexpr int callbackIndex(int value) { return value & 0xFFF; }

namespace button {
inline constexpr char destroyOrAttack[] = "button.destroy_or_attack";
inline constexpr char buildOrInteract[] = "button.build_or_interact";
inline constexpr char up[] = "button.up";
inline constexpr char down[] = "button.down";
inline constexpr char left[] = "button.left";
inline constexpr char right[] = "button.right";
inline constexpr char jump[] = "button.jump";
inline constexpr char sneak[] = "button.sneak";
inline constexpr char sprint[] = "button.sprint";
inline constexpr char shift[] = "button.shift";
inline constexpr char control[] = "button.control";
inline constexpr char alt[] = "button.alt";
inline constexpr char pointerPressed[] = "button.pointer_pressed";
inline constexpr char menuSecondarySelect[] = "button.menu_secondary_select";
inline constexpr char menuTertiarySelect[] = "button.menu_tertiary_select";
inline constexpr char menuCancel[] = "button.menu_cancel";
inline constexpr char inventoryLeft[] = "button.inventory_left";
inline constexpr char inventoryRight[] = "button.inventory_right";
}
inline int wheelNotches(std::uint64_t leftSeq, std::uint64_t leftSeen,
                        std::uint64_t rightSeq, std::uint64_t rightSeen)
{
    return static_cast<int>(leftSeq - leftSeen) - static_cast<int>(rightSeq - rightSeen);
}

inline bool sneakHeldAt(std::uint64_t at, std::uint64_t press, std::uint64_t release, std::uint64_t grace)
{
    return at != 0 && press != 0 && press <= at + grace
        && (release < press || release >= at || at - release <= grace);
}

class ChordInputs {
public:
    bool add(std::span<const int> combo)
    {
        for (int raw : combo) {
            const int key = keys::normalize(raw);
            if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU) continue;
            if (std::find(m_keys.begin(), m_keys.end(), key) != m_keys.end()) continue;
            if (m_keys.size() == kMaxInputRows) return false;
            m_keys.push_back(key);
        }
        return true;
    }
    const std::vector<int>& keys() const { return m_keys; }
    std::string buttonName(int raw) const
    {
        const int key = keys::normalize(raw);
        if (key == VK_CONTROL) return button::control;
        if (key == VK_SHIFT) return button::shift;
        if (key == VK_MENU) return button::alt;
        const auto it = std::find(m_keys.begin(), m_keys.end(), key);
        if (it == m_keys.end()) return {};
        const int row = static_cast<int>(it - m_keys.begin());
        std::string name = "button.tk.kb";
        name.push_back(static_cast<char>('0' + row / 10));
        name.push_back(static_cast<char>('0' + row % 10));
        return name;
    }
private:
    std::vector<int> m_keys;
};

class PadChordInputs {
public:
    bool add(std::span<const int> combo)
    {
        for (int key : combo) {
            if (std::find(m_keys.begin(), m_keys.end(), key) != m_keys.end()) continue;
            if (m_keys.size() == kMaxPadInputRows) return false;
            m_keys.push_back(key);
        }
        return true;
    }
    const std::vector<int>& keys() const { return m_keys; }
    std::string buttonName(int key) const
    {
        const auto it = std::find(m_keys.begin(), m_keys.end(), key);
        if (it == m_keys.end()) return {};
        const int row = static_cast<int>(it - m_keys.begin());
        std::string name = "button.tk.pb";
        name.push_back(static_cast<char>('0' + row / 10));
        name.push_back(static_cast<char>('0' + row % 10));
        return name;
    }
private:
    std::vector<int> m_keys;
};

struct ChordSpec {
    std::string output;
    std::vector<std::string> inputs;
    bool operator==(const ChordSpec&) const = default;
    bool ours() const
    {
        if (output.rfind("button.tk.", 0) == 0) return true;
        for (const auto& input : inputs) {
            if (input.rfind("button.tk.", 0) == 0) return true;
        }
        return false;
    }
};

inline std::vector<ChordSpec> reorderChords(const std::vector<ChordSpec>& current,
                                             const std::vector<ChordSpec>& desired)
{
    std::vector<ChordSpec> result;
    result.reserve(current.size() + desired.size());
    for (const auto& entry : current) {
        if (!entry.ours()) result.push_back(entry);
    }
    result.insert(result.end(), desired.begin(), desired.end());
    return result;
}

class CaptureKeys {
public:
    void reset() { m_previous.clear(); m_pending.clear(); }
    bool update(std::span<const int> current, std::vector<int>& result)
    {
        for (int raw : current) {
            const int key = keys::normalize(raw);
            if (std::find(m_previous.begin(), m_previous.end(), key) == m_previous.end()
                && std::find(m_pending.begin(), m_pending.end(), key) == m_pending.end()) m_pending.push_back(key);
        }
        m_previous.clear();
        for (int raw : current) m_previous.push_back(keys::normalize(raw));
        if (!current.empty() || m_pending.empty()) return false;
        if (std::all_of(m_pending.begin(), m_pending.end(), keys::isModifier)) return false;
        result = m_pending;
        reset();
        return true;
    }
private:
    std::vector<int> m_previous;
    std::vector<int> m_pending;
};

inline std::uint64_t aloneAfterRelease(std::uint64_t before, std::uint64_t now,
                                       std::uint64_t sequence)
{
    return sequence + (before == now ? 1 : 0);
}

inline int gameKeyCode(int key)
{
    switch (key) {
    case VK_LBUTTON:  return -99;
    case VK_RBUTTON:  return -98;
    case VK_MBUTTON:  return -97;
    case VK_XBUTTON1: return -95;
    case VK_XBUTTON2: return -94;
    default:          return key;
    }
}

inline bool freshPress(std::uint64_t now, std::uint64_t pressedAt)
{
    return pressedAt != 0 && now >= pressedAt && now - pressedAt <= kPressLifetimeMs;
}

class SlotAllocator {
public:
    int attach()
    {
        if (m_next == kMaxSlots) return -1;
        return m_next++;
    }
    int count() const { return m_next; }
private:
    int m_next = 0;
};
}

class GameButtons {
public:
    static GameButtons& instance();

    int attach(std::vector<int> combo, const char* output = nullptr);
    int watchKey(int vk);
    int watchButton(const char* buttonName);
    void setKeys(int slot, std::vector<int> combo);
    void setPadKeys(int slot, std::vector<int> combo);
    void install();
    void shutdown();

    bool ready(int slot) const;
    void pumpNotice();
    std::uint64_t pressSeq(int slot) const;
    std::uint64_t aloneReleaseSeq(int slot) const;
    std::uint64_t lastPressMs(int slot) const;
    bool held(int slot) const;
    bool keyHeld(int slot) const { return held(slot); }
    std::uint64_t keyPressSeq(int slot) const { return pressSeq(slot); }
    bool buttonHeld(int index) const;
    std::uint64_t buttonPressSeq(int index) const;
    std::uint64_t buttonLastPressMs(int index) const;
    std::uint64_t buttonLastReleaseMs(int index) const;

private:
    GameButtons() = default;

    struct Slot {
        std::atomic<int> key{0};
        std::array<int, gamebuttonlogic::kMaxComboKeys> combo{};
        int comboCount = 0;
        std::array<int, gamebuttonlogic::kMaxComboKeys> pad{};
        std::atomic<int> padCount{0};
        std::atomic<std::uint64_t> aloneSeq{0};
        std::uint64_t modifierAtPress = 0;
        std::atomic<std::uint64_t> pressSeq{0};
        std::atomic<std::uint64_t> lastPressMs{0};
        std::atomic<bool> held{false};
    };
    struct WatchedButton {
        std::string name;
        std::atomic<bool> held{false};
        std::atomic<std::uint64_t> pressSeq{0};
        std::atomic<std::uint64_t> lastPressMs{0};
        std::atomic<std::uint64_t> lastReleaseMs{0};
    };
    gamebuttonlogic::SlotAllocator m_allocator;
    std::mutex m_attachMutex;
    std::array<Slot, gamebuttonlogic::kMaxSlots> m_slots{};
    std::array<bool, gamebuttonlogic::kMaxSlots> m_keyWatch{};
    std::array<WatchedButton, gamebuttonlogic::kMaxWatchedButtons> m_watches{};
    std::atomic<int> m_watchCount{0};
    std::atomic<int> m_count{0};
    std::atomic<std::uint64_t> m_builtRevision{0};
    std::atomic<std::uint64_t> m_appliedRevision{0};
    std::atomic<bool> m_rowsSeen{false};
    std::atomic<bool> m_chordsSynced{false};
    std::atomic<bool> m_hadChords{false};
    std::atomic<bool> m_chordTablesChecked{false};
    std::atomic<bool> m_anyChords{false};
    std::array<std::atomic<std::uint64_t>, 256> m_modifierUses{};
    std::atomic<bool> m_installed{false};
    std::atomic<bool> m_padInstalled{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<std::uint64_t> m_revision{0};
    std::atomic<int> m_registeredCount{0};
    std::atomic<int> m_registeredWatchCount{0};
    std::array<std::atomic<bool>, gamebuttonlogic::kMaxInputRows> m_inputHeld{};
    std::atomic<int> m_registeredInputCount{0};
    std::array<std::atomic<bool>, gamebuttonlogic::kMaxPadInputRows> m_padInputHeld{};
    std::atomic<int> m_registeredPadInputCount{0};
    int m_modifierWatch[3] = {-1, -1, -1};
    bool keyHeldForChord(int vk) const;
    bool chordPartsHeld(int key) const;
    bool padChordPartsHeld(int pad) const;
    void* m_registeredHandler = nullptr;
    std::uint64_t m_lastRebuildMs = 0;
    unsigned char m_handlerOffset = 0;
    void* m_callbackPage = nullptr;

    std::atomic<const wchar_t*> m_failure{nullptr};
    std::atomic<bool> m_noticeShown{false};
    void fail(const wchar_t* reason);

    void onCallback(int packed);
    void onInputUpdate(void* self);
    void onRowAdded();
    bool syncChordTables(void* client);
    bool hasChords() const;
    friend void __fastcall onGameButton(void*, int, const void*, void*);
    friend void* __fastcall gameButtonActionName(void*, int);
    friend void* __fastcall gameButtonFindKeymap(void*, const void*);
    friend void __fastcall gameButtonBindAction(void*, void*, void*, const void*, int, bool);
    friend void __fastcall gameButtonPadBind(void*, void*, const void*, int);
    friend void __fastcall gameButtonInputUpdate(void*, void*, void*, void*, std::uint64_t);
};

}
