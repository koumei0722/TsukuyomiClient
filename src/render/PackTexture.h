#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi::pack {

int packRank(const std::string& name, int limit);

int versionRank(const std::string& version);

void noteOpenedFile(const wchar_t* ntPath, std::size_t chars);

std::wstring findFile(const std::string& resourcePath);

bool decodeImage(const std::wstring& file, std::vector<std::uint8_t>& rgba,
                 std::uint32_t& width, std::uint32_t& height);

}
