#pragma once

namespace tsukuyomi::leveltick {

struct Seat {
    const void* level = nullptr;
    unsigned long long at = 0, since = 0;
};

inline bool tickingTogether(const Seat& a, const Seat& b, unsigned long long now, unsigned long long staleMs = 2000)
{
    for (const Seat* s : {&a, &b}) {
        if (s->level == nullptr || s->at == 0 || (now > s->at && now - s->at > staleMs)) return false;
    }
    return a.level != b.level && a.at > b.since && b.at > a.since;
}

}
