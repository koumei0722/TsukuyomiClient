#pragma once
#include "BotProtocol.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace tsukuyomi::bot::gameterrain {

bool initialize();
bool available();
void disable();

using EnqueueFn = void(__fastcall*)(void* list, void** job);
using ConsumeFn = void(__fastcall*)(void* list);
using FinishFn = void*(__fastcall*)(void* task, void* a, void* b, void* c);
void* enqueueTarget();
void* consumeTarget();
void* finishTarget();
void onEnqueue(EnqueueFn original, void* list, void** job);
void onConsume(ConsumeFn original, void* list);
void* onFinish(FinishFn original, void* task, void* a, void* b, void* c);

bool begin(const void* owner, std::int32_t dimension, protocol::Vec3 feet, std::string& error);
bool active();
const void* owner();
std::int32_t dimension();
bool hasChunk(std::int32_t chunkX, std::int32_t chunkZ);
bool handLevelChunk(protocol::Bytes body);
bool handPacket(protocol::Bytes whole);
void pump();
struct Status { int chunks = 0, finished = 0, waiting = 0, loading = 0, loaded = 0; };
std::optional<Status> status(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t radius, std::int32_t minIndex, std::int32_t maxIndex);
struct World { void* level; void* dimension; void* source; };
std::optional<World> world();
void end(bool worldAlive);

}
