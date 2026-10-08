#include "game/LevelDbIndex.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace tsukuyomi::leveldb {
namespace {

constexpr std::size_t kBlock = 32768;
constexpr std::uint64_t kMaxManifest = 64ull * 1024 * 1024;
constexpr std::size_t kMaxHandleSnapshot = 16 * 1024 * 1024;

struct ProcessHandleTableEntryInfo {
    HANDLE handleValue;
    ULONG_PTR handleCount;
    ULONG_PTR pointerCount;
    ULONG grantedAccess;
    ULONG objectTypeIndex;
    ULONG handleAttributes;
    ULONG reserved;
};

struct ProcessHandleSnapshotInformation {
    ULONG_PTR numberOfHandles;
    ULONG_PTR reserved;
    ProcessHandleTableEntryInfo handles[1];
};

using NtQueryInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);

wchar_t asciiLower(wchar_t ch)
{
    return ch >= L'A' && ch <= L'Z' ? ch + (L'a' - L'A') : ch;
}

bool asciiEqualIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    return true;
}

std::optional<std::wstring> diskPath(HANDLE handle)
{
    if (GetFileType(handle) != FILE_TYPE_DISK) return std::nullopt;
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD count = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (count == 0) return std::nullopt;
        if (count < path.size()) {
            path.resize(count);
            return path;
        }
        if (count >= 32768) return std::nullopt;
        path.resize(static_cast<std::size_t>(count) + 1);
    }
}

struct OpenManifest {
    std::optional<ManifestPath> path;
    bool multiple = false;
};

OpenManifest findOpenManifest()
{
    OpenManifest result;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return result;
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (query == nullptr) return result;

    std::size_t bytes = 64 * 1024;
    for (;;) {
        std::vector<ULONG_PTR> buffer((bytes + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
        ULONG required = 0;
        const LONG status = query(GetCurrentProcess(), 51, buffer.data(), static_cast<ULONG>(bytes), &required);
        if (status == static_cast<LONG>(0xC0000004u)) {
            const std::size_t next = std::max<std::size_t>(required, bytes * 2);
            if (next > kMaxHandleSnapshot) return result;
            bytes = next;
            continue;
        }
        if (status < 0 || bytes < offsetof(ProcessHandleSnapshotInformation, handles)) return result;
        const auto* snapshot = reinterpret_cast<const ProcessHandleSnapshotInformation*>(buffer.data());
        const std::size_t capacity = (bytes - offsetof(ProcessHandleSnapshotInformation, handles))
                                   / sizeof(ProcessHandleTableEntryInfo);
        if (snapshot->numberOfHandles > capacity) return result;
        for (std::size_t i = 0; i < snapshot->numberOfHandles; ++i) {
            const auto path = diskPath(snapshot->handles[i].handleValue);
            if (!path) continue;
            auto manifest = parseOpenManifestPath(*path);
            if (!manifest) continue;
            if (result.path) result.multiple = true;
            else result.path = std::move(manifest);
        }
        return result;
    }
}

bool varint(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::uint64_t& out)
{
    out = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (pos >= size) return false;
        const std::uint8_t byte = data[pos++];
        if (shift == 63 && (byte & 0x7e) != 0) return false;
        out |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

bool lengthPrefixed(const std::uint8_t* data, std::size_t size, std::size_t& pos, std::string& out)
{
    std::uint64_t length = 0;
    if (!varint(data, size, pos, length) || length > size - pos) return false;
    out.assign(reinterpret_cast<const char*>(data + pos), static_cast<std::size_t>(length));
    pos += static_cast<std::size_t>(length);
    return true;
}

bool edit(const std::uint8_t* data, std::size_t size, std::vector<FileRange>& files)
{
    std::size_t pos = 0;
    while (pos < size) {
        std::uint64_t tag = 0, level = 0, number = 0, ignored = 0;
        std::string bytes, largest;
        if (!varint(data, size, pos, tag)) return false;
        switch (tag) {
        case 1:
            if (!lengthPrefixed(data, size, pos, bytes)) return false;
            break;
        case 2: case 3: case 4: case 9:
            if (!varint(data, size, pos, ignored)) return false;
            break;
        case 5:
            if (!varint(data, size, pos, level) || !lengthPrefixed(data, size, pos, bytes)) return false;
            break;
        case 6:
            if (!varint(data, size, pos, level) || !varint(data, size, pos, number)) return false;
            std::erase_if(files, [number](const FileRange& file) { return file.number == number; });
            break;
        case 7:
            if (!varint(data, size, pos, level) || !varint(data, size, pos, number)
                || !varint(data, size, pos, ignored) || !lengthPrefixed(data, size, pos, bytes)
                || !lengthPrefixed(data, size, pos, largest) || bytes.size() < 8 || largest.size() < 8) return false;
            std::erase_if(files, [number](const FileRange& file) { return file.number == number; });
            bytes.resize(bytes.size() - 8);
            largest.resize(largest.size() - 8);
            files.push_back({number, level, std::move(bytes), std::move(largest)});
            break;
        default:
            return false;
        }
    }
    return true;
}

int byteCompare(const std::string& a, const std::string& b)
{
    const std::size_t count = std::min(a.size(), b.size());
    const int cmp = count ? std::memcmp(a.data(), b.data(), count) : 0;
    if (cmp != 0) return cmp;
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

void little32(std::vector<std::uint8_t>& out, std::int32_t value)
{
    const auto bits = static_cast<std::uint32_t>(value);
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};

bool readFile(const std::wstring& path, std::uint64_t maxSize, std::vector<std::uint8_t>& out,
              std::uint64_t* sizeOut = nullptr, std::uint64_t* modifiedOut = nullptr)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) return false;
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > maxSize) return false;
    if (sizeOut) *sizeOut = size;
    if (modifiedOut) *modifiedOut = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32)
                                     | info.ftLastWriteTime.dwLowDateTime;
    out.resize(static_cast<std::size_t>(size));
    std::size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1024 * 1024));
        if (!ReadFile(file.value, out.data() + done, wanted, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

bool fileInfo(const std::wstring& path, std::uint64_t& size, std::uint64_t& modified)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) return false;
    size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32)
             | info.ftLastWriteTime.dwLowDateTime;
    return true;
}

}

