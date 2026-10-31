#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace tsukuyomi::skinface {

struct Quad {
    float position[4][3]{};
    float uv[4][2]{};
    float normal[3]{};
    int texture = 0;
};

struct Texture {
    int width = 0, height = 0;
    std::function<bool(int x, int y, std::uint8_t* rgba)> sample;
};

bool personaFace(const std::vector<Quad>& quads, const std::vector<Texture>& textures, int side, std::uint8_t* out);

}
