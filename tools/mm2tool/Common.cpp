#include "Common.h"

#include "core/StringUtil.h"
#include "vfs/AresArchive.h"
#include "vfs/DaveArchive.h"
#include "vfs/DirectoryFs.h"
#include "vfs/GameSource.h"
#include "vfs/IsoImage.h"

#include <array>
#include <print>

namespace mm2::tool {
namespace {

// Adapts a Vfs mount stack to the FileSystem interface.
class VfsView final : public vfs::FileSystem {
public:
    VfsView(std::shared_ptr<vfs::Vfs> v, std::string label) : m_vfs(std::move(v)), m_label(std::move(label)) {}
    std::string describe() const override { return m_label; }
    std::shared_ptr<RandomAccessFile> open(std::string_view path) const override { return m_vfs->open(path); }
    bool exists(std::string_view path) const override { return m_vfs->exists(path); }
    void forEachFile(const std::function<void(const vfs::EntryInfo&)>& fn) const override {
        for (const auto& e : m_vfs->listFiles())
            fn(e);
    }

private:
    std::shared_ptr<vfs::Vfs> m_vfs;
    std::string m_label;
};

} // namespace

std::shared_ptr<vfs::FileSystem> openContainer(const std::string& arg) {
    const auto path = str::toPath(arg);
    std::string err;
    std::error_code ec;
    const auto ext = str::lower(str::fromPath(path.extension()));
    if (ext == ".ar") {
        // MM1's AngelRes archives share the extension; tell them apart by magic.
        if (auto file = OsFile::open(path)) {
            std::array<std::byte, 4> magic{};
            if (file->readExact(0, magic) && vfs::AresArchive::isAres(magic)) {
                auto ares = vfs::AresArchive::open(std::move(file), arg, &err);
                if (!ares)
                    std::println(stderr, "error: {}: {}", arg, err);
                return ares;
            }
        }
        auto ar = vfs::DaveArchive::open(path, &err);
        if (!ar)
            std::println(stderr, "error: {}: {}", arg, err);
        return ar;
    }
    // A game source (disc image, disc folder, install folder) mounts everything.
    if (auto source = vfs::probeGameSource(path); source && source->usable()) {
        auto v = std::make_shared<vfs::Vfs>();
        if (!vfs::mountGameSource(*v, *source, &err)) {
            std::println(stderr, "error: {}: {}", arg, err);
            return nullptr;
        }
        return std::make_shared<VfsView>(std::move(v), source->describe());
    }
    if (std::filesystem::is_directory(path, ec))
        return std::make_shared<vfs::DirectoryFs>(path);
    auto iso = vfs::IsoImage::open(path, &err);
    if (!iso)
        std::println(stderr, "error: {}: {}", arg, err);
    return iso;
}

std::optional<std::vector<std::byte>> readFile(const vfs::FileSystem& fs, const std::string& path) {
    auto f = fs.open(path);
    if (!f) {
        std::println(stderr, "error: '{}' not found in {}", path, fs.describe());
        return std::nullopt;
    }
    auto data = f->readAll();
    if (data.size() != f->size()) {
        std::println(stderr, "error: short read on '{}'", path);
        return std::nullopt;
    }
    return data;
}

bool globMatch(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, starP = std::string_view::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || str::toLower(pattern[p]) == str::toLower(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

} // namespace mm2::tool
