// rendertest: exercises the platform layer and both render backends.
//
// Draws a lit, textured, fogged spinning cube over a ground plane, an
// alpha-tested quad, a 640x480 overlay (letterboxed to the window) and a
// Dear ImGui panel for switching display settings live.
//
//   rendertest [--backend auto|vulkan|opengl] [--frames N --screenshot out.png]
//              [--width W --height H] [--mode windowed|borderless|fullscreen]
//              [--msaa N] [--scale S] [--vsync on|off|adaptive|mailbox]
//              [--ui fit|stretch|integer] [--fov hor+|vert-|stretch] [--validation]
//              [--no-ui] [--stress] [--post]
//
// --stress walks through MSAA, render scale, vsync, anisotropy, window size
// and mode changes and finally a backend switch while rendering.
//
// With --frames the animation is driven by the frame index (60 Hz steps), so
// screenshots from different backends are directly comparable. The exit code
// is non-zero if the graphics API reported validation errors.

#include "core/Log.h"
#include "core/StringUtil.h"
#include "platform/Clock.h"
#include "platform/Dialogs.h"
#include "platform/ImGuiPlatform.h"
#include "platform/Input.h"
#include "platform/Platform.h"
#include "render/ImGuiRenderer.h"
#include "render/ImageUtil.h"
#include "render/Overlay2D.h"
#include "render/Projection.h"
#include "render/Renderer.h"

#include <SDL3/SDL_main.h>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <vector>

using namespace mm2;
using namespace mm2::render;

namespace {

struct Options {
    DisplaySettings settings;
    int frames = 0;
    std::string screenshot;
    bool showUi = true;
    bool stress = false; // change settings while running (exercises recreation paths)
    bool post = false;   // every OpenMM2 image filter on, bloom included (render::PostProcess)
};

bool parseArgs(int argc, char** argv, Options& o) {
    o.settings.windowWidth = 1280;
    o.settings.windowHeight = 720;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string_view { return i + 1 < argc ? std::string_view(argv[++i]) : ""; };
        if (a == "--backend") {
            if (!parseBackend(next(), o.settings.backend))
                return false;
        } else if (a == "--frames") {
            o.frames = static_cast<int>(str::parseInt(next()).value_or(0));
        } else if (a == "--screenshot") {
            o.screenshot = next();
        } else if (a == "--width") {
            o.settings.windowWidth = static_cast<int>(str::parseInt(next()).value_or(1280));
        } else if (a == "--height") {
            o.settings.windowHeight = static_cast<int>(str::parseInt(next()).value_or(720));
        } else if (a == "--mode") {
            if (!platform::parseWindowMode(next(), o.settings.windowMode))
                return false;
        } else if (a == "--msaa") {
            o.settings.msaa = static_cast<std::uint32_t>(str::parseInt(next()).value_or(4));
        } else if (a == "--scale") {
            o.settings.renderScale = static_cast<float>(str::parseDouble(next()).value_or(1.0));
        } else if (a == "--vsync") {
            if (!parseVsyncMode(next(), o.settings.vsync))
                return false;
        } else if (a == "--ui") {
            if (!parseUiScaleMode(next(), o.settings.uiScale))
                return false;
        } else if (a == "--fov") {
            if (!parseFovMode(next(), o.settings.fovMode))
                return false;
        } else if (a == "--validation") {
            o.settings.validation = true;
        } else if (a == "--no-ui") {
            o.showUi = false;
        } else if (a == "--stress") {
            o.stress = true;
        } else if (a == "--post") {
            o.post = true;
        } else {
            return false;
        }
    }
    o.settings.sanitize();
    return true;
}

// Camera transform looking from `eye` to `target` (objects face -Z).
Mat34 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Mat34 m;
    m.m2 = (eye - target).normalized();
    m.m0 = up.cross(m.m2).normalized();
    m.m1 = m.m2.cross(m.m0);
    m.m3 = eye;
    return m;
}

