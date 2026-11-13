// MM1 city object lists (.bng, .fcd) and their placement matrices.
#include "city/CityData.h"
#include "city/Mm1City.h"
#include "vfs/DirectoryFs.h"
#include "city/Mm1Instances.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string_view>
#include <vector>

using namespace mm2;
using namespace mm2::city;

namespace {

struct Writer {
    std::vector<std::byte> d;
    Writer& u16(std::uint16_t v) {
        d.push_back(std::byte(v & 0xFF));
        d.push_back(std::byte(v >> 8));
        return *this;
    }
    Writer& u32(std::uint32_t v) { return u16(v & 0xFFFF).u16(v >> 16); }
    Writer& f32(float v) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        return u32(u);
    }
    Writer& vec(float x, float y, float z) { return f32(x).f32(y).f32(z); }
    Writer& str(std::string_view s) {
        for (char c : s)
            d.push_back(std::byte(c));
        d.push_back(std::byte{0});
        return *this;
    }
};

} // namespace

TEST(Mm1Instances, ParsesBangers) {
    Writer w;
    w.u32(2);
    w.u16(7).u16(0x18).vec(1, 0, 2).vec(2, 0, 2).str("tppmtr");
    w.u16(9).u16(0x58).vec(-3, 0.5f, 4).vec(-3, 0.5f, 5).str("tpltst");
    std::string err;
    auto b = parseMm1Bng(w.d, &err);
    ASSERT_TRUE(b) << err;
    ASSERT_EQ(b->size(), 2u);
    EXPECT_EQ((*b)[0].cell, 7);
    EXPECT_EQ((*b)[0].flags, 0x18);
    EXPECT_EQ((*b)[0].name, "tppmtr");
    EXPECT_EQ((*b)[1].ahead.z, 5.f);
}

TEST(Mm1Instances, ParsesFacadesAndRejectsTrailingBytes) {
    Writer w;
    w.u32(1).u16(358).u16(0x21).vec(0, 0.14f, 0).vec(24, 0.14f, 0).vec(60, -0.14f, 0).f32(23.99f).str("dfbldg04");
    std::string err;
    auto f = parseMm1Fcd(w.d, &err);
    ASSERT_TRUE(f) << err;
    ASSERT_EQ(f->size(), 1u);
    EXPECT_EQ((*f)[0].name, "dfbldg04");
    EXPECT_EQ((*f)[0].sides.x, 60.f);
    EXPECT_FLOAT_EQ((*f)[0].width, 23.99f);

    // A text-mode CR LF conversion (the bug once in mm2tool cat) leaves bytes over.
    w.d.push_back(std::byte{0x0D});
    EXPECT_FALSE(parseMm1Fcd(w.d, &err));
    // A truncated record.
    Writer t;
    t.u32(1).u16(1).u16(1).vec(0, 0, 0);
    EXPECT_FALSE(parseMm1Fcd(t.d, &err));
}

TEST(Mm1Instances, MatrixFromPointsStretchesAlongTheFront) {
    // A front about 12 m long along +Z, for a mesh 24 m wide.
    const Mat34 m = mm1MatrixFromPoints({10, 2, 5}, {10, 1, 17}, 24.0f);
    EXPECT_NEAR(m.m0.x, 0.0f, 1e-6f);
    EXPECT_NEAR(m.m0.mag(), std::sqrt(145.0f) / 24.0f, 1e-5f); // |end - start| / width
    EXPECT_EQ(m.m1.y, 1.0f);
    // z = unit(x) cross y: for x along +Z (and slightly down) z points -X.
    EXPECT_LT(m.m2.x, -0.99f);
    // The origin is the start, lowered to the end's height.
    EXPECT_EQ(m.m3.x, 10.f);
    EXPECT_EQ(m.m3.y, 1.f);
    // The mesh's far corner (width, 0, 0) lands on the end point in x/z.
    const Vec3 far = m.transform({24.0f, 0.0f, 0.0f});
    EXPECT_NEAR(far.z, 17.0f, 1e-4f);
}

TEST(Mm1Instances, BuildingMatrixUsesThePointsAsAxes) {
    const Mat34 m = mm1BuildingMatrix({1, 0, 1}, {3, 0, 1}, {1, 0, -2});
    EXPECT_EQ(m.m0.x, 2.f);
    EXPECT_EQ(m.m2.z, -3.f);
    EXPECT_EQ(m.m1.y, 1.f);
    EXPECT_EQ(m.m3.x, 1.f);
}

TEST(Mm1City, ParsesCellsWithAndWithoutTags) {
    int maxCells = 0;
    std::string err;
    auto cells = parseMm1Cells("3\r\n10\r\n1,460,0,0\r\n2,462,0,1,8\r\n9,448\r\n", &maxCells, &err);
    ASSERT_TRUE(cells) << err;
    EXPECT_EQ(maxCells, 11); // LoadCells: the max id + 1
    ASSERT_EQ(cells->size(), 3u);
    EXPECT_EQ((*cells)[0].cullFlags, 460);
    ASSERT_EQ((*cells)[1].visitTags.size(), 1u);
    EXPECT_EQ((*cells)[1].visitTags[0], 8);
    EXPECT_EQ((*cells)[2].index, 9);
    EXPECT_FALSE(parseMm1Cells("2\n5\n1,0\n9,0\n", nullptr, &err)); // id above the max
    EXPECT_FALSE(parseMm1Cells("5\n10\n1,0\n", nullptr, &err));     // fewer lines than counted
}

