#pragma once

namespace tsukuyomi::flyspeed {
void registerCommand();
void onScansReady();
void onServerTick(void* session);
void onClientTick();
}
