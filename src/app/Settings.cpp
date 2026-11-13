#include "app/Settings.h"

#include "core/Paths.h"
#include "core/StringUtil.h"

#include <algorithm>

namespace mm2::app {
namespace {

float volume(const IniFile& ini, const char* key, float fallback) {
    return std::clamp(static_cast<float>(ini.getDouble("Audio", key, fallback)), 0.0f, 1.0f);
}

} // namespace

void Settings::load(const std::filesystem::path& path) {
    *this = Settings{};
    ini.load(path);
    gameSource = ini.getString("GameData", "Source");
    mm1Source = ini.getString("GameData", "MM1Source");
    masterVolume = volume(ini, "Master", masterVolume);
    effectsVolume = volume(ini, "Effects", effectsVolume);
    engineVolume = volume(ini, "Engine", engineVolume);
    voiceVolume = volume(ini, "Voice", voiceVolume);
    musicVolume = volume(ini, "Music", musicVolume);
    soundEffects = ini.getBool("Audio", "SoundEffects", soundEffects);
    commentary = ini.getBool("Audio", "Commentary", commentary);
    music = ini.getBool("Audio", "MusicOn", music);
    citySounds = ini.getBool("Audio", "CitySounds", citySounds);
    if (music && citySounds)
        citySounds = false; // MM2's toggles are exclusive (AudioOptions::ToggleMusic)
    // Files from before the three-way option only have Stereo.
    const long long oldStereo = ini.getBool("Audio", "Stereo", true) ? 1 : 0;
    stereoFx = static_cast<int>(std::clamp<long long>(ini.getInt("Audio", "StereoFx", oldStereo), 0, 2));
    // Files from before the three-step option only have HighQuality.
    const bool high = ini.getBool("Audio", "HighQuality", true);
    soundQuality = static_cast<int>(std::clamp<long long>(ini.getInt("Audio", "Quality", high ? 2 : 0), 0, 2));
    audioHighQuality = true; // the quality never selects the 11 kHz sounds
    balance = std::clamp(static_cast<float>(ini.getDouble("Audio", "Pan", balance)), -1.0f, 1.0f);
    playerName = ini.getString("Network", "PlayerName", playerName);
    port = static_cast<int>(std::clamp<long long>(ini.getInt("Network", "Port", port), 0, 65535));
    upnp = ini.getBool("Network", "UPnP", upnp);
    metricUnits = ini.getBool("Game", "MetricUnits", metricUnits);
}

bool Settings::save(const std::filesystem::path& path) {
    ini.set("GameData", "Source", gameSource);
    if (!mm1Source.empty())
        ini.set("GameData", "MM1Source", mm1Source);
    ini.setDouble("Audio", "Master", masterVolume);
    ini.setDouble("Audio", "Effects", effectsVolume);
    ini.setDouble("Audio", "Engine", engineVolume);
    ini.setDouble("Audio", "Voice", voiceVolume);
    ini.setDouble("Audio", "Music", musicVolume);
    ini.remove("Audio", "HighQuality"); // superseded by Quality
    ini.setInt("Audio", "Quality", soundQuality);
    ini.setBool("Audio", "SoundEffects", soundEffects);
    ini.setBool("Audio", "Commentary", commentary);
    ini.setBool("Audio", "MusicOn", music);
    ini.setBool("Audio", "CitySounds", citySounds);
    ini.setInt("Audio", "StereoFx", stereoFx);
    ini.remove("Audio", "Stereo"); // superseded by StereoFx
    ini.setDouble("Audio", "Pan", balance);
    ini.remove("Audio", "Balance"); // 0..1 balance of earlier versions
    ini.set("Network", "PlayerName", playerName);
    ini.setInt("Network", "Port", port);
    ini.setBool("Network", "UPnP", upnp);
    ini.setBool("Game", "MetricUnits", metricUnits);
    return ini.save(path);
}

std::filesystem::path Settings::defaultPath() { return paths::userConfigDir() / "openmm2.ini"; }

std::filesystem::path installDefaultsPath() { return paths::executableDir() / "openmm2-install.ini"; }

std::string installDefaultGameSource() {
    IniFile ini;
    if (!ini.load(installDefaultsPath()))
        return {};
    return ini.getString("GameData", "Source");
}

} // namespace mm2::app
