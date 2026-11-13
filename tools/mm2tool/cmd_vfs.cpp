// Container commands: ls, cat, extract, source.
#include "Command.h"
#include "Common.h"

#include "core/File.h"
#include "core/StringUtil.h"
#include "vfs/GameSource.h"

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <print>

namespace mm2::tool {
namespace {

std::vector<vfs::EntryInfo> matching(const vfs::FileSystem& fs, std::string_view pattern) {
    std::vector<vfs::EntryInfo> files;
    fs.forEachFile([&](const vfs::EntryInfo& e) {
        if (pattern.empty() || globMatch(pattern, e.path))
            files.push_back(e);
    });
    std::ranges::sort(files, {}, &vfs::EntryInfo::path);
    return files;
}

int cmdLs(std::span<char* const> args) {
    if (args.empty())
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    for (const auto& e : matching(*fs, args.size() > 1 ? args[1] : ""))
        std::println("{:>10}  {}", e.size, e.path);
    return 0;
}

int cmdCat(std::span<char* const> args) {
    if (args.size() < 2)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    auto data = readFile(*fs, args[1]);
    if (!data)
        return 1;
#ifdef _WIN32
    // The CRT's stdout is in text mode, which writes every 0x0A as CR LF and
    // so corrupts binary files piped or redirected from here.
    std::fflush(stdout);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(data->data(), 1, data->size(), stdout);
    return 0;
}

int cmdExtract(std::span<char* const> args) {
    if (args.size() < 2)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    const auto outDir = str::toPath(args[1]);
    int failures = 0, count = 0;
    for (const auto& e : matching(*fs, args.size() > 2 ? args[2] : "")) {
        auto data = readFile(*fs, e.path);
        if (!data || !file::writeAtomic(outDir / str::toPath(e.path), *data)) {
            std::println(stderr, "failed: {}", e.path);
            ++failures;
            continue;
        }
        ++count;
    }
    std::println("extracted {} files to {}", count, args[1]);
    return failures ? 1 : 0;
}

int cmdSource(std::span<char* const> args) {
    if (args.empty()) {
        for (const auto& p : vfs::suggestGameSources())
            std::println("{}", str::fromPath(p));
        return 0;
    }
    std::string err;
    auto s = vfs::probeGameSource(str::toPath(args[0]), &err);
    if (!s) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    std::println("{}", s->describe());
    for (const auto& a : s->archives)
        std::println("  archive {}", a);
    for (const auto& m : s->missing)
        std::println("  MISSING {}", m);
    return s->usable() ? 0 : 1;
}

const Registrar r1({"ls", "<container> [glob]", "list files with their sizes", &cmdLs});
const Registrar r2({"cat", "<container> <path>", "write a file to stdout", &cmdCat});
const Registrar r3({"extract", "<container> <outdir> [glob]", "extract files (all, or those matching glob)",
                    &cmdExtract});
const Registrar r4({"source", "[path]", "identify a game source, or list detected sources", &cmdSource});

} // namespace
} // namespace mm2::tool
