#pragma once

#include <string>

namespace tsukuyomi::gamecommand {

bool available();
bool run(const std::string& text, bool& handled);

}
