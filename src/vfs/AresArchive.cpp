#include "vfs/AresArchive.h"

#include "core/Log.h"
#include "core/StringUtil.h"

#include <array>
#include <cstring>
#include <format>

namespace mm2::vfs {
namespace {

constexpr std::uint32_t kHeaderSize = 16;
constexpr std::uint32_t kNodeSize = 12;
constexpr std::uint32_t kMaxNodes = 1u << 20;
constexpr std::uint32_t kMaxNameTable = 16u << 20;
// Directory nesting in the retail archives is shallow; this only stops a
// corrupt archive whose directories refer back to themselves.
constexpr int kMaxDepth = 32;

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

// VirtualFileInode (Open1560 vfsystem.h).
struct Node {
    std::uint32_t f0; // file: data offset; directory: index of the first child
    std::uint32_t f4; // bits 0-22 size / child count, bits 23-31 extension name offset
    std::uint32_t f8; // bit 0 directory, bits 1-13 name number, bits 14-31 name offset

    std::uint32_t offset() const { return f0; }
    std::uint32_t size() const { return f4 & 0x7FFFFFu; }
    std::uint32_t extOffset() const { return (f4 >> 23) & 0x1FFu; }
    bool isDirectory() const { return (f8 & 1u) != 0; }
    std::uint32_t nameNumber() const { return (f8 >> 1) & 0x1FFFu; }
    std::uint32_t nameOffset() const { return (f8 >> 14) & 0x3FFFFu; }
};

// VirtualFileSystem::ExpandName: a '\1' in the stored name stands for the
// node's name number in decimal (so "TEX\1" names TEX0..TEX8191 with one
// string), and a nonzero extension offset appends "." and that string.
std::string expandName(const Node& n, std::string_view names) {
    std::string out;
    auto cstr = [&](std::uint32_t off) {
        const std::string_view rest = names.substr(off);
        return rest.substr(0, rest.find('\0'));
    };
    for (char c : cstr(n.nameOffset())) {
        if (c == '\1')
            out += std::to_string(n.nameNumber());
        else
            out += c;
    }
    if (n.extOffset() != 0) {
        out += '.';
        out += cstr(n.extOffset());
    }
    return out;
}

} // namespace

bool AresArchive::isAres(std::span<const std::byte> header) {
    return header.size() >= 4 && std::memcmp(header.data(), "ARES", 4) == 0;
}

std::shared_ptr<AresArchive> AresArchive::open(const std::filesystem::path& path, std::string* error) {
    auto file = OsFile::open(path);
    if (!file) {
        setError(error, std::format("cannot open '{}'", str::fromPath(path)));
        return nullptr;
    }
    return open(std::move(file), str::fromPath(path), error);
}

std::shared_ptr<AresArchive> AresArchive::open(std::shared_ptr<const RandomAccessFile> file, std::string label,
                                               std::string* error) {
    std::shared_ptr<AresArchive> ar(new AresArchive());
    ar->m_file = std::move(file);
    ar->m_label = std::move(label);
    if (!ar->load(error))
        return nullptr;
    return ar;
}

bool AresArchive::load(std::string* error) {
    std::array<std::byte, kHeaderSize> header{};
    if (!m_file->readExact(0, header) || !isAres(header)) {
        setError(error, "not an AngelRes (ARES) archive");
        return false;
    }
    const auto nodeCount = loadLE<std::uint32_t>(header.data() + 4);
    const auto rootCount = loadLE<std::uint32_t>(header.data() + 8);
    const auto namesSize = loadLE<std::uint32_t>(header.data() + 12);
    if (nodeCount > kMaxNodes || rootCount > nodeCount || namesSize > kMaxNameTable ||
        kHeaderSize + std::uint64_t{nodeCount} * kNodeSize + namesSize > m_file->size()) {
        setError(error, "corrupt ARES header");
        return false;
    }

    std::vector<std::byte> raw(static_cast<std::size_t>(nodeCount) * kNodeSize);
    std::string names(namesSize, '\0');
    if (!m_file->readExact(kHeaderSize, raw) ||
        !m_file->readExact(kHeaderSize + std::uint64_t{nodeCount} * kNodeSize,
                           std::as_writable_bytes(std::span(names)))) {
        setError(error, "truncated ARES directory");
        return false;
    }
    std::vector<Node> nodes(nodeCount);
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        const std::byte* p = raw.data() + static_cast<std::size_t>(i) * kNodeSize;
        nodes[i] = {loadLE<std::uint32_t>(p), loadLE<std::uint32_t>(p + 4), loadLE<std::uint32_t>(p + 8)};
    }

    // VirtualFileSystem::ValidateNodes, then a walk of the tree from the roots.
    const std::uint64_t fileSize = m_file->size();
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        const Node& n = nodes[i];
        if (n.nameOffset() >= namesSize || n.extOffset() >= namesSize) {
            setError(error, std::format("node {} has an out-of-range name", i));
            return false;
        }
        const bool bad = n.isDirectory() ? std::uint64_t{n.offset()} + n.size() > nodeCount
                                         : std::uint64_t{n.offset()} + n.size() > fileSize;
        if (bad) {
            setError(error, std::format("invalid node {} '{}'", i, expandName(n, names)));
            return false;
        }
    }

    m_entries.clear();
    m_index.clear();
    bool ok = true;
    auto walk = [&](auto&& self, std::uint32_t first, std::uint32_t count, const std::string& prefix,
                    int depth) -> void {
        if (depth > kMaxDepth) {
            setError(error, "ARES directories nest too deeply (cycle?)");
            ok = false;
            return;
        }
        for (std::uint32_t i = first; ok && i < first + count; ++i) {
            const Node& n = nodes[i];
            std::string path = prefix + expandName(n, names);
            if (n.isDirectory()) {
                self(self, n.offset(), n.size(), path + '/', depth + 1);
                continue;
            }
            const std::string key = str::normalizeVirtualPath(path);
            if (m_index.try_emplace(key, m_entries.size()).second)
                m_entries.push_back(Entry{std::move(path), n.offset(), n.size()});
        }
    };
    walk(walk, 0, rootCount, "", 0);
    if (!ok)
        return false;
    log::debug("ares: '{}' {} files", m_label, m_entries.size());
    return true;
}

std::string AresArchive::describe() const { return "MM1 archive " + m_label; }

const AresArchive::Entry* AresArchive::find(std::string_view path) const {
    const auto it = m_index.find(str::normalizeVirtualPath(path));
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

std::shared_ptr<RandomAccessFile> AresArchive::open(std::string_view path) const {
    const Entry* e = find(path);
    if (!e)
        return nullptr;
    return std::make_shared<SubFile>(m_file, e->dataOffset, e->size);
}

bool AresArchive::exists(std::string_view path) const { return find(path) != nullptr; }

void AresArchive::forEachFile(const std::function<void(const EntryInfo&)>& fn) const {
    for (const auto& e : m_entries)
        fn(EntryInfo{str::normalizeVirtualPath(e.name), e.size, false});
}

} // namespace mm2::vfs
