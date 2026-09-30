#pragma once

namespace tsukuyomi::worldmesh {

struct DebugLine {
    double a[3];
    double b[3];
    float rgba[4];
    float width = 1.0F;
    bool onTop = false;
};

}
