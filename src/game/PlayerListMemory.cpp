#include "game/PlayerListMemory.h"
#include "memory/Memory.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tsukuyomi::playerlist::mem {
namespace {
constexpr std::size_t kMapHead = 0x08, kMapCount = 0x10;
constexpr std::size_t kNext = 0x00, kPrev = 0x08;
constexpr std::size_t kPlayerId = 0x20, kPlayerName = 0x38;
constexpr std::size_t kDisplayHead = 0x18, kDisplayCount = 0x20;
constexpr std::size_t kDisplayKey = 0x10, kDisplayObjective = 0x30;
constexpr std::size_t kIdHead = 0x58, kIdCount = 0x60, kIdKey = 0x10, kIdValue = 0x18;
constexpr std::size_t kScoreHead = 0x20, kScoreCount = 0x28, kScoreKey = 0x10, kScoreValue = 0x20;
constexpr std::size_t kStringLength = 0x10, kStringCapacity = 0x18, kMaxString = 256;
constexpr std::size_t kPlayerSkin = 0xA0, kSkinImage = 0xA0;
constexpr std::size_t kImageWidth = 0x04, kImageHeight = 0x08, kImageData = 0x20, kImageSize = 0x28;
constexpr std::size_t kAttrIdsBegin = 0x00, kAttrIdsEnd = 0x08, kAttrInstances = 0x18;
constexpr std::size_t kAttrStride = 0x88, kAttrMaximum = 0x78;
constexpr std::size_t kMaxAttributes = 512;

template<class T> bool read(const void* base, std::size_t offset, T& out)
{
    return base != nullptr && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out));
}

template<class Visitor> bool walk(const void* base, std::size_t headOffset, std::size_t countOffset,
                                  std::size_t limit, Visitor visit)
{
    const void* head = nullptr;
    std::size_t count = 0;
    if (!read(base, countOffset, count) || count > limit || !read(base, headOffset, head)
        || !memory::plausiblePointer(head)) return false;
    const void* node = nullptr;
    if (!read(head, kNext, node)) return false;
    const void* previous = head;
    std::size_t walked = 0;
    while (node != head) {
        if (walked >= count || !memory::plausiblePointer(node)) return false;
        const void* prev = nullptr;
        const void* next = nullptr;
        if (!read(node, kPrev, prev) || prev != previous || !read(node, kNext, next) || !visit(node)) return false;
        previous = node;
        node = next;
        ++walked;
    }
    const void* tail = nullptr;
    return walked == count && read(head, kPrev, tail) && tail == previous;
}

bool readString(const void* base, std::size_t offset, char (&out)[kMaxString + 1], std::size_t& length)
{
    const auto* string = static_cast<const char*>(base) + offset;
    std::size_t capacity = 0;
    if (!read(string, kStringLength, length) || !read(string, kStringCapacity, capacity)
        || length > capacity || length > kMaxString) return false;
    const void* data = string;
    if (capacity >= 16 && (!read(string, 0, data) || data == nullptr)) return false;
    if (!memory::copyGuarded(data, out, length)) return false;
    out[length] = '\0';
    for (std::size_t i = 0; i < length;) {
        const auto c = static_cast<unsigned char>(out[i]);
        const std::size_t n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2
            : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (n == 0 || i + n > length || c == 0) return false;
        for (std::size_t j = 1; j < n; ++j)
            if ((static_cast<unsigned char>(out[i + j]) & 0xC0) != 0x80) return false;
        if (n >= 3) {
            const auto second = static_cast<unsigned char>(out[i + 1]);
            if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second >= 0xA0)
                || (c == 0xF0 && second < 0x90) || (c == 0xF4 && second >= 0x90)) return false;
        }
        i += n;
    }
    return true;
}
}

int readPlayerMap(const void* map, RawPlayer* out, int cap)
{
    if (cap < 0 || (cap > 0 && out == nullptr)) return -1;
    int used = 0;
    const bool ok = walk(map, kMapHead, kMapCount, kMaxPlayerEntries, [&](const void* node) {
        std::int64_t actorId = 0;
        char name[kMaxString + 1]{};
        std::size_t length = 0;
        if (!read(node, kPlayerId, actorId) || !readString(node, kPlayerName, name, length)) return false;
        if (length == 0 || used == cap) return true;
        std::size_t copied = std::min(length, std::size_t{96});
        if (copied < length)
            while (copied > 0 && (static_cast<unsigned char>(name[copied]) & 0xC0) == 0x80) --copied;
        out[used].actorId = actorId;
        std::memcpy(out[used].name, name, copied);
        out[used].name[copied] = '\0';
        out[used].nameLength = static_cast<std::uint32_t>(copied);
        const void* skin = nullptr;
        out[used].skin = read(node, kPlayerSkin, skin) && memory::plausiblePointer(skin) ? skin : nullptr;
        ++used;
        return true;
    });
    return ok ? used : -1;
}

bool findListObjective(const void* scoreboard, const void** outObjective)
{
    if (outObjective == nullptr) return false;
    *outObjective = nullptr;
    const void* result = nullptr;
    const bool ok = walk(scoreboard, kDisplayHead, kDisplayCount, 3, [&](const void* node) {
        char key[kMaxString + 1]{};
        std::size_t length = 0;
        if (!readString(node, kDisplayKey, key, length)) return false;
        if (length == 4 && std::memcmp(key, "list", 4) == 0)
            return read(node, kDisplayObjective, result);
        return true;
    });
    if (ok) *outObjective = result;
    return ok;
}

