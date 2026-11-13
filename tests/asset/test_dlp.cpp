// Angel DLP7 templates (MM1 reads named points such as PIVOT from them).
#include "asset/Dlp.h"

#include <gtest/gtest.h>

#include <bit>
#include <string_view>
#include <vector>

using namespace mm2;
using namespace mm2::asset;

namespace {

// Big-endian, as the files are.
struct BigWriter {
    std::vector<std::byte> d;
    BigWriter& u8(std::uint8_t v) {
        d.push_back(std::byte{v});
        return *this;
    }
    BigWriter& u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v >> 8)).u8(v & 0xFF); }
    BigWriter& u32(std::uint32_t v) { return u16(static_cast<std::uint16_t>(v >> 16)).u16(v & 0xFFFF); }
    BigWriter& f32(float v) { return u32(std::bit_cast<std::uint32_t>(v)); }
    BigWriter& text(std::string_view s) {
        for (char c : s)
            u8(static_cast<std::uint8_t>(c));
        return *this;
    }
};

// Two groups (a mesh group of one patch, and PIVOT of one vertex), one
// triangle patch, four vertices.
std::vector<std::byte> sampleDlp(bool pivotByPatch = false) {
    BigWriter w;
    w.text("DLP7").u32(2).u32(1).u32(4);
    w.u8(2).text("H").u8(0).u32(0).u32(1).u16(0);
    w.u8(6).text("PIVOT").u8(0);
    if (pivotByPatch)
        w.u32(0).u32(1).u16(0);
    else
        w.u32(1).u32(0).u16(3);
    // The patch: SRes 3, TRes 1, flags, ROpts, material, texture, physics.
    w.u16(3).u16(1).u16(0).u16(0).u16(1).u16(0).u16(0);
    for (std::uint16_t id : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{0}}) {
        w.u16(id).f32(0).f32(1).f32(0).f32(0).f32(0).u32(0xFFFFFFFFu);
    }
    w.u32(3).text("tri");
    for (float x : {0.0f, 1.0f, 2.0f})
        w.f32(x).f32(0).f32(0);
    w.f32(0.5f).f32(0.92f).f32(-0.25f);
    return w.d;
}

} // namespace

TEST(Dlp, ReadsGroupsPatchesAndVertices) {
    std::string err;
    auto d = parseDlp(sampleDlp(), &err);
    ASSERT_TRUE(d) << err;
    ASSERT_EQ(d->groups.size(), 2u);
    EXPECT_EQ(d->groups[0].name, "H");
    EXPECT_EQ(d->groups[1].name, "PIVOT");
    ASSERT_EQ(d->patches.size(), 1u);
    EXPECT_EQ(d->patches[0], (std::vector<std::uint16_t>{1, 2, 0}));
    ASSERT_EQ(d->vertices.size(), 4u);
    const auto pivot = d->point("PIVOT");
    ASSERT_TRUE(pivot);
    EXPECT_FLOAT_EQ(pivot->y, 0.92f);
    EXPECT_FALSE(d->point("NONE"));
}

TEST(Dlp, PointFromAPatch) {
    // mmAnimDOF::Init: a group of one patch gives that patch's first vertex.
    auto d = parseDlp(sampleDlp(true));
    ASSERT_TRUE(d);
    const auto pivot = d->point("PIVOT");
    ASSERT_TRUE(pivot);
    EXPECT_FLOAT_EQ(pivot->x, 1.0f); // vertex 1
}

TEST(Dlp, RejectsOtherAndShortData) {
    std::string err;
    std::vector<std::byte> other(16, std::byte{0});
    EXPECT_FALSE(parseDlp(other, &err));
    auto cut = sampleDlp();
    cut.resize(cut.size() - 6);
    EXPECT_FALSE(parseDlp(cut, &err));
}
