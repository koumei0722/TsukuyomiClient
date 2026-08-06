#include "game/Abilities.h"

#include <Windows.h>

namespace tsukuyomi::abilities {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

constexpr ptrdiff_t kContextLayeredOffset = 0x08;

bool readPointerGuarded(const void* address, std::byte*& value)
{
    __try {
        value = *static_cast<std::byte* const*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool readInt(const void* address, int& value)
{
    __try {
        value = *static_cast<const int*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool writeInt(void* address, int value)
{
    __try {
        *static_cast<int*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readFloat(const void* address, float& value)
{
    __try {
        value = *static_cast<const float*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool writeFloat(void* address, float value)
{
    __try {
        *static_cast<float*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

std::byte* fromContext(void* context)
{
    if (context == nullptr) {
        return nullptr;
    }
    std::byte* layered = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(context) + kContextLayeredOffset, layered)) {
        return nullptr;
    }
    return layered;
}

std::byte* at(std::byte* layered, int layer, int index)
{
    return layered + kArrayOffset + kLayerStride * layer + kStride * index;
}

bool looksValid(std::byte* layered)
{
    if (layered == nullptr) {
        return false;
    }

    int type = 0;
    if (!readInt(at(layered, kPlayerLayer, kFlySpeed) + kTypeOffset, type)
        || type != kTypeFloat) {
        return false;
    }
    if (!readInt(at(layered, kPlayerLayer, kWalkSpeed) + kTypeOffset, type)
        || type != kTypeFloat) {
        return false;
    }
    return true;
}

std::byte* slotOf(std::byte* layered, int index)
{
    for (int layer = kLayerCount - 1; layer >= 0; --layer) {
        std::byte* const slot = at(layered, layer, index);
        int type = 0;
        if (!readInt(slot + kTypeOffset, type)) {
            return nullptr;
        }
        if (type == kTypeBool || type == kTypeFloat) {
            return slot;
        }
    }

    return at(layered, kPlayerLayer, index);
}

}
