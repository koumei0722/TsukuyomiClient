#pragma once

#include <cstdint>

namespace tsukuyomi::gamecallable {

struct GameCallable {
    const void* ops = nullptr;
    void* capture = nullptr;
    std::uintptr_t spare[6]{};
};
static_assert(sizeof(GameCallable) == 0x40);

struct CallableOps {
    void(__fastcall* move)(GameCallable* src, GameCallable* dst);
    void(__fastcall* destroy)(GameCallable* self);
    const void* invoke;
};

inline void __fastcall moveCapture(GameCallable* src, GameCallable* dst)
{
    dst->capture = src->capture;
}
inline void __fastcall destroyNothing(GameCallable*) {}

}
