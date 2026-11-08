#pragma once

#include "app/CommandLine.h"
#include "app/Settings.h"
#include "audio/AudioDevice.h"
#include "audio/Music.h"
#include "audio/Mixer.h"
#include "game/Catalog.h"
#include "game/Strings.h"
#include "game/net/NetGame.h"
#include "platform/Input.h"
#include "render/DisplaySettings.h"
#include "render/ImGuiRenderer.h"
#include "render/Overlay2D.h"
#include "render/Renderer.h"
#include "vfs/GameSource.h"
#include "vfs/Vfs.h"

#include <filesystem>
#include <functional>
#include <vector>
#include <memory>
#include <optional>

namespace mm2::app {

class Screen;

// The mounted original game data and the tables every screen needs.
struct GameData {
    vfs::GameSource source;
    vfs::Vfs vfs;
    game::Catalog catalog;
    game::Strings strings;
};

// Long-lived state shared by all screens.
struct Context {
    CommandLine commandLine;
    Settings settings;
    std::filesystem::path settingsPath;
    render::DisplaySettings display;

    render::Renderer renderer;
    platform::Input input;
    std::unique_ptr<render::Overlay2D> overlay;
    std::unique_ptr<render::ImGuiRenderer> imgui;

    std::shared_ptr<audio::Mixer> mixer;
    audio::AudioDevice audioDevice;

    std::unique_ptr<GameData> game; // null until a usable game source is mounted

    // Multiplayer: exists while the multiplayer menus are open and during
    // multiplayer races (see docs/multiplayer.md).
    std::unique_ptr<game::NetGame> netGame;

    render::Device& device() { return *renderer.device; }
    platform::Window& window() { return *renderer.window; }

    // Work to run after the current frame is presented (e.g. applying new
    // display settings outside of any render pass).
    std::vector<std::function<void()>> afterFrame;

    // Screen switching: set `nextScreen` to change screens at the end of the
    // frame; set `quit` to leave the game.
    std::unique_ptr<Screen> nextScreen;
    bool quit = false;
    // OpenMM2: F9 in a race shows the original look (no tweaks) until pressed
    // again; not saved (Tweaks.h drawnTweaks).
    bool originalLook = false;
    // Automation: this frame is the last (as --frames' last, with its
    // --screenshot); set by OPENMM2_DEBUG_NET_SHOT_MS in a network race.
    bool lastFrameRequested = false;
    // Automation: --screenshot's picture of this frame as well, saved as
    // "<name>-<tag>.<ext>" (OPENMM2_DEBUG_NET_SHOT_MS's earlier times).
    int captureTag = -1;

    // Writes settings (including [Display]) to disk.
    void saveSettings();
    // Pushes volume settings to the mixer.
    void applyAudioSettings();

    // The interactive soundtrack, created on first use once game data is
    // mounted (null before that). Its streams play on Bus::Music.
    audio::MusicPlayer* music();
    void shutdownMusic();

private:
    std::unique_ptr<audio::MusicPlayer> m_music;
    int m_musicStream = 0, m_ambienceStream = 0;

public:
};

// One full-window state of the application: setup, frontend menus, a race.
// Per frame the app calls update() (inside an ImGui frame, so screens may use
// ImGui), then drawScene() if usesScene(), then drawOverlay().
class Screen {
public:
    virtual ~Screen() = default;
    virtual void update(Context& ctx, double dt) = 0;
    virtual bool usesScene() const { return false; }
    virtual void drawScene(Context&) {}
    virtual void drawOverlay(Context&) {}
    // The window was activated again after another application had it.
    virtual void activated(Context&) {}
};

// Mounts `source` and loads the catalog and strings. Returns null (and sets
// `error`) when the archives cannot be read.
std::unique_ptr<GameData> loadGameData(const vfs::GameSource& source, std::string* error);

} // namespace mm2::app
