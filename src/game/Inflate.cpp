#include "game/Inflate.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::inflate {

namespace {

class Bits {
public:
    Bits(const unsigned char* bytes, std::size_t size) : m_bytes(bytes), m_size(size) {}

    void refill()
    {
        while (m_count <= 56 && m_at < m_size) {
            m_buffer |= static_cast<std::uint64_t>(m_bytes[m_at++]) << m_count;
            m_count += 8;
        }
    }

    bool take(unsigned n, std::uint32_t& value)
    {
        if (m_count < n) {
            refill();
            if (m_count < n) return false;
        }
        value = n == 0 ? 0 : static_cast<std::uint32_t>(m_buffer & ((std::uint64_t{1} << n) - 1));
        m_buffer >>= n;
        m_count -= n;
        return true;
    }

    std::uint64_t peek() { refill(); return m_buffer; }
    unsigned count() const { return m_count; }
    void drop(unsigned n) { m_buffer >>= n; m_count -= n; }

    void align() { drop(m_count % 8); }

    bool copyBytes(std::size_t n, std::vector<char>& out)
    {
        while (n != 0 && m_count >= 8) {
            out.push_back(static_cast<char>(m_buffer & 0xff));
            drop(8);
            --n;
        }
        if (n > m_size - m_at) return false;
        out.insert(out.end(), reinterpret_cast<const char*>(m_bytes + m_at),
                   reinterpret_cast<const char*>(m_bytes + m_at + n));
        m_at += n;
        return true;
    }

    std::size_t used() const { return m_at - m_count / 8; }

private:
    const unsigned char* m_bytes;
    std::size_t m_size;
    std::size_t m_at = 0;
    std::uint64_t m_buffer = 0;
    unsigned m_count = 0;
};

constexpr unsigned kMaxBits = 15;
constexpr unsigned kFastBits = 10;

struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, 320> symbol{};
    std::array<std::uint16_t, 1u << kFastBits> fast{};

