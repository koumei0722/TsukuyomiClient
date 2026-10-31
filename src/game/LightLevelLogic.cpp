#include "game/LightLevelLogic.h"

#include <array>
#include <string>

namespace tsukuyomi::lightlevel {

Spawn classify(int blockLight, int skyLight)
{
    if (blockLight > 0) return Spawn::Never;
    return skyLight >= 7 ? Spawn::NightOnly : Spawn::Always;
}

namespace {

constexpr const char* kGlyphs[10][kGlyphHeight] = {
    {"###", "#.#", "#.#", "#.#", "###"},
    {"##.", ".#.", ".#.", ".#.", "###"},
    {"###", "..#", "###", "#..", "###"},
    {"###", "..#", "###", "..#", "###"},
    {"#.#", "#.#", "###", "..#", "..#"},
    {"###", "#..", "###", "..#", "###"},
    {"###", "#..", "###", "#.#", "###"},
    {"###", "..#", "..#", "..#", "..#"},
    {"###", "#.#", "###", "#.#", "###"},
    {"###", "#.#", "###", "..#", "###"},
};

std::vector<Rect> buildRects(int digit)
{
    std::vector<Rect> rects;
    for (int row = 0; row < kGlyphHeight; ++row) {
        const char* const line = kGlyphs[digit][row];
        for (int col = 0; col < kGlyphWidth;) {
            if (line[col] != '#') {
                ++col;
                continue;
            }
            int end = col;
            while (end < kGlyphWidth && line[end] == '#') ++end;
            bool merged = false;
            for (Rect& r : rects) {
                if (r.x == col && r.w == end - col && r.y + r.h == row) {
                    ++r.h;
                    merged = true;
                    break;
                }
            }
            if (!merged) rects.push_back({col, row, end - col, 1});
            col = end;
        }
    }
    return rects;
}

}

const std::vector<Rect>& digitRects(int digit)
{
    static const std::array<std::vector<Rect>, 10> table = [] {
        std::array<std::vector<Rect>, 10> built;
        for (int d = 0; d < 10; ++d) built[d] = buildRects(d);
        return built;
    }();
    static const std::vector<Rect> empty;
    return digit >= 0 && digit <= 9 ? table[digit] : empty;
}

std::vector<Rect> numberRects(int value, int* width)
{
    if (width != nullptr) *width = 0;
    if (value < 0) return {};
    const std::string text = std::to_string(value);
    std::vector<Rect> out;
    int x = 0;
    for (const char c : text) {
        for (Rect r : digitRects(c - '0')) {
            r.x += x;
            out.push_back(r);
        }
        x += kGlyphWidth + 1;
    }
    if (width != nullptr) *width = x - 1;
    return out;
}

std::vector<Quad> numberQuads(int value, int x, int y, int z, int facing, double pixel, double lift)
{
    int width = 0;
    const std::vector<Rect> rects = numberRects(value, &width);
    double f[2] = {0.0, 1.0};
    switch (facing) {
    case 2: f[0] = 0.0; f[1] = -1.0; break;
    case 4: f[0] = -1.0; f[1] = 0.0; break;
    case 5: f[0] = 1.0; f[1] = 0.0; break;
    default: break;
    }
    const double r[2] = {-f[1], f[0]};
    const double cx = x + 0.5;
    const double cz = z + 0.5;
    const double top = y + 1.0 + lift;
    const double left = -0.5 * width * pixel;
    const double upper = 0.5 * kGlyphHeight * pixel;
    std::vector<Quad> out;
    out.reserve(rects.size());
    for (const Rect& rect : rects) {
        const double u0 = left + rect.x * pixel;
        const double u1 = left + (rect.x + rect.w) * pixel;
        const double v0 = upper - rect.y * pixel;
        const double v1 = upper - (rect.y + rect.h) * pixel;
        const double uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        Quad q{};
        for (int i = 0; i < 4; ++i) {
            q.p[i][0] = cx + r[0] * uv[i][0] + f[0] * uv[i][1];
            q.p[i][1] = top;
            q.p[i][2] = cz + r[1] * uv[i][0] + f[1] * uv[i][1];
        }
        out.push_back(q);
    }
    return out;
}

Quad faceQuad(int x, int y, int z, double lift, double inset)
{
    const double top = y + 1.0 + lift;
    const double corners[4][2] = {{x + inset, z + inset}, {x + 1.0 - inset, z + inset},
                                  {x + 1.0 - inset, z + 1.0 - inset}, {x + inset, z + 1.0 - inset}};
    Quad q{};
    for (int i = 0; i < 4; ++i) {
        q.p[i][0] = corners[i][0];
        q.p[i][1] = top;
        q.p[i][2] = corners[i][1];
    }
    return q;
}

}
