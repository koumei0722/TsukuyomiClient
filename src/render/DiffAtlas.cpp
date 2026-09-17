#include "render/DiffAtlas.h"

#include "core/Logger.h"
#include "memory/Memory.h"
#include "render/PackTexture.h"
#include "core/Paths.h"

#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsukuyomi::atlas {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kTileCol = 63;
constexpr int kTileRow = 63;

std::atomic<int> g_bakeCol{-2};
std::atomic<int> g_bakeRow{-2};

void loadBakeTarget()
{
    if (g_bakeCol.load(std::memory_order_acquire) != -2) {
        return;
    }
    int col = kTileCol;
    int row = kTileRow;
    std::error_code ec;
    const auto path = paths::dataDir() / L"diag-bake.txt";
    if (std::filesystem::exists(path, ec)) {
        std::ifstream file(path);
        int c = -1;
        int r = -1;
        if (file >> c >> r && c >= 0 && c < 64 && r >= 0 && r < 64) {
            col = c;
            row = r;
        }
    }
    g_bakeRow.store(row, std::memory_order_relaxed);
    g_bakeCol.store(col, std::memory_order_release);
}

int bakeCol()
{
    loadBakeTarget();
    return g_bakeCol.load(std::memory_order_acquire);
}

int bakeRow()
{
    loadBakeTarget();
    return g_bakeRow.load(std::memory_order_acquire);
}

constexpr unsigned kTilePixels = 16;

constexpr unsigned kMinAtlas = 512;
constexpr unsigned kMinSide = 256;
constexpr unsigned kMaxAtlas = 8192;

constexpr unsigned kGenericRead = 0xAC3;

constexpr unsigned kNonPixelShaderResource = 0x40;
constexpr unsigned kPixelShaderResource = 0x80;
constexpr unsigned kCopySource = 0x800;

constexpr unsigned kWriteStates = 0x4 | 0x8 | 0x10 | 0x400 | 0x1000;

bool isShaderReadable(unsigned state)
{
    if ((state & kWriteStates) != 0) {
        return false;
    }
    return (state & (kNonPixelShaderResource | kPixelShaderResource | kCopySource)) != 0
           || state == kGenericRead;
}

constexpr unsigned kAllSubresources = 0xffffffffu;

constexpr std::size_t kMaxCandidates = 64;
constexpr unsigned kCommon = 0;
constexpr unsigned kCopyDest = 0x400;

struct Candidate {
    std::atomic<ID3D12Resource*> res{nullptr};
    std::atomic<unsigned> state{0};
    std::atomic<unsigned> size{0};
    std::atomic<unsigned> mips{0};
    std::atomic<bool> done{false};
};
Candidate g_cands[kMaxCandidates];
std::atomic<std::size_t> g_candCount{0};
std::atomic<unsigned> g_candFullTold{0};
std::atomic<unsigned> g_bakedFromCreate{0};

constexpr std::size_t kShapeSlots = 12;
struct ShapeTally {
    std::atomic<unsigned> w{0};
    std::atomic<unsigned> h{0};
    std::atomic<unsigned> fmt{0};
    std::atomic<unsigned> mips{0};
    std::atomic<unsigned long long> count{0};
};
ShapeTally g_shapes[kShapeSlots];
std::atomic<std::size_t> g_shapeCount{0};

