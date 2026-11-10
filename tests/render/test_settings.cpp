#include "core/Ini.h"
#include "render/DisplaySettings.h"
#include "render/GpuConstants.h"
#include "render/HandleTable.h"
#include "render/Types.h"

#include <gtest/gtest.h>

#include <set>

using namespace mm2;
using namespace mm2::render;

TEST(DisplaySettings, RoundTripsThroughIni) {
    DisplaySettings s;
    s.backend = Backend::OpenGL;
    s.windowMode = platform::WindowMode::Fullscreen;
    s.display = 1;
    s.windowWidth = 1600;
    s.windowHeight = 900;
    s.fullscreenMode = {2560, 1440, 143.9f, 1.0f};
    s.vsync = VsyncMode::Mailbox;
    s.frameCap = 120;
    s.msaa = 8;
    s.anisotropy = 16;
    s.renderScale = 1.5f;
    s.uiScale = UiScaleMode::Integer;
    s.fovMode = FovMode::VertMinus;
    s.maxAspect = 2.4f;
    s.gpu = 1;
    s.validation = true;

    IniFile ini;
    s.save(ini);
    IniFile reread;
    reread.parse(ini.serialize());
    DisplaySettings r;
    r.load(reread);
    EXPECT_EQ(r.backend, s.backend);
    EXPECT_EQ(r.windowMode, s.windowMode);
    EXPECT_EQ(r.display, 1);
    EXPECT_EQ(r.windowWidth, 1600);
    EXPECT_EQ(r.fullscreenMode.width, 2560);
    EXPECT_NEAR(r.fullscreenMode.refreshRate, 143.9f, 1e-3f);
    EXPECT_EQ(r.vsync, VsyncMode::Mailbox);
    EXPECT_EQ(r.frameCap, 120);
    EXPECT_EQ(r.msaa, 8u);
    EXPECT_EQ(r.anisotropy, 16u);
    EXPECT_FLOAT_EQ(r.renderScale, 1.5f);
    EXPECT_EQ(r.uiScale, UiScaleMode::Integer);
    EXPECT_EQ(r.fovMode, FovMode::VertMinus);
    EXPECT_FLOAT_EQ(r.maxAspect, 2.4f);
    EXPECT_EQ(r.gpu, 1);
    EXPECT_TRUE(r.validation);
}

TEST(DisplaySettings, ToleratesHandEditedValues) {
    IniFile ini;
    ini.parse("[Display]\nBackend=GL\nMode=BORDERLESS\nVSync=0\nMSAA=6\nAnisotropy=99\nRenderScale=9\n"
              "FrameCap=3\nWindowWidth=10\nFOV=bogus\nUIScale=Stretch\n");
    DisplaySettings s;
    s.load(ini);
    EXPECT_EQ(s.backend, Backend::OpenGL);
    EXPECT_EQ(s.windowMode, platform::WindowMode::Borderless);
    EXPECT_EQ(s.vsync, VsyncMode::Off);
    EXPECT_EQ(s.msaa, 4u);        // rounded down to a power of two
    EXPECT_EQ(s.anisotropy, 16u); // clamped
    EXPECT_FLOAT_EQ(s.renderScale, 2.0f);
    EXPECT_EQ(s.frameCap, 10);
    EXPECT_EQ(s.windowWidth, 320);
    EXPECT_EQ(s.fovMode, FovMode::HorPlus); // unknown value keeps the default
    EXPECT_EQ(s.uiScale, UiScaleMode::Stretch);
}

TEST(PipelineState, KeysAreDistinct) {
    std::set<std::uint32_t> keys;
    int count = 0;
    for (int fmt = 0; fmt < 2; ++fmt)
        for (int blend = 0; blend < 5; ++blend)
            for (int cull = 0; cull < 3; ++cull)
                for (int dt = 0; dt < 2; ++dt)
                    for (int dw = 0; dw < 2; ++dw)
                        for (int bias = 0; bias < 2; ++bias) {
                            PipelineState s;
                            s.vertexFormat = static_cast<VertexFormat>(fmt);
                            s.blend = static_cast<BlendMode>(blend);
                            s.cull = static_cast<CullMode>(cull);
                            s.depthTest = dt;
                            s.depthWrite = dw;
                            s.depthBias = bias;
                            keys.insert(s.key());
                            ++count;
                        }
    EXPECT_EQ(static_cast<int>(keys.size()), count);
}

TEST(Types, PackColor) {
    EXPECT_EQ(packColor(0x11, 0x22, 0x33, 0x44), 0x44332211u);
    EXPECT_EQ(packColor(Vec4{1, 0, 0, 1}), 0xFF0000FFu);
}

TEST(HandleTable, ReusesSlots) {
    detail::HandleTable<int> t;
    const auto a = t.insert(10), b = t.insert(20);
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(*t.get(b), 20);
    EXPECT_EQ(t.remove(a), 10);
    EXPECT_EQ(t.get(a), nullptr);
    EXPECT_EQ(t.get(0), nullptr);
    EXPECT_EQ(t.insert(30), a);
    int sum = 0;
    t.forEach([&](std::uint32_t, int& v) { sum += v; });
    EXPECT_EQ(sum, 50);
}

// Motion blur eases in with the camera's speed: none at a crawl, full at speed.
TEST(MotionBlur, EasesInWithSpeed) {
    const Mat44 proj = Mat44::perspective(1.0f, 16.0f / 9.0f, 0.1f, 500.0f, true);
    PostProcess p;
    p.motionBlur = 1.0f;
    const float dt = 1.0f / 60.0f;
    auto exposure = [&](float speed) {
        Mat34 prev, now;
        now.m3 = {0.0f, 0.0f, -speed * dt};
        return detail::motionBlurConstants(proj, now, proj, prev, p, dt).params[3];
    };
    EXPECT_FLOAT_EQ(exposure(3.0f), 0.0f);  // 7 mph
    EXPECT_GT(exposure(10.0f), 0.0f);
    EXPECT_LT(exposure(10.0f), 1.0f);
    EXPECT_FLOAT_EQ(exposure(20.0f), 1.0f); // 45 mph
}

// The followed car reaches the blur in the camera's view space, so the
// ground around it (its shadow) stays sharp; without one, none.
TEST(MotionBlur, FocusInViewSpace) {
    const Mat44 proj = Mat44::perspective(1.0f, 16.0f / 9.0f, 0.1f, 500.0f, true);
    PostProcess p;
    p.motionBlur = 1.0f;
    Mat34 camera; // at (10, 3, 0), looking down -Z
    camera.m3 = {10.0f, 3.0f, 0.0f};
    const auto c = detail::motionBlurConstants(proj, camera, proj, camera, p, 1.0f / 60.0f, Vec3{10.0f, 1.0f, -8.0f});
    EXPECT_FLOAT_EQ(c.focus[0], 0.0f);
    EXPECT_FLOAT_EQ(c.focus[1], -2.0f);
    EXPECT_FLOAT_EQ(c.focus[2], -8.0f);
    EXPECT_GT(c.focus[3], 0.0f);
    EXPECT_FLOAT_EQ(detail::motionBlurConstants(proj, camera, proj, camera, p, 1.0f / 60.0f).focus[3], 0.0f);
}
