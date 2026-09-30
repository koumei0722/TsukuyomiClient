#include "game/FmodStats.h"

#include <windows.h>

namespace tsukuyomi::fmodstats {

namespace {

using GetMasterGroupFn = int(__fastcall*)(void* system, void** group);
using GetSoftwareChannelsFn = int(__fastcall*)(void* system, int* count);
using GetIntFn = int(__fastcall*)(void* group, int* count);
using GetIndexedFn = int(__fastcall*)(void* group, int index, void** out);
using IsPlayingFn = int(__fastcall*)(void* channel, bool* playing);
using GetCurrentSoundFn = int(__fastcall*)(void* channel, void** sound);
using GetModeFn = int(__fastcall*)(void* sound, unsigned int* mode);
using GetMasterSoundGroupFn = int(__fastcall*)(void* system, void** group);
using GetNumSoundsFn = int(__fastcall*)(void* group, int* count);
using MemoryGetStatsFn = int(__cdecl*)(int* current, int* maximum, int blocking);

struct Api {
    void* update = nullptr;
    GetMasterGroupFn getMasterChannelGroup = nullptr;
    GetSoftwareChannelsFn getSoftwareChannels = nullptr;
    GetIntFn getNumChannels = nullptr;
    GetIndexedFn getChannel = nullptr;
    GetIntFn getNumGroups = nullptr;
    GetIndexedFn getGroup = nullptr;
    IsPlayingFn isPlaying = nullptr;
    GetCurrentSoundFn getCurrentSound = nullptr;
    GetModeFn getMode = nullptr;
    GetMasterSoundGroupFn getMasterSoundGroup = nullptr;
    GetNumSoundsFn getNumSounds = nullptr;
    MemoryGetStatsFn memoryGetStats = nullptr;
};

Api g_api{};

constexpr unsigned int kCreateStream = 0x00000080;
constexpr int kMaxDepth = 8;
constexpr int kMaxChannels = 4096;
constexpr int kMaxGroups = 256;

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool walk(void* master, Counts& out)
{
    __try {
        void* stack[kMaxGroups];
        int depth[kMaxGroups];
        int top = 0;
        int seenGroups = 0;
        int seenChannels = 0;
        stack[top] = master;
        depth[top] = 0;
        ++top;
        while (top > 0) {
            --top;
            void* const group = stack[top];
            const int level = depth[top];
            if (++seenGroups > kMaxGroups) {
                break;
            }
            int channels = 0;
            if (g_api.getNumChannels(group, &channels) == 0) {
                for (int i = 0; i < channels && seenChannels < kMaxChannels; ++i, ++seenChannels) {
                    void* channel = nullptr;
                    if (g_api.getChannel(group, i, &channel) != 0 || channel == nullptr) {
                        continue;
                    }
                    bool playing = false;
                    if (g_api.isPlaying(channel, &playing) != 0 || !playing) {
                        continue;
                    }
                    void* sound = nullptr;
                    unsigned int mode = 0;
                    if (g_api.getCurrentSound(channel, &sound) == 0 && sound != nullptr
                        && g_api.getMode(sound, &mode) == 0 && (mode & kCreateStream) != 0) {
                        ++out.streams;
                    } else {
                        ++out.statics;
                    }
                }
            }
            if (level + 1 >= kMaxDepth) {
                continue;
            }
            int groups = 0;
            if (g_api.getNumGroups(group, &groups) != 0) {
                continue;
            }
            for (int i = 0; i < groups && top < kMaxGroups; ++i) {
                void* child = nullptr;
                if (g_api.getGroup(group, i, &child) == 0 && child != nullptr) {
                    stack[top] = child;
                    depth[top] = level + 1;
                    ++top;
                }
            }
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

}

void* updateAddress()
{
    if (g_api.update != nullptr) {
        return g_api.update;
    }
    HMODULE const fmod = GetModuleHandleW(L"fmod.dll");
    if (fmod == nullptr) {
        return nullptr;
    }
    const auto find = [fmod](const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(fmod, name));
    };
    Api api{};
    api.update = find("?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
    api.getMasterChannelGroup = reinterpret_cast<GetMasterGroupFn>(
        find("?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z"));
    api.getSoftwareChannels = reinterpret_cast<GetSoftwareChannelsFn>(
        find("?getSoftwareChannels@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z"));
    api.getNumChannels = reinterpret_cast<GetIntFn>(
        find("?getNumChannels@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z"));
    api.getChannel = reinterpret_cast<GetIndexedFn>(
        find("?getChannel@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAVChannel@2@@Z"));
    api.getNumGroups = reinterpret_cast<GetIntFn>(
        find("?getNumGroups@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z"));
    api.getGroup = reinterpret_cast<GetIndexedFn>(
        find("?getGroup@ChannelGroup@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAV12@@Z"));
    api.isPlaying = reinterpret_cast<IsPlayingFn>(
        find("?isPlaying@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@PEA_N@Z"));
    api.getCurrentSound = reinterpret_cast<GetCurrentSoundFn>(
        find("?getCurrentSound@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSound@2@@Z"));
    api.getMode = reinterpret_cast<GetModeFn>(find("?getMode@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAI@Z"));
    api.getMasterSoundGroup = reinterpret_cast<GetMasterSoundGroupFn>(
        find("?getMasterSoundGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSoundGroup@2@@Z"));
    api.getNumSounds = reinterpret_cast<GetNumSoundsFn>(
        find("?getNumSounds@SoundGroup@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z"));
    api.memoryGetStats = reinterpret_cast<MemoryGetStatsFn>(find("FMOD_Memory_GetStats"));
    if (api.update == nullptr || api.getMasterChannelGroup == nullptr || api.getSoftwareChannels == nullptr
        || api.getNumChannels == nullptr || api.getChannel == nullptr || api.getNumGroups == nullptr
        || api.getGroup == nullptr || api.isPlaying == nullptr || api.getCurrentSound == nullptr
        || api.getMode == nullptr) {
        return nullptr;
    }
    g_api = api;
    return g_api.update;
}

bool sample(void* system, Counts& out)
{
    out = Counts{};
    if (system == nullptr || g_api.update == nullptr) {
        return false;
    }
    void* master = nullptr;
    int cap = 0;
    __try {
        if (g_api.getMasterChannelGroup(system, &master) != 0 || master == nullptr
            || g_api.getSoftwareChannels(system, &cap) != 0) {
            return false;
        }
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
    out.cap = cap;
    __try {
        void* group = nullptr;
        int sounds = 0;
        if (g_api.getMasterSoundGroup != nullptr && g_api.getNumSounds != nullptr
            && g_api.getMasterSoundGroup(system, &group) == 0 && group != nullptr
            && g_api.getNumSounds(group, &sounds) == 0 && sounds >= 0) {
            out.loadedSounds = sounds;
        }
        int current = 0;
        int maximum = 0;
        if (g_api.memoryGetStats != nullptr && g_api.memoryGetStats(&current, &maximum, 0) == 0 && current >= 0) {
            out.memoryBytes = current;
        }
    } __except (accessFilter(GetExceptionCode())) {
        out.loadedSounds = -1;
        out.memoryBytes = -1;
    }
    return walk(master, out);
}

}
