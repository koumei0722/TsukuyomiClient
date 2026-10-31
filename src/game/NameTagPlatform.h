#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace tsukuyomi::nametag {

inline std::string_view platformLabel(std::int32_t platform)
{
    switch (platform) {
    case 1: return "Android";
    case 2: return "iOS";
    case 3: return "macOS";
    case 4: return "Fire OS";
    case 5: return "Gear VR";
    case 6: return "HoloLens";
    case 7:
    case 8: return "Windows";
    case 9: return "Dedicated";
    case 10: return "tvOS";
    case 11: return "PlayStation";
    case 12: return "Switch";
    case 13: return "Xbox";
    case 14: return "Windows Phone";
    case 15: return "Linux";
    default: return {};
    }
}

inline std::string withPlatform(std::string_view text, std::string_view label)
{
    const std::size_t end = std::min(text.find('\n'), text.size());
    std::string out;
    out.reserve(text.size() + label.size() + 8);
    out.append(text.substr(0, end));
    out.append(" \xC2\xA7" "7[");
    out.append(label);
    out.append("]\xC2\xA7r");
    out.append(text.substr(end));
    return out;
}

}