Image makeChecker(std::uint32_t size, bool holes) {
    Image img;
    img.width = img.height = size;
    img.pixels.resize(static_cast<std::size_t>(size) * size * 4);
    const std::uint32_t cell = size / 8;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool odd = ((x / cell) + (y / cell)) & 1;
            std::uint8_t* p = &img.pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            const std::uint8_t r = odd ? 230 : 40, g = odd ? 180 : 90, b = odd ? 40 : 200;
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
            if (holes) {
                const float cx = static_cast<float>(x % cell) - cell * 0.5f;
                const float cy = static_cast<float>(y % cell) - cell * 0.5f;
                p[3] = (cx * cx + cy * cy) < (cell * 0.3f) * (cell * 0.3f) ? 0 : 255;
            }
        }
    }
    return img;
}

struct Mesh {
    BufferHandle vb, ib;
    std::uint32_t indexCount = 0;
};

Mesh makeCube(Device& dev) {
    std::vector<Vertex3D> v;
    std::vector<std::uint16_t> idx;
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const std::uint32_t colors[6] = {packColor(255, 120, 120), packColor(120, 255, 120), packColor(120, 120, 255),
                                     packColor(255, 255, 120), packColor(255, 120, 255), packColor(120, 255, 255)};
    for (int f = 0; f < 6; ++f) {
        const Vec3 n = normals[f];
        // Tangent frame so the face's vertices wind counter-clockwise seen from outside.
        const Vec3 u = std::abs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 a = u.cross(n), b = n.cross(a);
        const auto base = static_cast<std::uint16_t>(v.size());
        const Vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int c = 0; c < 4; ++c) {
            const Vec3 p = n + a * corners[c].x + b * corners[c].y;
            Vertex3D vx{};
            vx.position[0] = p.x;
            vx.position[1] = p.y;
            vx.position[2] = p.z;
            vx.normal[0] = n.x;
            vx.normal[1] = n.y;
            vx.normal[2] = n.z;
            vx.color = colors[f];
            vx.uv0[0] = corners[c].x * 0.5f + 0.5f;
            vx.uv0[1] = 0.5f - corners[c].y * 0.5f;
            v.push_back(vx);
        }
        for (std::uint16_t i : {0, 1, 2, 0, 2, 3})
            idx.push_back(static_cast<std::uint16_t>(base + i));
    }
    Mesh m;
    m.vb = dev.createBuffer(BufferKind::Vertex, v.size() * sizeof(Vertex3D), v.data());
    m.ib = dev.createBuffer(BufferKind::Index, idx.size() * sizeof(std::uint16_t), idx.data());
    m.indexCount = static_cast<std::uint32_t>(idx.size());
    return m;
}

Mesh makeQuad(Device& dev, float half, float uvScale) {
    // XZ plane facing +Y, counter-clockwise from above.
    const Vec3 c[4] = {{-half, 0, half}, {half, 0, half}, {half, 0, -half}, {-half, 0, -half}};
    std::vector<Vertex3D> v;
    for (int i = 0; i < 4; ++i) {
        Vertex3D vx{};
        vx.position[0] = c[i].x;
        vx.position[1] = c[i].y;
        vx.position[2] = c[i].z;
        vx.normal[1] = 1.0f;
        vx.color = 0xFFFFFFFFu;
        vx.uv0[0] = (c[i].x / half * 0.5f + 0.5f) * uvScale;
        vx.uv0[1] = (c[i].z / half * 0.5f + 0.5f) * uvScale;
        v.push_back(vx);
    }
    const std::uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    Mesh m;
    m.vb = dev.createBuffer(BufferKind::Vertex, v.size() * sizeof(Vertex3D), v.data());
    m.ib = dev.createBuffer(BufferKind::Index, sizeof(idx), idx);
    m.indexCount = 6;
    return m;
}

struct Scene {
    Mesh cube, ground, card;
    TextureHandle checker, holes;
    void create(Device& dev) {
        cube = makeCube(dev);
        ground = makeQuad(dev, 40.0f, 20.0f);
        card = makeQuad(dev, 1.2f, 1.0f);
        checker = createTextureWithMips(dev, makeChecker(256, false), "checker");
        holes = createTextureWithMips(dev, makeChecker(128, true), "holes");
    }
    void destroy(Device& dev) {
        for (Mesh* m : {&cube, &ground, &card}) {
            dev.destroyBuffer(m->vb);
            dev.destroyBuffer(m->ib);
        }
        dev.destroyTexture(checker);
        dev.destroyTexture(holes);
    }
};

