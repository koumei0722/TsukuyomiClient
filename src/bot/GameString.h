#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace tsukuyomi::bot::gamestring {

void* allocate(std::size_t size);
bool free(void* data, std::size_t size);
void initialize(void* string);

std::optional<std::string> read(const void* string);
bool assign(void* string, std::string_view bytes);
bool destroy(void* string);

}
