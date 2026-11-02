#include "replay/FlashbackFile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace tsukuyomi::replay {
namespace {
bool write(std::ostream& stream, std::span<const std::uint8_t> bytes)
{ stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); return static_cast<bool>(stream); }
void little(std::ostream& stream, std::uint32_t value, unsigned count)
{ for (unsigned i = 0; i < count; ++i) { stream.put(static_cast<char>(value)); value >>= 8; } }
const auto& crcTable()
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            result[i] = c;
        }
        return result;
    }();
    return table;
}
}
std::string uuidString(const Uuid& uuid)
{
    std::string result;
    char hex[3]{};
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        std::snprintf(hex, sizeof(hex), "%02x", uuid[i]); result += hex;
    }
    return result;
}
FlashbackFile::FlashbackFile(std::filesystem::path dir, Uuid uuid, std::string name)
    : workDir_(std::move(dir)), uuid_(uuid), name_(std::move(name)) {}
bool FlashbackFile::fail(std::string text) { if (error_.empty()) error_ = std::move(text); return false; }
bool FlashbackFile::beginSegment()
{
    std::error_code ec;
    std::filesystem::create_directories(workDir_ / "level_chunk_caches", ec);
    if (ec) return fail("could not create recording directory: " + ec.message());
    segment_.open(workDir_ / ("c" + std::to_string(segments_.size()) + ".flashback"), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!segment_) return fail("could not open segment");
    ByteWriter w; w.i32(0xd780e884u); w.varint(6);
    for (const auto* name : {"next_tick", "game_packet", "configuration_packet", "create_local_player", "move_entities", "level_chunk_cached"}) w.string(std::string("flashback:action/") + name);
    snapshotAt_ = static_cast<std::streamoff>(w.data.size()); w.i32(0);
    bytesWritten_ += w.data.size();
    return write(segment_, w.data) || fail("could not write segment header");
}
bool FlashbackFile::action(int id, std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > INT32_MAX) return fail("action too large");
    ByteWriter w; w.varint(id); w.i32(static_cast<std::uint32_t>(bytes.size()));
    bytesWritten_ += w.data.size() + bytes.size();
    return (write(segment_, w.data) && write(segment_, bytes)) || fail("could not write action");
}
bool FlashbackFile::endSnapshot()
{
    const auto end = segment_.tellp();
    const auto size = static_cast<std::streamoff>(end) - snapshotAt_ - 4;
    if (size < 0 || size > INT32_MAX) return fail("snapshot too large");
    segment_.seekp(snapshotAt_); ByteWriter w; w.i32(static_cast<std::uint32_t>(size));
    if (!write(segment_, w.data)) return fail("could not finish snapshot");
    segment_.seekp(end);
    return static_cast<bool>(segment_);
}
bool FlashbackFile::metadata()
{
    nlohmann::ordered_json chunks = nlohmann::ordered_json::object();
    for (std::size_t i = 0; i < segments_.size(); ++i)
        chunks["c" + std::to_string(i) + ".flashback"] = {{"duration", segments_[i].first}, {"forcePlaySnapshot", segments_[i].second}};
    const nlohmann::ordered_json meta = {{"uuid", uuidString(uuid_)}, {"name", name_}, {"version_string", "26.3"},
        {"world_name", "Bedrock"}, {"data_version", 5023}, {"protocol_version", 777}, {"total_ticks", totalTicks_},
        {"markers", nlohmann::ordered_json::object()}, {"chunks", chunks}};
    std::error_code ec;
    const auto path = workDir_ / "metadata.json";
    if (std::filesystem::exists(path, ec)) {
        std::filesystem::remove(workDir_ / "metadata.json.old", ec); ec.clear();
        std::filesystem::rename(path, workDir_ / "metadata.json.old", ec);
        if (ec) return fail("could not rotate metadata: " + ec.message());
    }
    const auto text = meta.dump(2);
    std::ofstream out(path, std::ios::binary); out << text; out.close();
    if (out) bytesWritten_ += text.size();
    return static_cast<bool>(out) || fail("could not write metadata");
}
bool FlashbackFile::endSegment(std::uint64_t total, bool force)
{
    segment_.flush(); const bool ok = static_cast<bool>(segment_); segment_.close();
    if (!ok || !segment_) return fail("could not close segment");
    segments_.emplace_back(total - totalTicks_, force); totalTicks_ = total;
    return metadata();
}
int FlashbackFile::cacheChunk(std::span<const std::uint8_t> bytes)
{
    std::error_code ec; std::filesystem::create_directories(workDir_ / "level_chunk_caches", ec);
    if (ec || bytes.size() > INT32_MAX) { fail("could not create chunk cache"); return -1; }
    std::ofstream out(workDir_ / "level_chunk_caches" / std::to_string(cached_ / 10000), std::ios::binary | std::ios::app);
    ByteWriter w; w.i32(static_cast<std::uint32_t>(bytes.size()));
    const bool ok = write(out, w.data) && write(out, bytes); out.close();
    if (!ok || !out) { fail("could not write chunk cache"); return -1; }
    bytesWritten_ += 4 + bytes.size(); return cached_++;
}
bool FlashbackFile::finish(const std::filesystem::path& zipPath)
{
    struct Entry { std::string name; std::uint32_t size, crc, offset; };
    std::vector<Entry> entries;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(workDir_, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().filename() == "metadata.json.old") continue;
        const auto size = it->file_size(ec);
        if (ec || size > UINT32_MAX) return fail("file too large for zip");
        entries.push_back({it->path().lexically_relative(workDir_).generic_string(), static_cast<std::uint32_t>(size), 0, 0});
    }
    if (ec || entries.size() > UINT16_MAX) return fail("could not enumerate recording files");
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    std::uint64_t projected = 22;
    for (const auto& e : entries) projected += 76 + 2 * e.name.size() + e.size;
    if (projected > UINT32_MAX) return fail("recording too large for zip");
    auto partial = zipPath; partial += ".part";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) return fail("could not open zip");
    std::array<char, 65536> buffer{};
    for (auto& e : entries) {
        std::ifstream in(workDir_ / e.name, std::ios::binary);
        std::uint32_t crc = 0xffffffffu;
        std::uint64_t read = 0;
        while (in) {
            in.read(buffer.data(), buffer.size()); const auto count = in.gcount(); read += count;
            for (std::streamsize i = 0; i < count; ++i) crc = crcTable()[(crc ^ static_cast<unsigned char>(buffer[i])) & 255] ^ (crc >> 8);
        }
        if (read != e.size || !in.eof()) { out.close(); std::filesystem::remove(partial, ec); return fail("could not read recording file"); }
        e.crc = crc ^ 0xffffffffu; e.offset = static_cast<std::uint32_t>(out.tellp());
        little(out, 0x04034b50, 4); little(out, 20, 2); little(out, 0x800, 2); little(out, 0, 2);
        little(out, 0, 2); little(out, 33, 2); little(out, e.crc, 4); little(out, e.size, 4); little(out, e.size, 4);
        little(out, static_cast<std::uint32_t>(e.name.size()), 2); little(out, 0, 2); out << e.name;
        in.clear(); in.seekg(0);
        std::uint64_t copied = 0;
        while (in) { in.read(buffer.data(), buffer.size()); const auto count = in.gcount(); out.write(buffer.data(), count); copied += count; }
        if (copied != e.size || !in.eof()) { out.close(); std::filesystem::remove(partial, ec); return fail("could not copy recording file"); }
    }
    const auto central = static_cast<std::uint32_t>(out.tellp());
    for (const auto& e : entries) {
        little(out, 0x02014b50, 4); little(out, 20, 2); little(out, 20, 2); little(out, 0x800, 2); little(out, 0, 2);
        little(out, 0, 2); little(out, 33, 2); little(out, e.crc, 4); little(out, e.size, 4); little(out, e.size, 4);
        little(out, static_cast<std::uint32_t>(e.name.size()), 2); little(out, 0, 2); little(out, 0, 2);
        little(out, 0, 2); little(out, 0, 2); little(out, 0, 4); little(out, e.offset, 4); out << e.name;
    }
    const auto centralSize = static_cast<std::uint32_t>(out.tellp()) - central;
    little(out, 0x06054b50, 4); little(out, 0, 2); little(out, 0, 2);
    little(out, static_cast<std::uint32_t>(entries.size()), 2); little(out, static_cast<std::uint32_t>(entries.size()), 2);
    little(out, centralSize, 4); little(out, central, 4); little(out, 0, 2); out.close();
    if (!out) { std::filesystem::remove(partial, ec); return fail("could not write zip (disk full?)"); }
    std::filesystem::rename(partial, zipPath, ec);
    if (ec) return fail("could not finalize zip: " + ec.message());
    abandon(); return true;
}
void FlashbackFile::abandon()
{
    if (segment_.is_open()) segment_.close();
    std::error_code ec; std::filesystem::remove_all(workDir_, ec);
}
}
