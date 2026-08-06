#pragma once

#include <filesystem>

namespace tsukuyomi::paths {

const std::filesystem::path& dataDir();

std::filesystem::path configFile();
std::filesystem::path logFile();

}
