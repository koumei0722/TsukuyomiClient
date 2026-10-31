#include "game/EasyPlaceLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tsukuyomi::easyplace {
std::set<std::string> varyingStates(const std::vector<States>& results)
{
    std::set<std::string> out;
    std::map<std::string, std::string> first;
    std::map<std::string, std::size_t> seen;
    for (const auto& states : results) for (const auto& [name, value] : states) {
        const auto [at, inserted] = first.try_emplace(name, value);
        ++seen[name];
        if (!inserted && at->second != value) out.insert(name);
    }
    for (const auto& [name, count] : seen) if (count != results.size()) out.insert(name);
    return out;
}
std::set<std::string> placementStates(const std::vector<std::vector<States>>& perSpot, bool multiSupport)
{
    if (!multiSupport) {
        std::vector<States> all;
        for (const auto& spot : perSpot) all.insert(all.end(), spot.begin(), spot.end());
        return varyingStates(all);
    }
    std::set<std::string> out;
    for (const auto& spot : perSpot) {
        const auto one = varyingStates(spot);
        out.insert(one.begin(), one.end());
    }
    return out;
}
bool matchesOn(const States& have, const States& want, const std::set<std::string>& names)
{
    for (const auto& name : names) {
        const auto w = want.find(name);
        if (w == want.end()) continue;
        const auto h = have.find(name);
        if (h == have.end() || h->second != w->second) return false;
    }
    return true;
}
bool offsetFace(const std::int32_t pos[3], int face, int sign, std::int32_t out[3])
{
    if (!pos || !out || face < 0 || face > 5 || (sign != 1 && sign != -1)) return false;
    constexpr int offsets[6][3]{{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
    std::int32_t result[3]{};
    for (int a = 0; a < 3; ++a) {
        const auto value = static_cast<std::int64_t>(pos[a]) + sign * offsets[face][a];
        if (value < INT32_MIN || value > INT32_MAX) return false;
        result[a] = static_cast<std::int32_t>(value);
    }
    std::copy(std::begin(result), std::end(result), out);
    return true;
}
std::size_t candidates(const std::int32_t target[3], std::array<Candidate, 10>& out)
{
    std::size_t count = 0;
    for (int f = 0; f < 6; ++f) {
        Candidate one;
        if (!offsetFace(target, f, -1, one.neighbor)) continue;
        one.face = static_cast<std::uint8_t>(f);
        one.click[0] = one.click[1] = one.click[2] = 0.5F;
        const int axis = f < 2 ? 1 : f < 4 ? 2 : 0;
        one.click[axis] = (f & 1) != 0 ? 1.0F : 0.0F;
        if (f < 2) out[count++] = one;
        else {
            one.click[1] = 0.25F; out[count++] = one;
            one.click[1] = 0.75F; out[count++] = one;
        }
    }
    return count;
}
bool parseSlotMask(std::string_view text, unsigned& mask)
{
    unsigned result = 0;
    std::size_t i = 0;
    const auto skipSpace = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '	')) ++i; };
    const auto digit = [&](int& out) {
        skipSpace();
        if (i >= text.size() || text[i] < '1' || text[i] > '9') return false;
        out = text[i++] - '0';
        skipSpace();
        return true;
    };
    skipSpace();
    if (i == text.size()) { mask = 0; return true; }
    while (true) {
        int lo = 0, hi = 0;
        if (!digit(lo)) return false;
        hi = lo;
        if (i < text.size() && text[i] == '-') { ++i; if (!digit(hi) || hi < lo) return false; }
        for (int n = lo; n <= hi; ++n) result |= 1u << (n - 1);
        if (i == text.size()) break;
        if (text[i] != ',') return false;
        ++i;
    }
    mask = result;
    return true;
}
std::string slotMaskText(unsigned mask)
{
    std::string out;
    for (int n = 0; n < 9;) {
        if ((mask >> n & 1u) == 0) { ++n; continue; }
        int end = n;
        while (end + 1 < 9 && (mask >> (end + 1) & 1u) != 0) ++end;
        if (!out.empty()) out += ',';
        out += static_cast<char>('1' + n);
        if (end > n) { out += '-'; out += static_cast<char>('1' + end); }
        n = end + 1;
    }
    return out;
}
int pickTargetSlot(unsigned allowed, int hand, unsigned emptyMask)
{
    allowed &= kAllHotbarSlots;
    if (allowed == 0) return -1;
    const bool handOk = hand >= 0 && hand < 9 && (allowed >> hand & 1u) != 0;
    if (handOk && (emptyMask >> hand & 1u) != 0) return hand;
    for (int n = 0; n < 9; ++n) if ((allowed & emptyMask) >> n & 1u) return n;
    if (handOk) return hand;
    const int start = hand >= 0 && hand < 9 ? hand : -1;
    for (int k = 1; k <= 9; ++k) {
        const int n = (start + k + 9) % 9;
        if (allowed >> n & 1u) return n;
    }
    return -1;
}
bool movesVertically(const Motion& motion) { return !motion.grounded || motion.jumpHeld; }

