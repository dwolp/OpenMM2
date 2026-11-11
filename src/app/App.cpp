#include "app/App.h"

#include "app/Context.h"
#include "app/FrameTiming.h"
#include "app/GameData.h"
#include "app/IntroScreen.h"
#include "app/Screens.h"
#include "app/Tweaks.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/StringUtil.h"
#include "core/Trace.h"
#include "core/Version.h"
#include "platform/Clock.h"
#include "platform/Dialogs.h"
#include "platform/ImGuiPlatform.h"
#include "platform/Platform.h"
#include "render/ImageUtil.h"
#include "ui/Font.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace mm2::app {
namespace {

void applyCommandLine(const CommandLine& cl, render::DisplaySettings& d) {
    if (cl.backend) {
        render::Backend b;
        if (render::parseBackend(*cl.backend, b))
            d.backend = b;
    }
    if (cl.fullscreen)
        d.windowMode = *cl.fullscreen ? platform::WindowMode::Borderless : platform::WindowMode::Windowed;
    if (cl.width)
        d.windowWidth = *cl.width;
    if (cl.height)
        d.windowHeight = *cl.height;
    if (cl.vsync && !*cl.vsync)
        d.vsync = render::VsyncMode::Off; // -novblank
    d.sanitize();
}

void setupImGui(Context& ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (const auto font = ui::bundledFontPath("LiberationSans-Regular.ttf"); !font.empty())
        io.Fonts->AddFontFromFileTTF(str::fromPath(font).c_str(), 17.0f);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    const float scale = ctx.window().displayScale();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    platform::imgui::init(ctx.window());
    ctx.imgui = std::make_unique<render::ImGuiRenderer>(ctx.device());
}

// gfxPipeline::gfxWindowProc and gfxPipeline::Manage: while another
// application is active the original's main loop blocks in GetMessage, so the
// whole game (simulation, menus, the intro movie) stands still until it is
// activated again, and its DirectSound buffers, created without global focus,
// fall silent. OpenMM2 does the same, except in multiplayer (MM2 left the
// network session when a race lost focus, mmGameMulti's lost-focus callback;
// OpenMM2 keeps the player connected and running so the other players are not
// stalled) and in automation runs (--frames, OPENMM2_FRONTEND_SCRIPT), which
// may not have focus. The first-run setup screen (no game data yet) is
// OpenMM2's own and keeps running.
bool freezesWhenInactive(const Context& ctx) {
    return ctx.game && !ctx.commandLine.frames && !ctx.netGame && !std::getenv("OPENMM2_FRONTEND_SCRIPT");
}

// Mounts the configured game source if it is usable.
void tryMountConfiguredSource(Context& ctx) {
    const CommandLine& cl = ctx.commandLine;
    const std::string path = cl.source ? *cl.source : configuredGameSource(ctx.settings);
    if (path.empty() || cl.forceSetup)
        return;
    const auto check = checkGameSource(str::toPath(path));
    if (!check.ok) {
        log::warn("game data: {}", check.message);
        return;
    }
    std::string error;
    ctx.game = loadGameData(*check.source, &error);
    if (!ctx.game)
        log::error("game data: {}", error);
    else
        log::info("game data: {}", check.source->describe());
}

// OpenMM2's frame rate counter (F11): frames per second and milliseconds per
// frame on a dark plate in the window's top right corner, above everything.
void drawFpsCounter(double fps, double ms) {
    const ImGuiIO& io = ImGui::GetIO();
    const float size = std::max(16.0f, io.DisplaySize.y / 45.0f);
    const std::string text = std::format("{:.0f} FPS  {:.1f} ms", fps, ms);
    ImFont* font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const float pad = size * 0.35f;
    const ImVec2 at{io.DisplaySize.x - extent.x - pad * 3.0f, pad * 2.0f};
    // Green when smooth, yellow under 60, red under 30.
    const ImU32 color = fps >= 60.0   ? IM_COL32(120, 255, 120, 255)
                        : fps >= 30.0 ? IM_COL32(255, 220, 90, 255)
                                      : IM_COL32(255, 100, 90, 255);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddRectFilled({at.x - pad, at.y - pad * 0.6f}, {at.x + extent.x + pad, at.y + extent.y + pad * 0.6f},
                        IM_COL32(0, 0, 0, 150), pad * 0.6f);
    list->AddText(font, size, at, color, text.c_str());
}

} // namespace

