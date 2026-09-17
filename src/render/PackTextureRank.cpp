#include "render/PackTexture.h"

#include <cstdlib>

namespace tsukuyomi::pack {
namespace {

int parseVersion(const char* at, const char* end)
{
    int part[3] = {0, 0, 0};
    int index = 0;
    bool anyDigit = false;
    for (const char* p = at; p != end; ++p) {
        if (*p >= '0' && *p <= '9') {
            part[index] = part[index] * 10 + (*p - '0');
            if (part[index] > 999) {
                return -1;
            }
            anyDigit = true;
            continue;
        }
        if (*p == '.') {
            if (!anyDigit || index >= 2) {
                return -1;
            }
            ++index;
            anyDigit = false;
            continue;
        }
        return -1;
    }
    if (!anyDigit) {
        return -1;
    }
    return part[0] * 1000000 + part[1] * 1000 + part[2];
}

}

int packRank(const std::string& name, int limit)
{
    if (name == "vanilla" || name == "vanilla_base") {
        return 0;
    }
    constexpr char kPrefix[] = "vanilla_";
    constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (name.size() <= kPrefixLen || name.compare(0, kPrefixLen, kPrefix) != 0) {
        return -1;
    }
    const int rank = parseVersion(name.data() + kPrefixLen, name.data() + name.size());
    if (rank < 0) {
        return -1;
    }
    return rank <= limit ? rank : -1;
}

int versionRank(const std::string& version)
{
    const char* const begin = version.data();
    const char* end = begin + version.size();
    int dots = 0;
    for (const char* p = begin; p != end; ++p) {
        if (*p == '.' && ++dots == 3) {
            end = p;
            break;
        }
    }
    const int rank = parseVersion(begin, end);
    return rank < 0 ? 0x7fffffff : rank;
}

}
