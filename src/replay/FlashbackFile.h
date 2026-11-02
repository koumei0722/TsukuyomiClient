#pragma once
#include "replay/JavaPackets.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace tsukuyomi::replay {
class FlashbackFile {
public:
    FlashbackFile(std::filesystem::path workDir, Uuid uuid, std::string name);
    bool beginSegment();
    bool action(int id, std::span<const std::uint8_t> bytes = {});
    bool endSnapshot();
    bool endSegment(std::uint64_t totalTicks, bool forcePlaySnapshot);
    int cacheChunk(std::span<const std::uint8_t> bytes);
    bool finish(const std::filesystem::path& zipPath);
    void abandon();
    std::uint64_t bytesWritten() const { return bytesWritten_; }
    const std::string& error() const { return error_; }
private:
    bool fail(std::string text);
    bool metadata();
    std::filesystem::path workDir_;
    Uuid uuid_;
    std::string name_, error_;
    std::fstream segment_;
    std::streamoff snapshotAt_ = 0;
    std::uint64_t bytesWritten_ = 0, totalTicks_ = 0;
    int cached_ = 0;
    std::vector<std::pair<std::uint64_t, bool>> segments_;
};
std::string uuidString(const Uuid& uuid);
}
