#include "game/FlexibleLogic.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi::flexible {
int opposite(int dir) { return dir >= 0 && dir < 6 ? dir ^ 1 : dir; }
int clockwise(int dir)
{
    switch (dir) { case 2: return 5; case 5: return 3; case 3: return 4; case 4: return 2; default: return dir; }
}
int counterClockwise(int dir) { return clockwise(opposite(dir)) == opposite(dir) ? dir : opposite(clockwise(dir)); }
void offset(const std::int32_t pos[3], int dir, std::int32_t out[3])
{
    std::copy_n(pos, 3, out);
    if (dir < 0 || dir >= 6) return;
    const int axis = dir < 2 ? 1 : dir < 4 ? 2 : 0;
    const auto value = static_cast<std::int64_t>(out[axis]) + ((dir & 1) ? 1 : -1);
    out[axis] = static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(INT32_MIN), static_cast<std::int64_t>(INT32_MAX)));
}
int horizontalFromYaw(float yaw)
{
    if (!std::isfinite(yaw)) return 3;
    const int quarter = static_cast<int>(std::floor(std::remainder(yaw, 360.0F) / 90.0F + 0.5F)) & 3;
    constexpr int directions[]{3, 4, 2, 5};
    return directions[quarter];
}
float yawOf(int dir)
{
    switch (dir) { case 4: return 90; case 2: return 180; case 5: return -90; default: return 0; }
}
HitPart hitPart(int side, int facing, const float rel[3])
{
    float h = 0.5F, v = rel[1];
    if (side < 2) {
        switch (facing) {
        case 2: h = rel[0]; v = 1 - rel[2]; break;
        case 3: h = 1 - rel[0]; v = rel[2]; break;
        case 4: h = 1 - rel[2]; v = 1 - rel[0]; break;
        case 5: h = rel[2]; v = rel[0]; break;
        default: break;
        }
        if (side == 0) v = 1 - v;
    } else if (side == 2 || side == 3) h = side == 3 ? rel[0] : 1 - rel[0];
    else if (side == 4 || side == 5) h = side == 4 ? rel[2] : 1 - rel[2];
    const float dh = std::fabs(h - 0.5F), dv = std::fabs(v - 0.5F);
    if (dh <= 0.25F && dv <= 0.25F) return HitPart::Center;
    if (dh > dv) return h < 0.5F ? HitPart::Left : HitPart::Right;
    return v < 0.5F ? HitPart::Bottom : HitPart::Top;
}
int rotatedFacing(int side, int facing, HitPart part)
{
    if (side < 2) {
        switch (part) {
        case HitPart::Left: return clockwise(facing);
        case HitPart::Right: return counterClockwise(facing);
        case HitPart::Bottom: return side == 1 ? facing : opposite(facing);
        case HitPart::Top: return side == 0 ? facing : opposite(facing);
        default: return opposite(side);
        }
    }
    switch (part) {
    case HitPart::Left: return counterClockwise(side);
    case HitPart::Right: return clockwise(side);
    case HitPart::Bottom: return 1;
    case HitPart::Top: return 0;
    default: return opposite(side);
    }
}
Look lookToward(int dir, Look real)
{
    if (dir >= 2 && dir <= 5) real.yaw = yawOf(dir);
    return real;
}
Plan plan(const std::int32_t placePos[3], int side, const float rel[3], Look real,
          const Keys& keys, bool isObserver)
{
    Plan result;
    std::copy_n(placePos, 3, result.target);
    result.side = side; result.look = real;
    const int facing = horizontalFromYaw(real.yaw);
    const auto part = hitPart(side, facing, rel);
    const int rotated = rotatedFacing(side, facing, part);
    if (keys.adjacent && part != HitPart::Center) {
        offset(result.target, opposite(rotated), result.target);
        offset(result.target, opposite(side), result.target);
    }
    if (keys.rotation) {
        result.side = rotated;
        int lookFacing = facing;
        if (part == HitPart::Center) lookFacing = opposite(facing);
        else if (part == HitPart::Left) lookFacing = counterClockwise(facing);
        else if (part == HitPart::Right) lookFacing = clockwise(facing);
        result.look = lookToward(lookFacing, real);
    }
    if (keys.offset) offset(result.target, opposite(rotated), result.target);
    if (keys.accurateIn || keys.accurateReverse) {
        result.look = keys.accurateIn ? lookToward(isObserver ? opposite(side) : side, real) : real;
        if (keys.accurateReverse) result.look.yaw = std::remainder(result.look.yaw + 180.0F, 360.0F);
    }
    result.changesLook = std::fabs(std::remainder(result.look.yaw - real.yaw, 360.0F)) > 0.5F;
    return result;
}
void facePoint(int face, float y, float out[3])
{
    out[0] = out[1] = out[2] = 0.5F;
    if (face >= 2) out[1] = y;
    const int axis = face < 2 ? 1 : face < 4 ? 2 : 0;
    out[axis] = (face & 1) ? 1.0F : 0.0F;
}
}