int run(const CommandLine& cl) {
    log::setFile(paths::userDataDir() / "openmm2.log");
    log::info("{} {} starting", kProjectName, kProjectVersion);
    // OpenMM2 hitch log (OPENMM2_DEBUG_HITCHES): the game's notes of slow work.
    if (std::getenv("OPENMM2_DEBUG_HITCHES"))
        trace::setSink([](std::string_view what) { timing::note(what); });
    for (const auto& o : cl.ignoredOptions)
        log::info("command line: {} has no OpenMM2 equivalent; ignored", o);
    for (const auto& o : cl.unknownOptions)
        log::warn("command line: unknown option {}; ignored", o);

    Context ctx;
    ctx.commandLine = cl;
    ctx.settingsPath = cl.configPath ? str::toPath(*cl.configPath) : Settings::defaultPath();
    ctx.settings.load(ctx.settingsPath);
    ctx.display.load(ctx.settings.ini);
    applyCommandLine(cl, ctx.display);

    std::string error;
    platform::InitOptions init;
    init.appVersion = kProjectVersion;
    if (!platform::init(init, &error)) {
        log::error("platform: {}", error);
        platform::showMessage(platform::MessageKind::Error, "OpenMM2", "Cannot initialise SDL:\n" + error);
        return 1;
    }

    render::RendererConfig rc;
    rc.settings = ctx.display;
    rc.title = "Midtown Madness 2 (OpenMM2)";
    rc.pipelineCachePath = paths::userDataDir() / "vk_pipeline_cache.bin";
    auto renderer = render::createRenderer(rc, &error);
    if (!renderer) {
        log::error("renderer: {}", error);
        platform::showMessage(platform::MessageKind::Error, "OpenMM2",
                              "Cannot start Vulkan or OpenGL:\n" + error +
                                  "\n\nUpdate your graphics driver, or start with --backend opengl.");
        platform::shutdown();
        return 1;
    }
    ctx.renderer = std::move(*renderer);
    if (!ctx.renderer.fallbackReason.empty())
        log::warn("renderer: {}", ctx.renderer.fallbackReason);
    const auto& info = ctx.device().info();
    log::info("renderer: {} on {}", info.apiVersion, info.deviceName);

    setupImGui(ctx);
    ctx.overlay = std::make_unique<render::Overlay2D>(ctx.device());

    ctx.mixer = std::make_shared<audio::Mixer>(48000);
    if (!ctx.audioDevice.open(ctx.mixer, &error))
        log::warn("audio: {} (continuing without sound)", error);
    ctx.applyAudioSettings();

    tryMountConfiguredSource(ctx);
    std::unique_ptr<Screen> screen;
    if (ctx.game && cl.quickstart) {
        game::RaceConfig config;
        config.city = *cl.quickstart;
        // Development aid: OPENMM2_DEBUG_VEHICLE selects the car.
        if (const char* v = std::getenv("OPENMM2_DEBUG_VEHICLE"))
            config.vehicle = v;
        // OPENMM2_DEBUG_TIME (0 morning .. 3 night) and OPENMM2_DEBUG_WEATHER
        // (0 clear, 1 cloudy, 2 fog, 3 rain, 4 snow) set the cruise's light.
        if (const char* v = std::getenv("OPENMM2_DEBUG_TIME"))
            config.timeOfDay = static_cast<game::TimeOfDay>(std::clamp(std::atoi(v), 0, 3));
        if (const char* v = std::getenv("OPENMM2_DEBUG_WEATHER"))
            config.weather = static_cast<game::Weather>(std::clamp(std::atoi(v), 0, 4));
        screen = makeRaceScreen(ctx, config);
    } else if (ctx.game) {
        // The original played its logo movie (LOGOS.AVI in the game folder)
        // on every start unless started with -nomovie (OpenMM2: --skip-intro
        // or [Game] SkipIntro). It did not play it in a window (only when
        // inWindow was false); OpenMM2 draws the movie itself and plays it in
        // every window mode, except when started with the original's -window
        // or -max (parseCommandLine).
        const bool intro = !cl.skipIntro && !ctx.settings.ini.getBool("Game", "SkipIntro", false) && !cl.frames;
        screen = intro ? makeIntroScreen(ctx) : makeFrontendScreen(ctx);
    } else {
        screen = makeSetupScreen(ctx);
    }

    platform::FrameClock clock;
    platform::FrameLimiter limiter;
    limiter.setTargetFps(ctx.display.frameCap);
    int frame = 0;
    // gfxPipeline's "inactive" event flag: set when another application is
    // activated, cleared when the game is activated again (initially active).
    bool active = true;
    bool audioPaused = false;
    double fpsTime = 0.0, fpsShown = 0.0, msShown = 0.0;
    int fpsFrames = 0;
    while (!ctx.quit) {
        ctx.input.beginFrame();
        const auto ev = platform::pollEvents(&ctx.input, [](const SDL_Event& e) { platform::imgui::processEvent(e); });
        if (ev.quitRequested)
            ctx.quit = true;
        if (ev.resized)
            ctx.device().notifyResized();
        if (ev.focusLost || ev.focusGained) {
            const bool wasActive = active;
            active = ctx.window().focused();
            if (active && !wasActive)
                screen->activated(ctx);
        }
        const bool frozen = !active && !ctx.quit && freezesWhenInactive(ctx);
        if (frozen != audioPaused) {
            ctx.audioDevice.setPaused(frozen);
            audioPaused = frozen;
        }
        if (frozen) {
            // No tick: the first frame after reactivation measures the whole
            // pause, which the clock holds to MM2's 0.1 s limit.
            platform::waitForEvents(0.25);
            continue;
        }
        const double dt = clock.tick();

        render::Device& dev = ctx.device();
        bool begun = false;
        {
            const timing::Scope t("begin");
            begun = dev.beginFrame();
        }
        if (!begun) {
            platform::sleepPrecise(0.01); // minimised
            continue;
        }
        platform::imgui::newFrame();
        ImGui::NewFrame();
        {
            const timing::Scope t("update");
            screen->update(ctx, dt);
        }
        // OpenMM2: F11 toggles a frame rate counter in the top right corner
        // ([Display] ShowFps; not in MM2), averaged over half a second.
        if (ctx.input.keyPressed(platform::Key::F11)) {
            ctx.settings.ini.setBool("Display", "ShowFps", !ctx.settings.ini.getBool("Display", "ShowFps", false));
            ctx.saveSettings();
        }
        fpsTime += dt;
        ++fpsFrames;
        if (fpsTime >= 0.5) {
            fpsShown = fpsFrames / fpsTime;
            msShown = 1000.0 * fpsTime / fpsFrames;
            fpsTime = 0.0;
            fpsFrames = 0;
        }
        if (fpsShown > 0.0 && ctx.settings.ini.getBool("Display", "ShowFps", false))
            drawFpsCounter(fpsShown, msShown);
        ImGui::Render();

        // OpenMM2 tweaks: the image filters, read every frame so the Tweaks
        // page's changes show at once.
        render::PostProcess post = postProcessFromTweaks(drawnTweaks(ctx.settings.ini, ctx.originalLook));
        post.time = static_cast<float>(clock.elapsed());
        dev.setPostProcess(post);
        if (screen->usesScene()) {
            const timing::Scope t("scene");
            screen->drawScene(ctx);
        }
        {
            const timing::Scope t("overlay");
            dev.beginOverlay({0, 0, 0, 1});
            screen->drawOverlay(ctx);
            ctx.imgui->render(ImGui::GetDrawData());
            dev.endOverlay();
        }

        const bool lastFrame = (cl.frames && frame + 1 >= *cl.frames) || ctx.lastFrameRequested;
        // Development aid: OPENMM2_DEBUG_SHOT_FRAMES=<first>-<last> saves those
        // frames too, numbered by frame (with --screenshot), to look at what
        // changes from one frame to the next (flicker).
        if (static const auto range = [] {
                std::pair<long long, long long> r{-1, -1};
                if (const char* v = std::getenv("OPENMM2_DEBUG_SHOT_FRAMES")) {
                    const auto parts = str::split(v, '-');
                    if (parts.size() == 2)
                        r = {str::parseInt(parts[0]).value_or(-1), str::parseInt(parts[1]).value_or(-1)};
                }
                return r;
            }();
            static_cast<long long>(frame) >= range.first && static_cast<long long>(frame) <= range.second)
            ctx.captureTag = static_cast<int>(frame);
        const int tag = std::exchange(ctx.captureTag, -1);
        // OpenMM2: F12 saves a screenshot to <user data>/screenshots.
        const bool userShot = ctx.input.keyPressed(platform::Key::F12);
        if (((lastFrame || tag >= 0) && cl.screenshot) || userShot)
            dev.requestCapture();
        limiter.wait();
        {
            const timing::Scope t("end");
            dev.endFrame();
        }
        timing::endFrame();
        ++frame;

        if (userShot) {
            const auto dir = paths::userDataDir() / "screenshots";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const auto now = std::chrono::system_clock::now();
            const auto path = dir / std::format("openmm2-{:%Y%m%d-%H%M%S}.png",
                                                std::chrono::floor<std::chrono::seconds>(now));
            render::Image image;
            if (dev.readCapture(image) && render::writePng(path, image))
                log::info("screenshot: {}", str::fromPath(path));
            else
                log::error("screenshot: capture failed");
        }

        if (tag >= 0 && !lastFrame && cl.screenshot) {
            auto path = str::toPath(*cl.screenshot);
            path.replace_filename(
                std::format("{}-{}{}", path.stem().string(), tag, path.extension().string()));
            render::Image image;
            if (dev.readCapture(image) && render::writePng(path, image))
                log::info("screenshot: {}", path.string());
            else
                log::error("screenshot: capture failed");
        }

        if (lastFrame) {
            if (cl.screenshot) {
                render::Image image;
                if (dev.readCapture(image) && render::writePng(str::toPath(*cl.screenshot), image))
                    log::info("screenshot: {}", *cl.screenshot);
                else
                    log::error("screenshot: capture failed");
            }
            ctx.quit = true;
        }
        for (auto& fn : std::exchange(ctx.afterFrame, {}))
            fn();
        if (ctx.nextScreen)
            screen = std::move(ctx.nextScreen);
    }

    ctx.device().waitIdle();
    screen.reset();
    ctx.saveSettings();
    ctx.shutdownMusic();
    ctx.audioDevice.close();
    ctx.imgui.reset();
    ctx.overlay.reset();
    platform::imgui::shutdown();
    ImGui::DestroyContext();
    ctx.renderer.device.reset();
    ctx.renderer.window.reset();
    platform::shutdown();
    log::info("bye");
    return 0;
}

} // namespace mm2::app
