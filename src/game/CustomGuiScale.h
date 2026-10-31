#pragma once

#include "ui/Menu.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace tsukuyomi::customguiscale {

constexpr int kCustomValue = 0x40000000;
constexpr const char* kSliderIds[2] = {"tk.ug.v", "tk.ug.a"};
struct OptionSpan {
    std::uintptr_t count;
    const void* first;
};
using ComputeFn = float(__fastcall*)(void*, const float*, const float*, const float*);
using OptionListFn = OptionSpan*(__fastcall*)(void*, OptionSpan*);
using OptionGetFn = int(__fastcall*)(void*);
using OptionSetFn = void(__fastcall*)(void*, int);
extern ComputeFn originalCompute;
extern OptionListFn originalList;
extern OptionGetFn originalGet;
extern OptionSetFn originalSet;

bool prepareHooks();
void* providerFunction(std::size_t slot);
void setHooksReady(bool ready);
float __fastcall detourCompute(void* self, const float* a2, const float* a3, const float* a4);
OptionSpan* __fastcall detourList(void* self, OptionSpan* out);
int __fastcall detourGet(void* self);
void __fastcall detourSet(void* self, int value);

void start();
void onFrame();
void shutdown();
void requestRefresh();
void update();
bool active();
bool isCustom();
bool isCustomComponentId(const std::string& id);
MenuItem sliderItem();

}

namespace tsukuyomi::uiprobe {
bool appendCustomGuiScaleOption(std::uintptr_t& count, const void*& first);
void requestGuiScalePublish();
}
