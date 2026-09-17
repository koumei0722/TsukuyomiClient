#include "game/OreUiPatch.h"
#include "hooks/Detours.h"
#include "render/PackTexture.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::oreui {
namespace {

struct UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct IO_STATUS_BLOCK {
    union {
        LONG Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*,
                                    LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtOpenFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*,
                                  ULONG, ULONG);

NtCreateFileFn g_realCreate = nullptr;
NtOpenFileFn g_realOpen = nullptr;

constexpr wchar_t kNeedle[] = L"\\gui\\dist\\hbui\\index-";
constexpr wchar_t kSuffix[] = L".js";

constexpr wchar_t kUiArchiveTail[] = L"\\resource_packs\\vanilla\\__brarchive\\ui.brarchive";

std::wstring g_patchedNtPath;
std::wstring g_startScreenNtPath;
bool g_ready = false;
volatile bool g_done = false;

constexpr const char* kOwnIds[] = {
    "tk.k0",  "tk.k1",  "tk.k2",  "tk.k3",  "tk.k4",  "tk.k5",  "tk.k6",  "tk.k7",
    "tk.k8",  "tk.k9",  "tk.k10", "tk.k11", "tk.k12", "tk.k13", "tk.k14", "tk.k15",
    "tk.k16", "tk.k17", "tk.k18", "tk.k19", "tk.k20", "tk.k21", "tk.k22", "tk.k23",
    "tk.k24", "tk.k25", "tk.k26", "tk.k27", "tk.k28", "tk.k29", "tk.k30", "tk.k31"};
constexpr int kOwnIdCount = static_cast<int>(sizeof(kOwnIds) / sizeof(kOwnIds[0]));

constexpr char kAnchorHead[] = "\"keyboardAndMouse.inputGroup.standard\":";

bool replaceOnce(std::vector<char>& blob, const char* from, const char* to)
{
    const std::string_view haystack(blob.data(), blob.size());
    const size_t at = haystack.find(from);
    if (at == std::string_view::npos) {
        return false;
    }
    const size_t fromLen = std::strlen(from);
    const size_t toLen = std::strlen(to);
    std::vector<char> made;
    made.reserve(blob.size() + toLen);
    made.insert(made.end(), blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(at));
    made.insert(made.end(), to, to + toLen);
    made.insert(made.end(), blob.begin() + static_cast<std::ptrdiff_t>(at + fromLen), blob.end());
    blob.swap(made);
    return true;
}

void toLf(std::vector<char>& blob)
{
    size_t out = 0;
    for (size_t in = 0; in < blob.size(); ++in) {
        if (blob[in] == '\r' && in + 1 < blob.size() && blob[in + 1] == '\n') {
            continue;
        }
        blob[out++] = blob[in];
    }
    blob.resize(out);
}

std::wstring versionDir();

void note(const wchar_t* text)
{
    static HANDLE file = INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE) {
        const std::wstring root = versionDir();
        if (root.empty()) {
            return;
        }
        CreateDirectoryW((root + L"\\Tsukuyomi").c_str(), nullptr);
        const std::wstring full = root + L"\\Tsukuyomi\\oreui-open.log";
        DeleteFileW(full.c_str());
        file = CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
    }
    char line[1024]{};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, line, sizeof(line) - 2, nullptr,
                                      nullptr);
    if (n <= 1) {
        return;
    }
    line[n - 1] = '\n';
    DWORD put = 0;
    WriteFile(file, line, static_cast<DWORD>(n), &put, nullptr);
}

bool endsWithJs(const wchar_t* text, size_t chars)
{
    if (chars < 4) {
        return false;
    }
    const wchar_t a = text[chars - 3];
    const wchar_t b = text[chars - 2];
    const wchar_t c = text[chars - 1];
    return a == L'.' && (b == L'j' || b == L'J') && (c == L's' || c == L'S');
}

bool leafIsBundle(const wchar_t* text, size_t chars)
{
    size_t at = chars;
    while (at > 0 && text[at - 1] != L'\\' && text[at - 1] != L'/') {
        --at;
    }
    static const wchar_t kLeaf[] = L"index-";
    const size_t need = sizeof(kLeaf) / sizeof(kLeaf[0]) - 1;
    if (chars - at < need) {
        return false;
    }
    for (size_t i = 0; i < need; ++i) {
        const wchar_t c = text[at + i];
        const wchar_t want = kLeaf[i];
        if (c != want && c != (want - 32)) {
            return false;
        }
    }
    return true;
}

