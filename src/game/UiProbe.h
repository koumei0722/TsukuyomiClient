#pragma once

#include <cstdint>

namespace tsukuyomi::uiprobe {

void onLookup(const void* space, const void* name);

void onOptionRegister(void* self, int id, const void* name);

void onLookupResult(const void* space, const void* name, void* value);

void* substitute(void* self, const void* space, const void* name);

void installCrashProbe();
void removeCrashProbe();

void onButtonMappingsBegin(void* out);
void onButtonMappingsEnd(void* out);

void onBagLookup(void* self, const char* key, void* result);

void* onResolveVarBegin(void* self, const void* name);
void onResolveVarEnd(void* saved);

void onBindingRead(void* bag);

void onKeybindListBuilt(void* self);

void onControlsBindingName(void* self, void* arg3);

void onControlsRowBindings(void* rcx, void* rdx, void* ctx);

void onControlsSectionSetup(void* self, void* arg2);

void onOreFacetBind(void* out, void* rdx, void* name, unsigned flag);

void onOreKeyboardInputGroup(void* self, void* out);

void onKeyActionName(void* out, int index);

bool overrideKeyActionName(void* out, int index);

void onKeyBindingLookup(const void* name);

void onOreKeyRowsWrap(void* out, void* container);

void setOreKeyRowsWrapAddr(void* address);

void onRowDataCandidate(int which, void* arg4);

void onOreKeyNameToIndex(void* arg1);

void onTranslate(const void* key);

void onOreKeyRowsConsume(void* rcx, void* rdx, bool after);

void onKeyRowListBuild(void* array, void* rdx, bool after);

int bumpKeyRowLimit(void* container, int delta);

void dumpKeyRowContainer(void* container, void* arg3);

bool substituteKeyRows(void* container, bool refresh = false);

bool purgeOwnKeyRows(void* container);

void popKeyRowSubstitution();

void restoreKeyRows();

void pumpControlsKeybind();

void onOreKeyRowData(void* container, std::uintptr_t index, bool substituted);

void onOreKeyRowDataResult(void* out, std::uintptr_t index);

bool overrideTranslation(const void* key, void* out) noexcept;

void onOreKeyRowsBegin();
void onOreKeyRowsEnd(void* out);

void onUiEvent(void* self, const void* event);

void pumpMenuSelection();

void onSettingsGroupRegister(void* registry, const void* idView, void* provider);

void afterSettingsGroupRegister(void* registry, const void* idView, void* provider);

void onSettingsProviderCall(void* self, void* out);

bool beforeSettingsGroupInfoUpdate(void* self);
void afterSettingsGroupInfoUpdate(void* self, bool swapped);

void onSettingsFindComponent(void* registry, void* out, const void* idView);

void pumpSettingsToggle();

bool takeSettingsDirty();

bool installPublishPump();

}