std::optional<ManifestPath> parseOpenManifestPath(std::wstring_view path)
{
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    const auto manifestSlash = path.find_last_of(L'\\');
    if (manifestSlash == std::wstring_view::npos) return std::nullopt;
    const auto name = path.substr(manifestSlash + 1);
    constexpr std::wstring_view prefix = L"MANIFEST-";
    if (name.size() <= prefix.size() || !asciiEqualIgnoreCase(name.substr(0, prefix.size()), prefix))
        return std::nullopt;
    for (std::size_t i = prefix.size(); i < name.size(); ++i)
        if (name[i] < L'0' || name[i] > L'9') return std::nullopt;

    const auto dbSlash = path.substr(0, manifestSlash).find_last_of(L'\\');
    if (dbSlash == std::wstring_view::npos
        || !asciiEqualIgnoreCase(path.substr(dbSlash + 1, manifestSlash - dbSlash - 1), L"db"))
        return std::nullopt;
    const auto folderSlash = path.substr(0, dbSlash).find_last_of(L'\\');
    if (folderSlash == std::wstring_view::npos || folderSlash + 1 == dbSlash) return std::nullopt;
    const auto worldsSlash = path.substr(0, folderSlash).find_last_of(L'\\');
    if (worldsSlash == std::wstring_view::npos
        || !asciiEqualIgnoreCase(path.substr(worldsSlash + 1, folderSlash - worldsSlash - 1), L"minecraftWorlds"))
        return std::nullopt;
    return ManifestPath{std::wstring(path.substr(0, manifestSlash)),
                        std::wstring(path.substr(folderSlash + 1, dbSlash - folderSlash - 1)),
                        std::wstring(name)};
}

Manifest parseManifest(const std::uint8_t* data, std::size_t size)
{
    Manifest result{};
    if (data == nullptr && size != 0) return result;
    std::vector<std::uint8_t> pending;
    bool fragmented = false;
    std::size_t pos = 0;
    while (pos < size) {
        const std::size_t left = kBlock - pos % kBlock;
        if (left < 7) { pos += std::min(left, size - pos); continue; }
        if (size - pos < 7) break;
        const std::size_t length = static_cast<std::size_t>(data[pos + 4]) | (static_cast<std::size_t>(data[pos + 5]) << 8);
        const std::uint8_t type = data[pos + 6];
        if (length > left - 7 || length > size - pos - 7) return result;
        const std::uint8_t* fragment = data + pos + 7;
        pos += 7 + length;
        if (type == 0 && length == 0) continue;
        if (type == 1) {
            if (fragmented || !edit(fragment, length, result.files)) return result;
        } else if (type == 2) {
            if (fragmented) return result;
            pending.assign(fragment, fragment + length);
            fragmented = true;
        } else if (type == 3 || type == 4) {
            if (!fragmented) return result;
            pending.insert(pending.end(), fragment, fragment + length);
            if (type == 4) {
                if (!edit(pending.data(), pending.size(), result.files)) return result;
                pending.clear();
                fragmented = false;
            }
        } else return result;
    }
    if (fragmented) return result;
    std::sort(result.files.begin(), result.files.end(), [](const FileRange& a, const FileRange& b) {
        if (a.level != b.level) return a.level < b.level;
        return a.number > b.number;
    });
    result.ok = true;
    return result;
}

