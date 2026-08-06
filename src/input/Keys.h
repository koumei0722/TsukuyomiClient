#pragma once

#include <span>
#include <string>
#include <vector>

namespace tsukuyomi::keys {

int normalize(int virtualKey);

std::wstring name(int virtualKey);

std::wstring comboName(std::span<const int> combo);

bool isModifier(int virtualKey);

bool isComboDown(std::span<const int> combo);

}
