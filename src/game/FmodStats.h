#pragma once

namespace tsukuyomi::fmodstats {

void* updateAddress();

struct Counts {
    int statics = 0;
    int streams = 0;
    int cap = 0;
    int loadedSounds = -1;
    long long memoryBytes = -1;
};

bool sample(void* system, Counts& out);

}
