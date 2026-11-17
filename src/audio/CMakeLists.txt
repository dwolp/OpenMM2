# In-game sound effects (src/audio/game), picked up automatically.
file(GLOB audio_game_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.cpp")
add_library(mm2_audio STATIC
    ${audio_game_sources}
    AngelRandom.cpp
    AngelUnits.cpp
    EchoEffect.cpp
    SynthReverb.cpp
    Wav.cpp
    Mixer.cpp
    SoundBank.cpp
    TextFields.cpp
    AudioDevice.cpp
    Mm1CdMusic.cpp
    Music.cpp
    MusicDirector.cpp)
target_link_libraries(mm2_audio
    PUBLIC OpenMM2::core OpenMM2::vfs
    PRIVATE SDL3::SDL3 OpenMM2::data OpenMM2::dmusic)
add_library(OpenMM2::audio ALIAS mm2_audio)