bool leafIs(const wchar_t* text, size_t chars, const wchar_t* leaf, size_t need)
{
    size_t at = chars;
    while (at > 0 && text[at - 1] != L'\\' && text[at - 1] != L'/') {
        --at;
    }
    if (chars - at != need) {
        return false;
    }
    for (size_t i = 0; i < need; ++i) {
        const wchar_t c = text[at + i];
        const wchar_t want = leaf[i];
        if (c != want && c != (want - 32)) {
            return false;
        }
    }
    return true;
}

bool pathEndsWith(const wchar_t* text, size_t chars, const wchar_t* tail, size_t need)
{
    if (chars < need) {
        return false;
    }
    for (size_t i = 0; i < need; ++i) {
        wchar_t c = text[chars - need + i];
        wchar_t want = tail[i];
        if (c == L'/') {
            c = L'\\';
        }
        if (want == L'/') {
            want = L'\\';
        }
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c + 32);
        }
        if (want >= L'A' && want <= L'Z') {
            want = static_cast<wchar_t>(want + 32);
        }
        if (c != want) {
            return false;
        }
    }
    return true;
}

template <typename Call>
LONG withSwap(OBJECT_ATTRIBUTES* attrs, Call call)
{
    if (hooks::beModelsOn() && attrs != nullptr && attrs->ObjectName != nullptr
        && attrs->ObjectName->Buffer != nullptr) {
        pack::noteOpenedFile(attrs->ObjectName->Buffer,
                             attrs->ObjectName->Length / sizeof(wchar_t));
    }
    if (!g_ready || g_done || attrs == nullptr || attrs->ObjectName == nullptr
        || attrs->ObjectName->Buffer == nullptr) {
        return call();
    }
    const wchar_t* const text = attrs->ObjectName->Buffer;
    const size_t chars = attrs->ObjectName->Length / sizeof(wchar_t);

    const std::wstring* swapTo = nullptr;
    if (endsWithJs(text, chars) && leafIsBundle(text, chars)) {
        swapTo = &g_patchedNtPath;
    } else if (pathEndsWith(text, chars, kUiArchiveTail,
                            sizeof(kUiArchiveTail) / sizeof(kUiArchiveTail[0]) - 1)
               && !g_startScreenNtPath.empty()) {
        swapTo = &g_startScreenNtPath;
    }
    if (swapTo == nullptr) {
        return call();
    }

    UNICODE_STRING mine{};
    mine.Buffer = const_cast<PWSTR>(swapTo->c_str());
    mine.Length = static_cast<USHORT>(swapTo->size() * sizeof(wchar_t));
    mine.MaximumLength = static_cast<USHORT>(mine.Length + sizeof(wchar_t));
    UNICODE_STRING* const saved = attrs->ObjectName;
    const HANDLE savedRoot = attrs->RootDirectory;
    attrs->ObjectName = &mine;
    attrs->RootDirectory = nullptr;
    const LONG result = call();
    attrs->ObjectName = saved;
    attrs->RootDirectory = savedRoot;
    if (result >= 0) {
        static volatile LONG told = 0;
        static volatile LONG toldTitle = 0;
        if (swapTo == &g_startScreenNtPath) {
            if (InterlockedIncrement(&toldTitle) <= 3) {
                note(L"[oreui] redirected the title JSON to our own copy");
            }
        } else if (InterlockedIncrement(&told) <= 4) {
            note(L"[oreui] redirected the bundle open to our own copy");
        }
        return result;
    }
    {
        wchar_t line[256]{};
        wsprintfW(line, L"[oreui] could not open our copy (%08X); retrying with the original name",
                  static_cast<unsigned>(result));
        note(line);
    }
    g_done = true;
    return call();
}

