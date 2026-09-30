#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tsukuyomi::dbgscreen {

enum class ElementState : int { Off = 0, InOverlay = 1, Always = 2 };
enum class Column { Left, Right };
enum class Port { Priority, Regular, Group };

struct Element {
    const char* id;
    const wchar_t* label;
    Port port;
    const char* group;
    ElementState defaultState;
    ElementState performanceState;
};

inline constexpr int kElementCount = 27;
inline constexpr int kMaxRowsPerElement = 7;
inline constexpr int kRowsPerColumn = 48;
inline constexpr int kTextSlotCount = 2 * kRowsPerColumn;
inline constexpr int kFirstBoolSlot = 16;
inline constexpr int kFirstBlankSlot = kFirstBoolSlot + kTextSlotCount;

inline constexpr Element kElements[kElementCount] = {
    {"biome", L"Biome", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"chunk_generation_stats", L"Chunk generation stats", Port::Group, "chunk_generation",
     ElementState::Off, ElementState::Off},
    {"chunk_render_stats", L"Chunk render stats", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"chunk_source_stats", L"Chunk source stats", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"day_count", L"Day count", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"entity_render_stats", L"Entity render stats", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"fps", L"FPS", Port::Priority, nullptr, ElementState::InOverlay, ElementState::Always},
    {"game_version", L"Game version", Port::Priority, nullptr, ElementState::InOverlay, ElementState::Off},
    {"gpu_utilization", L"GPU utilization", Port::Regular, nullptr, ElementState::Off, ElementState::InOverlay},
    {"light_levels", L"Light levels", Port::Group, "light", ElementState::Off, ElementState::Off},
    {"local_difficulty", L"Local difficulty", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"looking_at_block_state", L"Looking at block state", Port::Group, "looking_at_block",
     ElementState::Off, ElementState::Off},
    {"looking_at_block_tags", L"Looking at block tags", Port::Group, "looking_at_block",
     ElementState::Off, ElementState::Off},
    {"looking_at_entity", L"Looking at entity", Port::Group, "looking_at_entity", ElementState::Off, ElementState::Off},
    {"looking_at_entity_tags", L"Looking at entity tags", Port::Group, "looking_at_entity",
     ElementState::Off, ElementState::Off},
    {"looking_at_fluid_state", L"Looking at fluid state", Port::Group, "looking_at_fluid",
     ElementState::Off, ElementState::Off},
    {"looking_at_fluid_tags", L"Looking at fluid tags", Port::Group, "looking_at_fluid",
     ElementState::Off, ElementState::Off},
    {"memory", L"Memory", Port::Group, "memory", ElementState::InOverlay, ElementState::InOverlay},
    {"particle_render_stats", L"Particle render stats", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"player_position", L"Player position", Port::Group, "position", ElementState::InOverlay, ElementState::Off},
    {"player_section_position", L"Player section position", Port::Group, "position",
     ElementState::InOverlay, ElementState::Off},
    {"player_speed", L"Player speed", Port::Group, "position", ElementState::Off, ElementState::Off},
    {"simple_performance_impactors", L"Simple performance impactors", Port::Regular, nullptr,
     ElementState::InOverlay, ElementState::InOverlay},
    {"sound_cache", L"Sound cache", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"sound_mood", L"Sound mood", Port::Regular, nullptr, ElementState::Off, ElementState::Off},
    {"system_specs", L"System specs", Port::Group, "system", ElementState::InOverlay, ElementState::Off},
    {"tps", L"TPS", Port::Regular, nullptr, ElementState::InOverlay, ElementState::InOverlay},
};

namespace detail {
constexpr bool sameId(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}
constexpr bool lessId(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0' && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) < static_cast<unsigned char>(*b);
}
}

constexpr bool elementsSorted()
{
    for (int i = 0; i + 1 < kElementCount; ++i) {
        if (!detail::lessId(kElements[i].id, kElements[i + 1].id)) return false;
    }
    return true;
}
static_assert(elementsSorted());

constexpr int indexOfElement(const char* id)
{
    if (id == nullptr) return -1;
    for (int i = 0; i < kElementCount; ++i) {
        if (detail::sameId(kElements[i].id, id)) return i;
    }
    return -1;
}

constexpr int textSlotOf(Column column, int row)
{
    return row < 0 || row >= kRowsPerColumn ? -1 : row + (column == Column::Right ? kRowsPerColumn : 0);
}
constexpr int boolSlotOf(Column column, int row)
{
    const int slot = textSlotOf(column, row);
    return slot < 0 ? -1 : kFirstBoolSlot + slot;
}
constexpr int blankSlotOf(Column column, int row)
{
    const int slot = textSlotOf(column, row);
    return slot < 0 ? -1 : kFirstBlankSlot + slot;
}

struct Contribution {
    int element;
    int rows;
    bool groupPresent;
};
struct LineRef {
    std::int8_t contribution;
    std::int8_t row;
};
inline constexpr std::int8_t kBlankLine = -1;
struct Arranged {
    int rows[2];
    LineRef line[2][kRowsPerColumn];
    int dropped[2];
};
void arrange(const Contribution* items, int count, Arranged& out);

void visibleBindName(Column column, int row, char* out, std::size_t cap);
void blankBindName(Column column, int row, char* out, std::size_t cap);
void textBindName(Column column, int row, char* out, std::size_t cap);
std::string layoutJson();

inline constexpr int kHideCoordsSlot = kFirstBlankSlot + kTextSlotCount;
inline constexpr int kHideDaysSlot = kHideCoordsSlot + 1;
inline constexpr int kChatBottomSlot = kHideCoordsSlot + 2;
inline constexpr const char* kBindHideCoords = "#tk_f3_hide_coords";
inline constexpr const char* kBindHideDays = "#tk_f3_hide_days";
inline constexpr const char* kBindChatBottom = "#tk_f3_chat_bottom";
inline constexpr int kChatBottomOffsetY = -48;

std::string hideBindingJson(const char* vanillaVisible, const char* hideBind);
std::string chatHideBindingJson();
std::string chatJson();

inline constexpr int kGamePausedSlot = kHideCoordsSlot + 3;
inline constexpr const char* kBindGamePaused = "#tk_f3_paused";
inline constexpr int kGamePausedOffsetY = 10;
std::string pausedJson();

inline constexpr int kPauseWheelSlot = kHideCoordsSlot + 4;
inline constexpr const char* kBindPauseWheel = "#tk_f3_pause_wheel";
std::string pauseWheelHideJson();

}
