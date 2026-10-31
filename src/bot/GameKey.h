#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot::gamekey {

bool initialize();
void* secretTarget();
using SecretFn = void*(__fastcall*)(void* self, void* out, const void* peer);
void* onComputeSecret(SecretFn original, void* self, void* out, const void* peer);
void setOriginal(SecretFn original);
bool held();
std::optional<std::vector<std::uint8_t>> sharedSecret(const std::string& peerX5u);
void noteClientData(const void* loginInfo);
std::optional<std::vector<std::uint8_t>> sign(const std::string& x5u, const std::string& content);
void release();

}
