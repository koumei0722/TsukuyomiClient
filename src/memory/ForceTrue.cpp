#include "memory/ForceTrue.h"

namespace tsukuyomi::memory {

bool makeForceTrueBytes(const std::byte* at, std::size_t& sizeOut,
                        std::vector<std::byte>& out)
{
    const auto* const b = reinterpret_cast<const unsigned char*>(at);
    unsigned char rex = 0;
    std::size_t head = 0;
    if (b[0] >= 0x40 && b[0] <= 0x4F) {
        rex = b[0];
        head = 1;
    }
    const bool isMovzx = (b[head] == 0x0F && b[head + 1] == 0xB6);
    const bool isMov8 = (b[head] == 0x8A);
    if (!isMovzx && !isMov8) {
        return false;
    }
    const std::size_t modrmAt = head + (isMovzx ? 2u : 1u);
    const unsigned char modrm = b[modrmAt];
    if ((modrm & 0xC7) != 0x40 || b[modrmAt + 1] != 0x10) {
        return false;
    }
    const unsigned reg = (modrm >> 3) & 7;
    const bool regHigh = (rex & 0x04) != 0;
    const std::size_t size = modrmAt + 2;

    out.clear();
    if (isMov8) {
        if (rex != 0 || regHigh) {
            out.push_back(static_cast<std::byte>(0x40 | (regHigh ? 0x01 : 0x00)));
        }
        out.push_back(static_cast<std::byte>(0xB0 + reg));
        out.push_back(std::byte{0x01});
    } else {
        if (regHigh) {
            return false;
        }
        out.push_back(std::byte{0x31});
        out.push_back(static_cast<std::byte>(0xC0 | (reg << 3) | reg));
        out.push_back(static_cast<std::byte>(0xB0 + reg));
        out.push_back(std::byte{0x01});
    }
    if (out.size() > size) {
        return false;
    }
    while (out.size() < size) {
        out.push_back(std::byte{0x90});
    }
    sizeOut = size;
    return true;
}

}
