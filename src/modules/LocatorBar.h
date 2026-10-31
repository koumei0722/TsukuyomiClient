#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "modules/Module.h"

namespace tsukuyomi {

class LocatorBar : public Module {
public:
    static LocatorBar& instance();

    const wchar_t* name() const override { return L"LocatorBar"; }
    bool available() const override { return m_ready.load(std::memory_order_relaxed); }

    void onScansReady() override;
    void shutdown() override;

    using RenderFn = void(__fastcall*)(void* self, void* ctx, void* client, void* owner, int pass, void* area);
    using IconFn = void(__fastcall*)(void* a, void* mesh, void* texture, float x, float y, float u0, float v0, float u1,
                                    float v1, float w, float h, float scale, float alpha, std::uint32_t color,
                                    std::uint32_t tint, float s0, float s1);
    void onRender(RenderFn original, void* self, void* ctx, void* client, void* owner, int pass, void* area);
    void onIcon(IconFn original, void* a, void* mesh, void* texture, float x, float y, float u0, float v0, float u1,
                float v1, float w, float h, float scale, float alpha, std::uint32_t color, std::uint32_t tint, float s0,
                float s1);
    void onPlayerViewUpdate();

private:
    LocatorBar() = default;

    struct Person {
        std::string facePath;
        bool loaded = false;
        float position[3]{};
    };
    struct Slot {
        std::byte* waypoint = nullptr;
        std::uint8_t active = 0;
        std::uint8_t hasColor = 0;
        float color[4]{};
        float position[3]{};
        bool moved = false;
        bool hidden = false;
        std::uint32_t argb = 0;
        std::string facePath;
        float distance = 0;
        float rect[4]{};
        float alpha = 0;
    };

    void restoreSlots();
    void drawPlaced(void* ctx, void* client);
    void prepareFaceDirectory();
    std::string facePathFor(const std::uint8_t* rgba, int side);
    bool drawFace(void* ctx, const Slot& slot, float x, float y, float w, float h, float alpha);
    bool drawDistance(void* ctx, void* client, const Slot& slot, float x, float y, float w, float h, float alpha);

    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_inside{0};

    std::int32_t m_localPlayerSlot = 0;
    std::int32_t m_listField = 0;
    std::uint8_t m_activeField = 0;
    std::uint8_t m_positionField = 0;
    std::uint8_t m_colorField = 0;
    std::uint8_t m_hasColorField = 0;
    float m_defaultColor[4]{};

    std::vector<Slot> m_slots;
    std::vector<std::size_t> m_placed;
    bool m_inRender = false;
    bool m_loggedListGuard = false;
    bool m_loggedFaceFail = false;
    bool m_loggedDraw = false;
    bool m_loggedBusy = false;
    std::atomic<bool> m_drawBroken{false};

    std::mutex m_peopleMutex;
    std::map<std::int64_t, Person> m_people;
    std::uint64_t m_lastSample = 0;
    const void* m_levelVtable = nullptr;
    std::int32_t m_playersField = 0;
    const void* m_fetchActor = nullptr;
    std::filesystem::path m_faceDirectory;
    std::set<std::string> m_writtenFaces;
    std::map<const void*, std::string> m_faceCache;
    bool m_faceWriteLogged = false;
    bool m_levelLogged = false;
};

}