LONG NTAPI detourNtCreateFile(PHANDLE handle, ACCESS_MASK access, OBJECT_ATTRIBUTES* attrs,
                              IO_STATUS_BLOCK* io, LARGE_INTEGER* size, ULONG fileAttrs,
                              ULONG share, ULONG disposition, ULONG options, PVOID ea,
                              ULONG eaLength)
{
    return withSwap(attrs, [&] {
        return g_realCreate(handle, access, attrs, io, size, fileAttrs, share, disposition,
                            options, ea, eaLength);
    });
}

LONG NTAPI detourNtOpenFile(PHANDLE handle, ACCESS_MASK access, OBJECT_ATTRIBUTES* attrs,
                            IO_STATUS_BLOCK* io, ULONG share, ULONG options)
{
    return withSwap(attrs, [&] { return g_realOpen(handle, access, attrs, io, share, options); });
}

constexpr size_t kStolenMax = 24;

size_t stealLength(const unsigned char* at)
{
    if (at[0] == 0x4C && at[1] == 0x8B && at[2] == 0xD1 && at[3] == 0xB8) {
        if (at[8] == 0xF6 && at[9] == 0x04 && at[10] == 0x25) {
            return 16;
        }
    }
    return 0;
}

void* makeTrampoline(unsigned char* target, size_t stolen)
{
    auto* const pad = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (pad == nullptr) {
        return nullptr;
    }
    std::memcpy(pad, target, stolen);
    pad[stolen + 0] = 0xFF;
    pad[stolen + 1] = 0x25;
    pad[stolen + 2] = 0x00;
    pad[stolen + 3] = 0x00;
    pad[stolen + 4] = 0x00;
    pad[stolen + 5] = 0x00;
    const auto back = reinterpret_cast<std::uintptr_t>(target + stolen);
    std::memcpy(pad + stolen + 6, &back, sizeof(back));
    return pad;
}

bool hookOne(const char* name, void* detour, void** original,
             unsigned char (&saved)[kStolenMax], size_t& stolenOut)
{
    HMODULE const ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }
    auto* const target = reinterpret_cast<unsigned char*>(GetProcAddress(ntdll, name));
    if (target == nullptr) {
        return false;
    }
    const size_t stolen = stealLength(target);
    if (stolen == 0 || stolen > kStolenMax) {
        return false;
    }
    stolenOut = stolen;
    std::memcpy(saved, target, stolen);
    void* const pad = makeTrampoline(target, stolen);
    if (pad == nullptr) {
        return false;
    }
    *original = pad;

    DWORD old = 0;
    if (VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old) == 0) {
        return false;
    }
    target[0] = 0xFF;
    target[1] = 0x25;
    target[2] = 0x00;
    target[3] = 0x00;
    target[4] = 0x00;
    target[5] = 0x00;
    const auto to = reinterpret_cast<std::uintptr_t>(detour);
    std::memcpy(target + 6, &to, sizeof(to));
    VirtualProtect(target, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    return true;
}

unsigned char g_savedCreate[kStolenMax]{};
unsigned char g_savedOpen[kStolenMax]{};
size_t g_stolenCreate = 0;
size_t g_stolenOpen = 0;
bool g_hooked = false;

void unhookOne(const char* name, const unsigned char* saved, size_t stolen)
{
    HMODULE const ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr || stolen == 0) {
        return;
    }
    auto* const target = reinterpret_cast<unsigned char*>(GetProcAddress(ntdll, name));
    if (target == nullptr) {
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old) == 0) {
        return;
    }
    std::memcpy(target, saved, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
}

std::wstring versionDir()
{
    wchar_t path[MAX_PATH]{};
    const DWORD chars = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (chars == 0 || chars >= MAX_PATH) {
        return {};
    }
    std::wstring full(path, chars);
    const size_t cut = full.find_last_of(L"\\/");
    if (cut == std::wstring::npos) {
        return {};
    }
    return full.substr(0, cut);
}

bool sameAsExistingCopy(const std::wstring& path, const std::vector<char>& made)
{
    WIN32_FILE_ATTRIBUTE_DATA have{};
    if (made.empty() || GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &have) == 0
        || have.nFileSizeHigh != 0 || have.nFileSizeLow != made.size()) {
        return false;
    }
    HANDLE const in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::vector<char> existing(made.size());
    DWORD got = 0;
    const BOOL read =
        ReadFile(in, existing.data(), static_cast<DWORD>(existing.size()), &got, nullptr);
    CloseHandle(in);
    return read != 0 && got == made.size()
           && std::memcmp(existing.data(), made.data(), made.size()) == 0;
}

}

