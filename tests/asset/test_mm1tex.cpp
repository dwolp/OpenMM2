// MM1 textures: DDS decoding and the texture sheet (mtl/global.tsh).
#include "asset/Image.h"
#include "asset/Mm1TexSheet.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace mm2::asset;

namespace {

std::vector<std::byte> ddsHeader(std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t pfFlags,
                                 std::uint32_t bits, std::uint32_t r, std::uint32_t g, std::uint32_t b,
                                 std::uint32_t a) {
    std::vector<std::byte> d(128, std::byte{0});
    auto put = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            d[off + i] = std::byte(v >> (8 * i));
    };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(12, h);
    put(16, w);
    put(28, mips);
    put(76, 32);
    put(80, pfFlags);
    put(88, bits);
    put(92, r);
    put(96, g);
    put(100, b);
    put(104, a);
    return d;
}

void put16(std::vector<std::byte>& d, std::uint16_t v) {
    d.push_back(std::byte(v & 0xFF));
    d.push_back(std::byte(v >> 8));
}

} // namespace

TEST(Dds, DecodesRgb565WithMipsInFileOrder) {
    auto d = ddsHeader(2, 2, 2, 0x40, 16, 0xF800, 0x07E0, 0x001F, 0);
    put16(d, 0xF800); // top-left: red
    put16(d, 0x07E0); // green
    put16(d, 0x001F); // blue
    put16(d, 0xFFFF); // white
    put16(d, 0x0000); // 1x1 mip: black
    std::string err;
    auto img = decodeDds(d, &err);
    ASSERT_TRUE(img) << err;
    ASSERT_EQ(img->levels.size(), 2u);
    EXPECT_FALSE(img->alphaFormat);
    const auto& px = img->levels[0].rgba;
    // Row 0 is the file's first row (the picture's top, v = 0 in Direct3D).
    EXPECT_EQ(px[0], 255); // red
    EXPECT_EQ(px[1], 0);
    EXPECT_EQ(px[5], 255); // green
    EXPECT_EQ(px[10], 255); // blue
    EXPECT_EQ(px[12], 255); // white, full alpha
    EXPECT_EQ(px[15], 255);
    EXPECT_EQ(img->levels[1].rgba[0], 0);
}

TEST(Dds, DecodesArgb4444Alpha) {
    auto d = ddsHeader(1, 1, 1, 0x41, 16, 0x0F00, 0x00F0, 0x000F, 0xF000);
    put16(d, 0x8F00); // half alpha, red
    std::string err;
    auto img = decodeDds(d, &err);
    ASSERT_TRUE(img) << err;
    EXPECT_TRUE(img->alphaFormat);
    EXPECT_EQ(img->levels[0].rgba[0], 255);
    EXPECT_EQ(img->levels[0].rgba[3], 136); // 8/15 of 255, rounded
}

TEST(Dds, RejectsUnsupportedAndTruncated) {
    std::string err;
    auto pal = ddsHeader(1, 1, 1, 0x60, 8, 0, 0, 0, 0);
    pal.push_back(std::byte{0});
    EXPECT_FALSE(decodeDds(pal, &err)); // palettised: MM1's palettes live in .lut files
    auto dxt = ddsHeader(4, 4, 1, 0x4, 0, 0, 0, 0, 0);
    EXPECT_FALSE(decodeDds(dxt, &err));
    auto shortTop = ddsHeader(2, 2, 1, 0x40, 16, 0xF800, 0x07E0, 0x001F, 0);
    put16(shortTop, 0);
    EXPECT_FALSE(decodeDds(shortTop, &err));
}

namespace {

constexpr std::string_view kSheet = "name,neighborhood,h,m,l,flags,alternate,sibling,xres,yres,hexdaycolor,hexnightcolor\r\n"
                                    "07_WIN,0,0,0,1,,07_WIN_N,,64,64,ff585b5e,ff555554\r\n"
                                    "07_WIN_N,0,0,0,1,n,,,64,64,ff585b5e,ff555554\r\n"
                                    "G_1,g,0,0,0,t,,G_2,16,16,00000000,00000000\r\n"
                                    "G_2,g,0,0,0,t,,G_3,16,16,00000000,00000000\r\n"
                                    "G_3,g,0,0,0,tu,,,16,16,00000000,00000000\r\n"
                                    "t_grass,0,0,0,1,w,,,64,64,ff2b4a22,ff1a2a15\r\n"
                                    "T_GRASS_FALL,0,0,0,1,,,,64,64,ff4a3a22,ff2a2015\r\n";

} // namespace

TEST(Mm1TexSheet, ParsesFlagsAndColours) {
    std::string err;
    auto sheet = Mm1TexSheet::parse(kSheet, &err);
    ASSERT_TRUE(sheet) << err;
    EXPECT_EQ(sheet->entries().size(), 7u);
    EXPECT_TRUE(sheet->warnings().empty());
    const auto* g3 = sheet->lookup("g_3");
    ASSERT_TRUE(g3);
    EXPECT_EQ(g3->flags, Mm1TexFlag::Transparent | Mm1TexFlag::ClampUOrBoth);
    const auto* grass = sheet->lookup("T_GRASS");
    ASSERT_TRUE(grass); // names are stored in upper case
    EXPECT_EQ(grass->flags, Mm1TexFlag::Snowable);
    EXPECT_EQ(grass->dayColor, 0xFF2B4A22u);
}

TEST(Mm1TexSheet, ResolvesVariationsNightAndWeather) {
    auto sheet = Mm1TexSheet::parse(kSheet);
    ASSERT_TRUE(sheet);
    EXPECT_EQ(sheet->variationCount("G_1"), 3);
    EXPECT_EQ(sheet->resolve("G_1", 1, false, Mm1TexWeather::Clear), "G_2");
    EXPECT_EQ(sheet->resolve("G_1", 9, false, Mm1TexWeather::Clear), "G_3"); // stops at the chain's end
    EXPECT_EQ(sheet->resolve("07_win", 0, true, Mm1TexWeather::Clear), "07_WIN_N");
    EXPECT_EQ(sheet->resolve("07_win", 0, false, Mm1TexWeather::Clear), "07_WIN");
    EXPECT_EQ(sheet->resolve("T_GRASS", 0, false, Mm1TexWeather::Rain), "T_GRASS_FALL");
    EXPECT_EQ(sheet->resolve("T_GRASS", 0, false, Mm1TexWeather::Snow), "T_GRASS"); // no _WIN entry
    EXPECT_EQ(sheet->resolve("NOPE", 0, false, Mm1TexWeather::Clear), "");
}

TEST(Mm1TexSheet, WarnsOnUnknownFlagsAndRejectsShortLines) {
    auto sheet = Mm1TexSheet::parse("header\nA,0,0,0,0,tz,,,1,1,0,0\n");
    ASSERT_TRUE(sheet);
    EXPECT_EQ(sheet->entries()[0].flags, Mm1TexFlag::Transparent);
    EXPECT_EQ(sheet->warnings().size(), 1u);
    std::string err;
    EXPECT_FALSE(Mm1TexSheet::parse("header\nA,0,0\n", &err));
}
