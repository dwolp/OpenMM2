#pragma once

#include "vfs/FileSystem.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace mm2::vfs {

// A Midtown Madness 1 "AngelRes" archive (CORE.AR, AUDIO.AR, UI.AR), read as
// MM1's VirtualFileSystem does. MM2 itself never reads this format; OpenMM2
// uses it only to convert MM1 content (see docs/formats/ares.md).
//
// Layout follows Open1560 (code/midtown/stream/vfsystem.{h,cpp}, GPL-3.0):
//   0x00  char[4]  "ARES"
//   0x04  u32      node count
//   0x08  u32      root count (nodes [0, rootCount) are the top directory)
//   0x0C  u32      name table size
//   0x10  node[nodeCount], 12 bytes each, then the name table.
// Files are stored uncompressed.
class AresArchive final : public FileSystem {
public:
    struct Entry {
        std::string name; // full path as stored, e.g. "CITY/CHICAGO.PSDL"
        std::uint32_t dataOffset;
        std::uint32_t size;
    };

    static std::shared_ptr<AresArchive> open(const std::filesystem::path& path, std::string* error = nullptr);
    static std::shared_ptr<AresArchive> open(std::shared_ptr<const RandomAccessFile> file, std::string label,
                                             std::string* error = nullptr);

    // True when `header` starts with the "ARES" magic.
    static bool isAres(std::span<const std::byte> header);

    std::string describe() const override;
    std::shared_ptr<RandomAccessFile> open(std::string_view path) const override;
    bool exists(std::string_view path) const override;
    void forEachFile(const std::function<void(const EntryInfo&)>& fn) const override;

    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry* find(std::string_view path) const;

private:
    AresArchive() = default;
    bool load(std::string* error);

    std::shared_ptr<const RandomAccessFile> m_file;
    std::string m_label;
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, std::size_t> m_index; // normalized path -> entry
};

} // namespace mm2::vfs
