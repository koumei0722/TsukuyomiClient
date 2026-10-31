#pragma once

#include <cstdint>
#include <string_view>

namespace tsukuyomi::flexible {
enum class HitPart { Center, Left, Right, Bottom, Top };
int opposite(int dir);
int clockwise(int dir);
int counterClockwise(int dir);
void offset(const std::int32_t pos[3], int dir, std::int32_t out[3]);
int horizontalFromYaw(float yaw);
float yawOf(int horizontalDir);
HitPart hitPart(int side, int playerFacingH, const float rel[3]);
int rotatedFacing(int side, int playerFacingH, HitPart part);
struct Keys {
    bool rotation = false, offset = false, adjacent = false, accurateIn = false, accurateReverse = false;
};
struct Look { float pitch = 0, yaw = 0; };
struct Plan {
    std::int32_t target[3]{};
    int side = 1;
    Look look;
    bool changesLook = false;
};
Plan plan(const std::int32_t placePos[3], int side, const float rel[3], Look real,
          const Keys& keys, bool isObserver);
Look lookToward(int dir, Look real);

struct Scenario {
    std::int32_t neighbor[3]{};
    int face = 1;
    float click[3]{};
    Look look;
};
void facePoint(int face, float y, float out[3]);
}
