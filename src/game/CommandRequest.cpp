#include "game/CommandRequest.h"

#include "game/GameData.h"

#include <Windows.h>

#include <cstring>

#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

constexpr std::size_t kSenderVtableDisp = 3;

constexpr std::size_t kPlayerVtableDisp = 3;

const std::byte* vtableFrom(Target target, std::size_t disp)
{
    std::byte* const ref = Scanner::instance().address(target);
    if (ref == nullptr) {
        return nullptr;
    }
    return static_cast<const std::byte*>(memory::ripTarget(ref, disp));
}

constexpr std::ptrdiff_t kEntityContextOffset = 0x08;

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool readPointerGuarded(const void* address, void*& value)
{
    __try {
        value = *static_cast<void* const*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

void collectRegionGuarded(const std::byte* vtable, std::byte* base, const std::byte* end,
                          std::byte** out, int capacity, int& count)
{
    __try {
        for (auto* p = base; p + 0x20 <= end; p += sizeof(void*)) {
            if (*reinterpret_cast<const std::byte* const*>(p) != vtable) {
                continue;
            }
            if (count < capacity) {
                out[count] = p;
            }
            ++count;
        }
    } __except (accessViolationFilter(GetExceptionCode())) {

    }
}

int faultFilter(EXCEPTION_POINTERS* info, const void** faultPc, const void** faultAddress)
{
    const unsigned long code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    *faultPc = info->ExceptionRecord->ExceptionAddress;
    *faultAddress = info->ExceptionRecord->NumberParameters >= 2
                        ? reinterpret_cast<const void*>(
                              info->ExceptionRecord->ExceptionInformation[1])
                        : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

template <class Fn, class... Args>
bool callGuarded(const void** faultPc, const void** faultAddress, Fn fn, Args... args)
{
    __try {
        fn(args...);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

struct ModuleRange {
    const std::byte* base = nullptr;
    std::size_t size = 0;

    bool contains(const void* address) const
    {
        const auto* const value = static_cast<const std::byte*>(address);
        return base != nullptr && value >= base && value < base + size;
    }

    std::size_t rvaOf(const void* address) const
    {
        return contains(address)
                   ? static_cast<std::size_t>(static_cast<const std::byte*>(address) - base)
                   : 0;
    }
};

const ModuleRange& mainModule()
{
    static const ModuleRange range = [] {
        ModuleRange result;
        const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
        if (base == nullptr) {
            return result;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return result;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return result;
        }
        result.base = base;
        result.size = nt->OptionalHeader.SizeOfImage;
        return result;
    }();
    return range;
}

bool hasVtable(const void* address, Target target, std::size_t disp)
{
    const std::byte* const vtable = vtableFrom(target, disp);
    if (address == nullptr || vtable == nullptr) {
        return false;
    }
    void* head = nullptr;
    return readPointerGuarded(address, head) && head == vtable;
}

}

CommandRequest& CommandRequest::instance()
{
    static CommandRequest object;
    return object;
}

void CommandRequest::onScansReady()
{
    m_send = Scanner::instance().addressAs<SendCommandFn>(Target::SendCommandRequest);
    m_makeOrigin = Scanner::instance().addressAs<MakeCommandOriginFn>(Target::MakeCommandOrigin);
    if (!available()) {
        log().warn(L"CommandRequest: the chat command path is unavailable");
    }
}

bool CommandRequest::available() const
{
    return m_send != nullptr && m_makeOrigin != nullptr;
}

void CommandRequest::onEntityContext(void* entityContext)
{

    if (entityContext == nullptr) {
        return;
    }
    auto* const player = static_cast<std::byte*>(entityContext) - kEntityContextOffset;
    if (!hasVtable(player, Target::PlayerVtableRef, kPlayerVtableDisp)) {

        if (!m_warnedBadPlayer.exchange(true, std::memory_order_acq_rel)) {
            void* head = nullptr;
            readPointerGuarded(player, head);
            log().info(L"CommandRequest: skipped a game mode target whose owner is not the "
                       L"local player (vtable rva {:#x})",
                       mainModule().rvaOf(head));
        }
        return;
    }

    GameData::instance().setPlayer(player);

    if (m_player.exchange(player, std::memory_order_acq_rel) != player) {

        m_sender.store(nullptr, std::memory_order_release);
        m_warnedNoPlayer.store(false, std::memory_order_release);
        m_nextSenderWarnMs.store(0, std::memory_order_release);
        m_nextPlayerWarnMs.store(0, std::memory_order_release);
        log().info(L"CommandRequest: player found at {:#x}",
                   reinterpret_cast<std::uintptr_t>(player));
    }
}

int CommandRequest::scoreSender(const std::byte* candidate)
{
    const ModuleRange& module = mainModule();

    if ((reinterpret_cast<std::uintptr_t>(candidate) & 0xF) != 0) {
        return -1;
    }

    constexpr std::ptrdiff_t kFields[] = {0x08, 0x10, 0x18};
    constexpr int kFieldCount = 3;
    void* fields[kFieldCount]{};
    for (int i = 0; i < kFieldCount; ++i) {
        if (!memory::isReadable(candidate + kFields[i], sizeof(void*))
            || !readPointerGuarded(candidate + kFields[i], fields[i])) {
            return -1;
        }
        const auto value = reinterpret_cast<std::uintptr_t>(fields[i]);

        if (value == 0 || value >= kUserAddressLimit || (value & 0x7) != 0
            || module.contains(fields[i])) {
            return -1;
        }
    }

    int score = 0;
    for (int i = 0; i < kFieldCount; ++i) {
        if (kFields[i] == 0x10) {
            continue;
        }
        void* head = nullptr;
        if (memory::isReadable(fields[i], sizeof(void*)) && readPointerGuarded(fields[i], head)
            && module.contains(head)) {
            ++score;
        }
    }
    return score;
}

void* CommandRequest::findSender(int& survivors)
{
    survivors = 0;
    const ModuleRange& module = mainModule();
    if (module.base == nullptr) {
        return nullptr;
    }
    const auto* const vtable = vtableFrom(Target::SenderVtableRef, kSenderVtableDisp);
    if (vtable == nullptr) {
        return nullptr;
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* address = static_cast<std::byte*>(info.lpMinimumApplicationAddress);
    auto* const limit = static_cast<std::byte*>(info.lpMaximumApplicationAddress);

    std::byte* raw[kMaxSenderCandidates]{};
    int rawCount = 0;

    MEMORY_BASIC_INFORMATION region{};
    while (address < limit && VirtualQuery(address, &region, sizeof(region)) == sizeof(region)) {
        auto* const base = static_cast<std::byte*>(region.BaseAddress);
        const std::size_t size = region.RegionSize;
        const bool usable = region.State == MEM_COMMIT && region.Type == MEM_PRIVATE
                            && (region.Protect == PAGE_READWRITE
                                || region.Protect == PAGE_WRITECOPY)
                            && (region.Protect & PAGE_GUARD) == 0;
        if (usable) {

            auto* const end = base + (size & ~static_cast<std::size_t>(7));
            collectRegionGuarded(vtable, base, end, raw, kMaxSenderCandidates, rawCount);
        }
        address = base + size;
    }

    std::byte* best = nullptr;
    int bestScore = -1;
    const int checked = (rawCount < kMaxSenderCandidates) ? rawCount : kMaxSenderCandidates;
    for (int i = 0; i < checked; ++i) {
        const int score = scoreSender(raw[i]);
        if (score < 0) {
            continue;
        }
        ++survivors;
        if (score > bestScore) {
            bestScore = score;
            best = raw[i];
        }
    }

    if (survivors > 1) {

        log().warn(L"CommandRequest: {} command sender candidates passed the check, "
                   L"using the most likely one (score {})",
                   survivors, bestScore);
    }
    return best;
}

void* CommandRequest::sender()
{
    void* cached = m_sender.load(std::memory_order_acquire);
    if (cached != nullptr) {

        if (hasVtable(cached, Target::SenderVtableRef, kSenderVtableDisp)) {
            return cached;
        }
        m_sender.store(nullptr, std::memory_order_release);
    }

    int survivors = 0;
    void* const found = findSender(survivors);
    if (found == nullptr) {

        const unsigned long long now = GetTickCount64();
        if (now >= m_nextSenderWarnMs.load(std::memory_order_acquire)) {
            m_nextSenderWarnMs.store(now + kWarnIntervalMs, std::memory_order_release);
            log().warn(L"CommandRequest: no command sender in memory, "
                       L"chat commands are not sent");
        }
        return nullptr;
    }

    m_sender.store(found, std::memory_order_release);
    m_nextSenderWarnMs.store(0, std::memory_order_release);
    log().info(L"CommandRequest: command sender found at {:#x} ({} candidate{} passed the check)",
               reinterpret_cast<std::uintptr_t>(found), survivors,
               (survivors == 1) ? L"" : L"s");
    return found;
}

const void* CommandRequest::makeOrigin()
{
    if (m_originBroken.load(std::memory_order_acquire)) {
        return nullptr;
    }
    void* const player = m_player.load(std::memory_order_acquire);

    if (player == nullptr
        || !hasVtable(player, Target::PlayerVtableRef, kPlayerVtableDisp)) {
        m_player.store(nullptr, std::memory_order_release);

        m_warnedNoPlayer.store(false, std::memory_order_release);

        const unsigned long long now = GetTickCount64();
        if (player != nullptr && now >= m_nextPlayerWarnMs.load(std::memory_order_acquire)) {
            m_nextPlayerWarnMs.store(now + kWarnIntervalMs, std::memory_order_release);
            log().warn(L"CommandRequest: the player went stale, re-enter the world");
        }
        return nullptr;
    }

    std::memset(m_origin, 0, kOriginSize);
    std::memset(m_origin + kOriginSize, static_cast<int>(kOriginGuardByte), kOriginGuardSize);

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!callGuarded(&faultPc, &faultAddress, m_makeOrigin, static_cast<void*>(m_origin),
                     player)) {
        const ModuleRange& module = mainModule();
        log().error(L"CommandRequest: building the command origin faulted "
                    L"(pc rva {:#x}, touched {:#x}), dropping the player",
                    module.rvaOf(faultPc), reinterpret_cast<std::uintptr_t>(faultAddress));
        m_player.store(nullptr, std::memory_order_release);
        m_warnedNoPlayer.store(false, std::memory_order_release);
        return nullptr;
    }

    for (std::size_t i = 0; i < kOriginGuardSize; ++i) {
        if (m_origin[kOriginSize + i] == kOriginGuardByte) {
            continue;
        }

        m_originBroken.store(true, std::memory_order_release);
        log().error(L"CommandRequest: the command origin constructor wrote past {} bytes, "
                    L"the signature is pointing at the wrong function",
                    kOriginSize);
        return nullptr;
    }

    void* head = nullptr;
    if (!readPointerGuarded(m_origin, head) || !mainModule().contains(head)) {
        m_originBroken.store(true, std::memory_order_release);
        log().error(L"CommandRequest: the command origin has no usable vtable");
        return nullptr;
    }
    if (!m_loggedOrigin.exchange(true, std::memory_order_acq_rel)) {

        log().info(L"CommandRequest: command origin built (vtable rva {:#x})",
                   mainModule().rvaOf(head));
    }
    return m_origin;
}

bool CommandRequest::run(const char* command)
{
    if (!available() || command == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(command);
    if (length == 0 || length > kMaxLength) {
        return false;
    }

    if (m_player.load(std::memory_order_acquire) == nullptr) {
        if (!m_warnedNoPlayer.exchange(true, std::memory_order_acq_rel)) {

            log().warn(L"CommandRequest: waiting for the player, re-enter the world");
        }
        return false;
    }

    void* const target = sender();
    if (target == nullptr) {
        return false;
    }

    const void* const origin = makeOrigin();
    if (origin == nullptr) {
        return false;
    }

    Args args;
    args.origin = origin;
    args.version = kCommandVersion;
    args.text.length = length;

    if (length <= 15) {
        std::memcpy(args.text.data.inline_, command, length);
        args.text.data.inline_[length] = '\0';
        args.text.capacity = 15;
    } else {
        std::memcpy(m_text, command, length);
        m_text[length] = '\0';
        args.text.data.pointer = m_text;
        args.text.capacity = kMaxLength;
    }

    std::int32_t out = 0;
    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;

    if (!callGuarded(&faultPc, &faultAddress, m_send, target, &out,
                     static_cast<const Args*>(&args), 0)) {
        const ModuleRange& module = mainModule();
        log().error(L"CommandRequest: running a command faulted "
                    L"(pc rva {:#x}, touched {:#x}), dropping the command sender",
                    module.rvaOf(faultPc), reinterpret_cast<std::uintptr_t>(faultAddress));
        m_sender.store(nullptr, std::memory_order_release);
        return false;
    }
    return true;
}

}
