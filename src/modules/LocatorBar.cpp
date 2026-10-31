#include "modules/LocatorBar.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/BlockWrite.h"
#include "game/GameData.h"
#include "game/LocatorBarLogic.h"
#include "game/PlayerListLayout.h"
#include "game/PlayerListMemory.h"
#include "game/PngWrite.h"
#include "game/UiDrawContext.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

namespace tsukuyomi {
namespace {

constexpr std::size_t kRenderLocalPlayerSlot = 134;
constexpr std::size_t kListField = 3;
constexpr std::size_t kNodeValue = 46;
constexpr std::size_t kFieldsActive = 6;
constexpr std::size_t kFieldsPosition = 26;
constexpr std::size_t kColorField = 10;
constexpr std::size_t kHasColorField = 5;
constexpr std::size_t kDefaultColorDisp = 14;

constexpr std::size_t kActorIdField = 0x78;

constexpr std::size_t kBlockSourceLevel = 0x20;
constexpr std::size_t kLevelPlayersSlot = 0x9f8;
constexpr std::size_t kLevelFetchSlot = 0x1f0;

constexpr float kHiddenAlpha = 0.45f;
constexpr float kFaceSize = 8.0f;
constexpr float kTextScale = 0.6f;
constexpr float kTextHeight = 9.0f * kTextScale;
constexpr std::ptrdiff_t kTextQueueBegin = 0x80, kTextQueueEnd = 0x88;
constexpr std::uint64_t kSampleMs = 250;
constexpr int kRawFileSystem = 2;

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                 : EXCEPTION_CONTINUE_SEARCH;
}

bool readByte(const std::byte* at, std::uint8_t& out)
{
    return at != nullptr && memory::copyGuarded(at, &out, sizeof(out));
}

bool readBytes(const void* at, void* out, std::size_t size)
{
    return at != nullptr && memory::copyGuarded(at, out, size);
}

bool readPointer(const void* base, std::size_t offset, const void*& out)
{
    return memory::plausiblePointer(base)
        && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}

bool writeGuarded(void* at, const void* from, std::size_t size)
{
    __try {
        std::memcpy(at, from, size);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

using LocalPlayerFn = void*(__fastcall*)(void* client);
void* localPlayerGuarded(void* client, std::int32_t slot)
{
    __try {
        void** const vtable = *static_cast<void***>(client);
        return reinterpret_cast<LocalPlayerFn>(vtable[slot / 8])(client);
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

const void* fetchActorGuarded(const void* level, const void* fetch, std::int64_t id)
{
    __try {
        using Fetch = const void*(__fastcall*)(const void*, std::int64_t, bool);
        return reinterpret_cast<Fetch>(const_cast<void*>(fetch))(level, id, false);
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

bool finite3(const float* v)
{
    constexpr float kWorldLimit = 3.0e7f;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > kWorldLimit) return false;
    }
    return true;
}

}

LocatorBar& LocatorBar::instance()
{
    static LocatorBar module;
    return module;
}

void LocatorBar::onScansReady()
{
    auto& scanner = Scanner::instance();
    const std::byte* const render = scanner.address(Target::LocatorBarRendererRender);
    const std::byte* const list = scanner.address(Target::LocatorBarWaypointList);
    const std::byte* const fields = scanner.address(Target::LocatorBarWaypointFields);
    const std::byte* const visible = scanner.address(Target::LocatorBarWaypointVisible);
    const std::byte* const color = scanner.address(Target::LocatorBarWaypointColor);
    std::uint8_t nodeValue = 0;
    std::uint8_t visibleField = 0;
    bool ok = render != nullptr && list != nullptr && fields != nullptr && visible != nullptr && color != nullptr
              && scanner.found(Target::LocatorBarIconCall);
    if (ok) {
        ok = memory::copyGuarded(render + kRenderLocalPlayerSlot, &m_localPlayerSlot, sizeof(m_localPlayerSlot))
             && memory::copyGuarded(list + kListField, &m_listField, sizeof(m_listField))
             && readByte(list + kNodeValue, nodeValue)
             && readByte(fields + kFieldsActive, m_activeField) && readByte(fields + kFieldsPosition, m_positionField)
             && readByte(visible + 8, visibleField) && readByte(color + kColorField, m_colorField)
             && readByte(color + kHasColorField, m_hasColorField);
    }
    const void* const defaultColor = ok ? memory::ripTarget(color, kDefaultColorDisp) : nullptr;
    ok = ok && m_localPlayerSlot > 0 && m_localPlayerSlot < 0x2000 && m_localPlayerSlot % 8 == 0 && m_listField > 0
         && m_listField < 0x10000 && nodeValue == 0x20 && m_hasColorField == m_colorField + 16
         && m_positionField + 12 <= m_colorField && m_activeField > m_hasColorField && m_activeField != visibleField
         && m_activeField < kActorIdField && readBytes(defaultColor, m_defaultColor, sizeof(m_defaultColor));
    uidraw::resolve();
    if (!ok) {
        log().warn(L"LocatorBar: NOT usable (renderer or waypoint fields not found)");
        return;
    }
    prepareFaceDirectory();
    m_ready.store(true, std::memory_order_relaxed);
    log().info(L"LocatorBar: ready (local player slot {:#x}, waypoints +{:#x}, active +{:#x}, position +{:#x}, "
               L"color +{:#x}/+{:#x}; faces from {})",
               m_localPlayerSlot, m_listField, m_activeField, m_positionField, m_colorField, m_hasColorField,
               m_faceDirectory.empty() ? L"(none)" : m_faceDirectory.wstring());
}

void LocatorBar::shutdown()
{
    m_stopping.store(true, std::memory_order_release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (m_inside.load(std::memory_order_acquire) > 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void LocatorBar::prepareFaceDirectory()
{
    std::error_code error;
    const auto dir = paths::dataDir() / "heads";
    std::filesystem::create_directories(dir, error);
    if (error) {
        log().warn(L"LocatorBar: cannot create the heads folder; waypoints keep the vanilla dots");
        return;
    }
    m_faceDirectory = dir;
}

std::string LocatorBar::facePathFor(const std::uint8_t* rgba, int side)
{
    if (m_faceDirectory.empty() || side <= 0) return {};
    const std::vector<std::uint8_t> face(rgba, rgba + static_cast<std::size_t>(side) * side * 4);
    const auto file = m_faceDirectory / playerlist::faceFileName(face, side);
    const std::string path = toUtf8(file.generic_wstring());
    if (m_writtenFaces.count(path) == 0) {
        std::error_code error;
        if (!std::filesystem::exists(file, error)) {
            const auto png = png::encodeRgba(face.data(), side, side);
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            if (!out) {
                if (!m_faceWriteLogged) {
                    m_faceWriteLogged = true;
                    log().warn(L"LocatorBar: cannot write a head image; that waypoint keeps the vanilla dot");
                }
                return {};
            }
        }
        m_writtenFaces.insert(path);
    }
    return path;
}

void LocatorBar::onPlayerViewUpdate()
{
    if (!m_ready.load(std::memory_order_relaxed) || !enabled() || m_stopping.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (m_lastSample != 0 && now - m_lastSample < kSampleMs) return;
    m_lastSample = now;
    const void* level = nullptr;
    const void* vtable = nullptr;
    if (!readPointer(blockwrite::renderRegion(), kBlockSourceLevel, level) || !readPointer(level, 0, vtable)) return;
    if (vtable != m_levelVtable) {
        m_levelVtable = vtable;
        m_playersField = 0;
        m_fetchActor = nullptr;
        const void* players = nullptr;
        const void* fetch = nullptr;
        unsigned char code[8]{};
        std::int32_t disp = 0;
        if (readPointer(vtable, kLevelPlayersSlot, players) && memory::inGameModule(players)
            && readBytes(players, code, sizeof(code)) && code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x81
            && code[7] == 0xC3) {
            std::memcpy(&disp, code + 3, sizeof(disp));
            if (disp > 0 && disp < 0x10000) m_playersField = disp;
        }
        if (readPointer(vtable, kLevelFetchSlot, fetch) && playerlist::mem::levelFetchLooksRight(fetch)) {
            m_fetchActor = fetch;
        }
        if ((m_playersField == 0 || m_fetchActor == nullptr) && !m_levelLogged) {
            m_levelLogged = true;
            log().warn(L"LocatorBar: the Level getters have an unexpected shape (players {}, fetch {}); faces are not shown",
                       m_playersField != 0, m_fetchActor != nullptr);
        }
    }
    const void* map = nullptr;
    if (m_playersField == 0 || !readPointer(level, static_cast<std::size_t>(m_playersField), map)) return;
    std::vector<playerlist::mem::RawPlayer> raw(playerlist::mem::kMaxPlayerEntries);
    const int count = playerlist::mem::readPlayerMap(map, raw.data(), static_cast<int>(raw.size()));
    if (count < 0) return;
    std::map<std::int64_t, Person> people;
    std::map<const void*, std::string> faces;
    for (int i = 0; i < count; ++i) {
        Person person;
        if (const auto cached = m_faceCache.find(raw[i].skin); cached != m_faceCache.end()) {
            person.facePath = cached->second;
        } else {
            std::uint8_t face[playerlist::mem::kMaxFaceSide * playerlist::mem::kMaxFaceSide * 4];
            int side = 0;
            if (playerlist::mem::readFace(raw[i].skin, face, playerlist::mem::kMaxFaceSide, side) && side > 0) {
                person.facePath = facePathFor(face, side);
            }
        }
        if (raw[i].skin != nullptr) faces.emplace(raw[i].skin, person.facePath);
        const void* const actor = m_fetchActor != nullptr ? fetchActorGuarded(level, m_fetchActor, raw[i].actorId) : nullptr;
        person.loaded = memory::plausiblePointer(actor)
                        && GameData::rawPosOf(actor, person.position[0], person.position[1], person.position[2]);
        people.emplace(raw[i].actorId, std::move(person));
    }
    m_faceCache = std::move(faces);
    std::lock_guard lock(m_peopleMutex);
    m_people = std::move(people);
}

void LocatorBar::onRender(RenderFn original, void* self, void* ctx, void* client, void* owner, int pass, void* area)
{
    m_inside.fetch_add(1, std::memory_order_acq_rel);
    m_slots.clear();
    try {
    if (m_ready.load(std::memory_order_relaxed) && enabled() && !m_stopping.load(std::memory_order_acquire)
        && !m_drawBroken.load(std::memory_order_relaxed) && client != nullptr) {
        void* const player = localPlayerGuarded(client, m_localPlayerSlot);
        const void* head = nullptr;
        const void* node = nullptr;
        float eye[3]{};
        const bool haveEye = GameData::instance().rawPlayerPos(eye[0], eye[1], eye[2]);
        std::unique_lock lock(m_peopleMutex);
        if (readPointer(player, static_cast<std::size_t>(m_listField), head) && readPointer(head, 0, node)) {
            constexpr std::size_t kListWalkGuard = std::size_t{1} << 16;
            for (std::size_t walked = 0; node != head; ++walked) {
                if (walked >= kListWalkGuard) {
                    if (!m_loggedListGuard) {
                        m_loggedListGuard = true;
                        log().warn(L"LocatorBar: the waypoint list did not end after {} entries; left the rest", walked);
                    }
                    break;
                }
                const void* value = nullptr;
                std::int64_t id = 0;
                if (readPointer(node, 0x20, value) && readBytes(static_cast<const char*>(value) + kActorIdField, &id, sizeof(id))) {
                    const auto found = m_people.find(id);
                    auto* const waypoint = static_cast<std::byte*>(const_cast<void*>(value));
                    Slot slot;
                    slot.waypoint = waypoint;
                    if (found != m_people.end() && readByte(waypoint + m_activeField, slot.active)
                        && readByte(waypoint + m_hasColorField, slot.hasColor)
                        && readBytes(waypoint + m_colorField, slot.color, sizeof(slot.color))
                        && readBytes(waypoint + m_positionField, slot.position, sizeof(slot.position))) {
                        const Person& person = found->second;
                        slot.hidden = slot.active == 0;
                        const bool usable = !slot.hidden || (person.loaded && finite3(person.position));
                        if (usable) {
                            slot.argb = locatorbar::packColor(slot.hasColor != 0 ? slot.color : m_defaultColor);
                            slot.facePath = person.facePath;
                            const float* const at = slot.hidden ? person.position : slot.position;
                            slot.distance = haveEye ? std::sqrt((at[0] - eye[0]) * (at[0] - eye[0])
                                                                + (at[1] - eye[1]) * (at[1] - eye[1])
                                                                + (at[2] - eye[2]) * (at[2] - eye[2])) : -1.0f;
                            float code[4]{};
                            locatorbar::markColor(static_cast<std::uint32_t>(m_slots.size()), code);
                            m_slots.push_back(std::move(slot));
                            Slot& saved=m_slots.back();
                            const std::uint8_t one = 1;
                            bool written = writeGuarded(waypoint + m_colorField, code, sizeof(code))
                                           && writeGuarded(waypoint + m_hasColorField, &one, 1);
                            if (written && saved.hidden) {
                                saved.moved = writeGuarded(waypoint + m_positionField, person.position, sizeof(saved.position));
                                written = saved.moved && writeGuarded(waypoint + m_activeField, &one, 1);
                            }
                            if (!written) break;
                        }
                    }
                }
                if (!readPointer(node, 0, node)) break;
            }
        }
    }
    m_inRender = !m_slots.empty();
    m_placed.reserve(m_slots.size());
    } catch(const std::bad_alloc&) {
        restoreSlots();
        m_inRender=false;
        if (!m_drawBroken.exchange(true)) log().error(L"LocatorBar: could not allocate waypoint records; left vanilla");
    }
    m_placed.clear();
    original(self, ctx, client, owner, pass, area);
    m_inRender = false;
    drawPlaced(ctx, client);
    restoreSlots();
    m_inside.fetch_sub(1, std::memory_order_acq_rel);
}

void LocatorBar::restoreSlots()
{
    for (Slot& slot : m_slots) {
        bool ok = writeGuarded(slot.waypoint + m_colorField, slot.color, sizeof(slot.color))
                  && writeGuarded(slot.waypoint + m_hasColorField, &slot.hasColor, 1);
        if (slot.moved) {
            ok = writeGuarded(slot.waypoint + m_positionField, slot.position, sizeof(slot.position))
                 && writeGuarded(slot.waypoint + m_activeField, &slot.active, 1) && ok;
        }
        if (!ok && !m_drawBroken.exchange(true)) {
            log().error(L"LocatorBar: restoring a waypoint faulted; the locator bar is left vanilla");
        }
        slot = Slot{};
    }
    m_slots.clear();
}

void LocatorBar::onIcon(IconFn original, void* a, void* mesh, void* texture, float x, float y, float u0, float v0,
                        float u1, float v1, float w, float h, float scale, float alpha, std::uint32_t color,
                        std::uint32_t tint, float s0, float s1)
{
    const auto index = locatorbar::decodeMark(color);
    if (!m_inRender || index >= m_slots.size()) {
        original(a, mesh, texture, x, y, u0, v0, u1, v1, w, h, scale, alpha, color, tint, s0, s1);
        return;
    }
    Slot& slot = m_slots[static_cast<std::size_t>(index)];
    slot.rect[0] = x; slot.rect[1] = y; slot.rect[2] = w; slot.rect[3] = h;
    slot.alpha = alpha;
    m_placed.push_back(index);
    if (slot.facePath.empty()) {
        original(a, mesh, texture, x, y, u0, v0, u1, v1, w, h, scale, alpha,
                 locatorbar::scaleAlpha(slot.argb, slot.hidden ? kHiddenAlpha : 1.0f), tint, s0, s1);
    }
}

void LocatorBar::drawPlaced(void* ctx, void* client)
{
    for (const auto placed : m_placed) {
        if (m_drawBroken.load(std::memory_order_relaxed)) break;
        const Slot& slot = m_slots[placed];
        const float mul = slot.hidden ? kHiddenAlpha : 1.0f;
        const float colorAlpha = static_cast<float>(slot.argb >> 24) / 255.0f;
        if (!slot.facePath.empty() && !drawFace(ctx, slot, slot.rect[0], slot.rect[1], slot.rect[2], slot.rect[3],
                                                slot.alpha * colorAlpha * mul) && !m_loggedFaceFail) {
            m_loggedFaceFail = true;
            log().warn(L"LocatorBar: a face could not be drawn (its waypoint is left empty this frame)");
        }
        if (slot.distance >= 0) drawDistance(ctx, client, slot, slot.rect[0], slot.rect[1], slot.rect[2], slot.rect[3], slot.alpha * mul);
    }
    m_placed.clear();
}

bool LocatorBar::drawFace(void* ctx, const Slot& slot, float x, float y, float w, float h, float alpha)
{
    if (ctx == nullptr || !uidraw::checkContext(ctx) || !uidraw::imagesReady() || !uidraw::api().tintReady) return false;
    bool empty = false;
    if (!uidraw::imageQueueEmptyGuarded(ctx, empty)) return false;
    if (!empty) {
        if (!m_loggedBusy) { m_loggedBusy = true; log().warn(L"LocatorBar: the UI image queue was not empty; a dot was left vanilla once"); }
        return false;
    }
    uidraw::ResourceLocation location;
    if (!uidraw::makeLocation(slot.facePath.c_str(), location, kRawFileSystem)) return false;
    float saved[4]{};
    if (!uidraw::readShaderColorGuarded(ctx, saved)) return false;
    uidraw::TexturePtr texture;
    bool ok = uidraw::getTextureGuarded(ctx, location, texture);
    if (ok && texture.texture != nullptr) {
        const float left = std::floor(x + w * 0.5f - kFaceSize * 0.5f);
        const float top = std::floor(y + h * 0.5f - kFaceSize * 0.5f);
        ok = uidraw::drawImageGuarded(ctx, texture, left, top, kFaceSize, kFaceSize);
        if (!uidraw::flushImagesGuarded(ctx, alpha)) ok = false;
    } else {
        ok = false;
    }
    if (texture.texture != nullptr || texture.textureCount != nullptr || texture.locationCount != nullptr) {
        if (!uidraw::releaseTextureGuarded(texture)) ok = false;
    }
    if (!uidraw::writeShaderColorGuarded(ctx, saved) && !m_drawBroken.exchange(true)) {
        log().error(L"LocatorBar: restoring the UI color faulted; the locator bar is left vanilla");
        return false;
    }
    if (ok && !m_loggedDraw) {
        m_loggedDraw = true;
        log().info(L"LocatorBar: drawing player faces on the locator bar");
    }
    return ok;
}

bool LocatorBar::drawDistance(void* ctx, void* client, const Slot& slot, float x, float y, float w, float h, float alpha)
{
    const auto& api = uidraw::api();
    if (ctx == nullptr || client == nullptr || !uidraw::checkContext(ctx) || uidraw::context().drawText == nullptr
        || uidraw::context().flushText == nullptr || api.clientFontSlot < 0 || api.fontOfHandle == nullptr
        || api.fontHandleDtor == nullptr || !api.tintReady) return false;
    struct ShortString { char text[16]{}; std::uint64_t size = 0, cap = 15; } text;
    static_assert(sizeof(ShortString) == 32);
    const std::string label = locatorbar::distanceText(slot.distance);
    if (label.empty() || label.size() >= sizeof(text.text)) return false;
    std::memcpy(text.text, label.data(), label.size());
    text.size = label.size();
    struct Measure { float size = kTextScale, padding = 0; bool shadow = true, symbols = false, hyphen = false; } measure;
    struct Caret { int position = -1; bool render = false; } caret;
    constexpr float white[4] = {1, 1, 1, 1};
    void* begin = nullptr;
    void* end = nullptr;
    const bool emptyQueue = readBytes(static_cast<char*>(ctx) + kTextQueueBegin, &begin, 8)
                            && readBytes(static_cast<char*>(ctx) + kTextQueueEnd, &end, 8) && begin == end;
    float saved[4]{};
    if (!uidraw::readShaderColorGuarded(ctx, saved)) return false;
    alignas(16) std::byte handle[0x80]{};
    void* const font = uidraw::fontGuarded(client, handle);
    bool ok = font != nullptr;
    if (ok) {
        const float center = x + w * 0.5f;
        const float bottom = std::floor(y + h * 0.5f - kFaceSize * 0.5f) - 0.5f;
        const float rect[4] = {center - 40.0f, center + 40.0f, bottom - kTextHeight, bottom};
        ok = uidraw::drawTextGuarded(ctx, font, rect, &text, white, alpha, 2, &measure, &caret);
        if (emptyQueue && !uidraw::flushTextGuarded(ctx)) ok = false;
        if (!uidraw::fontHandleDtorGuarded(handle)) ok = false;
    }
    if (!uidraw::writeShaderColorGuarded(ctx, saved) && !m_drawBroken.exchange(true)) {
        log().error(L"LocatorBar: restoring the UI color faulted; the locator bar is left vanilla");
        return false;
    }
    return ok;
}

}
