#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::easyplace {
using States = std::map<std::string, std::string>;
std::set<std::string> varyingStates(const std::vector<States>& results);
std::set<std::string> placementStates(const std::vector<std::vector<States>>& perSpot, bool multiSupport);
bool matchesOn(const States& have, const States& want, const std::set<std::string>& names);
bool offsetFace(const std::int32_t pos[3], int face, int sign, std::int32_t out[3]);
struct Candidate {
    std::int32_t neighbor[3]{};
    std::uint8_t face = 0;
    float click[3]{};
};
std::size_t candidates(const std::int32_t target[3], std::array<Candidate, 10>& out);
constexpr unsigned kAllHotbarSlots = 0x1FF;
bool parseSlotMask(std::string_view text, unsigned& mask);
std::string slotMaskText(unsigned mask);
int pickTargetSlot(unsigned allowed, int hand, unsigned emptyMask);
struct Motion {
    float eye[3]{};
    float move[3]{};
    bool grounded = false;
    bool jumpHeld = false;
};
constexpr float kJumpVelocity = 0.42F, kGravity = 0.08F, kAirDrag = 0.98F;
bool movesVertically(const Motion& motion);
constexpr std::size_t kMaxAheadEyes = 2;
std::size_t aheadEyes(const Motion& motion, int ticks, float out[kMaxAheadEyes][3]);
constexpr float kPositionEyeHeight = 1.62F;
int positionBoxMismatch(const float position[3], const float box[6]);
struct RayCursor {
    std::int32_t pos[3]{};
    double distance = 0;
    bool begin(const float eye[3], const float dir[3], double reach);
    bool next();
private:
    int step[3]{};
    double edge[3]{}, delta[3]{}, limit = 0;
};
}
