#include "game/SchematicPlacements.h"

#include <algorithm>
#include <limits>
#include <set>
#include <climits>
#include <type_traits>
#include "core/Strings.h"
#include "game/Rotation.h"

namespace tsukuyomi::schematic {
namespace {
template<class T> bool read(const nlohmann::json& j, const char* key, T& value)
{
    const auto at = j.find(key);
    if (at == j.end()) return true;
    if constexpr (std::is_same_v<T, bool>) { if (!at->is_boolean()) return false; }
    else if constexpr (std::is_same_v<T, std::string>) { if (!at->is_string()) return false; }
    else {
        if (!at->is_number_integer()) return false;
        if constexpr (std::is_same_v<T, std::uint64_t>) {
            if (at->is_number_unsigned()) { value = at->get<T>(); return true; }
            if (at->get<std::int64_t>() < 0) return false;
        } else {
            if (at->is_number_unsigned() && at->get<std::uint64_t>() > INT_MAX) return false;
            const auto n = at->get<std::int64_t>();
            if (n < INT_MIN || n > INT_MAX) return false;
        }
    }
    value = at->get<T>();
    return true;
}
bool triple(const nlohmann::json& j, const char* key, int (&value)[3])
{
    const auto at = j.find(key);
    if (at == j.end()) return true;
    if (!at->is_array() || at->size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!read(nlohmann::json{{"v", (*at)[i]}}, "v", value[i]) || value[i] < 0) return false;
    }
    return true;
}
}

nlohmann::json toJson(const PlacementSpec& p)
{
    return {{"id",p.id}, {"name",toUtf8(p.name)}, {"schematic",toUtf8(p.schematic)},
        {"x",p.x},{"y",p.y},{"z",p.z},{"rotation",p.rotation},{"mirror",static_cast<int>(p.mirror)},
        {"enabled",p.enabled},{"render",p.render},{"locked",p.locked},{"color",p.color},
        {"grid",{{"enabled",p.grid.enabled},{"size",p.grid.size},
                 {"repeatNeg",p.grid.repeatNeg},{"repeatPos",p.grid.repeatPos}}}};
}
bool fromJson(const nlohmann::json& j, PlacementSpec& out)
try {
    if (!j.is_object()) return false;
    PlacementSpec p;
    std::string name, schematic;
    int mirror = 0;
    if (!read(j,"id",p.id) || !read(j,"name",name) || !read(j,"schematic",schematic)
        || !read(j,"x",p.x) || !read(j,"y",p.y) || !read(j,"z",p.z)
        || !read(j,"rotation",p.rotation) || !read(j,"mirror",mirror)
        || !read(j,"enabled",p.enabled) || !read(j,"render",p.render)
        || !read(j,"locked",p.locked) || !read(j,"color",p.color)) return false;
    if (p.rotation < 0 || p.rotation > 3 || mirror < 0 || mirror > 2 || p.color < 0 || p.color > 7) return false;
    p.name = toUtf16(name); p.schematic = toUtf16(schematic); p.mirror = static_cast<Mirror>(mirror);
    if (p.schematic.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos
        || p.schematic == L"." || p.schematic == L"..") return false;
    if (const auto at = j.find("grid"); at != j.end()) {
        if (!at->is_object() || !read(*at,"enabled",p.grid.enabled)
            || !triple(*at,"size",p.grid.size) || !triple(*at,"repeatNeg",p.grid.repeatNeg)
            || !triple(*at,"repeatPos",p.grid.repeatPos)) return false;
    }
    out = std::move(p);
    return true;
} catch (const nlohmann::json::exception&) { return false; }

