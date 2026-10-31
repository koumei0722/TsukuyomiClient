#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tsukuyomi::bot::crypto {
std::vector<std::uint8_t> sha256(std::span<const std::uint8_t>);
std::vector<std::uint8_t> sha384(std::span<const std::uint8_t>);
std::vector<std::uint8_t> randomBytes(std::size_t);
std::string newUuid();
enum class Curve { P256, P384 };
class EcKey {
public:
    EcKey() = default;
    ~EcKey();
    EcKey(const EcKey&) = delete;
    EcKey& operator=(const EcKey&) = delete;
    EcKey(EcKey&&) noexcept;
    EcKey& operator=(EcKey&&) noexcept;
    static std::optional<EcKey> generate(Curve);
    static std::optional<EcKey> importPrivate(Curve, std::span<const std::uint8_t>);
    std::vector<std::uint8_t> exportPrivate() const;
    std::vector<std::uint8_t> publicX() const;
    std::vector<std::uint8_t> publicY() const;
    std::vector<std::uint8_t> spkiDer() const;
    std::vector<std::uint8_t> signDigest(std::span<const std::uint8_t>) const;
    std::optional<std::vector<std::uint8_t>> sharedSecret(std::span<const std::uint8_t> peerSpkiDer) const;
private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
    Curve curve_ = Curve::P256;
    std::vector<std::uint8_t> exportBlob(LPCWSTR type) const;
};
std::optional<std::vector<std::uint8_t>> ecdsaDerToRaw(std::span<const std::uint8_t> der, std::size_t half);
std::optional<std::vector<std::uint8_t>> protect(std::span<const std::uint8_t>);
std::optional<std::vector<std::uint8_t>> unprotect(std::span<const std::uint8_t>);
}
