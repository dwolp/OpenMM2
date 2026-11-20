#pragma once

#include "core/StringUtil.h"
#include "vfs/FileSystem.h"

#include <memory>
#include <string>
#include <utility>

namespace mm2::vfs {

// Serves another file system's files under a directory prefix ("mm1/"), so
// a second game's data can be mounted beside MM2's without any of its paths
// shadowing or being shadowed by MM2's.
class PrefixFs final : public FileSystem {
public:
    // `prefix` is normalized and gets a trailing '/'.
    PrefixFs(std::shared_ptr<const FileSystem> inner, std::string prefix)
        : m_inner(std::move(inner)), m_prefix(str::normalizeVirtualPath(prefix)) {
        if (!m_prefix.empty() && m_prefix.back() != '/')
            m_prefix += '/';
    }

    std::string describe() const override { return m_inner->describe() + " as " + m_prefix; }

    std::shared_ptr<RandomAccessFile> open(std::string_view path) const override {
        const auto inner = strip(path);
        return inner.empty() ? nullptr : m_inner->open(inner);
    }
    bool exists(std::string_view path) const override {
        const auto inner = strip(path);
        return !inner.empty() && m_inner->exists(inner);
    }
    void forEachFile(const std::function<void(const EntryInfo&)>& fn) const override {
        m_inner->forEachFile([&](const EntryInfo& e) {
            EntryInfo out = e;
            out.path = m_prefix + e.path;
            fn(out);
        });
    }

private:
    // The path inside the inner file system, or "" when outside the prefix.
    std::string strip(std::string_view path) const {
        std::string p = str::normalizeVirtualPath(path);
        if (!p.starts_with(m_prefix))
            return {};
        return p.substr(m_prefix.size());
    }

    std::shared_ptr<const FileSystem> m_inner;
    std::string m_prefix;
};

} // namespace mm2::vfs
