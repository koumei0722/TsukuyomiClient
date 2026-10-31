#include "game/OreUiBundlePatch.h"

#include <string_view>

namespace tsukuyomi::oreui {
namespace {

bool identifierChar(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9') || ch == '_' || ch == '$';
}

bool identifier(std::string_view name)
{
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) return false;
    for (const char ch : name) if (!identifierChar(ch)) return false;
    return true;
}

std::size_t identifierBegin(std::string_view text, std::size_t end)
{
    while (end > 0 && identifierChar(text[end - 1])) --end;
    return end;
}

}

bool patchSettingsTabs(std::string& bundle)
{
    constexpr std::string_view anchor = "\"settings-tabs-groups\"";
    const std::string_view source(bundle);
    const auto at = source.find(anchor);
    if (at == source.npos || source.find(anchor, at + anchor.size()) != source.npos || at == 0
        || source[at - 1] != '(' || source.substr(at + anchor.size(), 3) != ")])") return false;

    const auto queryBegin = identifierBegin(source, at - 1);
    if (!identifier(source.substr(queryBegin, at - 1 - queryBegin)) || queryBegin < 5
        || source.substr(queryBegin - 5, 5) != ",[],[") return false;
    const auto end = queryBegin - 5;
    constexpr std::string_view filter = ".settings.filter(";
    const auto filterAt = source.rfind(filter, end);
    if (filterAt == source.npos) return false;
    const auto receiverBegin = identifierBegin(source, filterAt);
    const auto receiver = source.substr(receiverBegin, filterAt - receiverBegin);
    if (!identifier(receiver) || receiverBegin < 2 || source.substr(receiverBegin - 2, 2) != "=>") return false;
    const auto begin = identifierBegin(source, receiverBegin - 2);
    if (source.substr(begin, receiverBegin - 2 - begin) != receiver) return false;

    const auto predicateBegin = filterAt + filter.size();
    const auto arrow = source.find("=>", predicateBegin);
    if (arrow == source.npos || arrow >= end) return false;
    const auto item = source.substr(predicateBegin, arrow - predicateBegin);
    if (!identifier(item)) return false;
    const std::string typeHead = std::string(item) + ".type===";
    const auto typeBegin = arrow + 2;
    if (source.substr(typeBegin, typeHead.size()) != typeHead) return false;
    const auto enumBegin = typeBegin + typeHead.size();
    const auto enumEnd = source.find('.', enumBegin);
    if (enumEnd == source.npos || enumEnd >= end) return false;
    const auto enumName = source.substr(enumBegin, enumEnd - enumBegin);
    if (!identifier(enumName) || source.substr(enumEnd, end - enumEnd) != ".GroupInfo)") return false;

    std::string local = "tsukuyomiTabs";
    while (local == receiver) local += '$';
    const std::string filtered(source.substr(receiverBegin, end - receiverBegin));
    const std::string replacement = std::string(receiver) + "=>{const " + local + '=' + filtered
        + ";return " + local + ".some(tab=>tab.id===\"tsukuyomi\")?" + local + ':' + local
        + ".concat({id:\"tsukuyomi\",type:" + std::string(enumName) + ".GroupInfo})}";
    bundle.replace(begin, end - begin, replacement);
    return true;
}

}
