#include "game/AreaSelection.h"

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>

namespace tsukuyomi::selection {
bool Area::boundsOf(std::int32_t lo[3], std::int32_t hi[3]) const
{
    bool found = false;
    for (const auto& b : boxes) {
        if (!b.has1 || !b.has2) continue;
        for (int a = 0; a < 3; ++a) {
            const auto low = (std::min)(b.p1[a], b.p2[a]);
            const auto high = (std::max)(b.p1[a], b.p2[a]);
            lo[a] = found ? (std::min)(lo[a], low) : low;
            hi[a] = found ? (std::max)(hi[a], high) : high;
        }
        found = true;
    }
    return found;
}

bool Area::insideAnyBox(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    const std::int32_t p[3]{x,y,z};
    for (const auto& b : boxes) {
        if (!b.has1 || !b.has2) continue;
        bool inside = true;
        for (int a = 0; a < 3; ++a)
            inside &= p[a] >= (std::min)(b.p1[a],b.p2[a]) && p[a] <= (std::max)(b.p1[a],b.p2[a]);
        if (inside) return true;
    }
    return false;
}

std::uint64_t Area::blockCount() const
{
    std::vector<std::int64_t> xs, ys;
    for (const auto& b : boxes) if (b.has1 && b.has2) {
        xs.push_back((std::min)(b.p1[0],b.p2[0])); xs.push_back(std::int64_t((std::max)(b.p1[0],b.p2[0]))+1);
        ys.push_back((std::min)(b.p1[1],b.p2[1])); ys.push_back(std::int64_t((std::max)(b.p1[1],b.p2[1]))+1);
    }
    const auto unique = [](auto& v) { std::sort(v.begin(),v.end()); v.erase(std::unique(v.begin(),v.end()),v.end()); };
    unique(xs); unique(ys);
    std::uint64_t result = 0;
    const auto max = (std::numeric_limits<std::uint64_t>::max)();
    for (std::size_t i=1;i<xs.size();++i) for (std::size_t j=1;j<ys.size();++j) {
        std::vector<std::pair<std::int64_t,std::int64_t>> zs;
        for (const auto& b : boxes) if (b.has1 && b.has2
            && xs[i-1]>=(std::min)(b.p1[0],b.p2[0]) && xs[i-1]<=(std::max)(b.p1[0],b.p2[0])
            && ys[j-1]>=(std::min)(b.p1[1],b.p2[1]) && ys[j-1]<=(std::max)(b.p1[1],b.p2[1]))
            zs.emplace_back((std::min)(b.p1[2],b.p2[2]),std::int64_t((std::max)(b.p1[2],b.p2[2]))+1);
        std::sort(zs.begin(),zs.end());
        std::uint64_t depth=0;
        std::int64_t end=(std::numeric_limits<std::int64_t>::min)();
        for (const auto& [low,high] : zs) if (high>end) { depth+=static_cast<std::uint64_t>(high-(std::max)(low,end)); end=high; }
        const auto width=static_cast<std::uint64_t>(xs[i]-xs[i-1]), height=static_cast<std::uint64_t>(ys[j]-ys[j-1]);
        if (depth && width>max/height) return max;
        const auto volume=width*height;
        if (depth && (volume>max/depth || volume*depth>max-result)) return max;
        result+=volume*depth;
    }
    return result;
}

std::string toJson(const Area& area)
{
    nlohmann::json j{{"name",area.name},{"selected",area.selected},{"originSet",area.originSet},
        {"origin",area.origin},{"boxes",nlohmann::json::array()}};
    for (const auto& b : area.boxes) j["boxes"].push_back({{"name",b.name},{"has1",b.has1},{"has2",b.has2},{"p1",b.p1},{"p2",b.p2}});
    return j.dump(2);
}

bool fromJson(std::string_view text, Area& area)
{
    try {
        const auto j=nlohmann::json::parse(text,nullptr,false);
        if (!j.is_object() || !j.at("boxes").is_array()) return false;
        Area next;
        next.name=j.at("name").get<std::string>();
        const auto& picked=j.at("selected");
        if (!picked.is_number_integer() || (picked.is_number_unsigned() && picked.get<std::uint64_t>()>INT32_MAX)) return false;
        const auto selected=picked.get<std::int64_t>();
        if (selected < -1 || selected>INT32_MAX) return false;
        next.selected=static_cast<int>(selected);
        next.originSet=j.at("originSet").get<bool>();
        const auto pos=[](const auto& value,std::int32_t out[3]) {
            if (!value.is_array() || value.size()!=3) return false;
            for (int a=0;a<3;++a) {
                if (!value[a].is_number_integer()) return false;
                if (value[a].is_number_unsigned() && value[a].template get<std::uint64_t>()>INT32_MAX) return false;
                const auto n=value[a].template get<std::int64_t>();
                if (n<INT32_MIN || n>INT32_MAX) return false;
                out[a]=static_cast<std::int32_t>(n);
            }
            return true;
        };
        if (!pos(j.at("origin"),next.origin)) return false;
        for (const auto& item : j.at("boxes")) {
            Box b;
            b.name=item.at("name").get<std::string>(); b.has1=item.at("has1").get<bool>(); b.has2=item.at("has2").get<bool>();
            if (!pos(item.at("p1"),b.p1) || !pos(item.at("p2"),b.p2)) return false;
            next.boxes.push_back(std::move(b));
        }
        if (next.selected < -1 || (next.selected>=0 && static_cast<std::size_t>(next.selected)>=next.boxes.size())) return false;
        area=std::move(next);
        return true;
    } catch (const nlohmann::json::exception&) { return false; }
}
}
