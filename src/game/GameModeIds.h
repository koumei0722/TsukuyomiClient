#pragma once

namespace tsukuyomi::gamemode {

inline constexpr int kSurvival = 0;
inline constexpr int kCreative = 1;
inline constexpr int kAdventure = 2;
inline constexpr int kSpectator = 6;
inline constexpr int kUnknown = -1;

inline constexpr int kDefault = 5;

inline constexpr int kCycle[] = {kCreative, kSurvival, kAdventure, kSpectator};
inline constexpr int kCycleCount = 4;

static_assert(sizeof(kCycle) / sizeof(kCycle[0]) == kCycleCount);

inline bool isSelectable(int mode)
{
    for (const int candidate : kCycle) {
        if (candidate == mode) {
            return true;
        }
    }
    return false;
}

inline const wchar_t* name(int mode)
{
    switch (mode) {
    case kSurvival:
        return L"survival";
    case kCreative:
        return L"creative";
    case kAdventure:
        return L"adventure";
    case kSpectator:
        return L"spectator";
    case kDefault:
        return L"default";
    default:
        return L"unknown";
    }
}

inline const wchar_t* shortName(int mode)
{
    switch (mode) {
    case kSurvival:
        return L"Survival";
    case kCreative:
        return L"Creative";
    case kAdventure:
        return L"Adventure";
    case kSpectator:
        return L"Spectator";
    default:
        return L"";
    }
}

inline const char* command(int mode)
{
    switch (mode) {
    case kSurvival:
        return "/gamemode survival";
    case kCreative:
        return "/gamemode creative";
    case kAdventure:
        return "/gamemode adventure";
    case kSpectator:
        return "/gamemode spectator";
    default:
        return nullptr;
    }
}

inline const wchar_t* commandW(int mode)
{
    switch (mode) {
    case kSurvival:
        return L"/gamemode survival";
    case kCreative:
        return L"/gamemode creative";
    case kAdventure:
        return L"/gamemode adventure";
    case kSpectator:
        return L"/gamemode spectator";
    default:
        return L"";
    }
}

inline const wchar_t* displayName(int mode)
{
    switch (mode) {
    case kSurvival:
        return L"Survival Mode";
    case kCreative:
        return L"Creative Mode";
    case kAdventure:
        return L"Adventure Mode";
    case kSpectator:
        return L"Spectator Mode";
    default:
        return L"";
    }
}

}
