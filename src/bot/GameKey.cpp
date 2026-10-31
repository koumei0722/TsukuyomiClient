#include "GameKey.h"
#include "Base64.h"
#include "BotNet.h"
#include "Crypto.h"
#include "GameString.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <algorithm>
#include <format>
#include <mutex>

namespace tsukuyomi::bot::gamekey {
namespace {
using CtorFn = void*(__fastcall*)(void* self, const std::string* publicKey, int system);
using DtorFn = void*(__fastcall*)(void* self, unsigned flags);
using SignFn = void*(__fastcall*)(void* self, void* out, const void* key, const std::string* content, int hash, int encoding);

constexpr std::size_t kKeyManagerSize = 0x30;
constexpr std::ptrdiff_t kPublicKeyOffset = 0x08;
constexpr int kSystem = 5;
constexpr std::size_t kStringSize = 0x20;

CtorFn g_ctor = nullptr;
SecretFn g_secret = nullptr;
SecretFn g_original = nullptr;
const void* g_keyManagerVtable = nullptr;
std::mutex g_mutex;
void* g_held = nullptr;
const void* g_heldVtable = nullptr;
std::string g_heldKey;
int g_logged = 0, g_mismatchLogged = 0;

int callException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == 0xE06D7363u
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
std::wstring shortHash(const std::string& text)
{
    if (text.empty()) return L"(empty)";
    const auto digest = crypto::sha256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    std::wstring hex;
    for (std::size_t i = 0; i < 6 && i < digest.size(); ++i) hex += std::format(L"{:02x}", digest[i]);
    return hex;
}
const void* readVtable(const void* object)
{
    const void* table = nullptr;
    if (!memory::plausiblePointer(object) || !memory::copyGuarded(object, &table, sizeof table) || !memory::inGameModule(table)) return nullptr;
    return table;
}
std::optional<std::string> publicKeyOf(const void* manager)
{
    return gamestring::read(static_cast<const std::uint8_t*>(manager) + kPublicKeyOffset);
}
bool sameKey(const std::string& managerKey, const std::string& x5u)
{
    if (managerKey == x5u) return true;
    const auto der = base64::decode(x5u);
    if (!der || der->empty()) return false;
    if (managerKey.size() == der->size() && std::equal(der->begin(), der->end(), reinterpret_cast<const std::uint8_t*>(managerKey.data()))) return true;
    for (const auto& decoded : {base64::decode(managerKey), base64::decodeUrl(managerKey)})
        if (decoded && *decoded == *der) return true;
    return false;
}
std::wstring describeKey(const std::string& key)
{
    const bool text = std::all_of(key.begin(), key.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
    return std::format(L"{} bytes, {}{}", key.size(), text ? L"text" : L"binary", base64::decode(key) ? L", base64" : L"");
}
bool computeGuarded(SecretFn secret, void* manager, const std::string* peerKey, void* peer, void* out, bool* constructed, unsigned* code)
{
    __try {
        g_ctor(peer, peerKey, kSystem);
        *constructed = true;
        secret(manager, out, peer);
        return true;
    } __except (*code = GetExceptionCode(), callException(*code)) { return false; }
}
bool destroyGuarded(void* peer)
{
    __try {
        const auto fn = (*reinterpret_cast<DtorFn* const*>(peer))[0];
        if (!memory::inGameModule(reinterpret_cast<const void*>(fn))) return false;
        fn(peer, 0);
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
}

bool initialize()
{
    auto* const site = net::site(Site::ClientHandshakeSecretSite);
    if (!site) { log().warn(L"Bot: the game's key exchange was not found - switching to a bot on its own connection is unavailable on RakNet"); return false; }
    std::int32_t ctorRel = 0, secretRel = 0, vtableRel = 0;
    if (!memory::copyGuarded(site + 0x2c, &ctorRel, sizeof ctorRel) || !memory::copyGuarded(site + 0x40, &secretRel, sizeof secretRel)
        || !memory::copyGuarded(site + 0x48, &vtableRel, sizeof vtableRel)) {
        log().warn(L"Bot: cannot read the game's key exchange"); return false;
    }
    void* const ctor = site + 0x30 + ctorRel;
    void* const secret = site + 0x44 + secretRel;
    const void* const vtable = site + 0x4c + vtableRel;
    if (!memory::inGameModule(ctor) || !memory::inGameModule(secret) || !memory::inGameModule(vtable)) {
        log().warn(L"Bot: the game's key exchange points outside the game"); return false;
    }
    g_ctor = reinterpret_cast<CtorFn>(ctor);
    g_secret = reinterpret_cast<SecretFn>(secret);
    g_keyManagerVtable = vtable;
    return true;
}
void* secretTarget() { return reinterpret_cast<void*>(g_secret); }
void setOriginal(SecretFn original) { g_original = original; }

void* onComputeSecret(SecretFn original, void* self, void* out, const void* peer)
{
    void* const result = original(self, out, peer);
    const auto gameKey = net::gameLoginKey();
    if (gameKey.empty()) return result;
    {
        std::lock_guard lock(g_mutex);
        if (self == g_held && g_heldKey == gameKey) return result;
    }
    const void* const table = readVtable(self);
    const auto key = table ? publicKeyOf(self) : std::nullopt;
    std::lock_guard lock(g_mutex);
    if (!key || !sameKey(*key, gameKey)) {
        if (g_mismatchLogged < 8) {
            ++g_mismatchLogged;
            log().info(L"Bot: the game computed a shared secret with another key ({} [{}]; the game's login key {} [{}])",
                key ? shortHash(*key) : std::wstring(L"unreadable"), key ? describeKey(*key) : std::wstring(L"-"), shortHash(gameKey), describeKey(gameKey));
        }
        return result;
    }
    g_held = self; g_heldVtable = table; g_heldKey = gameKey;
    if (g_logged < 16) { ++g_logged; log().info(L"Bot: holding the game's login key manager (key {} [{}])", shortHash(gameKey), describeKey(*key)); }
    return result;
}

bool held()
{
    const auto gameKey = net::gameLoginKey();
    std::lock_guard lock(g_mutex);
    return g_held && !gameKey.empty() && g_heldKey == gameKey;
}

void noteClientData(const void* loginInfo)
{
    void* manager = nullptr;
    if (!memory::isReadable(loginInfo, sizeof(manager)) || !memory::copyGuarded(loginInfo, &manager, sizeof(manager))
        || !memory::isReadable(manager, 0x50)) return;
    const auto* table = readVtable(manager);
    const auto key = table ? publicKeyOf(manager) : std::nullopt;
    if (!key || key->empty()) return;
    std::string canonical;
    if (key->front() == '\x30') canonical = base64::encode({reinterpret_cast<const std::uint8_t*>(key->data()), key->size()});
    else if (const auto der = base64::decode(*key)) canonical = base64::encode(*der);
    else if (const auto urlDer = base64::decodeUrl(*key)) canonical = base64::encode(*urlDer);
    if (canonical.empty()) return;
    std::lock_guard lock(g_mutex);
    if ((g_held != manager || g_heldKey != canonical) && g_logged < 16) {
        ++g_logged;
        log().info(L"Bot: holding the game's login key manager from the clientData (key {} [{}])", shortHash(canonical), describeKey(*key));
    }
    g_held = manager; g_heldVtable = table; g_heldKey = std::move(canonical);
}

bool signGuarded(void* manager, void* out, const std::string* content)
{
    __try {
        const auto* bytes = static_cast<const std::byte*>(manager);
        if (!memory::isReadable(bytes + 0x28, 0x28)) return false;
        void* const signer = *reinterpret_cast<void* const*>(bytes + 0x28);
        if (!memory::isReadable(signer, sizeof(void*))) return false;
        const auto* table = *reinterpret_cast<void* const* const*>(signer);
        if (!memory::inGameModule(table) || !memory::isReadable(table, 0x48)) return false;
        const auto fn = reinterpret_cast<SignFn>(table[8]);
        if (!memory::inGameModule(reinterpret_cast<const void*>(fn)) || !memory::isExecutable(reinterpret_cast<const void*>(fn), 1)) return false;
        fn(signer, out, bytes + 0x30, content, 3, 0);
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}

std::optional<std::vector<std::uint8_t>> sign(const std::string& x5u, const std::string& content)
{
    std::lock_guard lock(g_mutex);
    static int failLogged = 0;
    const auto fail = [&](const std::wstring& why) -> std::optional<std::vector<std::uint8_t>> {
        if (failLogged < 8) {
            ++failLogged;
            log().warn(L"Bot: the game's login key could not sign ({}; login key {}, held {})", why, shortHash(x5u), shortHash(g_heldKey));
        }
        return std::nullopt;
    };
    if (!g_held) return fail(L"no key manager held");
    if (x5u.empty() || !sameKey(g_heldKey, x5u)) return fail(L"the held key is another key");
    if (!memory::isReadable(g_held, 0x50) || readVtable(g_held) != g_heldVtable) return fail(L"the key manager is gone");
    const auto key = publicKeyOf(g_held);
    if (!key || !sameKey(*key, x5u)) return fail(L"the key manager now holds another key");
    alignas(16) std::uint8_t out[kStringSize]{};
    if (!signGuarded(g_held, out, &content)) return fail(L"the signing call failed");
    const auto signature = gamestring::read(out);
    gamestring::destroy(out);
    if (!signature) return fail(L"the signature is unreadable");
    const auto raw = crypto::ecdsaDerToRaw({reinterpret_cast<const std::uint8_t*>(signature->data()), signature->size()}, 48);
    if (!raw) return fail(std::format(L"the signature is not a P-384 DER signature ({} bytes)", signature->size()));
    return raw;
}

std::optional<std::vector<std::uint8_t>> sharedSecret(const std::string& peerX5u)
{
    if (!g_ctor || !g_secret || peerX5u.empty()) return std::nullopt;
    const auto gameKey = net::gameLoginKey();
    std::lock_guard lock(g_mutex);
    if (!g_held || gameKey.empty() || g_heldKey != gameKey) return std::nullopt;
    const auto key = readVtable(g_held) == g_heldVtable ? publicKeyOf(g_held) : std::nullopt;
    if (!key || !sameKey(*key, gameKey)) {
        log().warn(L"Bot: the game's login key manager changed; cannot compute the shared secret");
        g_held = nullptr; g_heldVtable = nullptr; g_heldKey.clear();
        return std::nullopt;
    }
    const std::string peerKey = peerX5u;
    alignas(16) std::uint8_t peer[kKeyManagerSize]{};
    alignas(16) std::uint8_t out[kStringSize]{};
    bool constructed = false;
    unsigned code = 0;
    const bool ok = computeGuarded(g_original ? g_original : g_secret, g_held, &peerKey, peer, out, &constructed, &code);
    std::optional<std::string> secret;
    if (ok) secret = gamestring::read(out);
    if (ok) gamestring::destroy(out);
    if (constructed && readVtable(peer) == g_keyManagerVtable && !destroyGuarded(peer)) log().warn(L"Bot: destroying the server's key faulted");
    if (!ok) { log().warn(L"Bot: the game's key exchange faulted ({:#x}, {})", code, constructed ? L"in computeSecret" : L"building the server's key"); return std::nullopt; }
    if (!secret || secret->empty()) { log().warn(L"Bot: the game's key exchange returned nothing (the server's key may be invalid)"); return std::nullopt; }
    return std::vector<std::uint8_t>(secret->begin(), secret->end());
}

void release()
{
    std::lock_guard lock(g_mutex);
    g_held = nullptr; g_heldVtable = nullptr; g_heldKey.clear();
}

}
