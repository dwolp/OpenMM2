#include "app/Context.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <cstdlib>

namespace mm2::app {

void Context::saveSettings() {
    display.save(settings.ini);
    if (!settings.save(settingsPath))
        log::warn("settings: cannot write {}", settingsPath.string());
}

void Context::applyAudioSettings() {
    if (!mixer)
        return;
    // Options > Audio: the volumes, with the toggles muting their buses
    // (sound FX: effects, engines and voices, the city's wave ambient sounds
    // included; commentary: voices; music; city sounds: the city's DirectMusic
    // ambience segment, which DMusicWaveBuffer::SetVolume plays at the MUSIC
    // volume).
    // AudioOptions::SetAudioState: turning SOUND FX off sets the volume of
    // every AudSoundBase sound to zero (audManager::SetVolAllSounds on the
    // sound class that SOUND FX VOLUME scales through
    // AudManager::AssignWaveVolume). The commentary's AudSpeech streams are
    // AudSoundBase sounds, so commentary falls silent too.
    // The original's command-line switches mute for this run only, leaving
    // the saved options alone: -noaudio/-nosoundfx (InitAudioManager never
    // initialises AudManager), -nomusic (mmGameMusicData::Load loads neither
    // the race music nor the ambience segment, so the race starts neither;
    // the menu music still plays) and -nospeech (mmPlayer::InitSpeechAudio
    // loads no announcer).
    const CommandLine& cl = commandLine;
    const bool music = settings.music;
    const bool ambience = settings.citySounds;
    const bool speech = settings.soundEffects && settings.commentary && !cl.noSpeech;
    mixer->setMasterVolume(cl.noAudio ? 0.0f : settings.masterVolume);
    mixer->setBusVolume(audio::Bus::Effects, settings.soundEffects ? settings.effectsVolume : 0.0f);
    mixer->setBusVolume(audio::Bus::Engine, settings.soundEffects ? settings.engineVolume : 0.0f);
    mixer->setBusVolume(audio::Bus::Ambient, ambience ? settings.musicVolume : 0.0f);
    mixer->setBusVolume(audio::Bus::Voice, speech ? settings.voiceVolume : 0.0f);
    mixer->setBusVolume(audio::Bus::Music, music ? settings.musicVolume : 0.0f);
    mixer->setBalance(settings.balance);
    // AudioOptions::SetStereoFX: mono centres every voice; surround only adds
    // an EAX/3D flag, so it plays as stereo. Mono also makes the music mono
    // (AudManagerBase::IsStereo sizes DirectMusic's port), from the next time
    // the menus or a race start their music.
    mixer->setStereo(settings.stereoFx != 0);
    if (m_music)
        m_music->setStereo(settings.stereoFx != 0);
}

audio::MusicPlayer* Context::music() {
    if (!m_music && game && mixer) {
        m_music = std::make_unique<audio::MusicPlayer>(game->vfs, mixer->sampleRate());
        m_music->setStereo(settings.stereoFx != 0);
        if (!m_music->ok()) {
            log::warn("music: soundtrack unavailable");
        } else {
            m_musicStream = mixer->addStream(m_music->musicStream(), audio::Bus::Music);
            // The city ambience segment: CITY SOUNDS turns it on, the MUSIC
            // volume sets its level (applyAudioSettings).
            m_ambienceStream = mixer->addStream(m_music->ambienceStream(), audio::Bus::Ambient);
        }
    }
    return m_music.get();
}

void Context::shutdownMusic() {
    if (mixer) {
        mixer->removeStream(m_musicStream);
        mixer->removeStream(m_ambienceStream);
        mixer->removeStream(m_cdMusicStream);
    }
    m_musicStream = m_ambienceStream = m_cdMusicStream = 0;
    m_music.reset();
    m_cdMusic.reset();
}

std::shared_ptr<audio::Mm1CdMusic> Context::mm1CdMusic() {
    if (!m_cdMusicTried && mixer) {
        m_cdMusicTried = true;
        const char* env = std::getenv("OPENMM2_MM1_SOURCE");
        const std::string path = env && *env ? std::string(env) : settings.mm1Source;
        if (!path.empty()) {
            m_cdMusic = audio::Mm1CdMusic::open(str::toPath(path), mixer->sampleRate());
            if (m_cdMusic)
                m_cdMusicStream = mixer->addStream(m_cdMusic, audio::Bus::Music);
            else
                log::info("mm1 music: {} has no CD audio after its data track", path);
        }
    }
    return m_cdMusic;
}

std::unique_ptr<GameData> loadGameData(const vfs::GameSource& source, std::string* error) {
    auto data = std::make_unique<GameData>();
    data->source = source;
    if (!vfs::mountGameSource(data->vfs, source, error))
        return nullptr;
    data->catalog = game::Catalog::load(data->vfs);
    data->strings = game::Strings::load(source);
    return data;
}

void mountConfiguredMm1Source(GameData& game, const Settings& settings) {
    const char* env = std::getenv("OPENMM2_MM1_SOURCE");
    const std::string path = env && *env ? std::string(env) : settings.mm1Source;
    if (path.empty())
        return;
    std::string error;
    if (!vfs::mountMm1Source(game.vfs, str::toPath(path), &error))
        log::warn("mm1: {}", error);
}

} // namespace mm2::app
