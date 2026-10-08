#pragma once

#include <string>

namespace tsukuyomi::commandsyntax {

enum class Result {
    Ok,
    SyntaxError,
    Unavailable,
};

Result check(const std::string& command, std::string& message);

}
