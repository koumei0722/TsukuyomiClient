#include "game/PngWrite.h"

#include <array>

namespace tsukuyomi::png {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0)
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void putBE32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void chunk(std::vector<std::uint8_t>& out, const char (&type)[5], const std::vector<std::uint8_t>& body)
{
    putBE32(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    putBE32(out, crc32(out.data() + start, out.size() - start));
}

}

std::vector<std::uint8_t> encodeRgba(const std::uint8_t* rgba, int width, int height)
{
    if (rgba == nullptr || width <= 0 || height <= 0 || width > 4096 || height > 4096) return {};
    std::vector<std::uint8_t> raw;
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    raw.reserve((stride + 1) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + y * stride, rgba + (y + 1) * stride);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t n = raw.size() - at < 65535 ? raw.size() - at : 65535;
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < raw.size());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    putBE32(z, (b << 16) | a);

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    putBE32(ihdr, static_cast<std::uint32_t>(width));
    putBE32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

}