// `shadows`: the OpenMM2 sun shadow pass first (rendertest --post).
void drawScene(Device& dev, const Scene& s, const DisplaySettings& ds, double t, bool shadows) {
    const Extent2D ext = dev.sceneExtent();
    const Vec4 sky{0.55f, 0.65f, 0.8f, 1.0f};
    ClearValues cv;
    cv.color = sky;

    const float aspect = static_cast<float>(ext.width) / static_cast<float>(std::max<std::uint32_t>(ext.height, 1));
    const ProjectionParams pp = computeProjection(70.0f * kDegToRad, aspect, ds.fovMode, ds.maxAspect);
    FrameConstants fc;
    // --post orbits the camera, so the motion blur has motion to blur.
    const float orbit = shadows ? static_cast<float>(t) * 1.5f : 0.0f;
    const Mat34 cam = lookAt({6.0f * std::sin(orbit), 2.2f, 6.0f * std::cos(orbit)}, {0.0f, 0.3f, 0.0f}, {0, 1, 0});
    fc.view = Mat44::fromMat34(cam.fastInverse());
    fc.proj = Mat44::perspective(pp.fovY, pp.aspect, 0.1f, 500.0f, true);
    dev.setSceneProjection(fc.proj); // for ambient occlusion with --post
    dev.setSceneCamera(cam);           // for the wet roads and motion blur with --post
    fc.cameraPosition = cam.m3;
    fc.fogMode = FogMode::Linear;
    fc.fogColor = sky.xyz();
    fc.fogStart = 8.0f;
    fc.fogEnd = 40.0f;
    fc.ambient = {0.35f, 0.35f, 0.4f};
    fc.lights[0] = {Vec3{-0.4f, -1.0f, -0.6f}, Vec3{0.8f, 0.75f, 0.65f}};
    fc.lights[1] = {Vec3{0.6f, -0.2f, 0.5f}, Vec3{0.2f, 0.25f, 0.35f}};

    std::vector<DrawCall> calls;
    // Ground: textured, fogged, lit.
    DrawCall ground;
    ground.vertices = {s.ground.vb, 0};
    ground.indices = {s.ground.ib, 0};
    ground.count = s.ground.indexCount;
    ground.constants.world = Mat44::fromMat34(Mat34::translation({0, -1.0f, 0}));
    ground.constants.flags = DrawFlag::Texture0 | DrawFlag::VertexColor | DrawFlag::Fog | DrawFlag::Lighting;
    ground.textures[0] = {s.checker, {Filter::Trilinear, AddressMode::Wrap, AddressMode::Wrap}};
    calls.push_back(ground);

    // Spinning cube.
    const auto angle = static_cast<float>(t * 0.8);
    DrawCall cube;
    cube.vertices = {s.cube.vb, 0};
    cube.indices = {s.cube.ib, 0};
    cube.count = s.cube.indexCount;
    cube.constants.world =
        Mat44::fromMat34(Mat34::rotationX(angle * 0.5f) * Mat34::rotationY(angle) * Mat34::translation({0, 0.3f, 0}));
    cube.constants.flags = DrawFlag::Texture0 | DrawFlag::VertexColor | DrawFlag::Fog | DrawFlag::Lighting;
    cube.textures[0] = {s.checker, {Filter::Trilinear, AddressMode::Wrap, AddressMode::Wrap}};
    calls.push_back(cube);

    // Alpha-tested, double-sided card standing to the right.
    DrawCall card;
    card.state.cull = CullMode::None;
    card.vertices = {s.card.vb, 0};
    card.indices = {s.card.ib, 0};
    card.count = s.card.indexCount;
    card.constants.world = Mat44::fromMat34(Mat34::rotationX(kHalfPi) * Mat34::rotationY(-0.5f) *
                                            Mat34::translation({2.6f, 0.2f, -0.5f}));
    card.constants.flags = DrawFlag::Texture0 | DrawFlag::AlphaTest | DrawFlag::Fog;
    card.textures[0] = {s.holes, {Filter::Bilinear, AddressMode::Wrap, AddressMode::Wrap}};
    calls.push_back(card);

    // Translucent card on the left (alpha blending, no depth write).
    DrawCall glass = card;
    glass.state.blend = BlendMode::Alpha;
    glass.state.depthWrite = false;
    glass.constants.world = Mat44::fromMat34(Mat34::rotationX(kHalfPi) * Mat34::rotationY(0.5f) *
                                             Mat34::translation({-2.6f, 0.2f, -0.5f}));
    glass.constants.color = {0.3f, 0.9f, 1.0f, 0.5f};
    glass.constants.flags = DrawFlag::Fog;
    calls.push_back(glass);

    if (shadows) {
        // An orthographic view along the key light over the 16 m around the
        // origin (as game::sunShadowView, without the snapping).
        constexpr std::uint32_t kSize = 1024;
        const Vec3 l = fc.lights[0].direction.normalized();
        const Mat34 light = lookAt(l * -20.0f, {0.0f, 0.0f, 0.0f}, {0, 1, 0});
        FrameConstants sf = fc;
        sf.view = Mat44::fromMat34(light.fastInverse());
        sf.proj = Mat44::orthographic(-8.0f, 8.0f, -8.0f, 8.0f, 0.0f, 40.0f, true);
        sf.cameraPosition = light.m3;
        if (dev.beginShadowPass(kSize)) {
            dev.setFrameConstants(sf);
            for (const DrawCall& c : calls)
                dev.draw(c);
            dev.endShadowPass();
            fc.shadowViewProj = sf.view * sf.proj;
            fc.shadowStrength = 0.6f;
            fc.shadowTexel = 16.0f / static_cast<float>(kSize);
            fc.shadowSize = static_cast<float>(kSize);
        }
    }
    dev.beginScene(cv);
    dev.setFrameConstants(fc);
    for (const DrawCall& c : calls)
        dev.draw(c);
    dev.endScene();
}

