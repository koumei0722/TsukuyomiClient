#pragma once
#include "BotSession.h"
#include <cstdint>
#include <optional>

namespace tsukuyomi::bot::playerinteract {

using GetInteractionFn = void*(__fastcall*)(void* self, void* out, void* player, void* position, std::uintptr_t fifth);

bool initialize();
void* target();
void* onGetInteraction(GetInteractionFn original, void* self, void* out, void* player, void* position, std::uintptr_t fifth);
void readLayout();
bool available();
void setEnabled(bool enabled);

struct Request {
    void* actor = nullptr;
    std::uint64_t runtimeId = 0;
};
std::optional<Request> takeRequest();
bool stillValid(const Request& request);
std::optional<BotSession::InventorySnapshot> snapshot(void* actor);

}
