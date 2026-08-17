#include "game/OreUiPatch.h"

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

constexpr wchar_t kStartScreenLeaf[] = L"start_screen.json";

std::wstring g_patchedNtPath;
std::wstring g_startScreenNtPath;
bool g_ready = false;

volatile bool g_done = false;

constexpr const char* kOwnIds[] = {"tk.k0", "tk.k1", "tk.k2",  "tk.k3", "tk.k4", "tk.k5",
                                   "tk.k6", "tk.k7", "tk.k8",  "tk.k9", "tk.k10"};
constexpr int kOwnIdCount = static_cast<int>(sizeof(kOwnIds) / sizeof(kOwnIds[0]));

constexpr char kAnchor[] = "\"keyboardAndMouse.inputGroup.standard\":hne,";

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

bool endsWithJson(const wchar_t* text, size_t chars)
{
    static const wchar_t kEnd[] = L".json";
    const size_t need = sizeof(kEnd) / sizeof(kEnd[0]) - 1;
    if (chars < need) {
        return false;
    }
    for (size_t i = 0; i < need; ++i) {
        const wchar_t c = text[chars - need + i];
        const wchar_t want = kEnd[i];
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

template <typename Call>
LONG withSwap(OBJECT_ATTRIBUTES* attrs, Call call)
{

    if (!g_ready || g_done || attrs == nullptr || attrs->ObjectName == nullptr
        || attrs->ObjectName->Buffer == nullptr) {
        return call();
    }
    const wchar_t* const text = attrs->ObjectName->Buffer;
    const size_t chars = attrs->ObjectName->Length / sizeof(wchar_t);

    const std::wstring* swapTo = nullptr;
    if (endsWithJs(text, chars) && leafIsBundle(text, chars)) {
        swapTo = &g_patchedNtPath;
    } else if (endsWithJson(text, chars)
               && leafIs(text, chars, kStartScreenLeaf,
                         sizeof(kStartScreenLeaf) / sizeof(kStartScreenLeaf[0]) - 1)
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

bool buildPatchedStartScreen()
{

    g_startScreenNtPath.clear();

    const std::wstring root = versionDir();
    if (root.empty()) {
        return false;
    }
    const std::wstring source =
        root + L"\\data\\resource_packs\\vanilla\\ui\\start_screen.json";
    HANDLE const in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        note(L"[oreui] start_screen.json is missing");
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(in, &size);
    std::vector<char> blob(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL read = ReadFile(in, blob.data(), static_cast<DWORD>(blob.size()), &got, nullptr);
    CloseHandle(in);
    if (read == 0 || got != blob.size()) {
        note(L"[oreui] cannot read start_screen.json");
        return false;
    }

    static const char kAnchor[] =
        "\"version\": {\n"
        "    \"type\": \"panel\",\n"
        "    \"anchor_from\": \"top_right\",\n"
        "    \"anchor_to\": \"top_right\",\n"
        "    \"size\": [ \"50%\", \"100%\" ],\n"
        "    \"controls\": [\n";
    static const char kAdded[] =
        "      {\n"
        "        \"tsukuyomi_version\": {\n"
        "          \"type\": \"label\",\n"
        "          \"color\": \"$main_header_text_color\",\n"
        "          \"layer\": 2,\n"
        "          \"text\": \"Tsukuyomi v" TSUKUYOMI_VERSION "\",\n"
        "          \"size\": [ \"default\", 10 ],\n"
        "          \"max_size\": [ \"100%\", \"100%\" ],\n"
        "          \"anchor_from\": \"top_right\",\n"
        "          \"anchor_to\": \"top_right\",\n"
        "          \"offset\": [ 0, -12 ]\n"
        "        }\n"
        "      },\n"
        "      {\n"
        "        \"tsukuyomi_version_background\": {\n"
        "          \"type\": \"image\",\n"
        "          \"texture\": \"textures/ui/Black\",\n"
        "          \"anchor_from\": \"top_right\",\n"
        "          \"anchor_to\": \"top_right\",\n"
        "          \"offset\": [ 1, -13 ],\n"
        "          \"alpha\": 0.6,\n"
        "          \"size\": [ \"100%sm + 2px\", \"100%sm + 2px\" ],\n"
        "          \"layer\": 1\n"
        "        }\n"
        "      },\n";
    static const char kLabelBlock[] =
        "      {\n"
        "        \"label\": {\n"
        "          \"type\": \"label\",\n"
        "          \"color\": \"$main_header_text_color\",\n"
        "          \"layer\": 2,\n"
        "          \"text\": \"#version\",\n"
        "          \"size\": [ \"default\", 10 ],\n"
        "          \"max_size\": [ \"100%\", \"100%\" ],\n"
        "          \"anchor_from\": \"top_right\",\n"
        "          \"anchor_to\": \"top_right\",\n"
        "          \"bindings\": [\n"
        "            {\n"
        "              \"binding_name\": \"#version\"\n"
        "            }\n"
        "          ]\n"
        "        }\n"
        "      },\n";
    static const char kLabelWrapped[] =
        "      {\n"
        "        \"tsukuyomi_mc_line\": {\n"
        "          \"type\": \"stack_panel\",\n"
        "          \"orientation\": \"horizontal\",\n"
        "          \"size\": [ \"100%c\", 10 ],\n"
        "          \"anchor_from\": \"top_right\",\n"
        "          \"anchor_to\": \"top_right\",\n"
        "          \"controls\": [\n"
        "            {\n"
        "              \"tsukuyomi_mc_name\": {\n"
        "                \"type\": \"label\",\n"
        "                \"color\": \"$main_header_text_color\",\n"
        "                \"layer\": 2,\n"
        "                \"text\": \"Minecraft \"\n"
        "              }\n"
        "            },\n"
        "            {\n"
        "              \"label\": {\n"
        "                \"type\": \"label\",\n"
        "                \"color\": \"$main_header_text_color\",\n"
        "                \"layer\": 2,\n"
        "                \"text\": \"#version\",\n"
        "                \"bindings\": [\n"
        "                  {\n"
        "                    \"binding_name\": \"#version\"\n"
        "                  }\n"
        "                ]\n"
        "              }\n"
        "            }\n"
        "          ]\n"
        "        }\n"
        "      },\n";

    toLf(blob);
    const std::string with = std::string(kAnchor) + kAdded;
    if (!replaceOnce(blob, kAnchor, with.c_str())) {
        note(L"[oreui] could not find where to insert the version line (different game version?)");
        return false;
    }

    if (!replaceOnce(blob, kLabelBlock, kLabelWrapped)) {
        note(L"[oreui] could not find the game version label (different game version?)");
        return false;
    }

    const std::wstring outDir = root + L"\\Tsukuyomi\\ui";
    CreateDirectoryW((root + L"\\Tsukuyomi").c_str(), nullptr);
    CreateDirectoryW(outDir.c_str(), nullptr);
    const std::wstring outPath = outDir + L"\\start_screen.json";
    HANDLE const out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        note(L"[oreui] cannot create the start_screen.json copy");
        return false;
    }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(out, blob.data(), static_cast<DWORD>(blob.size()), &wrote, nullptr);
    CloseHandle(out);
    if (ok == 0 || wrote != blob.size()) {
        note(L"[oreui] cannot write the start_screen.json copy");
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
    const size_t at = haystack.find(kAnchor);
    if (at == std::string_view::npos) {
        note(L"[oreui] could not find where to insert (different game version?)");
        return false;
    }
    std::string body;
    for (const char* id : kOwnIds) {
        body += '"';
        body += id;
        body += "\":hne,";
    }
    std::vector<char> made;
    made.reserve(blob.size() + body.size());
    made.insert(made.end(), blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(at));
    made.insert(made.end(), body.begin(), body.end());
    made.insert(made.end(), blob.begin() + static_cast<std::ptrdiff_t>(at), blob.end());
    blob.swap(made);

    if (!replaceOnce(blob, "return n.createElement(r.Mount,{when:s},"
                           "n.createElement(Lte,{id:e,type:\"ResetButton\"",
                     "return n.createElement(r.Mount,{when:(0,r.useFacetMap)("
                     "((a,b)=>a&&\"tk-hide\"!==b),[],[s,d])},"
                     "n.createElement(Lte,{id:e,type:\"ResetButton\"")) {
        note(L"[oreui] could not find the reset-button hook point (different game version?)");
        return false;
    }

    const std::wstring outDir = root + L"\\Tsukuyomi\\hbui";
    CreateDirectoryW((root + L"\\Tsukuyomi").c_str(), nullptr);
    CreateDirectoryW(outDir.c_str(), nullptr);
    const std::wstring outPath = outDir + L"\\" + name;

    {
        WIN32_FILE_ATTRIBUTE_DATA have{};
        if (GetFileAttributesExW(outPath.c_str(), GetFileExInfoStandard, &have) != 0
            && have.nFileSizeHigh == 0 && have.nFileSizeLow == blob.size()) {
            g_patchedNtPath = L"\\??\\" + outPath;
            g_ready = true;
            note((L"[oreui] the copy already exists " + g_patchedNtPath).c_str());
            return true;
        }
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
