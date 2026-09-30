#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::leveldb {

struct FileRange {
    std::uint64_t number;
    std::uint64_t level;
    std::string smallest, largest;
};

struct Manifest {
    bool ok = false;
    std::vector<FileRange> files;
};

Manifest parseManifest(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> subChunkKey(std::int32_t cx, std::int32_t cz, std::int32_t dim, std::int32_t subY);
std::uint64_t findFile(const std::vector<FileRange>& files, const std::vector<std::uint8_t>& key);
std::string keyHex(const std::vector<std::uint8_t>& key);

struct ManifestPath {
    std::wstring db, folderName, manifestName;
};

std::optional<ManifestPath> parseOpenManifestPath(std::wstring_view path);

enum class UpdateEvent { None, Found, Missing, Unreadable };

class WorldIndex {
public:
    void reset();
    UpdateEvent update(std::uint64_t now, std::uint64_t worldEntry = 0);
    std::uint64_t find(const std::vector<std::uint8_t>& key) const;
    const std::wstring& folderName() const { return m_folderName; }
    bool multipleManifestHandlesSeen() const { return m_multipleManifestHandlesSeen; }

private:
    std::wstring m_db, m_folderName, m_manifestName;
    std::uint64_t m_lastSearch = 0, m_lastFullSearch = 0, m_lastCheck = 0, m_size = 0, m_modified = 0;
    std::uint64_t m_worldEntry = 0;
    bool m_multipleManifestHandlesSeen = false;
    Manifest m_index;
};

}
