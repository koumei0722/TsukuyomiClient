#pragma once

#include <cstdint>

namespace tsukuyomi::uidraw {
using GetFontHandleFn = void*(__fastcall*)(void* client, void* out);
using FontOfHandleFn = void*(__fastcall*)(void* handle);
using FontHandleDtorFn = void(__fastcall*)(void* handle);
using FillFn = void(__fastcall*)(void* ctx, const float* rect, const float* color, float alpha);
using DrawTextFn = void(__fastcall*)(void* ctx, void* font, const float* rect, void* text, const float* color,
                                   float alpha, int align, const void* measure, const void* caret);
using FlushTextFn = void(__fastcall*)(void* ctx, float obfuscation, std::uint64_t alphaOverride);
struct Api {
    int boxW = -1, boxH = -1, clientFontSlot = -1;
    FontOfHandleFn fontOfHandle = nullptr;
    FontHandleDtorFn fontHandleDtor = nullptr;
    bool textFieldsOk = false;
    int screenInCtx = -1, shaderColor = -1, shaderColorDirty = -1;
    bool tintReady = false;
};
struct TexturePtr {
    void* texture = nullptr;
    void* textureCount = nullptr;
    void* location = nullptr;
    void* locationCount = nullptr;
};
static_assert(sizeof(TexturePtr) == 0x20);
struct ResourceLocation {
    std::int32_t fileSystem = 0;
    std::uint32_t padding = 0;
    const char* path = nullptr;
    std::uint64_t pathUnused = 0, pathSize = 0, pathCapacity = 0;
    std::uint64_t pathHash = 0, fullHash = 0;
};
static_assert(sizeof(ResourceLocation) == 0x38);
bool makeLocation(const char* path, ResourceLocation& out);
using GetTextureFn = TexturePtr*(__fastcall*)(void* ctx, TexturePtr* out, const ResourceLocation* location, bool forceReload);
using DrawImageFn = void(__fastcall*)(void* ctx, const void* textureData, const float* position, const float* size,
                                      const float* uvPosition, const float* uvSize, bool flag);
using FlushImagesFn = void(__fastcall*)(void* ctx, const float* color, float alpha, const void* materialName);
struct CtxApi {
    const void* vtable = nullptr;
    FillFn fill = nullptr;
    DrawTextFn drawText = nullptr;
    FlushTextFn flushText = nullptr;
    GetTextureFn getTexture = nullptr;
    DrawImageFn drawImage = nullptr;
    FlushImagesFn flushImages = nullptr;
    int imageQueueBegin = -1, imageQueueEnd = -1;
};
Api& api();
CtxApi& context();
void resolve();
bool checkContext(void* ctx);
bool readShaderColorGuarded(void* ctx, float* out4);
bool writeShaderColorGuarded(void* ctx, const float* in4);
bool fillGuarded(void* ctx, const float* rect, const float* color, float alpha);
void* fontGuarded(void* client, void* handle);
bool fontHandleDtorGuarded(void* handle);
bool drawTextGuarded(void* ctx, void* font, const float* rect, void* text, const float* color, float alpha,
                     int align, const void* measure, const void* caret);
bool flushTextGuarded(void* ctx);
bool imagesReady();
bool imageQueueEmptyGuarded(void* ctx, bool& empty);
bool getTextureGuarded(void* ctx, const ResourceLocation& location, TexturePtr& out);
bool drawImageGuarded(void* ctx, const TexturePtr& texture, float x, float y, float w, float h);
bool flushImagesGuarded(void* ctx, float alpha);
bool releaseTextureGuarded(TexturePtr& texture);
}