bool patchReady() { return g_ready; }

const char* ownGroupId(int index)
{
    return (index >= 0 && index < kOwnIdCount) ? kOwnIds[index] : nullptr;
}

int ownGroupIdCount() { return kOwnIdCount; }

bool installEarlyFileHook()
{
    if (g_hooked) {
        return true;
    }
    note(L"[oreui] installing the file-open hook");
    const bool a = hookOne("NtCreateFile", reinterpret_cast<void*>(&detourNtCreateFile),
                           reinterpret_cast<void**>(&g_realCreate), g_savedCreate,
                           g_stolenCreate);
    const bool b = hookOne("NtOpenFile", reinterpret_cast<void*>(&detourNtOpenFile),
                           reinterpret_cast<void**>(&g_realOpen), g_savedOpen, g_stolenOpen);
    g_hooked = a && b;
    return g_hooked;
}

void removeEarlyFileHook()
{
    if (!g_hooked) {
        return;
    }
    g_ready = false;
    unhookOne("NtCreateFile", g_savedCreate, g_stolenCreate);
    unhookOne("NtOpenFile", g_savedOpen, g_stolenOpen);
    g_hooked = false;
}

constexpr std::uint8_t kArchiveMagic[8] = {0x7D, 0x27, 0x25, 0xB1, 0xA0, 0x52, 0x70, 0x26};
constexpr std::size_t kArchiveRecord = 256;
constexpr std::size_t kArchiveOffsetAt = 0xF8;

std::uint32_t readU32(const std::vector<char>& blob, std::size_t at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, blob.data() + at, sizeof(value));
    return value;
}

void writeU32(std::vector<char>& blob, std::size_t at, std::uint32_t value)
{
    std::memcpy(blob.data() + at, &value, sizeof(value));
}