std::size_t aheadEyes(const Motion& motion, int ticks, float out[kMaxAheadEyes][3])
{
    const auto put = [&](std::size_t i, float steps, float y) {
        out[i][0] = motion.eye[0] + motion.move[0] * steps; out[i][1] = y; out[i][2] = motion.eye[2] + motion.move[2] * steps;
    };
    const float steps = ticks >= 2 ? 2.0F : 1.0F;
    if (motion.grounded) {
        put(0, steps, motion.eye[1]);
        if (!motion.jumpHeld) return 1;
        const float rise = ticks >= 2 ? kJumpVelocity + (kJumpVelocity - kGravity) * kAirDrag : kJumpVelocity;
        put(1, steps, motion.eye[1] + rise);
        return 2;
    }
    const float y1 = motion.eye[1] + motion.move[1];
    if (ticks < 2) {
        put(0, 0.0F, motion.eye[1]); put(1, 1.0F, y1);
        return 2;
    }
    put(0, 1.0F, y1); put(1, 2.0F, y1 + (motion.move[1] - kGravity) * kAirDrag);
    return 2;
}

bool RayCursor::begin(const float eye[3], const float dir[3], double reach)
{
    if (!eye || !dir || !std::isfinite(reach) || reach < 0) return false;
    double length2 = 0;
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(eye[a]) || !std::isfinite(dir[a])) return false;
        length2 += static_cast<double>(dir[a]) * dir[a];
    }
    if (length2 == 0) return false;
    limit = reach; distance = 0;
    const double length = std::sqrt(length2);
    for (int a = 0; a < 3; ++a) {
        const double cell = std::floor(eye[a]);
        if (cell <= INT32_MIN || cell >= INT32_MAX) return false;
        pos[a] = static_cast<std::int32_t>(cell);
        const double d = dir[a] / length;
        step[a] = d > 0 ? 1 : d < 0 ? -1 : 0;
        delta[a] = d == 0 ? std::numeric_limits<double>::infinity() : std::abs(1 / d);
        edge[a] = d == 0 ? delta[a] : ((d > 0 ? cell + 1 : cell) - eye[a]) / d;
    }
    return true;
}
bool RayCursor::next()
{
    const double at = (std::min)({edge[0], edge[1], edge[2]});
    if (!std::isfinite(at) || at > limit) return false;
    for (int a = 0; a < 3; ++a) if (edge[a] == at) {
        if ((step[a] > 0 && pos[a] == INT32_MAX) || (step[a] < 0 && pos[a] == INT32_MIN)) return false;
        pos[a] += step[a]; edge[a] += delta[a];
    }
    distance = at;
    return true;
}
int positionBoxMismatch(const float position[3], const float box[6])
{
    for (int i = 0; i < 3; ++i) {
        const float top = i == 1 ? std::max(box[4], box[1] + kPositionEyeHeight) : box[i + 3];
        if (!std::isfinite(position[i]) || !std::isfinite(box[i]) || !std::isfinite(box[i + 3]) || !(box[i] < box[i + 3])
            || box[i + 3] - box[i] > 4.0F || position[i] < box[i] - 0.01F || position[i] > top + 0.01F) return i;
    }
    return -1;
}
}
