#pragma once

namespace tsukuyomi::gamemode {

inline constexpr int kSurvival = 0;
inline constexpr int kCreative = 1;
inline constexpr int kAdventure = 2;
inline constexpr int kSpectator = 6;
inline constexpr int kUnknown = -1;

inline constexpr int kDefault = 5;

inline bool isSelectable(int mode)
{
    return mode == kCreative || mode == kSurvival || mode == kAdventure || mode == kSpectator;
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

}
