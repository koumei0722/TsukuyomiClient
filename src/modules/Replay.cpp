#include "modules/Replay.h"
#include "modules/ModuleManager.h"
#include "game/BlockWrite.h"
#include "game/GameData.h"
#include "config/WriteSwitches.h"
#include <cstdio>

namespace tsukuyomi {
Replay& Replay::instance() { static Replay instance; return instance; }
bool Replay::writeBlocked() const
{
    return !writes::allowed("SubChunkSetBlock");
}
void Replay::onEnabledChanged(bool on)
{
    bool rejected = false;
    {
        std::lock_guard lock(mutex_);
        if (on) {
            void* region = blockwrite::renderRegion();
            rejected = GameData::instance().msSinceView() >= 200 || !region || !blockwrite::regionIsAlive(region) || capture_.busy() || !capture_.start();
        } else capture_.stop();
    }
    if (rejected) { ModuleManager::instance().postNotice("Replay: enter a world and wait for the previous save before recording"); setEnabled(false); }
}
void Replay::onPlayerViewUpdate()
{
    std::lock_guard lock(mutex_);
    capture_.poll();
    capture_.onPlayerViewUpdate();
}
void Replay::onUpdate()
{
    bool stopped = false; replay::ReplayWorker::Result result; bool hasResult = false;
    {
        std::lock_guard lock(mutex_); capture_.poll(); stopped = !capture_.recording(); hasResult = capture_.takeResult(result);
    }
    if (stopped && enabled()) setEnabled(false);
    if (hasResult) {
        std::string text;
        if (result.saved) {
            char detail[96]{}; std::snprintf(detail, sizeof(detail), " (%.1f s, %.1f MB)", result.ticks / 20.0, result.bytes / (1024.0 * 1024));
            text = "Replay: saved " + result.path.filename().string() + detail;
            if (result.sizeLimit) text += " (3.5 GiB recording limit reached)";
        } else if (!result.error.empty()) text = "Replay: could not save: " + result.error;
        if (!text.empty()) ModuleManager::instance().postNotice(std::move(text));
    }
}
void Replay::shutdown()
{
    setEnabled(false);
    std::lock_guard lock(mutex_); capture_.stop(); capture_.wait();
}
}
