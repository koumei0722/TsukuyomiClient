#pragma once

namespace tsukuyomi::ghostmesh {
bool resolve();
void drawFrame(void* lrp, void* ctx);
void releaseOnRenderThread();
void shutdown();
void report();
}
