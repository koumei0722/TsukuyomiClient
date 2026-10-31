#pragma once

#include "render/DebugLine.h"

#include <vector>
#include <string>

namespace tsukuyomi::worldmesh {

struct WorldTag {
    double pos[3]{};
    std::string text;
    float textRgba[4]{};
    float backRgba[4]{};
    int minHalfWidth = 0;
    float size = 1.0F;
    float lift = 0.0F;
    bool appendDistance = false;
};
void setWorldTags(std::vector<WorldTag> tags);
bool worldTagsAvailable();

enum class LineSource : int { DebugKeys = 0, Schematica = 1, AreaSelection = 2, FlexiblePlacement = 3, LightOverlay = 4, Count };
void setDebugLines(LineSource source, std::vector<DebugLine> lines);
void setDebugFills(LineSource source, std::vector<DebugFill> fills);

bool installHooks();

void report();

void shutdown();

}
