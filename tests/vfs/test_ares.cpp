// MM1 AngelRes (ARES) archives, read as Open1560's VirtualFileSystem does.
#include "core/File.h"
#include "vfs/AresArchive.h"
#include "vfs/PrefixFs.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace mm2;
using namespace std::string_view_literals;

namespace {

void put32(std::vector<std::byte>& d, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        d.push_back(std::byte(v >> (8 * i)));
}

void putStr(std::vector<std::byte>& d, std::string_view s) {
    for (char c : s)
        d.push_back(std::byte(c));
}

void node(std::vector<std::byte>& d, std::uint32_t f0, std::uint32_t size, std::uint32_t ext, bool dir,
          std::uint32_t number, std::uint32_t name) {
    put32(d, f0);
    put32(d, size | (ext << 23));
    put32(d, (dir ? 1u : 0u) | (number << 1) | (name << 14));
}

// Roots: CITY/ (one child, TEX7.BMP via the '\1' number escape) and README.TXT.
std::vector<std::byte> sampleArchive() {
    const std::string names{"\0CITY\0README\0TXT\0TEX\1\0BMP\0", 26};
    constexpr std::uint32_t kData = 16 + 3 * 12 + 26;
    std::vector<std::byte> d;
    putStr(d, "ARES");
    put32(d, 3);
    put32(d, 2);
    put32(d, static_cast<std::uint32_t>(names.size()));
    node(d, 2, 1, 0, true, 0, 1);         // CITY/ -> node 2
    node(d, kData, 5, 13, false, 0, 6);   // README.TXT
    node(d, kData + 5, 3, 22, false, 7, 17); // TEX7.BMP
    putStr(d, names);
    putStr(d, "hellopix");
    return d;
}

std::string readAll(const RandomAccessFile& f) {
    std::string s(f.size(), '\0');
    EXPECT_TRUE(f.readExact(0, std::as_writable_bytes(std::span(s))));
    return s;
}

} // namespace

TEST(AresArchive, ListsAndReadsNestedFiles) {
    std::string err;
    auto ar = vfs::AresArchive::open(std::make_shared<MemoryFile>(sampleArchive()), "core.ar", &err);
    ASSERT_TRUE(ar) << err;
    ASSERT_EQ(ar->entries().size(), 2u);
    EXPECT_EQ(ar->entries()[0].name, "CITY/TEX7.BMP");
    EXPECT_EQ(ar->entries()[1].name, "README.TXT");

    auto tex = ar->open("city/tex7.bmp"sv); // lookups ignore case
    ASSERT_TRUE(tex);
    EXPECT_EQ(readAll(*tex), "pix");
    auto readme = ar->open("README.TXT"sv);
    ASSERT_TRUE(readme);
    EXPECT_EQ(readAll(*readme), "hello");
    EXPECT_FALSE(ar->exists("city"));
}

TEST(AresArchive, RejectsCorruptArchives) {
    std::string err;
    auto bad = sampleArchive();
    bad[0] = std::byte{'X'};
    EXPECT_FALSE(vfs::AresArchive::open(std::make_shared<MemoryFile>(bad), "a.ar", &err));

    // A file running past the end of the archive.
    auto past = sampleArchive();
    past.resize(past.size() - 2);
    EXPECT_FALSE(vfs::AresArchive::open(std::make_shared<MemoryFile>(past), "b.ar", &err));

    // A directory whose children run past the node table.
    auto dir = sampleArchive();
    dir[16 + 4] = std::byte{9};
    EXPECT_FALSE(vfs::AresArchive::open(std::make_shared<MemoryFile>(dir), "c.ar", &err));
}

TEST(PrefixFs, ServesFilesUnderThePrefixOnly) {
    std::string err;
    auto ar = vfs::AresArchive::open(std::make_shared<MemoryFile>(sampleArchive()), "core.ar", &err);
    ASSERT_TRUE(ar) << err;
    vfs::PrefixFs fs(ar, "MM1");
    EXPECT_TRUE(fs.exists("mm1/city/tex7.bmp"));
    EXPECT_FALSE(fs.exists("city/tex7.bmp"));
    auto f = fs.open("mm1/README.TXT"sv);
    ASSERT_TRUE(f);
    EXPECT_EQ(readAll(*f), "hello");
    std::vector<std::string> paths;
    fs.forEachFile([&](const vfs::EntryInfo& e) { paths.push_back(e.path); });
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], "mm1/city/tex7.bmp");
}
