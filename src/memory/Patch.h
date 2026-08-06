#pragma once

#include <cstddef>
#include <vector>

namespace tsukuyomi {

class Patch {
public:
    Patch() = default;
    Patch(void* address, std::vector<std::byte> patched);
    ~Patch();

    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;
    Patch(Patch&& other) noexcept;
    Patch& operator=(Patch&& other) noexcept;

    bool valid() const { return m_address != nullptr && !m_patched.empty(); }
    bool applied() const { return m_applied; }

    bool apply();
    bool restore();

    bool setEnabled(bool enabled) { return enabled ? apply() : restore(); }

private:
    void reset();

    std::byte* m_address = nullptr;
    std::vector<std::byte> m_original;
    std::vector<std::byte> m_patched;
    bool m_applied = false;
};

Patch makeNopPatch(void* address, size_t size);

}
