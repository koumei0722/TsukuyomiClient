#include "memory/Scanner.h"

#include "core/Logger.h"
#include "game/GameVersion.h"

#include <libhat/process.hpp>
#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>

namespace tsukuyomi {

ScanHit scanRange(std::span<std::byte> range, std::string_view signature)
{
    ScanHit hit;
    if (range.empty()) {
        return hit;
    }

    auto parsed = hat::parse_signature(signature);
    if (!parsed.has_value()) {
        return hit;
    }

    const hat::signature& sig = parsed.value();
    const hat::signature_view view{sig};

    std::byte* const begin = range.data();
    std::byte* const end = begin + range.size();

    const auto first = hat::find_pattern(begin, end, view);
    if (!first.has_result()) {
        return hit;
    }

    hit.address = first.get();
    hit.count = 1;

    std::byte* const next = first.get() + 1;
    if (next < end) {
        const auto second = hat::find_pattern(next, end, view);
        if (second.has_result()) {
            hit.count = 2;
        }
    }

    return hit;
}

ScanHit scanMainModule(std::string_view signature, std::string_view section)
{
    const auto mod = hat::process::get_process_module();
    return scanRange(mod.get_section_data(section), signature);
}

Scanner& Scanner::instance()
{
    static Scanner scanner;
    return scanner;
}

size_t Scanner::scanAll()
{
    gameVersion::logOnce();
    const auto mod = hat::process::get_process_module();
    const auto text = mod.get_section_data(".text");
    if (text.empty()) {
        log().error(L"Could not locate the .text section. All features are disabled");
        return 0;
    }

    const std::byte* const base = mod.get_module_data().data();
    size_t found = 0;

    for (const TargetInfo& info : kTargets) {
        const ScanHit hit = scanRange(text, info.signature);
        const size_t index = static_cast<size_t>(info.target);

        switch (hit.count) {
        case 1: {
            m_addresses[index] = hit.address;
            ++found;
            const auto rva = static_cast<uintptr_t>(hit.address - base);
            log().success(L"{} found (RVA {:#x})", info.name, rva);
            break;
        }
        case 0:
            log().warn(L"{} not found - {} is unavailable", info.name, info.purpose);
            break;
        default:

            log().warn(L"{} matched more than once - ignoring to avoid a false positive", info.name);
            break;
        }
    }

    log().info(L"Scanned {} signatures, {} found", std::size(kTargets), found);
    return found;
}

std::byte* Scanner::address(Target target) const
{
    const auto index = static_cast<size_t>(target);
    return index < m_addresses.size() ? m_addresses[index] : nullptr;
}

bool Scanner::found(Target target) const
{
    return address(target) != nullptr;
}

}
