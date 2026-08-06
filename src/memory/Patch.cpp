#include "memory/Patch.h"

#include <Windows.h>

#include <cstring>
#include <utility>

namespace tsukuyomi {

namespace {

bool writeBytes(std::byte* address, const std::byte* source, size_t size)
{
    if (address == nullptr || size == 0) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(address, source, size);

    DWORD ignored = 0;
    VirtualProtect(address, size, oldProtect, &ignored);

    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

}

Patch::Patch(void* address, std::vector<std::byte> patched)
    : m_address(static_cast<std::byte*>(address))
    , m_patched(std::move(patched))
{
}

Patch::~Patch()
{
    restore();
}

Patch::Patch(Patch&& other) noexcept
    : m_address(other.m_address)
    , m_original(std::move(other.m_original))
    , m_patched(std::move(other.m_patched))
    , m_applied(other.m_applied)
{
    other.reset();
}

Patch& Patch::operator=(Patch&& other) noexcept
{
    if (this != &other) {
        restore();

        m_address = other.m_address;
        m_original = std::move(other.m_original);
        m_patched = std::move(other.m_patched);
        m_applied = other.m_applied;

        other.reset();
    }
    return *this;
}

void Patch::reset()
{
    m_address = nullptr;
    m_original.clear();
    m_patched.clear();
    m_applied = false;
}

bool Patch::apply()
{
    if (!valid()) {
        return false;
    }
    if (m_applied) {
        return true;
    }

    if (m_original.empty()) {
        m_original.assign(m_address, m_address + m_patched.size());
    }

    if (!writeBytes(m_address, m_patched.data(), m_patched.size())) {
        return false;
    }

    m_applied = true;
    return true;
}

bool Patch::restore()
{
    if (!m_applied || m_original.empty()) {
        return false;
    }

    if (!writeBytes(m_address, m_original.data(), m_original.size())) {
        return false;
    }

    m_applied = false;
    return true;
}

Patch makeNopPatch(void* address, size_t size)
{
    return Patch(address, std::vector<std::byte>(size, std::byte{0x90}));
}

}