void tallyShape(const D3D12_RESOURCE_DESC& d)
{
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < 256 || d.Height < 256) {
        return;
    }
    const auto w = static_cast<unsigned>(d.Width);
    const auto h = static_cast<unsigned>(d.Height);
    const auto fmt = static_cast<unsigned>(d.Format);
    const std::size_t n = std::min(g_shapeCount.load(std::memory_order_acquire), kShapeSlots);
    for (std::size_t i = 0; i < n; ++i) {
        if (g_shapes[i].w.load(std::memory_order_relaxed) == w
            && g_shapes[i].h.load(std::memory_order_relaxed) == h
            && g_shapes[i].fmt.load(std::memory_order_relaxed) == fmt) {
            g_shapes[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    const std::size_t at = g_shapeCount.load(std::memory_order_relaxed);
    if (at >= kShapeSlots) {
        return;
    }
    g_shapes[at].w.store(w, std::memory_order_relaxed);
    g_shapes[at].h.store(h, std::memory_order_relaxed);
    g_shapes[at].fmt.store(fmt, std::memory_order_relaxed);
    g_shapes[at].mips.store(d.MipLevels, std::memory_order_relaxed);
    g_shapes[at].count.store(1, std::memory_order_relaxed);
    g_shapeCount.store(at + 1, std::memory_order_release);
}

std::atomic<bool> g_baked{false};

constexpr std::size_t kMaxBaked = 64;
std::atomic<ID3D12Resource*> g_bakedAt[kMaxBaked]{};
std::atomic<unsigned> g_bakedSize[kMaxBaked]{};
std::atomic<unsigned> g_bakedHeight[kMaxBaked]{};
std::atomic<std::size_t> g_bakedCount{0};
std::atomic<bool> g_bakedFullLogged{false};

constexpr unsigned long long kRebakeMs = 3000;
std::atomic<unsigned long long> g_bakedWhen[kMaxBaked]{};

std::size_t findCandidate(ID3D12Resource* resource)
{
    const std::size_t n = std::min(g_candCount.load(std::memory_order_acquire), kMaxCandidates);
    for (std::size_t i = 0; i < n; ++i) {
        if (g_cands[i].res.load(std::memory_order_acquire) == resource) {
            return i;
        }
    }
    return kMaxCandidates;
}

bool alreadyBaked(ID3D12Resource* resource)
{
    const std::size_t n = g_bakedCount.load(std::memory_order_acquire);
    const unsigned long long now = GetTickCount64();
    for (std::size_t i = 0; i < n && i < kMaxBaked; ++i) {
        if (g_bakedAt[i].load(std::memory_order_relaxed) == resource) {
            const unsigned long long was = g_bakedWhen[i].load(std::memory_order_relaxed);
            return was != 0 && now - was < kRebakeMs;
        }
    }
    return false;
}

void noteBakedAt(ID3D12Resource* resource, unsigned size, unsigned height)
{
    const unsigned long long now = GetTickCount64();
    const std::size_t n = g_bakedCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n && i < kMaxBaked; ++i) {
        if (g_bakedAt[i].load(std::memory_order_relaxed) == resource) {
            g_bakedWhen[i].store(now, std::memory_order_relaxed);
            return;
        }
    }
    const std::size_t at = g_bakedCount.load(std::memory_order_relaxed);
    if (at >= kMaxBaked) {
        return;
    }
    g_bakedSize[at].store(size, std::memory_order_relaxed);
    g_bakedHeight[at].store(height, std::memory_order_relaxed);
    g_bakedWhen[at].store(now, std::memory_order_relaxed);
    g_bakedAt[at].store(resource, std::memory_order_relaxed);
    g_bakedCount.store(at + 1, std::memory_order_release);
}
std::atomic<bool> g_baking{false};
std::atomic<std::size_t> g_reportedCount{0};
std::atomic<bool> g_teardown{false};
std::atomic<unsigned> g_bakedAtlas{0};
std::atomic<int> g_bakedGridCol{63};
std::atomic<int> g_bakedGridRow{63};
std::atomic<int> g_bakedGridCols{64};
std::atomic<int> g_bakedGridRows{64};

ID3D12Resource* g_upload = nullptr;

constexpr unsigned kGridCols = 64;

unsigned tilePixelsOf(unsigned width)
{
    return (width % kGridCols == 0) ? (width / kGridCols) : 0;
}

unsigned gridRowsOf(unsigned width, unsigned height)
{
    const unsigned tile = tilePixelsOf(width);
    if (tile == 0 || height % tile != 0) {
        return 0;
    }
    return height / tile;
}

bool looksLikeAtlas(const D3D12_RESOURCE_DESC& desc)
{
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return false;
    }
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        return false;
    }
    const auto width = static_cast<unsigned>(desc.Width);
    const auto height = static_cast<unsigned>(desc.Height);
    if (width < kMinAtlas || width > kMaxAtlas) {
        return false;
    }
    if (height < kMinSide || height > kMaxAtlas) {
        return false;
    }
    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) {
        return false;
    }
    const unsigned tile = tilePixelsOf(width);
    if (tile < 4) {
        return false;
    }
    return gridRowsOf(width, height) >= 4;
}

constexpr unsigned char kFaceAlpha = 90;

void buildLevel0(std::vector<unsigned char>& out, unsigned size)
{
    out.assign(static_cast<std::size_t>(size) * size * 4, 0);
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            const bool edge = (x == 0 || y == 0 || x == size - 1 || y == size - 1);
            unsigned char* const p = out.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = edge ? 255 : kFaceAlpha;
        }
    }
}

void shrinkHalf(const std::vector<unsigned char>& src, unsigned size,
                std::vector<unsigned char>& out)
{
    const unsigned half = size / 2;
    out.assign(static_cast<std::size_t>(half) * half * 4, 0);
    for (unsigned y = 0; y < half; ++y) {
        for (unsigned x = 0; x < half; ++x) {
            for (int k = 0; k < 4; ++k) {
                unsigned sum = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const std::size_t at =
                            ((static_cast<std::size_t>(y) * 2 + dy) * size + (x * 2 + dx)) * 4
                            + static_cast<std::size_t>(k);
                        sum += src[at];
                    }
                }
                out[(static_cast<std::size_t>(y) * half + x) * 4 + static_cast<std::size_t>(k)] =
                    static_cast<unsigned char>((sum + 2) / 4);
            }
        }
    }
}

struct UploadSlot {
    unsigned size = 0;
    unsigned mips = 0;
    ID3D12Resource* upload = nullptr;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
};
constexpr std::size_t kUploadSlots = 4;
UploadSlot g_uploadSlots[kUploadSlots];
std::size_t g_uploadCount = 0;

