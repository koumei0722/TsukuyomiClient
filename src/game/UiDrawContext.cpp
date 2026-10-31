#include "game/UiDrawContext.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include <Windows.h>
#include <intrin.h>
#include <cstring>

namespace tsukuyomi::uidraw {
namespace {
Api g_api;
bool g_resolved = false;
CtxApi g_ctx;

std::byte* findInFunction(std::byte* fn, std::string_view pattern)
{
    if (fn == nullptr) {
        return nullptr;
    }
    const std::size_t size = memory::functionSize(fn);
    if (size == 0) {
        return nullptr;
    }
    if (!memory::isReadable(fn, size)) {
        return nullptr;
    }
    const ScanHit hit = scanRange(std::span<std::byte>(fn, size), pattern);
    return hit.count == 1 ? hit.address : nullptr;
}

bool startsWith(const void* at, std::string_view pattern, std::size_t length)
{
    if (at == nullptr || !memory::inGameModule(at) || !memory::isReadable(at, length)) {
        return false;
    }
    auto* const p = static_cast<std::byte*>(const_cast<void*>(at));
    return scanRange(std::span<std::byte>(p, length), pattern).address == p;
}

void* callTarget(const std::byte* e8)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, e8 + 1, 4);
    void* const target = const_cast<std::byte*>(e8 + 5 + rel);
    return memory::inGameModule(target) ? target : nullptr;
}

int readDisp32(const std::byte* at)
{
    std::int32_t disp = 0;
    std::memcpy(&disp, at, 4);
    return disp;
}

