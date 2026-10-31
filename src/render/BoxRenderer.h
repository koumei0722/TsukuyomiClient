#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "game/SchematicSections.h"

namespace tsukuyomi::boxes {

void noteCamera(void* cameraBase, void* source);

struct SectionRequest {
    bool reset = false;
    bool xray = false;
    std::vector<schematic::SectionKey> collect;
    std::vector<schematic::SectionKey> remove;
};
void pushSectionRequest(SectionRequest request);
bool takeSectionRequests(SectionRequest& merged);
unsigned long long sectionRequestVersion();

void setStyle(bool on, float faceAlpha, bool xray);

bool boxStyle(schematic::DiffColor color, float rgb[3], float* faceAlpha);

bool cameraSnapshot(float eye[3], float vp[16]);
bool boxesOn();
float boxFaceAlpha();
bool boxXray();

}