bool prepareUpload(ID3D12Device* device, unsigned atlasSize, unsigned mips,
                   std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& layouts)
{
    for (std::size_t i = 0; i < g_uploadCount && i < kUploadSlots; ++i) {
        if (g_uploadSlots[i].size == atlasSize && g_uploadSlots[i].mips == mips
            && g_uploadSlots[i].upload != nullptr) {
            g_upload = g_uploadSlots[i].upload;
            layouts = g_uploadSlots[i].layouts;
            return true;
        }
    }
    const unsigned tile = atlasSize / 64;

    D3D12_RESOURCE_DESC tileDesc{};
    tileDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tileDesc.Width = tile;
    tileDesc.Height = tile;
    tileDesc.DepthOrArraySize = 1;
    tileDesc.MipLevels = static_cast<UINT16>(mips);
    tileDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tileDesc.SampleDesc.Count = 1;
    tileDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    layouts.assign(mips, D3D12_PLACED_SUBRESOURCE_FOOTPRINT{});
    std::vector<UINT> rows(mips, 0);
    std::vector<UINT64> rowBytes(mips, 0);
    UINT64 total = 0;
    device->GetCopyableFootprints(&tileDesc, 0, mips, 0, layouts.data(), rows.data(),
                                  rowBytes.data(), &total);
    if (total == 0) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&g_upload)))
        || g_upload == nullptr) {
        log().warn(L"DiffAtlas: could not create the upload buffer");
        return false;
    }

    unsigned char* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(g_upload->Map(0, &none, reinterpret_cast<void**>(&mapped))) || mapped == nullptr) {
        log().warn(L"DiffAtlas: could not copy into the upload buffer");
        g_upload->Release();
        g_upload = nullptr;
        return false;
    }

    std::vector<unsigned char> level;
    std::vector<unsigned char> next;
    buildLevel0(level, tile);
    unsigned size = tile;
    for (unsigned m = 0; m < mips; ++m) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = layouts[m];
        const unsigned rowsThis = fp.Footprint.Height;
        for (unsigned y = 0; y < rowsThis; ++y) {
            unsigned char* const dst = mapped + fp.Offset
                                       + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
            const unsigned char* const src =
                level.data() + static_cast<std::size_t>(y) * size * 4;
            std::memcpy(dst, src, static_cast<std::size_t>(size) * 4);
        }
        if (m + 1 < mips && size > 1) {
            shrinkHalf(level, size, next);
            level.swap(next);
            size /= 2;
        }
    }
    g_upload->Unmap(0, nullptr);
    if (g_uploadCount < kUploadSlots) {
        g_uploadSlots[g_uploadCount].size = atlasSize;
        g_uploadSlots[g_uploadCount].mips = mips;
        g_uploadSlots[g_uploadCount].upload = g_upload;
        g_uploadSlots[g_uploadCount].layouts = layouts;
        ++g_uploadCount;
    }
    return true;
}

std::mutex g_linkMutex;
std::unordered_map<const void*, ID3D12Resource*> g_link;
std::atomic<unsigned> g_linkCount{0};
std::atomic<unsigned> g_linkTries{0};

void linkTextureAtCreate(ID3D12Resource* res, const D3D12_RESOURCE_DESC& d)
{
    if (res == nullptr || d.Width > 2048 || d.Height > 2048
        || g_linkTries.fetch_add(1, std::memory_order_relaxed) >= 4096) {
        return;
    }
    const auto want0 = static_cast<std::uint32_t>(d.Width);
    const auto want1 = static_cast<std::uint32_t>(d.Height);
    const auto wantFmt = static_cast<std::uint32_t>(d.Format);
    const auto stackBase = static_cast<std::uintptr_t>(__readgsqword(0x08));
    const auto stackLimit = static_cast<std::uintptr_t>(__readgsqword(0x10));
    volatile int here = 0;
    auto at = reinterpret_cast<std::uintptr_t>(&here) & ~static_cast<std::uintptr_t>(7);
    if (stackBase <= stackLimit || at < stackLimit || at >= stackBase) {
        return;
    }
    const std::uintptr_t stop = std::min<std::uintptr_t>(stackBase, at + 0x2000);
    for (; at + 8 <= stop; at += 8) {
        void* v = nullptr;
        std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof(v));
        const auto raw = reinterpret_cast<std::uintptr_t>(v);
        if (raw < 0x10000 || (raw & 7) != 0 || raw >= 0x7fff'ffff'ffffULL) {
            continue;
        }
        if (!memory::isReadable(v, 0x24)) {
            continue;
        }
        const auto* const obj = static_cast<const unsigned char*>(v);
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        std::uint32_t fmt = 0;
        std::memcpy(&w, obj + 0x18, sizeof(w));
        std::memcpy(&h, obj + 0x1c, sizeof(h));
        std::memcpy(&fmt, obj + 0x20, sizeof(fmt));
        if (w != want0 || h != want1 || fmt != wantFmt) {
            continue;
        }
        void* vtable = nullptr;
        std::memcpy(&vtable, obj, sizeof(vtable));
        if (!memory::inGameModule(vtable)) {
            continue;
        }
        const std::lock_guard<std::mutex> lock{g_linkMutex};
        if (g_link.size() < 4096 && g_link.emplace(v, res).second) {
            g_linkCount.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
}

struct NotedTexture {
    ID3D12Resource* res = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    unsigned format = 0;
    unsigned mips = 1;
    unsigned state = 0;
};

constexpr std::size_t kNotedSlots = 8192;

std::mutex g_notedMutex;
std::vector<NotedTexture> g_noted;
std::atomic<unsigned> g_notedCount{0};

std::size_t notedSlotOf(const void* res)
{
    const auto raw = reinterpret_cast<std::uintptr_t>(res) >> 4;
    return static_cast<std::size_t>(raw) & (kNotedSlots - 1);
}

const NotedTexture* findNoted(const void* res)
{
    if (res == nullptr || g_noted.empty()) {
        return nullptr;
    }
    std::size_t at = notedSlotOf(res);
    for (std::size_t step = 0; step < 64; ++step) {
        const NotedTexture& one = g_noted[(at + step) & (kNotedSlots - 1)];
        if (one.res == nullptr) {
            return nullptr;
        }
        if (one.res == res) {
            return &one;
        }
    }
    return nullptr;
}

struct EntityImage {
    std::string path;
    std::vector<std::uint8_t> rgba;
    unsigned width = 0;
    unsigned height = 0;
    int col = -1;
    int row = -1;
    int tw = 0;
    int th = 0;
    bool baked = false;
};

constexpr std::size_t kMaxEntityImages = 48;

std::mutex g_entityMutex;
std::vector<EntityImage> g_entityImages;
std::vector<std::vector<char>> g_blank;
std::vector<std::vector<char>> g_taken;
int g_gridRows = 0;
std::atomic<bool> g_blankReady{false};
std::atomic<unsigned> g_entityBaked{0};
std::atomic<unsigned> g_entityNoRoom{0};
std::vector<ComPtr<ID3D12Resource>> g_uploadKeep;

bool reserveRegion(int tw, int th, int& outCol, int& outRow)
{
    if (tw <= 0 || th <= 0 || g_blank.empty()) {
        return false;
    }
    const int rows = static_cast<int>(g_blank.size());
    const int cols = static_cast<int>(kGridCols);
    for (int r = 0; r + th <= rows; ++r) {
        for (int c = 0; c + tw <= cols; ++c) {
            bool ok = true;
            for (int dr = 0; dr < th && ok; ++dr) {
                for (int dc = 0; dc < tw; ++dc) {
                    if (g_blank[r + dr][c + dc] == 0 || g_taken[r + dr][c + dc] != 0) {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) {
                continue;
            }
            for (int dr = 0; dr < th; ++dr) {
                for (int dc = 0; dc < tw; ++dc) {
                    g_taken[r + dr][c + dc] = 1;
                }
            }
            outCol = c;
            outRow = r;
            return true;
        }
    }
    return false;
}

ComPtr<ID3D12Resource> g_scanBuf;
UINT64 g_scanRowPitch = 0;
unsigned g_scanSize = 0;
unsigned g_scanRows = 0;
int g_scanWait = -1;

bool scanOn()
{
    static const bool on = [] {
        std::error_code ec;
        return std::filesystem::exists(paths::dataDir() / L"diag-atlas-scan.txt", ec);
    }();
    return on;
}

void scanCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* atlas,
              const D3D12_RESOURCE_DESC& desc, unsigned state)
{
    bool wantForEntity = false;
    {
        const std::lock_guard<std::mutex> lock{g_entityMutex};
        for (const EntityImage& one : g_entityImages) {
            if (!one.baked) {
                wantForEntity = true;
                break;
            }
        }
    }
    if ((!scanOn() && !wantForEntity) || g_scanBuf != nullptr || list == nullptr
        || atlas == nullptr) {
        return;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(atlas->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
        return;
    }
    const auto width = static_cast<unsigned>(desc.Width);
    const auto height = static_cast<unsigned>(desc.Height);
    const UINT64 pitch = (static_cast<UINT64>(width) * 4 + 255) & ~static_cast<UINT64>(255);
    const UINT64 bytes = pitch * height;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = bytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&g_scanBuf)))
        || g_scanBuf == nullptr) {
        log().warn(L"DiffAtlas: could not create the readback buffer");
        return;
    }
    const bool needBarrier = (state != kCommon && state != kCopySource);
    D3D12_RESOURCE_BARRIER toSrc{};
    toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrc.Transition.pResource = atlas;
    toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrc.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(state);
    toSrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (needBarrier) {
        list->ResourceBarrier(1, &toSrc);
    }
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = atlas;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_scanBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = desc.Format;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (needBarrier) {
        D3D12_RESOURCE_BARRIER back = toSrc;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(state);
        list->ResourceBarrier(1, &back);
    }
    g_scanRowPitch = pitch;
    g_scanSize = width;
    g_scanRows = height;
    g_scanWait = 600;
}