bool readGuarded(const void* at, void* out, std::size_t size)
{
    __try {
        std::memcpy(out, at, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}
float* shaderColorOf(void* ctx)
{
    std::byte* const screen = *reinterpret_cast<std::byte**>(static_cast<std::byte*>(ctx) + g_api.screenInCtx);
    return screen != nullptr ? *reinterpret_cast<float**>(screen + g_api.shaderColor) : nullptr;
}

bool readShaderColorGuarded(void* ctx, float* out4)
{
    __try {
        const float* const color = shaderColorOf(ctx);
        if (color == nullptr) {
            return false;
        }
        std::memcpy(out4, color, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeShaderColorGuarded(void* ctx, const float* in4)
{
    __try {
        float* const color = shaderColorOf(ctx);
        if (color == nullptr) {
            return false;
        }
        std::memcpy(color, in4, 16);
        reinterpret_cast<std::byte*>(color)[g_api.shaderColorDirty] = std::byte{1};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool fillGuarded(void* ctx, const float* rect, const float* color, float alpha)
{
    __try {
        g_ctx.fill(ctx, rect, color, alpha);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct FontHandleLayout {
    int flag = -1, index = -1, table = -1, tableBegin = -1, tableEnd = -1, fallback = -1;
    const std::uint64_t* invalidIndex = nullptr;
};
FontHandleLayout g_fontHandle;
void* __fastcall fontOfHandleInline(void* handle)
{
    const auto* const h = static_cast<const std::byte*>(handle);
    const auto* const flag = *reinterpret_cast<const std::uint8_t* const*>(h + g_fontHandle.flag);
    if (flag == nullptr || *flag == 0) {
        return nullptr;
    }
    const std::uint64_t index = *reinterpret_cast<const std::uint64_t*>(h + g_fontHandle.index);
    if (index != *g_fontHandle.invalidIndex) {
        const auto* const table = *reinterpret_cast<const std::byte* const*>(h + g_fontHandle.table);
        const auto* const begin = *reinterpret_cast<const std::byte* const*>(table + g_fontHandle.tableBegin);
        const auto* const end = *reinterpret_cast<const std::byte* const*>(table + g_fontHandle.tableEnd);
        if (index < static_cast<std::uint64_t>(end - begin) / 16) {
            return *reinterpret_cast<void* const*>(begin + index * 16);
        }
    }
    return *reinterpret_cast<void* const*>(h + g_fontHandle.fallback);
}

void* fontGuarded(void* client, void* handle)
{
    __try {
        void** const vt = *static_cast<void***>(client);
        reinterpret_cast<GetFontHandleFn>(vt[g_api.clientFontSlot / 8])(client, handle);
        return g_api.fontOfHandle(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool fontHandleDtorGuarded(void* handle)
{
    __try {
        g_api.fontHandleDtor(handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool drawTextGuarded(void* ctx, void* font, const float* rect, void* text, const float* color, float alpha,
                     int align, const void* measure, const void* caret)
{
    __try {
        g_ctx.drawText(ctx, font, rect, text, color, alpha, align, measure, caret);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool flushTextGuarded(void* ctx)
{
    __try {
        g_ctx.flushText(ctx, 0.0f, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool checkContext(void* ctx)
{
    const void* vt = nullptr;
    if (!readGuarded(ctx, &vt, 8) || vt == nullptr || !memory::inGameModule(vt)) {
        return false;
    }
    if (vt == g_ctx.vtable) {
        return true;
    }
    g_ctx = CtxApi{};
    g_ctx.vtable = vt;
    void* slots[40]{};
    if (!readGuarded(vt, slots, sizeof(slots))) {
        return false;
    }
    if (startsWith(slots[16], "41 56 56 57 53 48 83 EC 68 0F 29 74 24 50 0F 28 F3 4C 89 C3 48 89 D7 48 89 CE 48 8B 05",
                   30)) {
        g_ctx.fill = reinterpret_cast<FillFn>(slots[16]);
    }
    if (startsWith(slots[6],
                   "41 57 41 56 56 57 53 48 83 EC 70 4C 89 C0 49 89 D0 48 8B B4 24 E0 00 00 00 4C 8B 9C 24 D8 00 00 00 "
                   "4C 8B 94 24 C0 00 00 00 48 8B 91 88 00 00 00 48 3B",
                   50)) {
        g_ctx.drawText = reinterpret_cast<DrawTextFn>(slots[6]);
    }
    if (startsWith(slots[7], "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC", 15)
        && findInFunction(static_cast<std::byte*>(slots[7]), "4C 89 C7 F3 0F 11 8D ? ? ? ? 48 89 8D ? ? ? ? 48 8B 41 10")
               != nullptr) {
        g_ctx.flushText = reinterpret_cast<FlushTextFn>(slots[7]);
    }
    if (startsWith(slots[8],
                   "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 00 00 00 48 8D AC 24 80 00 00 00 44 0F 29 4D 20 "
                   "44 0F 29 45 10 0F 29 7D 00 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 4D 89 CE 4D 89 C4 48 89 55 D0 48 89 CF "
                   "4C 8B AD A8 00 00 00 48 8B B5 A0 00 00 00 0F B6 85 B0 00 00 00",
                   87)) {
        g_ctx.drawImage = reinterpret_cast<DrawImageFn>(slots[8]);
    }
    if (startsWith(slots[10],
                   "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 80 00 00 00 0F 29 B5 ? ? ? ? "
                   "48 C7 85 ? ? ? ? FE FF FF FF 4C 8B B1 ? ? ? ? 48 8B 99 ? ? ? ? 49 39 DE 0F 84",
                   64)) {
        auto* const at = static_cast<std::byte*>(slots[10]);
        const int begin = readDisp32(at + 0x30), end = readDisp32(at + 0x37);
        if (begin > 0 && begin < 0x400 && end == begin + 8) {
            g_ctx.flushImages = reinterpret_cast<FlushImagesFn>(slots[10]);
            g_ctx.imageQueueBegin = begin;
            g_ctx.imageQueueEnd = end;
        }
    }
    if (startsWith(slots[32],
                   "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 48 89 D3 49 83 78 18 00 74 ? "
                   "41 83 38 0A 0F 85",
                   37)) {
        g_ctx.getTexture = reinterpret_cast<GetTextureFn>(slots[32]);
    }
    static bool told = false;
    if (!told) {
        told = true;
        log().info(L"ShulkerPreview: UI render context: fill {} / drawText {} / flushText {} / getTexture {} / drawImage {} / "
                   L"flushImages {} (queue +{:#x})",
                   g_ctx.fill != nullptr, g_ctx.drawText != nullptr, g_ctx.flushText != nullptr, g_ctx.getTexture != nullptr,
                   g_ctx.drawImage != nullptr, g_ctx.flushImages != nullptr, g_ctx.imageQueueBegin);
    }
    return true;
}

bool imagesReady()
{
    return g_ctx.getTexture != nullptr && g_ctx.drawImage != nullptr && g_ctx.flushImages != nullptr;
}

bool makeLocation(const char* path, ResourceLocation& out, std::int32_t fileSystem)
{
    const std::size_t size = path != nullptr ? std::strlen(path) : 0;
    if (size < 16) {
        return false;
    }
    constexpr std::uint64_t kBasis = 0xCBF29CE484222325ull, kPrime = 0x100000001B3ull;
    std::uint64_t hash = kBasis;
    for (std::size_t i = 0; i < size; ++i) {
        hash = (hash * kPrime) ^ static_cast<unsigned char>(path[i]);
    }
    out = ResourceLocation{};
    out.fileSystem = fileSystem;
    out.path = path;
    out.pathSize = size;
    out.pathCapacity = size;
    out.pathHash = hash;
    out.fullHash = hash ^ ((kBasis ^ static_cast<std::uint8_t>(out.fileSystem)) * kPrime);
    return true;
}

bool imageQueueEmptyGuarded(void* ctx, bool& empty)
{
    __try {
        auto* const base = static_cast<std::byte*>(ctx);
        empty = *reinterpret_cast<void**>(base + g_ctx.imageQueueBegin) == *reinterpret_cast<void**>(base + g_ctx.imageQueueEnd);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool getTextureGuarded(void* ctx, const ResourceLocation& location, TexturePtr& out)
{
    __try {
        g_ctx.getTexture(ctx, &out, &location, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool drawImageGuarded(void* ctx, const TexturePtr& texture, float x, float y, float w, float h)
{
    const float position[2] = {x, y}, size[2] = {w, h}, uv[2] = {0, 0}, uvSize[2] = {1, 1};
    if (texture.texture == nullptr) {
        return false;
    }
    __try {
        g_ctx.drawImage(ctx, texture.texture, position, size, uv, uvSize, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool flushImagesGuarded(void* ctx, float alpha)
{
    struct EmptyHashedString {
        std::uint64_t hash = 0;
        char text[16]{};
        std::uint64_t size = 0, capacity = 15;
        const void* lastMatch = nullptr;
    } material;
    static_assert(sizeof(EmptyHashedString) == 0x30);
    constexpr float white[4] = {1, 1, 1, 1};
    __try {
        g_ctx.flushImages(ctx, white, alpha, &material);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
void releaseCount(void* count)
{
    if (count == nullptr) {
        return;
    }
    auto* const counts = reinterpret_cast<volatile long*>(static_cast<std::byte*>(count) + 8);
    using DestroyFn = void(__fastcall*)(void*);
    if (_InterlockedDecrement(&counts[0]) == 0) {
        void** const vt = *static_cast<void***>(count);
        reinterpret_cast<DestroyFn>(vt[0])(count);
        if (_InterlockedDecrement(&counts[1]) == 0) {
            reinterpret_cast<DestroyFn>(vt[1])(count);
        }
    }
}
}

bool releaseTextureGuarded(TexturePtr& texture)
{
    __try {
        releaseCount(texture.textureCount);
        releaseCount(texture.locationCount);
        texture = TexturePtr{};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Api& api() { return g_api; }
CtxApi& context() { return g_ctx; }
void resolve()
{
    if (g_resolved) return;
    g_resolved = true;
    Scanner& scanner = Scanner::instance();
    Api api;
    std::byte* const render = scanner.address(Target::HoverRendererRender);
    std::byte* const boxSize = scanner.address(Target::HoverBoxSizeStore);
    void* const drawItem = scanner.address(Target::UiDrawItem);
    if (boxSize != nullptr && memory::isReadable(boxSize, 4)) {
        api.boxW = static_cast<std::uint8_t>(boxSize[3]);
        api.boxH = api.boxW + static_cast<int>(sizeof(float));
    }
    if (std::byte* at = findInFunction(render, "48 8B 80 ? ? ? ? 48 8D 55 ? 48 89 D9 FF 15 ? ? ? ? 48 8B 45 ? 48 85 C0")) {
        const int base = static_cast<std::uint8_t>(at[10]);
        FontHandleLayout layout;
        layout.flag = static_cast<std::uint8_t>(at[23]) - base;
        std::byte* const pick = findInFunction(render, "48 8B 4D ? 48 3B 0D ? ? ? ? 74 ? 4C 8B 45 ? 49 8B 40 ? 4D 8B 40 ? 49 29 C0 49 C1 F8 04");
        std::byte* const fallback = findInFunction(render, "48 C1 E1 04 48 01 C8 EB ? 48 8D 45 ? 48 8B 08");
        if (pick != nullptr && fallback != nullptr) {
            layout.index = static_cast<std::uint8_t>(pick[3]) - base;
            layout.invalidIndex = reinterpret_cast<const std::uint64_t*>(pick + 11 + readDisp32(pick + 7));
            layout.table = static_cast<std::uint8_t>(pick[16]) - base;
            layout.tableBegin = static_cast<std::uint8_t>(pick[20]);
            layout.tableEnd = static_cast<std::uint8_t>(pick[24]);
            layout.fallback = static_cast<std::uint8_t>(fallback[12]) - base;
        }
        const bool fits = layout.flag >= 0 && layout.index >= 0 && layout.table >= 0 && layout.fallback >= 0
                          && layout.flag + 8 <= 0x80 && layout.index + 8 <= 0x80 && layout.table + 8 <= 0x80
                          && layout.fallback + 8 <= 0x80 && memory::inGameModule(layout.invalidIndex);
        if (fits) {
            g_fontHandle = layout;
            api.clientFontSlot = readDisp32(at + 3);
            api.fontOfHandle = &fontOfHandleInline;
        }
    }
    if (std::byte* at = findInFunction(render, "48 8D 4D ? E8 ? ? ? ? 0F 28 B5")) {
        api.fontHandleDtor = reinterpret_cast<FontHandleDtorFn>(callTarget(at + 4));
    }
    api.textFieldsOk = findInFunction(render, "48 8B 47 40 48 83 7F 48 10 72 ? 48 8B 7F 30") != nullptr;
    std::byte* const fillSite = scanner.address(Target::ShaderColorFillSite);
    std::byte* const tintSite = scanner.address(Target::GlintTintSite);
    if (fillSite != nullptr && tintSite != nullptr && memory::isReadable(fillSite, 14) && memory::isReadable(tintSite, 8)
        && tintSite[7] == fillSite[3] && fillSite[13] == std::byte{1}) {
        api.shaderColor = static_cast<std::uint8_t>(fillSite[3]);
        api.shaderColorDirty = static_cast<std::uint8_t>(fillSite[12]);
    }
    if (std::byte* at = findInFunction(static_cast<std::byte*>(drawItem), "48 8B 56 ? 4C 8D 7D ? 4C 89 F9 4D 89 F0 49 89 C1 E8")) {
        api.screenInCtx = static_cast<std::uint8_t>(at[3]);
    }
    api.tintReady = api.shaderColor > 0 && api.shaderColorDirty > 0 && api.screenInCtx > 0;

    g_api = api;
}
}