nlohmann::json toJson(const PlacementFile& f)
{
    auto list = nlohmann::json::array();
    for (const auto& p : f.placements) list.push_back(toJson(p));
    return {{"version",1},{"world",{{"kind",f.kind},{"levelId",f.levelId},{"name",f.name}}},
        {"selected",f.selected},{"nextId",f.nextId},{"placements",std::move(list)}};
}
bool fromJson(const nlohmann::json& j, PlacementFile& out)
try {
    PlacementFile f;
    int version = 1;
    if (!j.is_object() || !read(j,"version",version) || version != 1
        || !read(j,"selected",f.selected) || !read(j,"nextId",f.nextId)) return false;
    if (const auto w = j.find("world"); w != j.end()) {
        if (!w->is_object() || !read(*w,"kind",f.kind) || !read(*w,"levelId",f.levelId)
            || !read(*w,"name",f.name) || (f.kind != "local" && f.kind != "server")) return false;
    }
    std::set<std::uint64_t> ids;
    if (const auto a = j.find("placements"); a != j.end()) {
        if (!a->is_array()) return false;
        for (const auto& item : *a) {
            PlacementSpec p;
            if (!fromJson(item,p) || p.id == 0 || p.id == UINT64_MAX || !ids.insert(p.id).second) return false;
            f.nextId = std::max(f.nextId,p.id+1);
            f.placements.push_back(std::move(p));
        }
    }
    if (!ids.contains(f.selected)) f.selected = 0;
    f.nextId = std::max<std::uint64_t>(1,f.nextId);
    out = std::move(f);
    return true;
} catch (const nlohmann::json::exception&) { return false; }

bool migrateLegacy(const nlohmann::json& legacy, PlacementFile& file, bool& migrated)
{
    if (migrated || !legacy.is_array()) return false;
    for (const auto& j : legacy) {
        bool visible = false;
        std::string name;
        if (!j.is_object() || !read(j,"visible",visible) || !visible || !read(j,"name",name) || name.empty()) continue;
        auto item = j;
        item.erase("rotation");
        item["schematic"] = name;
        PlacementSpec p;
        int rotation = 0;
        if (!fromJson(item,p) || !read(j,"rotation",rotation)) continue;
        p.id = file.nextId++;
        p.rotation = rotation::normalize(rotation);
        p.color = static_cast<int>(file.placements.size() % 8);
        file.placements.push_back(std::move(p));
    }
    if (!file.placements.empty()) file.selected = file.placements.front().id;
    migrated = true;
    return true;
}