void scanRead()
{
    if (g_scanBuf == nullptr || g_scanWait < 0) {
        return;
    }
    if (--g_scanWait >= 0) {
        return;
    }
    void* raw = nullptr;
    const D3D12_RANGE all{0, static_cast<SIZE_T>(g_scanRowPitch * g_scanRows)};
    if (FAILED(g_scanBuf->Map(0, &all, &raw)) || raw == nullptr) {
        log().warn(L"DiffAtlas: could not map the readback buffer");
        g_scanBuf.Reset();
        return;
    }
    const auto* const base = static_cast<const unsigned char*>(raw);
    const unsigned tile = g_scanSize / kGridCols;
    const unsigned rows = g_scanRows / (tile != 0 ? tile : 1);
    std::string empty;
    unsigned emptyCount = 0;
    for (unsigned r = 0; r < rows && r < 64; ++r) {
        for (unsigned c = 0; c < kGridCols; ++c) {
            bool blank = true;
            for (unsigned y = 0; y < tile && blank; ++y) {
                const unsigned char* const line =
                    base + (static_cast<UINT64>(r) * tile + y) * g_scanRowPitch;
                for (unsigned x = 0; x < tile; ++x) {
                    if (line[(static_cast<UINT64>(c) * tile + x) * 4 + 3] != 0) {
                        blank = false;
                        break;
                    }
                }
            }
            if (blank) {
                ++emptyCount;
                if (empty.size() < 900) {
                    empty += std::to_string(c) + "," + std::to_string(r) + " ";
                }
            }
        }
    }
    const D3D12_RANGE none{0, 0};
    g_scanBuf->Unmap(0, &none);
    std::vector<std::vector<bool>> blank(rows, std::vector<bool>(kGridCols, false));
    {
        std::size_t at = 0;
        for (unsigned r = 0; r < rows; ++r) {
            for (unsigned c = 0; c < kGridCols; ++c) {
                bool b = true;
                for (unsigned y = 0; y < tile && b; ++y) {
                    const unsigned char* const line =
                        base + (static_cast<UINT64>(r) * tile + y) * g_scanRowPitch;
                    for (unsigned x = 0; x < tile; ++x) {
                        if (line[(static_cast<UINT64>(c) * tile + x) * 4 + 3] != 0) {
                            b = false;
                            break;
                        }
                    }
                }
                blank[r][c] = b;
            }
        }
        (void)at;
    }
    {
        const std::lock_guard<std::mutex> lock{g_entityMutex};
        g_blank.assign(rows, std::vector<char>(kGridCols, 0));
        g_taken.assign(rows, std::vector<char>(kGridCols, 0));
        for (unsigned r = 0; r < rows; ++r) {
            for (unsigned c = 0; c < kGridCols; ++c) {
                g_blank[r][c] = blank[r][c] ? 1 : 0;
            }
        }
        g_gridRows = static_cast<int>(rows);
    }
    g_blankReady.store(true, std::memory_order_release);
    std::string quads;
    unsigned quadCount = 0;
    std::vector<std::vector<bool>> taken(rows, std::vector<bool>(kGridCols, false));
    for (unsigned r = 0; r + 4 <= rows; ++r) {
        for (unsigned c = 0; c + 4 <= kGridCols; ++c) {
            bool all = true;
            for (unsigned dr = 0; dr < 4 && all; ++dr) {
                for (unsigned dc = 0; dc < 4; ++dc) {
                    if (!blank[r + dr][c + dc] || taken[r + dr][c + dc]) {
                        all = false;
                        break;
                    }
                }
            }
            if (!all) {
                continue;
            }
            for (unsigned dr = 0; dr < 4; ++dr) {
                for (unsigned dc = 0; dc < 4; ++dc) {
                    taken[r + dr][c + dc] = true;
                }
            }
            ++quadCount;
            if (quads.size() < 600) {
                quads += "(" + std::to_string(c) + "," + std::to_string(r) + ") ";
            }
        }
    }
    if (scanOn()) {
    }
    g_scanBuf.Reset();
    g_scanWait = -1;
}

