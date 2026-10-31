#include "Crypto.h"
#include <wincrypt.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace tsukuyomi::bot::crypto {
namespace {
bool fits(std::size_t size) { return size <= (std::numeric_limits<ULONG>::max)(); }
PUCHAR data(std::span<const std::uint8_t> bytes) { return const_cast<PUCHAR>(bytes.data()); }
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Algorithm(LPCWSTR name) { BCryptOpenAlgorithmProvider(&handle, name, nullptr, 0); }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
LPCWSTR algorithm(Curve curve)
{
    return curve == Curve::P256 ? BCRYPT_ECDSA_P256_ALGORITHM : BCRYPT_ECDSA_P384_ALGORITHM;
}
std::vector<std::uint8_t> hash(LPCWSTR name, std::span<const std::uint8_t> bytes, ULONG length)
{
    if (!fits(bytes.size())) return {};
    Algorithm alg(name);
    if (!alg.handle) return {};
    std::vector<std::uint8_t> result(length);
    if (BCryptHash(alg.handle, nullptr, 0, data(bytes), static_cast<ULONG>(bytes.size()),
        result.data(), length) < 0) return {};
    return result;
}
std::optional<std::vector<std::uint8_t>> dpapi(std::span<const std::uint8_t> bytes, bool encrypt)
{
    if (!fits(bytes.size())) return std::nullopt;
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), data(bytes)}, out{};
    const BOOL ok = encrypt
        ? CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
        : CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) return std::nullopt;
    std::vector<std::uint8_t> result;
    if (out.cbData) result.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}
}
std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> bytes) { return hash(BCRYPT_SHA256_ALGORITHM, bytes, 32); }
std::vector<std::uint8_t> sha384(std::span<const std::uint8_t> bytes) { return hash(BCRYPT_SHA384_ALGORITHM, bytes, 48); }
std::vector<std::uint8_t> randomBytes(std::size_t length)
{
    if (!fits(length)) return {};
    std::vector<std::uint8_t> bytes(length);
    if (length && BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    return bytes;
}
std::string newUuid()
{
    auto bytes = randomBytes(16);
    if (bytes.empty()) return {};
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return result;
}
EcKey::~EcKey() { if (key_) BCryptDestroyKey(key_); }
EcKey::EcKey(EcKey&& other) noexcept : key_(std::exchange(other.key_, nullptr)), curve_(other.curve_) {}
EcKey& EcKey::operator=(EcKey&& other) noexcept
{
    if (this != &other) {
        if (key_) BCryptDestroyKey(key_);
        key_ = std::exchange(other.key_, nullptr); curve_ = other.curve_;
    }
    return *this;
}
std::optional<EcKey> EcKey::generate(Curve curve)
{
    Algorithm alg(algorithm(curve));
    EcKey result; result.curve_ = curve;
    if (!alg.handle || BCryptGenerateKeyPair(alg.handle, &result.key_, curve == Curve::P256 ? 256 : 384, 0) < 0
        || BCryptFinalizeKeyPair(result.key_, 0) < 0) return std::nullopt;
    return result;
}
std::optional<EcKey> EcKey::importPrivate(Curve curve, std::span<const std::uint8_t> bytes)
{
    const ULONG width = curve == Curve::P256 ? 32 : 48;
    if (bytes.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 3 * width) return std::nullopt;
    BCRYPT_ECCKEY_BLOB header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const ULONG magic = curve == Curve::P256 ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PRIVATE_P384_MAGIC;
    if (header.dwMagic != magic || header.cbKey != width) return std::nullopt;
    Algorithm alg(algorithm(curve));
    EcKey result; result.curve_ = curve;
    if (!alg.handle || BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, &result.key_,
        data(bytes), static_cast<ULONG>(bytes.size()), 0) < 0) return std::nullopt;
    return result;
}
std::vector<std::uint8_t> EcKey::exportBlob(LPCWSTR type) const
{
    ULONG size = 0;
    if (!key_ || BCryptExportKey(key_, nullptr, type, nullptr, 0, &size, 0) < 0) return {};
    std::vector<std::uint8_t> bytes(size);
    if (BCryptExportKey(key_, nullptr, type, bytes.data(), size, &size, 0) < 0) return {};
    bytes.resize(size); return bytes;
}
std::vector<std::uint8_t> EcKey::exportPrivate() const { return exportBlob(BCRYPT_ECCPRIVATE_BLOB); }
std::vector<std::uint8_t> EcKey::publicX() const
{
    const auto bytes = exportBlob(BCRYPT_ECCPUBLIC_BLOB);
    const std::size_t width = curve_ == Curve::P256 ? 32 : 48;
    if (bytes.size() != 8 + 2 * width) return {};
    return {bytes.begin() + 8, bytes.begin() + 8 + width};
}
std::vector<std::uint8_t> EcKey::publicY() const
{
    const auto bytes = exportBlob(BCRYPT_ECCPUBLIC_BLOB);
    const std::size_t width = curve_ == Curve::P256 ? 32 : 48;
    if (bytes.size() != 8 + 2 * width) return {};
    return {bytes.begin() + 8 + width, bytes.end()};
}
std::vector<std::uint8_t> EcKey::spkiDer() const
{
    if (curve_ != Curve::P384) return {};
    const auto x = publicX(), y = publicY();
    if (x.size() != 48 || y.size() != 48) return {};
    std::vector<std::uint8_t> result{0x30,0x76,0x30,0x10,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,
        0x01,0x06,0x05,0x2b,0x81,0x04,0x00,0x22,0x03,0x62,0x00,0x04};
    result.insert(result.end(), x.begin(), x.end()); result.insert(result.end(), y.begin(), y.end());
    return result;
}
std::vector<std::uint8_t> EcKey::signDigest(std::span<const std::uint8_t> digest) const
{
    const ULONG width = curve_ == Curve::P256 ? 32 : 48;
    if (!key_ || digest.size() != width) return {};
    std::vector<std::uint8_t> signature(width * 2);
    ULONG written = 0;
    if (BCryptSignHash(key_, nullptr, data(digest), width, signature.data(), width * 2, &written, 0) < 0
        || written != width * 2) return {};
    return signature;
}
std::optional<std::vector<std::uint8_t>> EcKey::sharedSecret(std::span<const std::uint8_t> peerSpkiDer) const
{
    static constexpr std::uint8_t kHead[] = {0x30,0x76,0x30,0x10,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,
        0x01,0x06,0x05,0x2b,0x81,0x04,0x00,0x22,0x03,0x62,0x00,0x04};
    constexpr ULONG width = 48;
    if (!key_ || curve_ != Curve::P384 || peerSpkiDer.size() != sizeof kHead + 2 * width
        || std::memcmp(peerSpkiDer.data(), kHead, sizeof kHead) != 0) return std::nullopt;
    auto blob = exportPrivate();
    if (blob.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 3 * width) return std::nullopt;
    BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDH_PRIVATE_P384_MAGIC, width};
    std::memcpy(blob.data(), &header, sizeof header);
    std::vector<std::uint8_t> peer(sizeof(BCRYPT_ECCKEY_BLOB) + 2 * width);
    header = {BCRYPT_ECDH_PUBLIC_P384_MAGIC, width};
    std::memcpy(peer.data(), &header, sizeof header);
    std::memcpy(peer.data() + sizeof header, peerSpkiDer.data() + sizeof kHead, 2 * width);
    Algorithm alg(BCRYPT_ECDH_P384_ALGORITHM);
    BCRYPT_KEY_HANDLE mine = nullptr, theirs = nullptr;
    BCRYPT_SECRET_HANDLE secret = nullptr;
    std::optional<std::vector<std::uint8_t>> result;
    if (alg.handle
        && BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, &mine, blob.data(), static_cast<ULONG>(blob.size()), 0) >= 0
        && BCryptImportKeyPair(alg.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &theirs, peer.data(), static_cast<ULONG>(peer.size()), 0) >= 0
        && BCryptSecretAgreement(mine, theirs, &secret, 0) >= 0) {
        std::vector<std::uint8_t> raw(width);
        ULONG size = 0;
        if (BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, raw.data(), width, &size, 0) >= 0 && size == width) {
            std::reverse(raw.begin(), raw.end());
            result = std::move(raw);
        }
    }
    SecureZeroMemory(blob.data(), blob.size());
    if (secret) BCryptDestroySecret(secret);
    if (theirs) BCryptDestroyKey(theirs);
    if (mine) BCryptDestroyKey(mine);
    return result;
}
std::optional<std::vector<std::uint8_t>> ecdsaDerToRaw(std::span<const std::uint8_t> der, std::size_t half)
{
    std::size_t at = 0;
    const auto length = [&]() -> std::optional<std::size_t> {
        if (at >= der.size()) return std::nullopt;
        const std::uint8_t first = der[at++];
        if (first < 0x80) return first;
        if (first != 0x81 || at >= der.size() || der[at] < 0x80) return std::nullopt;
        return der[at++];
    };
    if (half == 0 || der.empty() || der[at++] != 0x30) return std::nullopt;
    const auto total = length();
    if (!total || at + *total != der.size()) return std::nullopt;
    std::vector<std::uint8_t> raw(half * 2, 0);
    for (std::size_t part = 0; part < 2; ++part) {
        if (at >= der.size() || der[at++] != 0x02) return std::nullopt;
        const auto size = length();
        if (!size || *size == 0 || at + *size > der.size()) return std::nullopt;
        auto begin = der.begin() + at, end = begin + *size;
        at += *size;
        if (*begin & 0x80) return std::nullopt;
        while (begin != end && *begin == 0) ++begin;
        const auto digits = static_cast<std::size_t>(end - begin);
        if (digits > half) return std::nullopt;
        std::copy(begin, end, raw.begin() + part * half + (half - digits));
    }
    if (at != der.size()) return std::nullopt;
    return raw;
}
std::optional<std::vector<std::uint8_t>> protect(std::span<const std::uint8_t> bytes) { return dpapi(bytes, true); }
std::optional<std::vector<std::uint8_t>> unprotect(std::span<const std::uint8_t> bytes) { return dpapi(bytes, false); }
}
