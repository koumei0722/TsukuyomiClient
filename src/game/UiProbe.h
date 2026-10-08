#pragma once

#include <cstdint>
#include <string>

namespace tsukuyomi::uiprobe {

void registerDefExtension(const char* space, const char* name, const std::string& frontJson,
                          const std::string& backJson);
void registerDefAppend(const char* space, const char* name, const char* key,
                       const std::string& elementsJson);
void registerDefReplaceArray(const char* space, const char* name, const char* key,
                             const std::string& elementsJson);
void registerDefReplaceControls(const char* space, const char* name, const std::string& controlsJson);
void registerDefProperty(const char* space, const char* name, const char* key, const std::string& valueJson);
void* extendDefinition(void* self, const void* space, const void* name, void* vanilla);

bool onSliderPublish(void* self, float value, float& reseed);

void onPageBag(void* bag);

void* substituteOwnPage(void* self, const void* space, const void* name);

bool substituteKeyRows(void* container);

void restoreKeyRows();

void pumpControlsKeybind();
void syncOwnKeyRows();

bool overrideTranslation(const void* key, void* out) noexcept;

void onUiEvent(void* self, const void* event);

void pumpMenuSelection();

void onSettingsGroupRegister(void* registry, const void* idView, void* provider);

void afterSettingsGroupRegister(void* registry, const void* idView, void* provider);

void onSettingsProviderCall(void* self, void* out);

void onSettingsFindComponent(void* registry, void* out, const void* idView);

bool openOwnPage(bool force = false);

bool refreshOwnPage();

void pumpSettingsToggle();

bool takeSettingsDirty();

void markSettingsDirty();

bool installPublishPump();

void removeCrashWatch();

}
