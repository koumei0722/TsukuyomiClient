#pragma once
#include "replay/FlashbackFile.h"
#include "replay/JavaWorld.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace tsukuyomi::replay {
class ReplayWorker {
public:
    struct Start {
        Dimension dim; Uuid uuid; int gamemode; std::uint64_t time; PlayerPosition playerPos;
        std::vector<bedrock::Received> world;
        std::optional<std::uint64_t> localRuntime;
        std::optional<std::int64_t> localUnique;
        std::string localName;
        std::shared_ptr<const BedrockLang> lang;
    };
    struct BedrockPackets { std::vector<bedrock::Received> packets; };
    struct LocalSlots { std::vector<LocalSlot> slots; int selected = 0; };
    struct ColumnEvent { Column column; };
    struct Forget { int cx, cz; };
    struct Blocks { std::vector<BlockChange> changes; };
    struct Player { PlayerPosition pos; };
    struct Time { std::uint64_t dayTicks; };
    struct Tick {};
    struct InitialDone {};
    struct DimensionEvent { Dimension dim; };
    struct Stop { bool save; };
    using Event = std::variant<Start, ColumnEvent, Forget, Blocks, Player, Time, Tick, InitialDone, DimensionEvent, Stop, BedrockPackets, LocalSlots>;
    struct Result { std::filesystem::path path; std::string error; std::uint64_t ticks = 0, bytes = 0; bool sizeLimit = false, saved = false; };
    ReplayWorker() = default;
    ~ReplayWorker();
    bool launch(const std::filesystem::path& workDir, const std::filesystem::path& zipPath);
    bool post(Event event);
    void wait();
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    bool accepting();
    bool takeResult(Result& out);
private:
    void run(std::filesystem::path workDir, std::filesystem::path zipPath);
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Event> events_;
    bool accepting_ = false, hasResult_ = false;
    Result result_;
    std::atomic<bool> finished_{true};
};
}