bool bakeOneEntityImage(ID3D12GraphicsCommandList* list, ID3D12Resource* atlas,
                        const D3D12_RESOURCE_DESC& desc, unsigned state, EntityImage& image)
{
    ComPtr<ID3D12Device> device;
    if (FAILED(atlas->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
        return false;
    }
    const auto atlasSize = static_cast<unsigned>(desc.Width);
    const unsigned mips = desc.MipLevels != 0 ? desc.MipLevels : 1;
    const unsigned tile = atlasSize / kGridCols;
    if (tile == 0 || image.width == 0 || image.height == 0) {
        return false;
    }

    D3D12_RESOURCE_DESC imgDesc{};
    imgDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    imgDesc.Width = image.width;
    imgDesc.Height = image.height;
    imgDesc.DepthOrArraySize = 1;
    imgDesc.MipLevels = static_cast<UINT16>(mips);
    imgDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    imgDesc.SampleDesc.Count = 1;
    imgDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(mips);
    std::vector<UINT> rowCounts(mips, 0);
    std::vector<UINT64> rowBytes(mips, 0);
    UINT64 total = 0;
    device->GetCopyableFootprints(&imgDesc, 0, mips, 0, layouts.data(), rowCounts.data(),
                                  rowBytes.data(), &total);
    if (total == 0) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload)))
        || upload == nullptr) {
        return false;
    }
    unsigned char* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mapped))) || mapped == nullptr) {
        return false;
    }

    std::vector<std::uint8_t> level = image.rgba;
    unsigned lw = image.width;
    unsigned lh = image.height;
    unsigned wrote = 0;
    for (unsigned m = 0; m < mips; ++m) {
        if (lw == 0 || lh == 0) {
            break;
        }
        const auto& fp = layouts[m].Footprint;
        for (unsigned y = 0; y < lh && y < fp.Height; ++y) {
            std::memcpy(mapped + layouts[m].Offset + static_cast<UINT64>(y) * fp.RowPitch,
                        level.data() + static_cast<std::size_t>(y) * lw * 4,
                        std::min<std::size_t>(static_cast<std::size_t>(lw) * 4, fp.RowPitch));
        }
        ++wrote;
        if (lw == 1 && lh == 1) {
            break;
        }
        const unsigned nw = lw > 1 ? lw / 2 : 1;
        const unsigned nh = lh > 1 ? lh / 2 : 1;
        std::vector<std::uint8_t> next(static_cast<std::size_t>(nw) * nh * 4, 0);
        for (unsigned y = 0; y < nh; ++y) {
            for (unsigned x = 0; x < nw; ++x) {
                for (unsigned ch = 0; ch < 4; ++ch) {
                    unsigned sum = 0;
                    for (unsigned dy = 0; dy < 2; ++dy) {
                        for (unsigned dx = 0; dx < 2; ++dx) {
                            const unsigned sx = std::min(lw - 1, x * 2 + dx);
                            const unsigned sy = std::min(lh - 1, y * 2 + dy);
                            sum += level[(static_cast<std::size_t>(sy) * lw + sx) * 4 + ch];
                        }
                    }
                    next[(static_cast<std::size_t>(y) * nw + x) * 4 + ch] =
                        static_cast<std::uint8_t>(sum / 4);
                }
            }
        }
        level.swap(next);
        lw = nw;
        lh = nh;
    }
    upload->Unmap(0, nullptr);

    const bool needBarrier = (state != kCommon && state != kCopyDest);
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = atlas;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(state);
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (needBarrier) {
        list->ResourceBarrier(1, &toCopy);
    }
    for (unsigned m = 0; m < wrote; ++m) {
        const unsigned x = (static_cast<unsigned>(image.col) * tile) >> m;
        const unsigned y = (static_cast<unsigned>(image.row) * tile) >> m;
        const auto& fp = layouts[m].Footprint;
        if (fp.Width == 0 || fp.Height == 0 || x + fp.Width > (atlasSize >> m)
            || y + fp.Height > (static_cast<unsigned>(desc.Height) >> m)) {
            break;
        }
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = atlas;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layouts[m];
        list->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
    }
    if (needBarrier) {
        D3D12_RESOURCE_BARRIER back = toCopy;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        back.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(state);
        list->ResourceBarrier(1, &back);
    }
    g_uploadKeep.push_back(upload);
    return true;
}