bool buildPatchedStartScreen()
{
    g_startScreenNtPath.clear();

    const std::wstring root = versionDir();
    if (root.empty()) {
        return false;
    }
    const std::wstring source =
        root + L"\\data\\resource_packs\\vanilla\\__brarchive\\ui.brarchive";
    HANDLE const in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        note(L"[oreui] ui.brarchive is missing");
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(in, &size);
    std::vector<char> blob(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL read = ReadFile(in, blob.data(), static_cast<DWORD>(blob.size()), &got, nullptr);
    CloseHandle(in);
    if (read == 0 || got != blob.size() || blob.size() < 0x10) {
        note(L"[oreui] cannot read ui.brarchive");
        return false;
    }
    if (std::memcmp(blob.data(), kArchiveMagic, sizeof(kArchiveMagic)) != 0) {
        note(L"[oreui] ui.brarchive has an unexpected header");
        return false;
    }

    const std::uint32_t count = readU32(blob, 8);
    const std::size_t base = 0x10 + static_cast<std::size_t>(count) * kArchiveRecord;
    if (count == 0 || count > 4096 || base > blob.size()) {
        note(L"[oreui] ui.brarchive has an unexpected table");
        return false;
    }

    std::uint32_t target = count;
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::size_t at = 0x10 + static_cast<std::size_t>(k) * kArchiveRecord;
        const auto nameLen = static_cast<unsigned char>(blob[at]);
        if (nameLen == 0 || nameLen > kArchiveOffsetAt - 1) {
            continue;
        }
        if (std::string_view(blob.data() + at + 1, nameLen) == "start_screen.json") {
            target = k;
            break;
        }
    }
    if (target == count) {
        note(L"[oreui] start_screen.json is not in ui.brarchive");
        return false;
    }

    const std::size_t rec = 0x10 + static_cast<std::size_t>(target) * kArchiveRecord;
    const std::uint32_t oldOffset = readU32(blob, rec + kArchiveOffsetAt);
    const std::uint32_t oldSize = readU32(blob, rec + kArchiveOffsetAt + 4);
    if (base + oldOffset + oldSize > blob.size()) {
        note(L"[oreui] the start_screen entry points outside the archive");
        return false;
    }
    std::string json(blob.data() + base + oldOffset, oldSize);

    static const char kAdded[] =
        "{\"tsukuyomi_version\":{\"type\":\"label\","
        "\"color\":\"$main_header_text_color\",\"layer\":2,"
        "\"text\":\"Tsukuyomi v" TSUKUYOMI_VERSION "\","
        "\"size\":[\"default\",10],\"max_size\":[\"100%\",\"100%\"],"
        "\"anchor_from\":\"top_right\",\"anchor_to\":\"top_right\","
        "\"offset\":[0,-12]}},"
        "{\"tsukuyomi_version_background\":{\"type\":\"image\","
        "\"texture\":\"textures/ui/Black\","
        "\"anchor_from\":\"top_right\",\"anchor_to\":\"top_right\","
        "\"offset\":[1,-13],\"alpha\":0.6,"
        "\"size\":[\"100%sm + 2px\",\"100%sm + 2px\"],\"layer\":1}},";
    const std::size_t panel = json.find("\"version\":{");
    if (panel == std::string::npos) {
        note(L"[oreui] could not find the version panel (different game version?)");
        return false;
    }
    const std::size_t controls = json.find("\"controls\":[", panel);
    if (controls == std::string::npos) {
        note(L"[oreui] the version panel has no controls (different game version?)");
        return false;
    }

    static const char kLabelWrapped[] =
        "{\"tsukuyomi_mc_line\":{\"type\":\"stack_panel\","
        "\"orientation\":\"horizontal\",\"size\":[\"100%c\",10],"
        "\"anchor_from\":\"top_right\",\"anchor_to\":\"top_right\","
        "\"controls\":["
        "{\"tsukuyomi_mc_name\":{\"type\":\"label\","
        "\"color\":\"$main_header_text_color\",\"layer\":2,"
        "\"text\":\"Minecraft \"}},"
        "{\"label\":{\"type\":\"label\","
        "\"color\":\"$main_header_text_color\",\"layer\":2,"
        "\"text\":\"#version\","
        "\"bindings\":[{\"binding_name\":\"#version\"}]}}"
        "]}}";
    if (json.find("tsukuyomi_mc_line") == std::string::npos) {
        const std::size_t label = json.find("{\"label\":{", controls);
        if (label == std::string::npos) {
            note(L"[oreui] could not find the game version label (different game version?)");
            return false;
        }
        std::size_t at = label;
        int depth = 0;
        bool inText = false;
        for (; at < json.size(); ++at) {
            const char c = json[at];
            if (inText) {
                if (c == '\\') {
                    ++at;
                } else if (c == '"') {
                    inText = false;
                }
                continue;
            }
            if (c == '"') {
                inText = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    ++at;
                    break;
                }
            }
        }
        if (depth != 0) {
            note(L"[oreui] the version label is not a closed object");
            return false;
        }
        json.replace(label, at - label, kLabelWrapped);
    }

    if (json.find("tsukuyomi_version") == std::string::npos) {
        json.insert(json.find("\"controls\":[", json.find("\"version\":{"))
                        + sizeof("\"controls\":[") - 1,
                    kAdded);
    }

    std::vector<char> made;
    made.reserve(blob.size() + json.size());
    made.insert(made.end(), blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(base));
    std::uint32_t cursor = 0;
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::size_t at = 0x10 + static_cast<std::size_t>(k) * kArchiveRecord;
        const std::uint32_t off = readU32(blob, at + kArchiveOffsetAt);
        const std::uint32_t len = readU32(blob, at + kArchiveOffsetAt + 4);
        if (base + off + len > blob.size()) {
            note(L"[oreui] an entry points outside the archive");
            return false;
        }
        writeU32(made, at + kArchiveOffsetAt, cursor);
        if (k == target) {
            writeU32(made, at + kArchiveOffsetAt + 4, static_cast<std::uint32_t>(json.size()));
            made.insert(made.end(), json.begin(), json.end());
            cursor += static_cast<std::uint32_t>(json.size());
        } else {
            made.insert(made.end(), blob.begin() + static_cast<std::ptrdiff_t>(base + off),
                        blob.begin() + static_cast<std::ptrdiff_t>(base + off + len));
            cursor += len;
        }
    }

    const std::wstring outDir = root + L"\\Tsukuyomi\\ui";
    CreateDirectoryW((root + L"\\Tsukuyomi").c_str(), nullptr);
    CreateDirectoryW(outDir.c_str(), nullptr);
    const std::wstring outPath = outDir + L"\\ui.brarchive";
    if (sameAsExistingCopy(outPath, made)) {
        g_startScreenNtPath = L"\\??\\" + outPath;
        note(L"[oreui] the title archive copy is already up to date");
        return true;
    }
    HANDLE const out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        note(L"[oreui] cannot create the ui.brarchive copy");
        return false;
    }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(out, made.data(), static_cast<DWORD>(made.size()), &wrote, nullptr);
    CloseHandle(out);
    if (ok == 0 || wrote != made.size()) {
        note(L"[oreui] cannot write the ui.brarchive copy");
        return false;
    }
    g_startScreenNtPath = L"\\??\\" + outPath;
    note(L"[oreui] added the Tsukuyomi version to the title screen");
    return true;
}

