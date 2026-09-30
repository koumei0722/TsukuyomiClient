#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12CommandList;
struct ID3D12CommandQueue;
struct ID3D12PipelineState;
struct D3D12_GRAPHICS_PIPELINE_STATE_DESC;
struct D3D12_RESOURCE_BARRIER;

namespace tsukuyomi::frametrace {

bool installHooks();

void notePso(ID3D12PipelineState* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc);
bool sawD3D12();
void onSetPso(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso);
void onOm(ID3D12GraphicsCommandList* list, unsigned numRt, std::uint64_t rtv0,
          std::uint64_t dsv);
void onDraw(ID3D12GraphicsCommandList* list, unsigned count, unsigned instances, bool indexed);
void onClearDsv(ID3D12GraphicsCommandList* list, std::uint64_t dsv, unsigned flags);
void onBarrier(ID3D12GraphicsCommandList* list, unsigned count,
               const D3D12_RESOURCE_BARRIER* barriers);
void onClose(ID3D12GraphicsCommandList* list);
void onViewport(ID3D12GraphicsCommandList* list, float width, float height, float minDepth,
                float maxDepth);

void onPresent();
void onExecute(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists);

void pump();

void shutdown();

}