Box placementBox(const PlacementSpec& p, int sx, int sy, int sz)
{
    const auto f = rotation::transformedFootprint(p.rotation,static_cast<int>(p.mirror),p.x,p.z,sx,sz);
    return {f.x0,p.y,f.z0,f.x1,p.y+sy,f.z1};
}
std::array<int, 3> CopyRange::origin(const std::array<int, 3>& base, const std::array<std::int64_t, 3>& index) const
{
    return {static_cast<int>(base[0]+index[0]*step[0]), static_cast<int>(base[1]+index[1]*step[1]),
            static_cast<int>(base[2]+index[2]*step[2])};
}
bool CopyRange::count(std::size_t& out) const
{
    out = empty() ? 0 : 1;
    if (!out) return true;
    for (int a = 0; a < 3; ++a) {
        const auto n = static_cast<std::size_t>(last[a]-first[a]+1);
        if (out > SIZE_MAX/n) return false;
        out *= n;
    }
    return true;
}
CopyRange copiesOverlapping(const CopyRange& copies, const Box& b, const Box& range)
{
    CopyRange out = copies;
    const int low[3]{b.x0,b.y0,b.z0}, high[3]{b.x1,b.y1,b.z1};
    const int rlow[3]{range.x0,range.y0,range.z0}, rhigh[3]{range.x1,range.y1,range.z1};
    const auto floorDiv = [](std::int64_t a, std::int64_t d) { return a/d-(a%d < 0 ? 1 : 0); };
    for (int a=0; a<3; ++a) {
        if (rhigh[a] <= rlow[a]) { out.last[0] = out.first[0]-1; return out; }
        if (!out.step[a]) {
            if (low[a] >= rhigh[a] || high[a] <= rlow[a]) { out.last[0] = out.first[0]-1; return out; }
        } else {
            out.first[a] = std::max(out.first[a], floorDiv(static_cast<std::int64_t>(rlow[a])-high[a],out.step[a])+1);
            out.last[a] = std::min(out.last[a], floorDiv(static_cast<std::int64_t>(rhigh[a])-1-low[a],out.step[a]));
        }
    }
    return out;
}
Box copiesBox(const Box& b, const CopyRange& copies)
{
    if (copies.empty()) return {};
    const auto narrow = [](std::int64_t v) { return static_cast<int>(std::clamp<std::int64_t>(v, INT_MIN, INT_MAX)); };
    return {narrow(b.x0+copies.first[0]*copies.step[0]), narrow(b.y0+copies.first[1]*copies.step[1]), narrow(b.z0+copies.first[2]*copies.step[2]),
            narrow(b.x1+copies.last[0]*copies.step[0]), narrow(b.y1+copies.last[1]*copies.step[1]), narrow(b.z1+copies.last[2]*copies.step[2])};
}
CopyRange copiesInBox(const GridSpec& grid, const Box& b, const std::array<int, 3>& origin, const Box& range)
{
    CopyRange out;
    const int low[3] = {b.x0,b.y0,b.z0}, high[3] = {b.x1,b.y1,b.z1};
    const int rlow[3] = {range.x0,range.y0,range.z0}, rhigh[3] = {range.x1,range.y1,range.z1};
    const auto floorDiv = [](std::int64_t a, std::int64_t d) { auto q=a/d; return q-(a%d < 0 ? 1 : 0); };
    for (int a=0; a<3; ++a) {
        if (rhigh[a] <= rlow[a] || high[a] <= low[a]) { out.last[0] = -1; return out; }
        if (!grid.enabled) {
            if (low[a] >= rhigh[a] || high[a] <= rlow[a]) out.last[0] = -1;
            continue;
        }
        out.step[a] = grid.size[a] > 0 ? grid.size[a] : static_cast<std::int64_t>(high[a])-low[a];
        out.first[a] = std::max(-static_cast<std::int64_t>(grid.repeatNeg[a]),
            floorDiv(static_cast<std::int64_t>(rlow[a])-high[a],out.step[a])+1);
        out.last[a] = std::min(static_cast<std::int64_t>(grid.repeatPos[a]),
            floorDiv(static_cast<std::int64_t>(rhigh[a])-1-low[a],out.step[a]));
        out.first[a] = std::max(out.first[a],-floorDiv(static_cast<std::int64_t>(origin[a])-INT_MIN,out.step[a]));
        out.last[a] = std::min(out.last[a],floorDiv(static_cast<std::int64_t>(INT_MAX)-origin[a],out.step[a]));
    }
    return out;
}
bool gridOutline(const Box& b, const CopyRange& copies, std::vector<GridEdge>& out, const std::atomic<bool>* cancel)
{
    out.clear();
    if (copies.empty()) return true;
    const std::int64_t low[3]{b.x0,b.y0,b.z0}, high[3]{b.x1,b.y1,b.z1};
    std::vector<std::int64_t> edges[3];
    for (int axis=0;axis<3;++axis) {
        for (auto i=copies.first[axis];i<=copies.last[axis];++i) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            edges[axis].push_back(low[axis]+i*copies.step[axis]);
            edges[axis].push_back(high[axis]+i*copies.step[axis]);
        }
        std::sort(edges[axis].begin(),edges[axis].end());
        edges[axis].erase(std::unique(edges[axis].begin(),edges[axis].end()),edges[axis].end());
    }
    for(int axis=0;axis<3;++axis) {
        const int u=(axis+1)%3,v=(axis+2)%3;
        const bool joined=copies.step[axis]<=high[axis]-low[axis];
        for(const auto a:edges[u]) for(const auto c:edges[v]) {
            for(auto i=copies.first[axis];i<=copies.last[axis];++i) {
                if (cancel && cancel->load(std::memory_order_relaxed)) return false;
                GridEdge line{};
                line[u]=line[u+3]=a;line[v]=line[v+3]=c;
                line[axis]=low[axis]+i*copies.step[axis];
                line[axis+3]=high[axis]+(joined?copies.last[axis]:i)*copies.step[axis];
                out.push_back(line);
                if(joined) break;
            }
        }
    }
    return true;
}

}
