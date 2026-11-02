#pragma once

#include <cstddef>

namespace tsukuyomi::tesslayout {
constexpr std::ptrdiff_t kLimit = 0x29c, kBuilding = 0x2a0, kVoid = 0x255, kNoColor = 0x254;
constexpr std::ptrdiff_t kOffset = 0x1cc, kScale = 0x1d8, kUseMatrix = 0x210;
constexpr std::ptrdiff_t kPosBegin = 0x10, kColorBegin = 0x70;
constexpr std::size_t kReadable = kBuilding + 4;

constexpr std::ptrdiff_t kResetWord = 0x298, kResetByte = 0x1ca, kResetPointer = 0x170;
constexpr std::ptrdiff_t kResetBytes[] = {0x1b4, 0x188, 0x1ba, 0x168, 0x194, 0x1a0, 0x1ac, 0x1be, 0x1c9};
constexpr std::ptrdiff_t kBuffers = 8, kMode = 8, kFormat = 0;
constexpr std::ptrdiff_t kBeginWord = 0x20c, kBeginCopyFrom = 0x1e8, kBeginCopyTo = 0x1f0;
constexpr std::ptrdiff_t kBeginByte = 0x158, kBeginDefaultColor = 0x1b0, kBeginColorFlag = 0x1b4;
constexpr std::ptrdiff_t kBeginFormatFlag = 0x159;

constexpr std::ptrdiff_t kBtRegion = 0x8, kBtLayer = 0x80;
constexpr std::ptrdiff_t kBtCacheOrigin = 0x6b8, kBtCacheRegion = 0x6c8;
constexpr std::ptrdiff_t kBtCacheFlags = 0x6d0, kBtCacheBlocks = 0x4550, kBtCacheTail = 0x23950;
constexpr std::size_t kBtCacheFlagsBytes = 0x3e80, kBtCacheBlockBytes = 0x1f400;
constexpr std::size_t kBtWritable = kBtCacheTail + kBtCacheBlockBytes;
constexpr std::ptrdiff_t kBlockClientPipeline = 0xa1;

constexpr std::ptrdiff_t kCtxMatrices = 0x18, kCtxColor = 0x30, kCtxSingleBlock = 0x7b, kCtxTess = 0xb0;
constexpr std::ptrdiff_t kLrpCamera = 0x658, kLrpTessOwner = 0x1180, kOwnerAtlas = 0x1d0;
constexpr std::size_t kTextureBytes = 0x20;
constexpr std::size_t kRenderParamsBytes = 0x150, kRenderParamVariant = 0x28;
constexpr std::size_t kRenderParamTag = 0x20, kRenderParamCount = 0x140;
constexpr std::ptrdiff_t kCtrlStrongCount = 0x8;
constexpr std::ptrdiff_t kMaterialInfoBody = 0x40;
}
