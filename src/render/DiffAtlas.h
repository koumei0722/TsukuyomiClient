#pragma once

#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace tsukuyomi::atlas {

void noteBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                 unsigned stateBefore, unsigned stateAfter, unsigned subresource);

void noteCreatedResource(ID3D12Resource* resource, unsigned initialState,
                         const void* desc);

void bakePending(ID3D12GraphicsCommandList* list);

bool tile(int& col, int& row, int& cols, int& rows);

bool requestEntityImage(const std::string& path);

bool entityTile(const std::string& path, int& col, int& row, int& tw, int& th, int& cols,
                int& rows);

void noteAnyTexture(ID3D12Resource* resource, const void* desc, unsigned initialState);

ID3D12Resource* findTextureResource(const void* object, unsigned span, unsigned& width,
                                    unsigned& height, unsigned& format, unsigned& mips);

unsigned notedTextureCount();

void report();

void shutdown();

}