bool buildPatchedBundle()
{
    const std::wstring root = versionDir();
    if (root.empty()) {
        return false;
    }
    const std::wstring dir = root + L"\\data\\gui\\dist\\hbui";

    WIN32_FIND_DATAW found{};
    const std::wstring pattern = dir + L"\\index-*.js";
    HANDLE const search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        note(L"[oreui] the bundle was not found");
        return false;
    }
    const std::wstring name = found.cFileName;
    FindClose(search);

    const std::wstring source = dir + L"\\" + name;
    HANDLE const in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        note(L"[oreui] cannot open the bundle");
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(in, &size);
    std::vector<char> blob(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL read = ReadFile(in, blob.data(), static_cast<DWORD>(blob.size()), &got, nullptr);
    CloseHandle(in);
    if (read == 0 || got != blob.size()) {
        note(L"[oreui] cannot read the bundle");
        return false;
    }

    const std::string_view haystack(blob.data(), blob.size());
    const size_t at = haystack.find(kAnchorHead);
    if (at == std::string_view::npos) {
        note(L"[oreui] could not find where to insert (different game version?)");
        return false;
    }
    const size_t valueAt = at + sizeof(kAnchorHead) - 1;
    const size_t comma = haystack.find(',', valueAt);
    if (comma == std::string_view::npos || comma <= valueAt || comma - valueAt > 24) {
        note(L"[oreui] the key group entry has an unexpected shape");
        return false;
    }
    const std::string groupName(haystack.substr(valueAt, comma - valueAt));
    std::string body;
    for (const char* id : kOwnIds) {
        body += '"';
        body += id;
        body += "\":";
        body += groupName;
        body += ",";
    }
    std::vector<char> made;
    made.reserve(blob.size() + body.size());
    made.insert(made.end(), blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(at));
    made.insert(made.end(), body.begin(), body.end());
    made.insert(made.end(), blob.begin() + static_cast<std::ptrdiff_t>(at), blob.end());
    blob.swap(made);

    const std::wstring outDir = root + L"\\Tsukuyomi\\hbui";
    CreateDirectoryW((root + L"\\Tsukuyomi").c_str(), nullptr);
    CreateDirectoryW(outDir.c_str(), nullptr);
    const std::wstring outPath = outDir + L"\\" + name;
    if (sameAsExistingCopy(outPath, blob)) {
        g_patchedNtPath = L"\\??\\" + outPath;
        g_ready = true;
        note((L"[oreui] the copy is already up to date " + g_patchedNtPath).c_str());
        return true;
    }
    HANDLE const out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        note(L"[oreui] cannot create the copy");
        return false;
    }
    DWORD put = 0;
    const BOOL wrote = WriteFile(out, blob.data(), static_cast<DWORD>(blob.size()), &put, nullptr);
    CloseHandle(out);
    if (wrote == 0 || put != blob.size()) {
        note(L"[oreui] cannot write the copy");
        return false;
    }

    g_patchedNtPath = L"\\??\\" + outPath;
    g_ready = true;
    note((L"[oreui] created the copy " + g_patchedNtPath).c_str());
    return true;
}

}
