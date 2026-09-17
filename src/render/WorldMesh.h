#pragma once

namespace tsukuyomi::worldmesh {

bool installHooks();

int mode();

bool active(bool xray);

void report();

void onPresent();

void shutdown();

}