void bakeEntityImages(ID3D12GraphicsCommandList* list, ID3D12Resource* atlas,
                      const D3D12_RESOURCE_DESC& desc, unsigned state)
{
    if (!g_blankReady.load(std::memory_order_acquire) || list == nullptr || atlas == nullptr) {
        return;
    }
    const unsigned tile = static_cast<unsigned>(desc.Width) / kGridCols;
    if (tile == 0) {
        return;
    }
    std::string path;
    unsigned width = 0;
    unsigned height = 0;
    int col = 0;
    int row = 0;
    int tw = 0;
    int th = 0;
    bool ok = false;
    {
        const std::lock_guard<std::mutex> lock{g_entityMutex};
        for (EntityImage& one : g_entityImages) {
            if (one.baked || one.rgba.empty()) {
                continue;
            }
            one.baked = true;
            tw = static_cast<int>((one.width + tile - 1) / tile);
            th = static_cast<int>((one.height + tile - 1) / tile);
            if (!reserveRegion(tw, th, col, row)) {
                g_entityNoRoom.fetch_add(1, std::memory_order_relaxed);
                log().warn(L"DiffAtlas: no free slot for {} ({}x{} tiles)",
                           std::wstring(one.path.begin(), one.path.end()),
                           tw,
                           th);
                continue;
            }
            one.col = col;
            one.row = row;
            one.tw = tw;
            one.th = th;
            path = one.path;
            width = one.width;
            height = one.height;
            ok = bakeOneEntityImage(list, atlas, desc, state, one);
            if (!ok) {
                one.col = -1;
                one.row = -1;
            }
            break;
        }
    }
    if (path.empty()) {
        return;
    }
    if (ok) {
        g_entityBaked.fetch_add(1, std::memory_order_relaxed);
        log().success(L"DiffAtlas: baked {} - {}x{} -> tile ({},{}) {}x{}",
                      std::wstring(path.begin(), path.end()),
                      width,
                      height,
                      col,
                      row,
                      tw,
                      th);
    } else {
        log().warn(L"DiffAtlas: could not bake {}", std::wstring(path.begin(), path.end()));
    }
}

ComPtr<ID3D12Resource> g_entityAtlas;
unsigned g_entityAtlasState = 0;

void bakeEntityPending(ID3D12GraphicsCommandList* list)
{
    ID3D12Resource* const atlas = g_entityAtlas.Get();
    if (atlas == nullptr || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!g_baking.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return;
    }
    const D3D12_RESOURCE_DESC d = atlas->GetDesc();
    bakeEntityImages(list, atlas, d, g_entityAtlasState);
    g_baking.store(false, std::memory_order_release);
}

void bake(ID3D12GraphicsCommandList* list, ID3D12Resource* atlas,
          const D3D12_RESOURCE_DESC& desc, unsigned state)
{
    ComPtr<ID3D12Device> device;
    if (FAILED(atlas->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
        return;
    }
    const auto atlasSize = static_cast<unsigned>(desc.Width);
    const unsigned mips = desc.MipLevels != 0 ? desc.MipLevels : 1;
    const unsigned tile = atlasSize / 64;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
    if (!prepareUpload(device.Get(), atlasSize, mips, layouts)) {
        return;
    }

    const bool needBarrier = (state != kCommon && state != kCopyDest);
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = atlas;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(state);
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (needBarrier) {
        list->ResourceBarrier(1, &toCopy);
    }

    for (unsigned m = 0; m < mips; ++m) {
        const unsigned shift = m;
        const unsigned tileAt = tile >> shift;
        if (tileAt == 0) {
            break;
        }
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = atlas;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layouts[m];

        const unsigned rows = gridRowsOf(atlasSize, static_cast<unsigned>(desc.Height));
        const int wantCol = std::min(bakeCol(), static_cast<int>(kGridCols) - 1);
        const int wantRow = std::min(bakeRow(),
                                     static_cast<int>(rows > 0 ? rows : 1) - 1);
        const UINT x = static_cast<UINT>(wantCol) * tileAt;
        const UINT y = static_cast<UINT>(wantRow) * tileAt;
        list->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
    }

    if (needBarrier) {
        D3D12_RESOURCE_BARRIER back = toCopy;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        back.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(state);
        list->ResourceBarrier(1, &back);
    }

    scanCopy(list, atlas, desc, state);

    if (g_entityAtlas.Get() != atlas) {
        const std::lock_guard<std::mutex> lock{g_entityMutex};
        for (EntityImage& one : g_entityImages) {
            one.baked = false;
            one.col = -1;
            one.row = -1;
        }
        g_blank.clear();
        g_taken.clear();
        g_gridRows = 0;
        g_blankReady.store(false, std::memory_order_release);
        g_scanBuf.Reset();
        g_scanWait = -1;
    }
    g_entityAtlas = atlas;
    g_entityAtlasState = state;

    noteBakedAt(atlas, atlasSize, static_cast<unsigned>(desc.Height));
    g_bakedAtlas.store(atlasSize, std::memory_order_relaxed);
    {
        const unsigned rows = gridRowsOf(atlasSize, static_cast<unsigned>(desc.Height));
        g_bakedGridCols.store(static_cast<int>(kGridCols), std::memory_order_relaxed);
        g_bakedGridRows.store(static_cast<int>(rows > 0 ? rows : kGridCols),
                              std::memory_order_relaxed);
        g_bakedGridCol.store(std::min(bakeCol(), static_cast<int>(kGridCols) - 1),
                             std::memory_order_relaxed);
        g_bakedGridRow.store(std::min(bakeRow(),
                                      static_cast<int>(rows > 0 ? rows : kGridCols) - 1),
                             std::memory_order_relaxed);
    }
    g_baked.store(true, std::memory_order_release);
}

}

void noteBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                 unsigned stateBefore, unsigned stateAfter, unsigned subresource)
{
    if (list == nullptr || resource == nullptr || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    if (subresource == kAllSubresources) {
        if (const std::size_t at = findCandidate(resource); at < kMaxCandidates) {
            g_cands[at].state.store(stateAfter, std::memory_order_relaxed);
        }
    }
    if (g_bakedCount.load(std::memory_order_acquire) >= kMaxBaked) {
        return;
    }
    if (alreadyBaked(resource)) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (!looksLikeAtlas(desc)) {
        tallyShape(desc);
        return;
    }
    if (!isShaderReadable(stateAfter)) {
    }
    if (subresource != kAllSubresources) {
        return;
    }
    bool expected = false;
    if (!g_baking.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return;
    }
    bake(list, resource, desc, stateAfter);
    g_baking.store(false, std::memory_order_release);
}

void noteCreatedResource(ID3D12Resource* resource, unsigned initialState, const void* desc)
{
    if (resource == nullptr || desc == nullptr || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    const auto& d = *static_cast<const D3D12_RESOURCE_DESC*>(desc);
    if (!looksLikeAtlas(d)) {
        return;
    }
    const std::size_t at = g_candCount.load(std::memory_order_acquire);
    if (at >= kMaxCandidates) {
        if (g_candFullTold.exchange(1, std::memory_order_relaxed) == 0) {
            log().warn(L"DiffAtlas: the list of atlas-like textures hit its limit of {}",
                       kMaxCandidates);
        }
        return;
    }
    const std::size_t slot = g_candCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kMaxCandidates) {
        return;
    }
    g_cands[slot].state.store(initialState, std::memory_order_relaxed);
    g_cands[slot].size.store(static_cast<unsigned>(d.Width), std::memory_order_relaxed);
    g_cands[slot].mips.store(d.MipLevels != 0 ? d.MipLevels : 1, std::memory_order_relaxed);
    g_cands[slot].done.store(false, std::memory_order_relaxed);
    g_cands[slot].res.store(resource, std::memory_order_release);
}

void bakePending(ID3D12GraphicsCommandList* list)
{
    if (list == nullptr || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    scanRead();
    bakeEntityPending(list);
    if (g_bakedCount.load(std::memory_order_acquire) >= kMaxBaked) {
        return;
    }
    const std::size_t n = std::min(g_candCount.load(std::memory_order_acquire), kMaxCandidates);
    for (std::size_t i = 0; i < n; ++i) {
        if (g_cands[i].done.load(std::memory_order_acquire)) {
            continue;
        }
        ID3D12Resource* const res = g_cands[i].res.load(std::memory_order_acquire);
        if (res == nullptr || alreadyBaked(res)) {
            g_cands[i].done.store(true, std::memory_order_release);
            continue;
        }
        bool expected = false;
        if (!g_baking.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            return;
        }
        g_cands[i].done.store(true, std::memory_order_release);
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        const unsigned state = g_cands[i].state.load(std::memory_order_relaxed);
        bake(list, res, d, state);
        g_baking.store(false, std::memory_order_release);
        g_bakedFromCreate.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void noteAnyTexture(ID3D12Resource* resource, const void* desc, unsigned initialState)
{
    if (resource == nullptr || desc == nullptr || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    const auto& d = *static_cast<const D3D12_RESOURCE_DESC*>(desc);
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width == 0 || d.Height == 0
        || d.Width > 8192 || d.Height > 8192) {
        return;
    }
    linkTextureAtCreate(resource, d);
    const std::lock_guard<std::mutex> lock{g_notedMutex};
    if (g_noted.empty()) {
        g_noted.assign(kNotedSlots, NotedTexture{});
    }
    std::size_t at = notedSlotOf(resource);
    for (std::size_t step = 0; step < 64; ++step) {
        NotedTexture& one = g_noted[(at + step) & (kNotedSlots - 1)];
        if (one.res == resource) {
            one.state = initialState;
            return;
        }
        if (one.res != nullptr) {
            continue;
        }
        one.res = resource;
        one.width = static_cast<unsigned>(d.Width);
        one.height = static_cast<unsigned>(d.Height);
        one.format = static_cast<unsigned>(d.Format);
        one.mips = d.MipLevels != 0 ? d.MipLevels : 1;
        one.state = initialState;
        g_notedCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) == 0) {
        log().warn(L"DiffAtlas: the texture registry is full ({} kept)",
                   g_notedCount.load(std::memory_order_relaxed));
    }
}

unsigned notedTextureCount()
{
    return g_notedCount.load(std::memory_order_relaxed);
}

ID3D12Resource* findTextureResource(const void* object, unsigned span, unsigned& width,
                                    unsigned& height, unsigned& format, unsigned& mips)
{
    width = 0;
    height = 0;
    format = 0;
    mips = 0;
    if (object == nullptr || span == 0 || span > 0x1000) {
        return nullptr;
    }
    std::uint32_t wantW = 0;
    std::uint32_t wantH = 0;
    if (memory::isReadable(object, 0x20)) {
        const auto* const self = static_cast<const unsigned char*>(object);
        std::memcpy(&wantW, self + 0x18, sizeof(wantW));
        std::memcpy(&wantH, self + 0x1c, sizeof(wantH));
    }
    {
        const std::lock_guard<std::mutex> lock{g_linkMutex};
        const auto it = g_link.find(object);
        if (it != g_link.end()) {
            const std::lock_guard<std::mutex> lock2{g_notedMutex};
            const NotedTexture* const hit = findNoted(it->second);
            if (hit != nullptr) {
                width = hit->width;
                height = hit->height;
                format = hit->format;
                mips = hit->mips;
                return hit->res;
            }
        }
    }
    const std::lock_guard<std::mutex> lock{g_notedMutex};
    if (g_noted.empty()) {
        return nullptr;
    }
    constexpr std::size_t kMaxNodes = 512;
    constexpr int kMaxDepth = 5;
    std::vector<const void*> seen;
    seen.reserve(kMaxNodes);
    std::vector<std::pair<const void*, int>> queue;
    queue.reserve(kMaxNodes);
    queue.emplace_back(object, 0);
    seen.push_back(object);
    for (std::size_t head = 0; head < queue.size() && seen.size() < kMaxNodes; ++head) {
        const auto* const base = static_cast<const unsigned char*>(queue[head].first);
        const int depth = queue[head].second;
        const unsigned look = depth == 0 ? span : 0x120;
        for (unsigned off = 0; off + 8 <= look; off += 8) {
            if (!memory::isReadable(base + off, 8)) {
                continue;
            }
            void* v = nullptr;
            std::memcpy(&v, base + off, sizeof(v));
            const auto raw = reinterpret_cast<std::uintptr_t>(v);
            if (raw < 0x10000 || (raw & 7) != 0 || raw >= 0x7fff'ffff'ffffULL) {
                continue;
            }
            const NotedTexture* const hit = findNoted(v);
            if (hit != nullptr && (wantW == 0 || (hit->width == wantW && hit->height == wantH))) {
                width = hit->width;
                height = hit->height;
                format = hit->format;
                mips = hit->mips;
                return hit->res;
            }
            if (depth + 1 >= kMaxDepth || seen.size() >= kMaxNodes
                || !memory::isReadable(v, 0x20)) {
                continue;
            }
            if (std::find(seen.begin(), seen.end(), v) != seen.end()) {
                continue;
            }
            seen.push_back(v);
            queue.emplace_back(v, depth + 1);
        }
    }
    return nullptr;
}

bool requestEntityImage(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock{g_entityMutex};
        if (g_entityImages.size() >= kMaxEntityImages) {
            return false;
        }
        for (const EntityImage& one : g_entityImages) {
            if (one.path == path) {
                return true;
            }
        }
    }
    const std::wstring file = pack::findFile(path);
    if (file.empty()) {
        log().warn(L"DiffAtlas: file not found: {}",
                   std::wstring(path.begin(), path.end()));
        return false;
    }
    EntityImage image;
    image.path = path;
    if (!pack::decodeImage(file, image.rgba, image.width, image.height)) {
        log().warn(L"DiffAtlas: could not decode {}", file);
        return false;
    }
    const std::lock_guard<std::mutex> lock{g_entityMutex};
    if (g_entityImages.size() >= kMaxEntityImages) {
        return false;
    }
    g_entityImages.push_back(std::move(image));
    return true;
}

bool entityTile(const std::string& path, int& col, int& row, int& tw, int& th, int& cols,
                int& rows)
{
    const std::lock_guard<std::mutex> lock{g_entityMutex};
    for (const EntityImage& one : g_entityImages) {
        if (one.path != path || one.col < 0 || one.tw <= 0) {
            continue;
        }
        col = one.col;
        row = one.row;
        tw = one.tw;
        th = one.th;
        cols = static_cast<int>(kGridCols);
        rows = g_gridRows;
        return rows > 0;
    }
    return false;
}

bool tile(int& col, int& row, int& cols, int& rows)
{
    if (!g_baked.load(std::memory_order_acquire)) {
        return false;
    }
    col = g_bakedGridCol.load(std::memory_order_relaxed);
    row = g_bakedGridRow.load(std::memory_order_relaxed);
    cols = g_bakedGridCols.load(std::memory_order_relaxed);
    rows = g_bakedGridRows.load(std::memory_order_relaxed);
    return cols > 0 && rows > 0;
}

void report()
{
    const std::size_t have = g_bakedCount.load(std::memory_order_acquire);
    std::size_t said = g_reportedCount.load(std::memory_order_relaxed);
    for (; said < have && said < kMaxBaked; ++said) {
        log().success(L"DiffAtlas: baked our own tile (#{}) - atlas {}x{} / grid ({},{}) (grid "
                      L"is {}x{})",
                      said + 1,
                      g_bakedSize[said].load(std::memory_order_relaxed),
                      g_bakedHeight[said].load(std::memory_order_relaxed),
                      g_bakedGridCol.load(std::memory_order_relaxed),
                      g_bakedGridRow.load(std::memory_order_relaxed),
                      g_bakedGridCols.load(std::memory_order_relaxed),
                      g_bakedGridRows.load(std::memory_order_relaxed));
    }
    g_reportedCount.store(said, std::memory_order_relaxed);
    if (have >= kMaxBaked && !g_bakedFullLogged.exchange(true, std::memory_order_relaxed)) {
        log().warn(L"DiffAtlas: baked the limit of {} atlases (no more will be baked; suspect "
                   L"this if the color boxes never show up)", kMaxBaked);
    }
}

void shutdown()
{
    g_entityAtlas.Reset();
    g_uploadKeep.clear();
    g_teardown.store(true, std::memory_order_release);
}

}
