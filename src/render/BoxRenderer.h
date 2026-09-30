#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "game/BlockRegistry.h"

namespace tsukuyomi::boxes {

void noteCamera(void* cameraBase);

void setBoxes(std::vector<blocks::DiffBox> list);

void setStyle(bool on, float faceAlpha, bool xray);

bool boxStyle(blocks::DiffColor color, float rgb[3], float* faceAlpha);

bool cameraSnapshot(float eye[3], float vp[16]);
void noteViewPerspective(int perspective);
std::shared_ptr<const std::vector<blocks::DiffBox>> boxSnapshot();
bool boxesOn();
float boxFaceAlpha();
bool boxXray();
unsigned long long boxVersion();

}
