#pragma once

#include "core/Ini.h"

#include <filesystem>
#include <string>

namespace mm2::app {

// User settings, persisted to <userConfigDir>/openmm2.ini. Sections owned by
// other subsystems (e.g. [Display]) are kept in `ini` and read/written by
// those subsystems, so unknown keys survive a load/save round trip.
struct Settings {
    // [GameData]
    std::string gameSource; // disc image, disc path or install/import directory
    // OpenMM2 Enhanced: Midtown Madness 1's disc image, disc copy, installation
    // or CORE.AR, for its cities (Chicago). Empty: none. Not in MM2.
    std::string mm1Source;

    // [Audio] linear volumes 0..1. The Options > Audio page sets effects,
    // engine and voice together (SOUND FX VOLUME) and music and ambient
    // together (MUSIC/CITY VOLUME), MM2's defaults being 1 for both
    // (AudioOptions::ResetDefaultAction).
    float masterVolume = 1.0f;
    float effectsVolume = 1.0f;
    float engineVolume = 1.0f;
    float voiceVolume = 1.0f;
    float musicVolume = 1.0f;
    // The audio toggles, with MM2's start-up defaults (mmStatePack::SetDefaults
    // audio flags 0xc73): music off and city sounds on; MM2 never has both on.
    bool soundEffects = true; // flag 0x1
    bool commentary = true;   // flag 0x400
    bool music = false;       // flag 0x4: interactive music
    bool citySounds = true;   // flag 0x800: city ambience
    // STEREO FX 0 Mono, 1 Stereo, 2 Surround (AudioOptions::SetStereoFX:
    // flags 0x40 and 0x100); surround plays as stereo.
    int stereoFx = 1;
    // SOUND QUALITY 0 Low, 1 Medium, 2 High: MM2's channel count 8/16/32
    // (AudioOptions::SetQuality). Stored only: AudManager::SetNumChannels is
    // empty in MM2, which always mixes 32 voices.
    int soundQuality = 2;
    // MM2 always plays the 22 kHz sounds (InitAudioManager sets aud22 and
    // .22k whatever the quality), so this stays true.
    bool audioHighQuality = true;
    float balance = 0.0f;         // BALANCE, -1 (left) .. 1 (right)

    // [Network]
    std::string playerName = "Player";
    int port = 0; // 0 = default game port
    bool upnp = true;

    // [Game]
    bool metricUnits = false;
    // OpenMM2 Enhanced: MM1's Controls.PhysicsRealism (0..1, default 0.75), the
    // player's mmCarSim Realism on MM1's cars (Chicago). Not in MM2.
    float physicsRealism = 0.75f;

    IniFile ini;

    // Loads `path`, falling back to defaults for anything missing.
    void load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path);

    static std::filesystem::path defaultPath();
};

// The Windows installer writes "<install dir>/openmm2-install.ini" with the
// game source the user picked; it is used when the per-user settings have
// no source yet.
std::filesystem::path installDefaultsPath();
std::string installDefaultGameSource();

} // namespace mm2::app
