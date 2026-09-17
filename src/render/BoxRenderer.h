#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "game/BlockRegistry.h"

struct ID2D1DeviceContext;

namespace tsukuyomi::boxes {

void noteCamera(void* cameraBase);

void setBoxes(std::vector<blocks::DiffBox> list);

void setStyle(bool on, float faceAlpha, bool xray);

bool boxStyle(blocks::DiffColor color, float rgb[3], float* faceAlpha);

void draw(ID2D1DeviceContext* context, float width, float height);

bool wantsDraw();

bool installDepthHooks();

void noteGamePso(const void* graphicsPipelineStateDesc);

void onPresent();

bool depthReady();

void shutdownDepth();

bool cameraSnapshot(float eye[3], float vp[16]);
std::shared_ptr<const std::vector<blocks::DiffBox>> boxSnapshot();
bool boxesOn();
float boxFaceAlpha();
bool boxXray();
unsigned long long boxVersion();

}