bool readScores(const void* scoreboard, const void* objective, const std::int64_t* ids, int count,
                bool* has, int* score)
{
    if (count < 0 || count > kMaxPlayerEntries || (count > 0 && (ids == nullptr || has == nullptr || score == nullptr))) return false;
    for (int i = 0; i < count; ++i) { has[i] = false; score[i] = 0; }
    if (count == 0) return true;
    if (!std::is_sorted(ids, ids + count)) return false;
    struct Wanted { std::int64_t id; int index; };
    std::vector<Wanted> wanted;
    wanted.reserve(static_cast<std::size_t>(count));
    const bool idsOk = walk(scoreboard, kIdHead, kIdCount, kMaxScoreEntries, [&](const void* node) {
        std::int64_t actor = 0, board = 0;
        if (!read(node, kIdKey, actor) || !read(node, kIdValue, board)) return false;
        const auto matches = std::equal_range(ids, ids + count, actor);
        for (auto at = matches.first; at != matches.second; ++at)
            wanted.push_back({board, static_cast<int>(at - ids)});
        return true;
    });
    if (!idsOk) return false;
    std::sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b) { return a.id < b.id; });
    struct Value { bool has = false; int score = 0; };
    std::vector<Value> values(static_cast<std::size_t>(count));
    const bool scoresOk = walk(objective, kScoreHead, kScoreCount, kMaxScoreEntries, [&](const void* node) {
        std::int64_t board = 0;
        std::int32_t value = 0;
        if (!read(node, kScoreKey, board) || !read(node, kScoreValue, value)) return false;
        auto at = std::lower_bound(wanted.begin(), wanted.end(), board,
                                  [](const Wanted& a, std::int64_t key) { return a.id < key; });
        for (; at != wanted.end() && at->id == board; ++at) values[at->index] = {true, value};
        return true;
    });
    if (!scoresOk) return false;
    for (int i = 0; i < count; ++i) { has[i] = values[i].has; score[i] = values[i].score; }
    return true;
}

bool readFace(const void* skin, std::uint8_t* out, int maxSide, int& side)
{
    side = 0;
    if (skin == nullptr || out == nullptr || maxSide <= 0) return false;
    const char* image = static_cast<const char*>(skin) + kSkinImage;
    std::uint32_t width = 0, height = 0;
    std::uint64_t size = 0;
    const std::uint8_t* data = nullptr;
    if (!read(image, kImageWidth, width) || !read(image, kImageHeight, height) || !read(image, kImageData, data)
        || !read(image, kImageSize, size) || data == nullptr) return false;
    if (width < 64 || width > 1024 || width % 64 != 0 || (height != width && height * 2 != width)
        || size != std::uint64_t(width) * height * 4) return false;
    const int scale = static_cast<int>(width / 64);
    const int full = 8 * scale;
    int step = 1;
    while (full / step > maxSide) step *= 2;
    side = full / step;
    std::vector<std::uint8_t> faceRow(static_cast<std::size_t>(full) * 4), hatRow(faceRow.size());
    for (int y = 0; y < side; ++y) {
        const std::size_t srcY = static_cast<std::size_t>(full + y * step);
        const std::uint8_t* rowStart = data + srcY * width * 4;
        if (!memory::copyGuarded(rowStart + static_cast<std::size_t>(full) * 4, faceRow.data(), faceRow.size())
            || !memory::copyGuarded(rowStart + static_cast<std::size_t>(5 * full) * 4, hatRow.data(), hatRow.size())) {
            side = 0;
            return false;
        }
        for (int x = 0; x < side; ++x) {
            const std::uint8_t* f = &faceRow[static_cast<std::size_t>(x * step) * 4];
            const std::uint8_t* h = &hatRow[static_cast<std::size_t>(x * step) * 4];
            std::uint8_t* o = out + (static_cast<std::size_t>(y) * side + x) * 4;
            const unsigned a = h[3];
            for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>((h[c] * a + f[c] * (255 - a) + 127) / 255);
            o[3] = static_cast<std::uint8_t>(std::max<unsigned>(f[3], a));
        }
    }
    return true;
}

bool readAttribute(const std::uint8_t* attributes, std::uint32_t id, float& current, float& maximum)
{
    if (attributes == nullptr) return false;
    const std::uint32_t* begin = nullptr;
    const std::uint32_t* end = nullptr;
    const char* instances = nullptr;
    std::memcpy(&begin, attributes + kAttrIdsBegin, sizeof(begin));
    std::memcpy(&end, attributes + kAttrIdsEnd, sizeof(end));
    std::memcpy(&instances, attributes + kAttrInstances, sizeof(instances));
    if (begin == nullptr || end < begin || instances == nullptr) return false;
    const std::size_t count = static_cast<std::size_t>(end - begin);
    if (count == 0 || count > kMaxAttributes) return false;
    std::uint32_t ids[kMaxAttributes];
    if (!memory::copyGuarded(begin, ids, count * sizeof(std::uint32_t))) return false;
    if (!std::is_sorted(ids, ids + count)) return false;
    const auto found = std::lower_bound(ids, ids + count, id);
    if (found == ids + count || *found != id) return false;
    const char* instance = instances + static_cast<std::size_t>(found - ids) * kAttrStride;
    float values[2]{};
    if (!memory::copyGuarded(instance + kAttrMaximum, values, sizeof(values))) return false;
    maximum = values[0];
    current = values[1];
    return maximum > 0.0f && maximum < 4096.0f && current >= 0.0f && current <= 4096.0f;
}
}
