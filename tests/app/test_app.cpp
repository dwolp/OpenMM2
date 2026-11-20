#include "app/CommandLine.h"
#include "app/GameData.h"
#include "app/Settings.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>

using namespace mm2;

namespace {

app::CommandLine parse(std::initializer_list<const char*> args) {
    std::vector<char*> argv{const_cast<char*>("openmm2")};
    for (const char* a : args)
        argv.push_back(const_cast<char*>(a));
    return app::parseCommandLine(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(CommandLine, InstallerContract) {
    auto a = parse({"--check-source", "D:\\"});
    EXPECT_EQ(a.action, app::CommandLine::Action::CheckSource);
    ASSERT_EQ(a.actionArgs.size(), 1u);
    EXPECT_EQ(a.actionArgs[0], "D:\\");

    auto b = parse({"--import-source", "x.iso", "C:\\Games\\OpenMM2\\gamedata"});
    EXPECT_EQ(b.action, app::CommandLine::Action::ImportSource);
    EXPECT_EQ(b.actionArgs.size(), 2u);

    auto c = parse({"--import-source", "x.iso", "--log-level", "debug"});
    EXPECT_EQ(c.actionArgs.size(), 1u);
    EXPECT_EQ(c.logLevel, "debug");
}

TEST(CommandLine, Errors) {
    EXPECT_FALSE(parse({"--bogus"}).error.empty());
    EXPECT_FALSE(parse({"--width"}).error.empty());
    EXPECT_FALSE(parse({"--width", "-5"}).error.empty());
    EXPECT_FALSE(parse({"--backend", "d3d"}).error.empty());
    auto ok = parse({"--backend", "opengl", "--windowed", "--width", "2560", "--height", "1080"});
    EXPECT_TRUE(ok.error.empty());
    EXPECT_EQ(ok.width, 2560);
    EXPECT_EQ(ok.fullscreen, false);
}

TEST(Settings, RoundTripKeepsUnknownSections) {
    const auto path = std::filesystem::temp_directory_path() / "openmm2_settings_test.ini";
    {
        app::Settings s;
        s.ini.parse("[Display]\nWidth=3440\n");
        s.gameSource = "/media/cdrom";
        s.musicVolume = 0.25f;
        s.upnp = false;
        ASSERT_TRUE(s.save(path));
    }
    app::Settings t;
    t.load(path);
    EXPECT_EQ(t.gameSource, "/media/cdrom");
    EXPECT_FLOAT_EQ(t.musicVolume, 0.25f);
    EXPECT_FALSE(t.upnp);
    EXPECT_EQ(t.ini.getInt("Display", "Width", 0), 3440);
    std::filesystem::remove(path);
}

TEST(Settings, AudioOptionsFollowMM2) {
    // MM2's start-up defaults: music off, city sounds on, high quality.
    const app::Settings d;
    EXPECT_FALSE(d.music);
    EXPECT_TRUE(d.citySounds);
    EXPECT_TRUE(d.soundEffects);
    EXPECT_EQ(d.soundQuality, 2);
    EXPECT_FLOAT_EQ(d.musicVolume, 1.0f);
    EXPECT_FLOAT_EQ(d.balance, 0.0f);

    const auto path = std::filesystem::temp_directory_path() / "openmm2_settings_audio.ini";
    {
        app::Settings s;
        s.music = true;
        s.citySounds = false;
        s.soundQuality = 1;
        s.balance = -0.5f;
        s.commentary = false;
        ASSERT_TRUE(s.save(path));
    }
    app::Settings t;
    t.load(path);
    EXPECT_TRUE(t.music);
    EXPECT_FALSE(t.citySounds);
    EXPECT_FALSE(t.commentary);
    EXPECT_EQ(t.soundQuality, 1);
    EXPECT_TRUE(t.audioHighQuality);
    EXPECT_FLOAT_EQ(t.balance, -0.5f);

    // Music and city sounds can both be on (OpenMM2 Enhanced); files from before the
    // three-step quality option keep their choice.
    app::Settings u;
    u.ini.parse("[Audio]\nMusicOn=true\nCitySounds=true\nHighQuality=false\n");
    ASSERT_TRUE(u.ini.save(path));
    u.load(path);
    EXPECT_TRUE(u.music);
    EXPECT_TRUE(u.citySounds);
    EXPECT_EQ(u.soundQuality, 0);
    // MM2 plays the 22 kHz sounds at every quality (InitAudioManager).
    EXPECT_TRUE(u.audioHighQuality);
    std::filesystem::remove(path);
}

TEST(GameData, RejectsNonGameFolder) {
    const auto r = app::checkGameSource(std::filesystem::temp_directory_path());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.message.empty());
}
