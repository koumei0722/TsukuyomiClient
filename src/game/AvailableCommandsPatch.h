#pragma once

namespace tsukuyomi::availablecommands {

using LoadPacketFn = void(__fastcall*)(void* registry, const void* packet);
void loadWithTk(LoadPacketFn original, void* registry, const void* packet);
void ensureTk(LoadPacketFn original, void* registry);
void* clientRegistry();

}
