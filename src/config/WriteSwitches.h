#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::writes {

void load();

bool allowed(std::string_view name);
bool allowed(const wchar_t* name);

bool blocked(std::string_view feature);

void noteUnknown(std::string_view name);

void finishStartup();

enum class Kind { Hook, Patch };
struct Entry {
    const char* name;
    Kind kind;
    const char* required;
    const char* optional;
    const char* anyOf;
};
const std::vector<Entry>& table();

std::string render(const std::vector<std::pair<std::string, bool>>& values);

bool parse(std::string_view text, std::vector<std::pair<std::string, bool>>& hooks,
           std::vector<std::pair<std::string, bool>>& patches, std::vector<std::string>* invalid = nullptr);

bool blockedBy(std::string_view feature, const std::vector<std::string>& off);

}