std::vector<std::uint8_t> subChunkKey(std::int32_t cx, std::int32_t cz, std::int32_t dim, std::int32_t subY)
{
    std::vector<std::uint8_t> key;
    key.reserve(dim == 0 ? 10 : 14);
    little32(key, cx);
    little32(key, cz);
    if (dim != 0) little32(key, dim);
    key.push_back(0x2f);
    key.push_back(static_cast<std::uint8_t>(subY));
    return key;
}

std::uint64_t findFile(const std::vector<FileRange>& files, const std::vector<std::uint8_t>& key)
{
    const std::string bytes(reinterpret_cast<const char*>(key.data()), key.size());
    const FileRange* best = nullptr;
    for (const FileRange& file : files) {
        if (byteCompare(file.smallest, bytes) > 0 || byteCompare(bytes, file.largest) > 0) continue;
        if (best == nullptr || file.level < best->level
            || (file.level == best->level && file.number > best->number)) best = &file;
    }
    return best != nullptr ? best->number : 0;
}

std::string keyHex(const std::vector<std::uint8_t>& key)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(key.size() * 2);
    for (const auto byte : key) { text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]); }
    return text;
}

namespace {
bool currentNames(const std::wstring& db, const std::wstring& manifest)
{
    std::vector<std::uint8_t> data;
    if (!readFile(db + L"\\CURRENT", 256, data)) return false;
    std::size_t length = data.size();
    while (length != 0 && (data[length - 1] == '\n' || data[length - 1] == '\r')) --length;
    if (length != manifest.size()) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (static_cast<wchar_t>(data[i]) != manifest[i]) return false;
    }
    return true;
}
}

void WorldIndex::reset()
{
    m_db.clear(); m_folderName.clear(); m_manifestName.clear();
    m_lastSearch = 0; m_lastFullSearch = 0; m_lastCheck = 0; m_size = 0; m_modified = 0;
    m_worldEntry = 0;
    m_multipleManifestHandlesSeen = false;
    m_index = {};
}

UpdateEvent WorldIndex::update(std::uint64_t now, std::uint64_t worldEntry)
{
    bool search = false;
    if (m_db.empty()) {
        search = m_lastSearch == 0 || now - m_lastSearch >= 5000;
    } else if (worldEntry != m_worldEntry || now - m_lastFullSearch >= 30000) {
        search = true;
    } else if (now - m_lastSearch >= 2000) {
        m_lastSearch = now;
        search = !currentNames(m_db, m_manifestName);
    }
    if (search) {
        m_lastSearch = now;
        m_lastFullSearch = now;
        m_worldEntry = worldEntry;
        OpenManifest open = findOpenManifest();
        if (open.multiple) m_multipleManifestHandlesSeen = true;
        if (!open.path) {
            m_db.clear(); m_folderName.clear(); m_manifestName.clear();
            m_lastCheck = 0; m_size = 0; m_modified = 0; m_index = {};
            return UpdateEvent::Missing;
        }
        if (m_db != open.path->db || m_manifestName != open.path->manifestName) {
            m_db = std::move(open.path->db);
            m_folderName = std::move(open.path->folderName);
            m_manifestName = std::move(open.path->manifestName);
            m_lastCheck = 0; m_size = 0; m_modified = 0; m_index = {};
        }
    }
    if (m_db.empty()) return UpdateEvent::None;
    if (m_lastCheck != 0 && now - m_lastCheck < 1000) return UpdateEvent::None;
    m_lastCheck = now;
    std::uint64_t size = 0, modified = 0;
    const std::wstring path = m_db + L"\\" + m_manifestName;
    if (!fileInfo(path, size, modified)) {
        m_index = {};
        return UpdateEvent::Unreadable;
    }
    if (m_index.ok && size == m_size && modified == m_modified)
        return UpdateEvent::None;
    if (size > kMaxManifest) { m_index = {}; return UpdateEvent::Unreadable; }
    std::vector<std::uint8_t> data;
    if (!readFile(path, kMaxManifest, data, &size, &modified)) {
        m_index = {};
        return UpdateEvent::Unreadable;
    }
    m_index = parseManifest(data.data(), data.size());
    if (m_index.ok) { m_size = size; m_modified = modified; }
    return m_index.ok ? UpdateEvent::Found : UpdateEvent::Unreadable;
}

std::uint64_t WorldIndex::find(const std::vector<std::uint8_t>& key) const
{
    return m_index.ok ? findFile(m_index.files, key) : 0;
}

}
