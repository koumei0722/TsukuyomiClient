#pragma once
#include "replay/ReplayWorker.h"
#include <memory>

namespace tsukuyomi::replay {
class ReplayCapture {
public:
    ReplayCapture();
    ~ReplayCapture();
    bool start();
    void stop(bool save = true);
    void onPlayerViewUpdate();
    void poll();
    void wait();
    bool recording() const;
    bool busy() const;
    bool takeResult(ReplayWorker::Result& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void onSubChunkWrite(void* self, unsigned int layer, unsigned int index);
}
