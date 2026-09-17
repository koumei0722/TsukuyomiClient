#pragma once

struct ID3D12GraphicsCommandList;

namespace tsukuyomi::ghost {

bool installGhostLayerHooks();

void setGhostMark(unsigned char writeMask);

void beginMarkWindow();
void endMarkWindow();

int diagBeMode();

void beforeDraw(ID3D12GraphicsCommandList* list);

void shutdownGhostLayer();

}
