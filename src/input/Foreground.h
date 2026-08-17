#pragma once

#include <Windows.h>

namespace tsukuyomi::input {

bool isGameForeground(HWND ownWindow = nullptr);

bool isInGameplay();

}
