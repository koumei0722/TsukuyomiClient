#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace tsukuyomi {

class Patch {
public:
    Patch() = default;
    Patch(void* address, std::vector<std::byte> patched, const char* name = nullptr);
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

    static bool setAll(std::span<Patch* const> patches, bool enabled);

private:
    friend Patch makeAtomicPatch(void* address, std::vector<std::byte> bytes, const char* name);
    friend Patch makeStubPatch(void* function, std::span<const std::byte> stub, const char* name);

    void reset();
    bool write(const std::byte* source);

    std::byte* m_address = nullptr;
    std::vector<std::byte> m_original;
    std::vector<std::byte> m_patched;
    bool m_applied = false;
    bool m_lockFree = false;
};

Patch makeSkipPatch(void* address, size_t size, const char* name = nullptr);

Patch makeAtomicPatch(void* address, std::vector<std::byte> bytes, const char* name = nullptr);

Patch makeStubPatch(void* function, std::span<const std::byte> stub, const char* name = nullptr);

std::size_t movssStoreLength(const void* address);

}