TEST(Mm1City, ParsesPortalsIntoPolygons) {
    Writer w;
    w.u32(0).u32(2);
    // Two edges: flags 1, cells 3 and 4, 5 m high, from (0,0,0) to (10,0,0).
    w.d.push_back(std::byte{1});
    w.d.push_back(std::byte{2});
    w.u16(0).u16(3).u16(4).f32(5.0f).vec(0, 0, 0).vec(10, 0, 0);
    // Three edges: an extra corner follows the record.
    w.d.push_back(std::byte{3});
    w.d.push_back(std::byte{3});
    w.u16(0).u16(4).u16(5).f32(2.0f).vec(0, 0, 0).vec(4, 0, 0).vec(2, -1, 0);
    std::string err;
    auto p = parseMm1Portals(w.d, &err);
    ASSERT_TRUE(p) << err;
    ASSERT_EQ(p->size(), 2u);
    ASSERT_EQ((*p)[0].polygon.size(), 4u);
    EXPECT_EQ((*p)[0].polygon[0].y, 5.f); // the top edge first
    EXPECT_EQ((*p)[0].polygon[2].x, 10.f);
    ASSERT_EQ((*p)[1].polygon.size(), 5u);
    EXPECT_EQ((*p)[1].polygon[3].y, -1.f); // the extra corner before the start
    EXPECT_EQ((*p)[1].cell2, 5);

    w.d.push_back(std::byte{0});
    EXPECT_FALSE(parseMm1Portals(w.d, &err)); // trailing bytes
}

TEST(Mm1City, ParsesBridgeSetsAndLeafFrames) {
    const auto sets = parseMm1Gizmo("DrawBridge3\r\n"
                                    "\ttpdrawbridge06,0,162.6,0.0,-552.6,177.6,0.0,-552.6\r\n"
                                    "\ttpcrossgate06,0,177.6,0.15,-551.6,178.6,0.15,-551.6\r\n"
                                    "\ttpdrawbridge06,0,162.6,0.0,-617.4,147.6,0.0,-617.4\r\n"
                                    "DrawBridge3\r\n"
                                    "DrawBridge4\r\n"
                                    "\ttpdrawbridge06,0,1,0,2,3,0,2\r\n"
                                    "DrawBridge4\r\n");
    ASSERT_EQ(sets.size(), 2u);
    EXPECT_EQ(sets[0].name, "DrawBridge3");
    ASSERT_EQ(sets[0].parts.size(), 3u);
    EXPECT_TRUE(sets[0].parts[0].leaf());
    EXPECT_FALSE(sets[0].parts[1].leaf());
    EXPECT_EQ(sets[1].parts.size(), 1u);
    // Both leaves reach 32.5 m along their -Z into the 65 m gap between them.
    const Vec3 tip1 = sets[0].parts[0].matrix().transform({0, 0, -32.5f});
    const Vec3 tip2 = sets[0].parts[2].matrix().transform({0, 0, -32.5f});
    EXPECT_NEAR(tip1.z, -585.1f, 0.01f);
    EXPECT_NEAR(tip2.z, -584.9f, 0.01f);
    EXPECT_NEAR(tip1.x, 162.6f, 0.01f);
}

TEST(Mm1City, ParsesLightingRows) {
    const auto rows = parseMm1Lighting(
        "TOD,weather,sun heading ,sun pitch,sun R,sun G,sun B,fill1 heading ,fill1 pitch,fill1 R,fill1 G,fill1 B,"
        "fill2 heading ,fill2 pitch,fill2 R,fill2 G,fill2 B,ambi R,ambi G,ambi B,fog end,fog R,fog G,fog B,"
        "shad alpha,shad R,shad G,shad B,\r\n"
        "2,1,3.14,0.65,1,0.6,0.3,-2.5,0.45,0.8,0.9,1,0,0.45,0.75,0.8,1,0.1,0.1,0.2,600,230,100,35,180,15,20,30,x\r\n"
        "short,row\r\n");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].timeOfDay, 2);
    EXPECT_EQ(rows[0].weather, 1);
    EXPECT_FLOAT_EQ(rows[0].sunPitch, 0.65f);
    EXPECT_FLOAT_EQ(rows[0].sunColor.y, 0.6f);
    EXPECT_FLOAT_EQ(rows[0].fill1Heading, -2.5f);
    EXPECT_FLOAT_EQ(rows[0].ambient.z, 0.2f);
    EXPECT_FLOAT_EQ(rows[0].fogEnd, 600.0f);
    EXPECT_EQ(rows[0].fogColor[0], 230);
    EXPECT_EQ(rows[0].shadowColor[2], 30);
}

TEST(Mm1City, ListsMm1CitiesBesideMm2Ones) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "openmm2_test_mm1_list";
    fs::remove_all(root);
    fs::create_directories(root / "tune");
    fs::create_directories(root / "mm1" / "city");
    std::ofstream(root / "tune" / "london.cinfo") << "LocalizedName=London\nMapName=london\nRaceDir=london\n";
    std::ofstream(root / "mm1" / "city" / "chicago.cells") << "0\n0\n";
    vfs::Vfs v;
    v.mount(std::make_shared<vfs::DirectoryFs>(root));
    const auto cities = listCities(v);
    fs::remove_all(root);
    ASSERT_EQ(cities.size(), 2u);
    EXPECT_EQ(cities[0].mapName, "chicago"); // sorted by map name
    EXPECT_EQ(cities[0].localizedName, "Chicago");
    EXPECT_TRUE(cities[0].mm1);
    EXPECT_EQ(cities[0].blitzCount, 0);
    EXPECT_EQ(cities[1].mapName, "london");
    EXPECT_FALSE(cities[1].mm1);
}
