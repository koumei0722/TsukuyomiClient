#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::chunkresend {

bool resolve();
bool ready();
std::int32_t subChunkStatusOffset();

bool start();
void pump();

struct PendingChunk {
    void* chunk = nullptr;
    std::int32_t chunkX = 0, chunkZ = 0;
    int pending = 0;
    int queued = 0;
    bool faulted = false;
};
bool askPending(PendingChunk* chunks, std::size_t count, int budget);

}
