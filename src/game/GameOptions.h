#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tsukuyomi::gameoptions {

class Options {
public:
    bool getInt(const char* key, int& out) const;

private:
    friend bool parseOptions(const char* text, std::size_t length, Options& out);
    const std::string* find(const char* key) const;
    std::vector<std::pair<std::string, std::string>> entries_;
};

bool parseOptions(const char* text, std::size_t length, Options& out);

bool reload();
bool getInt(const char* key, int& out);

}
