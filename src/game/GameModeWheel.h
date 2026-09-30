#pragma once

namespace tsukuyomi::gamemodewheel {

enum class Purpose : int { GameMode = 1, Pause = 2 };

bool installHooks();

bool open(Purpose purpose);

void closeOurs();

}