    bool build(const std::uint8_t* lengths, unsigned n)
    {
        count.fill(0);
        fast.fill(0);
        for (unsigned i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (unsigned len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        std::array<std::uint16_t, kMaxBits + 2> offset{};
        for (unsigned len = 1; len <= kMaxBits; ++len) offset[len + 1] = offset[len] + count[len];
        for (unsigned i = 0; i < n; ++i) {
            if (lengths[i] != 0) symbol[offset[lengths[i]]++] = static_cast<std::uint16_t>(i);
        }
        unsigned code = 0, index = 0;
        for (unsigned len = 1; len <= kFastBits; ++len) {
            for (unsigned k = 0; k < count[len]; ++k, ++code, ++index) {
                unsigned reversed = 0;
                for (unsigned b = 0; b < len; ++b) reversed |= ((code >> b) & 1u) << (len - 1 - b);
                const auto entry = static_cast<std::uint16_t>((len << 12) | symbol[index]);
                for (unsigned fill = reversed; fill < (1u << kFastBits); fill += 1u << len) fast[fill] = entry;
            }
            code <<= 1;
        }
        return true;
    }

    int decode(Bits& bits) const
    {
        const std::uint64_t peeked = bits.peek();
        const std::uint16_t entry = fast[peeked & ((1u << kFastBits) - 1)];
        if (entry != 0 && static_cast<unsigned>(entry >> 12) <= bits.count()) {
            bits.drop(entry >> 12);
            return entry & 0x0fff;
        }
        int code = 0, first = 0, index = 0;
        for (unsigned len = 1; len <= kMaxBits; ++len) {
            std::uint32_t bit = 0;
            if (!bits.take(1, bit)) return -1;
            code |= static_cast<int>(bit);
            const int here = count[len];
            if (code - here < first) return symbol[static_cast<std::size_t>(index + (code - first))];
            index += here;
            first += here;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr std::uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                           35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                           3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                         6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

const char* codes(Bits& bits, const Huffman& lengthCode, const Huffman& distCode, std::vector<char>& out,
                  std::size_t start, std::size_t maxOutput)
{
    for (;;) {
        const int symbol = lengthCode.decode(bits);
        if (symbol < 0) return "bad literal/length code";
        if (symbol < 256) {
            if (out.size() >= maxOutput) return "inflated data too large";
            out.push_back(static_cast<char>(symbol));
            continue;
        }
        if (symbol == 256) return nullptr;
        const int which = symbol - 257;
        if (which >= 29) return "bad length symbol";
        std::uint32_t extra = 0;
        if (!bits.take(kLengthExtra[which], extra)) return "truncated";
        const std::size_t length = kLengthBase[which] + extra;
        const int distSymbol = distCode.decode(bits);
        if (distSymbol < 0 || distSymbol >= 30) return "bad distance code";
        if (!bits.take(kDistExtra[distSymbol], extra)) return "truncated";
        const std::size_t distance = kDistBase[distSymbol] + extra;
        if (distance > out.size() - start) return "distance too far back";
        if (length > maxOutput - out.size()) return "inflated data too large";
        const std::size_t from = out.size() - distance;
        const std::size_t to = out.size();
        out.resize(to + length);
        char* const data = out.data();
        for (std::size_t i = 0; i < length; ++i) data[to + i] = data[from + i];
    }
}

const Huffman& fixedLength()
{
    static const Huffman table = [] {
        std::uint8_t lengths[288];
        unsigned i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        Huffman h;
        h.build(lengths, 288);
        return h;
    }();
    return table;
}

const Huffman& fixedDistance()
{
    static const Huffman table = [] {
        std::uint8_t lengths[30];
        std::memset(lengths, 5, sizeof lengths);
        Huffman h;
        h.build(lengths, 30);
        return h;
    }();
    return table;
}

const char* dynamicBlock(Bits& bits, std::vector<char>& out, std::size_t start, std::size_t maxOutput)
{
    std::uint32_t nlen = 0, ndist = 0, ncode = 0;
    if (!bits.take(5, nlen) || !bits.take(5, ndist) || !bits.take(4, ncode)) return "truncated";
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return "bad code counts";
    static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::uint8_t lengths[320]{};
    for (unsigned i = 0; i < ncode; ++i) {
        std::uint32_t len = 0;
        if (!bits.take(3, len)) return "truncated";
        lengths[kOrder[i]] = static_cast<std::uint8_t>(len);
    }
    Huffman lencode;
    if (!lencode.build(lengths, 19)) return "bad code-length code";
    std::memset(lengths, 0, sizeof lengths);
    unsigned at = 0;
    while (at < nlen + ndist) {
        const int symbol = lencode.decode(bits);
        if (symbol < 0) return "bad code-length symbol";
        if (symbol < 16) {
            lengths[at++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t repeat = 0;
        std::uint32_t times = 0;
        if (symbol == 16) {
            if (at == 0) return "repeat with no previous length";
            repeat = lengths[at - 1];
            if (!bits.take(2, times)) return "truncated";
            times += 3;
        } else if (symbol == 17) {
            if (!bits.take(3, times)) return "truncated";
            times += 3;
        } else {
            if (!bits.take(7, times)) return "truncated";
            times += 11;
        }
        if (at + times > nlen + ndist) return "too many code lengths";
        while (times-- != 0) lengths[at++] = repeat;
    }
    if (lengths[256] == 0) return "no end-of-block code";
    Huffman lengthCode, distCode;
    if (!lengthCode.build(lengths, nlen)) return "bad literal/length lengths";
    if (!distCode.build(lengths + nlen, ndist)) return "bad distance lengths";
    return codes(bits, lengthCode, distCode, out, start, maxOutput);
}

std::uint32_t crc32(const char* bytes, std::size_t size)
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ static_cast<unsigned char>(bytes[i])) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

std::uint32_t le32(const unsigned char* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}

bool isGzip(const char* bytes, std::size_t size)
{
    return bytes != nullptr && size >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1f
           && static_cast<unsigned char>(bytes[1]) == 0x8b;
}

const char* inflateRaw(const char* bytes, std::size_t size, std::vector<char>& out, std::size_t maxOutput,
                       std::size_t* used)
{
    Bits bits(reinterpret_cast<const unsigned char*>(bytes), size);
    const std::size_t start = out.size();
    std::uint32_t last = 0;
    do {
        std::uint32_t type = 0;
        if (!bits.take(1, last) || !bits.take(2, type)) return "truncated";
        const char* why = nullptr;
        if (type == 0) {
            bits.align();
            std::uint32_t len = 0, nlen = 0;
            if (!bits.take(16, len) || !bits.take(16, nlen)) return "truncated";
            if ((len ^ 0xffffu) != nlen) return "stored block length mismatch";
            if (len > maxOutput - out.size()) return "inflated data too large";
            if (!bits.copyBytes(len, out)) return "truncated";
        } else if (type == 1) {
            why = codes(bits, fixedLength(), fixedDistance(), out, start, maxOutput);
        } else if (type == 2) {
            why = dynamicBlock(bits, out, start, maxOutput);
        } else {
            return "bad block type";
        }
        if (why != nullptr) return why;
    } while (last == 0);
    if (used != nullptr) *used = bits.used();
    return nullptr;
}

const char* gunzip(const char* bytes, std::size_t size, std::vector<char>& out, std::size_t maxOutput)
{
    const auto* const p = reinterpret_cast<const unsigned char*>(bytes);
    std::size_t at = 0;
    bool any = false;
    while (size - at >= 18 && isGzip(bytes + at, size - at)) {
        if (p[at + 2] != 8) return "gzip: not deflate";
        const unsigned flags = p[at + 3];
        std::size_t head = at + 10;
        if (flags & 4) {
            if (size - head < 2) return "gzip: truncated header";
            const std::size_t extra = p[head] | (static_cast<std::size_t>(p[head + 1]) << 8);
            if (size - head - 2 < extra) return "gzip: truncated header";
            head += 2 + extra;
        }
        for (const unsigned flag : {8u, 16u}) {
            if (!(flags & flag)) continue;
            while (head < size && p[head] != 0) ++head;
            if (head >= size) return "gzip: truncated header";
            ++head;
        }
        if (flags & 2) head += 2;
        if (head > size) return "gzip: truncated header";
        const std::size_t begin = out.size();
        std::size_t used = 0;
        if (const char* why = inflateRaw(bytes + head, size - head, out, maxOutput, &used)) return why;
        const std::size_t tail = head + used;
        if (size - tail < 8) return "gzip: truncated trailer";
        if (le32(p + tail) != crc32(out.data() + begin, out.size() - begin)) return "gzip: crc mismatch";
        if (le32(p + tail + 4) != static_cast<std::uint32_t>(out.size() - begin)) return "gzip: size mismatch";
        at = tail + 8;
        any = true;
    }
    return any ? nullptr : "not gzip";
}

}
