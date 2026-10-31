#pragma once

#include "game/EasyPlaceLogic.h"
#include "game/FlexibleLogic.h"

#include <cstdint>
#include <functional>
#include <string>

namespace tsukuyomi::lookplace {
bool readLook(flexible::Look& out);
class ScopedLook {
public:
    explicit ScopedLook(flexible::Look look);
    ~ScopedLook();
    ScopedLook(const ScopedLook&) = delete;
    ScopedLook& operator=(const ScopedLook&) = delete;
    bool ok() const { return m_ok; }
private:
    void* m_player = nullptr;
    void* m_rotation = nullptr;
    flexible::Look m_saved;
    bool m_ok = false;
};
class ScopedPosition {
public:
    explicit ScopedPosition(const float eye[3]);
    ~ScopedPosition();
    ScopedPosition(const ScopedPosition&) = delete;
    ScopedPosition& operator=(const ScopedPosition&) = delete;
    bool ok() const { return m_ok; }
private:
    void* m_player = nullptr;
    float* m_position = nullptr;
    float* m_box = nullptr;
    float m_savedPosition[6]{};
    float m_savedBox[6]{};
    bool m_ok = false;
};
struct Request {
    void* gameMode = nullptr;
    std::int32_t pos[3]{};
    std::uint8_t face = 1;
    float click[3]{};
    flexible::Look look;
    std::function<bool()> stillValid;
    const char* owner = "";
};
bool busy();
bool lastSentLook(flexible::Look& out);
std::uint64_t inputsSent();
bool readMotion(easyplace::Motion& out);
std::wstring motionFailure();
bool schedule(Request request);
void onPacketSend(void* packet);
void pump();
void cancel();
}
