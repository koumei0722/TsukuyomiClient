#pragma once

#include <cstdint>

namespace tsukuyomi::freezewatch {

void start();
void stop();

void notePresent();

class Scope {
public:
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int m_slot = -1;
};

}
