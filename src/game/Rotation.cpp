#include "game/Rotation.h"

namespace tsukuyomi::rotation {

int normalize(int quarters)
{
    return ((quarters % 4) + 4) % 4;
}

void rotateLocal(int quarters, std::int32_t x, std::int32_t z, std::int32_t& outX,
                 std::int32_t& outZ)
{
    switch (normalize(quarters)) {
    case 1:
        outX = -z;
        outZ = x;
        break;
    case 2:
        outX = -x;
        outZ = -z;
        break;
    case 3:
        outX = z;
        outZ = -x;
        break;
    default:
        outX = x;
        outZ = z;
        break;
    }
}

void transformLocal(int quarters, int mirror, int x, int z, int& outX, int& outZ)
{
    if (mirror == 1) z = -z;
    if (mirror == 2) x = -x;
    rotateLocal(quarters, x, z, outX, outZ);
}

void inverseLocal(int quarters, int mirror, int x, int z, int& outX, int& outZ)
{
    int rx = 0, rz = 0;
    rotateLocal(-quarters, x, z, rx, rz);
    x = rx; z = rz;
    if (mirror == 1) z = -z;
    if (mirror == 2) x = -x;
    outX = x; outZ = z;
}

Footprint transformedFootprint(int quarters, int mirror, int originX, int originZ,
                               int sizeX, int sizeZ)
{
    Footprint out;
    if (sizeX <= 0 || sizeZ <= 0) {
        out.x0 = originX;
        out.z0 = originZ;
        out.x1 = originX;
        out.z1 = originZ;
        return out;
    }
    std::int32_t ax = 0;
    std::int32_t az = 0;
    std::int32_t bx = 0;
    std::int32_t bz = 0;
    transformLocal(quarters, mirror, 0, 0, ax, az);
    transformLocal(quarters, mirror, sizeX - 1, sizeZ - 1, bx, bz);
    out.x0 = originX + (ax < bx ? ax : bx);
    out.z0 = originZ + (az < bz ? az : bz);
    out.x1 = originX + (ax < bx ? bx : ax) + 1;
    out.z1 = originZ + (az < bz ? bz : az) + 1;
    return out;
}

}