void drawOverlay(Overlay2D& ov, const Scene& s, UiScaleMode mode) {
    ov.begin(mode);
    const UiLayout& l = ov.layout();
    // Outline of the 640x480 safe area.
    const std::uint32_t edge = packColor(255, 255, 255, 160);
    ov.rect(0, 0, 640, 2, edge);
    ov.rect(0, 478, 640, 2, edge);
    ov.rect(0, 0, 2, 480, edge);
    ov.rect(638, 0, 2, 480, edge);
    // Bottom bar and a textured "logo".
    ov.rect(0, 430, 640, 50, packColor(0, 0, 0, 140));
    ov.image(s.checker, 16, 16, 96, 96, {0, 0}, {1, 1}, packColor(255, 255, 255, 220));
    // Markers anchored to the real screen edges (outside 0..640 on wide screens).
    ov.rect(l.left + 8, 200, 24, 80, packColor(255, 60, 60, 220));
    ov.rect(l.right - 32, 200, 24, 80, packColor(60, 255, 60, 220));
    ov.end();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        std::println(stderr, "usage: rendertest [--backend auto|vulkan|opengl] [--frames N] [--screenshot out.png]\n"
                             "                  [--width W] [--height H] [--mode windowed|borderless|fullscreen]\n"
                             "                  [--msaa N] [--scale S] [--vsync on|off|adaptive|mailbox]\n"
                             "                  [--ui fit|stretch|integer] [--fov hor+|vert-|stretch]\n"
                             "                  [--validation] [--no-ui]");
        return 2;
    }
    if (const char* lvl = std::getenv("OPENMM2_LOG")) {
        log::Level l;
        if (log::parseLevel(lvl, l))
            log::setLevel(l);
    }
    std::string err;
    if (!platform::init({}, &err)) {
        std::println(stderr, "platform init failed: {}", err);
        return 1;
    }

    RendererConfig rc;
    rc.settings = opt.settings;
    rc.title = "OpenMM2 render test";
    auto renderer = createRenderer(rc, &err);
    if (!renderer) {
        platform::showMessage(platform::MessageKind::Error, "OpenMM2", "Cannot start the renderer:\n" + err);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(renderer->window->displayScale());
    ImGui::GetStyle().FontScaleDpi = renderer->window->displayScale();

    auto imguiRenderer = std::make_unique<ImGuiRenderer>(*renderer->device);
    platform::imgui::init(*renderer->window);
    auto overlay = std::make_unique<Overlay2D>(*renderer->device);
    Scene scene;
    scene.create(*renderer->device);

    DisplaySettings current = opt.settings;
    DisplaySettings pending = current;
    platform::Input input;
    platform::FrameClock clock;
    platform::FrameLimiter limiter;
    limiter.setTargetFps(current.frameCap);
    const auto displays = platform::enumerateDisplays();
    double t = 0.0;
    int frame = 0;
    bool running = true;
    std::uint32_t apiErrors = 0;
    FrameStats lastStats; // stats of the previous (complete) frame, for display

    while (running) {
        input.beginFrame();
        const auto ev = platform::pollEvents(&input, [](const SDL_Event& e) { platform::imgui::processEvent(e); });
        if (ev.quitRequested || input.keyPressed(platform::Key::Escape))
            running = false;
        if (ev.resized)
            renderer->device->notifyResized();
        if (input.keyPressed(platform::Key::F1))
            opt.showUi = !opt.showUi;

        const double dt = clock.tick();
        t = opt.frames > 0 ? frame / 60.0 : t + dt;

        Device& dev = *renderer->device;
        if (!dev.beginFrame()) {
            platform::sleepPrecise(0.01);
            continue;
        }

        platform::imgui::newFrame();
        ImGui::NewFrame();
        bool apply = false;
        if (opt.showUi) {
            ImGui::SetNextWindowPos({20, 140}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Display");
            const DeviceInfo& di = dev.info();
            ImGui::Text("%s", di.apiVersion.c_str());
            ImGui::Text("%s", di.deviceName.c_str());
            ImGui::Text("Output %ux%u  Scene %ux%u", dev.outputExtent().width, dev.outputExtent().height,
                        dev.sceneExtent().width, dev.sceneExtent().height);
            ImGui::Text("%.1f fps  %u draws  %u pipelines", ImGui::GetIO().Framerate, lastStats.drawCalls,
                        lastStats.pipelineBinds);
            ImGui::Separator();
            int mode = static_cast<int>(pending.windowMode);
            ImGui::Combo("Mode", &mode, "Windowed\0Borderless\0Fullscreen\0");
            pending.windowMode = static_cast<platform::WindowMode>(mode);
            if (!displays.empty()) {
                const auto& d = displays[static_cast<std::size_t>(std::clamp(pending.display, 0, static_cast<int>(displays.size()) - 1))];
                const std::string cur = std::format("{}x{} @ {:.0f}", pending.fullscreenMode.width,
                                                    pending.fullscreenMode.height, pending.fullscreenMode.refreshRate);
                if (ImGui::BeginCombo("Fullscreen mode", pending.fullscreenMode.width ? cur.c_str() : "Desktop")) {
                    if (ImGui::Selectable("Desktop"))
                        pending.fullscreenMode = {};
                    for (const auto& m : d.modes) {
                        const std::string label = std::format("{}x{} @ {:.2f} Hz", m.width, m.height, m.refreshRate);
                        if (ImGui::Selectable(label.c_str()))
                            pending.fullscreenMode = m;
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::InputInt("Window width", &pending.windowWidth);
            ImGui::InputInt("Window height", &pending.windowHeight);
            int vs = static_cast<int>(pending.vsync);
            ImGui::Combo("VSync", &vs, "Off\0On\0Adaptive\0Mailbox\0");
            pending.vsync = static_cast<VsyncMode>(vs);
            int msaaIdx = pending.msaa >= 8 ? 3 : pending.msaa >= 4 ? 2 : pending.msaa >= 2 ? 1 : 0;
            ImGui::Combo("MSAA", &msaaIdx, "Off\0" "2x\0" "4x\0" "8x\0");
            pending.msaa = 1u << msaaIdx;
            ImGui::SliderFloat("Render scale", &pending.renderScale, 0.25f, 2.0f, "%.2f");
            int aniso = static_cast<int>(pending.anisotropy);
            ImGui::SliderInt("Anisotropy", &aniso, 1, 16);
            pending.anisotropy = static_cast<std::uint32_t>(aniso);
            int ui = static_cast<int>(pending.uiScale);
            ImGui::Combo("UI scale", &ui, "Fit\0Stretch\0Integer\0");
            pending.uiScale = static_cast<UiScaleMode>(ui);
            int fov = static_cast<int>(pending.fovMode);
            ImGui::Combo("FOV", &fov, "Hor+\0Vert-\0Stretch\0");
            pending.fovMode = static_cast<FovMode>(fov);
            ImGui::InputInt("Frame cap", &pending.frameCap);
            apply = ImGui::Button("Apply");
            ImGui::End();
        }
        ImGui::Render();

        if (opt.stress && frame > 0 && frame % 10 == 0) {
            pending = current;
            switch (frame / 10) {
            case 1: pending.msaa = 8; pending.renderScale = 0.75f; break;
            case 2: pending.vsync = VsyncMode::Mailbox; pending.anisotropy = 1; break;
            case 3: pending.windowWidth = 800; pending.windowHeight = 600; pending.msaa = 1; break;
            case 4: pending.windowMode = platform::WindowMode::Borderless; break;
            case 5: pending.windowMode = platform::WindowMode::Fullscreen; break;
            case 6: pending.windowMode = platform::WindowMode::Windowed; pending.windowWidth = 1280; pending.windowHeight = 720; pending.renderScale = 1.5f; pending.msaa = 2; break;
            case 7:
                pending.backend = renderer->device->backend() == Backend::Vulkan ? Backend::OpenGL : Backend::Vulkan;
                break;
            default: break;
            }
            std::println("  (step {} result: output {}x{}, scene {}x{}, window mode {})", frame / 10 - 1,
                         renderer->device->outputExtent().width, renderer->device->outputExtent().height,
                         renderer->device->sceneExtent().width, renderer->device->sceneExtent().height,
                         platform::windowModeName(renderer->window->mode()));
            std::println("stress step {}: msaa {} scale {} vsync {} mode {} {}x{} backend {}", frame / 10, pending.msaa,
                         pending.renderScale, vsyncModeName(pending.vsync), platform::windowModeName(pending.windowMode),
                         pending.windowWidth, pending.windowHeight, backendName(pending.backend));
            apply = true;
        }

        if (opt.post) {
            PostProcess pp;
            pp.contrast = 1.1f;
            pp.saturation = 1.2f;
            pp.vignette = 0.3f;
            pp.sharpen = 0.5f;
            pp.fxaa = true;
            pp.bloom = 0.7f;
            pp.ao = 1.0f;
            pp.motionBlur = 1.0f;
            pp.time = static_cast<float>(t);
            dev.setPostProcess(pp);
        }
        drawScene(dev, scene, current, t, opt.post);
        dev.beginOverlay({0, 0, 0, 1});
        drawOverlay(*overlay, scene, current.uiScale);
        imguiRenderer->render(ImGui::GetDrawData());
        dev.endOverlay();

        const bool last = opt.frames > 0 && frame + 1 >= opt.frames;
        if (last && !opt.screenshot.empty())
            dev.requestCapture();
        limiter.wait();
        dev.endFrame();
        apiErrors = dev.stats().apiErrors;
        lastStats = dev.stats();
        ++frame;

        if (last) {
            if (!opt.screenshot.empty()) {
                Image shot;
                if (dev.readCapture(shot) && writePng(str::toPath(opt.screenshot), shot))
                    std::println("screenshot {}x{} -> {}", shot.width, shot.height, opt.screenshot);
                else
                    std::println(stderr, "screenshot failed");
            }
            running = false;
        }

        if (apply) {
            pending.sanitize();
            if (!applyDisplaySettings(*renderer, current, pending)) {
                // Backend/GPU change: rebuild everything on a new renderer.
                dev.waitIdle();
                scene.destroy(dev);
                overlay.reset();
                imguiRenderer.reset();
                platform::imgui::shutdown();
                renderer.reset();
                rc.settings = pending;
                renderer = createRenderer(rc, &err);
                if (!renderer) {
                    std::println(stderr, "renderer recreation failed: {}", err);
                    return 1;
                }
                imguiRenderer = std::make_unique<ImGuiRenderer>(*renderer->device);
                platform::imgui::init(*renderer->window);
                overlay = std::make_unique<Overlay2D>(*renderer->device);
                scene.create(*renderer->device);
            }
            current = pending;
            limiter.setTargetFps(current.frameCap);
        }
    }

    renderer->device->waitIdle();
    scene.destroy(*renderer->device);
    overlay.reset();
    imguiRenderer.reset();
    platform::imgui::shutdown();
    ImGui::DestroyContext();
    const std::string backend = backendName(renderer->device->backend());
    renderer.reset();
    platform::shutdown();
    std::println("backend {} frames {} api-errors {}", backend, frame, apiErrors);
    return apiErrors ? 3 : 0;
}
