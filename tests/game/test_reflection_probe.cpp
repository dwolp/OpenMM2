#include "game/ReflectionProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>

using namespace mm2;
using namespace mm2::game;

namespace {

// Where a world point drawn by face `face` lands in the atlas: the face's
// view and projection, then the viewport of its cell (row 0 at the top), as
// u, v in 0..1 of the whole atlas; nullopt behind the face or outside it.
std::optional<std::pair<float, float>> drawnAt(const Vec3& center, std::size_t face, const Vec3& p,
                                               std::uint32_t size) {
    const Mat44 view = Mat44::fromMat34(probeFaceCamera(center, face).fastInverse());
    const Mat44 m = view * probeProjection(0.25f, 1000.0f);
    const Vec4 c = m.transform(Vec4{p.x, p.y, p.z, 1.0f});
    if (c.w <= 0.0f)
        return std::nullopt;
    const float x = c.x / c.w, y = c.y / c.w;
    if (std::abs(x) > 1.0f || std::abs(y) > 1.0f)
        return std::nullopt;
    const ProbeCell cell = probeCell(face, size);
    const float px = static_cast<float>(cell.x) + (x * 0.5f + 0.5f) * static_cast<float>(size);
    const float py = static_cast<float>(cell.y) + (0.5f - y * 0.5f) * static_cast<float>(size);
    return std::pair{px / (3.0f * static_cast<float>(size)), py / (2.0f * static_cast<float>(size))};
}

} // namespace

TEST(ReflectionProbe, FacesAreRightHandedAndLookAlongTheirAxes) {
    for (std::size_t f = 0; f < kProbeFaces.size(); ++f) {
        const Mat34 cam = probeFaceCamera({1, 2, 3}, f);
        const Vec3 look = cam.m2 * -1.0f;
        EXPECT_NEAR(look.dot(kProbeFaces[f].forward), 1.0f, 1e-6f) << f;
        EXPECT_NEAR(cam.m0.cross(cam.m1).dot(cam.m2), 1.0f, 1e-6f) << f; // right x up = back
    }
}

TEST(ReflectionProbe, CellsTileTheAtlas) {
    bool used[3][2] = {};
    for (std::size_t f = 0; f < kProbeFaces.size(); ++f) {
        const ProbeCell c = probeCell(f, 256);
        EXPECT_EQ(c.size, 256u);
        used[c.x / 256][c.y / 256] = true;
    }
    for (auto& col : used)
        for (bool u : col)
            EXPECT_TRUE(u);
}

// The shader's lookup finds a direction where its face drew it.
TEST(ReflectionProbe, LookupMatchesWhereTheFacesDraw) {
    const Vec3 center{120.0f, 3.0f, -45.0f};
    constexpr std::uint32_t size = 256;
    int checked = 0;
    for (int i = 0; i < 400; ++i) {
        // A spread of directions (golden-angle spiral over the sphere).
        const float t = (static_cast<float>(i) + 0.5f) / 400.0f;
        const float y = 1.0f - 2.0f * t;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float a = 2.3999632f * static_cast<float>(i);
        const Vec3 d{r * std::cos(a), y, r * std::sin(a)};
        const ProbeLookup l = probeLookup(d, size);
        const auto drawn = drawnAt(center, l.face, center + d * 50.0f, size);
        ASSERT_TRUE(drawn.has_value()) << i;
        // Away from the cell edges (where the lookup is held inside) they agree.
        const float tol = 1.0f / (3.0f * size) * 3.5f;
        EXPECT_NEAR(drawn->first, l.u, tol) << i;
        EXPECT_NEAR(drawn->second, l.v, tol) << i;
        ++checked;
    }
    EXPECT_EQ(checked, 400);
}

TEST(ReflectionProbe, LookupStaysInsideItsCell) {
    constexpr std::uint32_t size = 128;
    for (const Vec3& d : {Vec3{1, 1, 0.999f}, Vec3{-1, -0.999f, 1}, Vec3{0.2f, -1, 0.999f}, Vec3{0, 0, -1}}) {
        const ProbeLookup l = probeLookup(d.normalized(), size);
        const ProbeFace& f = kProbeFaces[l.face];
        const float u = l.u * 3.0f - static_cast<float>(f.column), v = l.v * 2.0f - static_cast<float>(f.row);
        EXPECT_GT(u, 1.0f / size);
        EXPECT_LT(u, 1.0f - 1.0f / size);
        EXPECT_GT(v, 1.0f / size);
        EXPECT_LT(v, 1.0f - 1.0f / size);
    }
}
