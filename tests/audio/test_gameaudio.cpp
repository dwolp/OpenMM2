#include "TestData.h"
#include "audio/AngelRandom.h"
#include "audio/game/Ambience.h"
#include "audio/game/AudioTables.h"
#include "audio/game/CarAudio.h"
#include "audio/game/Object3D.h"
#include "audio/game/Voices.h"
#include "core/File.h"
#include "core/StringUtil.h"
#include "vfs/DirectoryFs.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

using namespace mm2;
using namespace mm2::audio;
using namespace mm2::audio::game;

namespace {

std::string_view asText(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

// A tiny 16-bit mono WAV.
std::vector<std::byte> wav(int frames, int rate = 22050) {
    std::vector<std::byte> out;
    auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto u32 = [&](std::uint32_t v) { put(&v, 4); };
    auto u16 = [&](std::uint16_t v) { put(&v, 2); };
    put("RIFF", 4);
    u32(36 + frames * 2);
    put("WAVE", 4);
    put("fmt ", 4);
    u32(16);
    u16(1);
    u16(1);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * 2));
    u16(2);
    u16(16);
    put("data", 4);
    u32(static_cast<std::uint32_t>(frames * 2));
    for (int i = 0; i < frames; ++i) {
        const auto s = static_cast<std::int16_t>(8000 * std::sin(i * 0.1));
        put(&s, 2);
    }
    return out;
}

// A throwaway game-data tree with generated sounds and given text files.
struct FakeData {
    std::filesystem::path root;
    vfs::Vfs vfs;
    explicit FakeData(std::initializer_list<std::pair<const char*, std::string>> files,
                      std::initializer_list<const char*> sounds, int frames = 2205) {
        root = std::filesystem::temp_directory_path() /
               ("openmm2_gameaudio_" + std::to_string(std::rand()) + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        for (const auto& [path, text] : files)
            file::writeAtomic(root / path, text);
        for (const char* s : sounds)
            file::writeAtomic(root / "aud/aud22" / (std::string(s) + ".22k.wav"), std::span<const std::byte>(wav(frames)));
        vfs.mount(std::make_shared<vfs::DirectoryFs>(root));
    }
    ~FakeData() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

const char* kDefaultCar = "Horn wave name,Horn volume,flags,Num Engine Samples,clutch wave name,clutch volume,,,,,\r\n"
                          "RACECARHORN,0.95,0,2,REVERSE,0.93,,,,,\r\n"
                          "Engine wave name,Min Volume,Max Volume,fade in  start RPM,fade in end RPM,fade out start RPM,"
                          "fade out end RPM,Min Pitch,Max Pitch,Pitch shift start RPM,Pitch shift end RPM\r\n"
                          "RACECARIDLE,0.65,0.96,1,850,2500,7000,0.8,1.64,1,7000\r\n"
                          "RACECARDRIVE,0.7,0.94,500,3500,9000,12500,0.65,4.5,500,40000\r\n";

const char* kSurfaceDry = "Tunnel sound index,,,,,,,,\n0,,,,,,,,\n"
                          "surface wave,max speed,min surface volume,max surface volume,min surface pitch,max surface "
                          "pitch,min skid volume,max skid volume,num skid samples\n"
                          "NOSOUND,125,0,0,0,0,0.5,0.88,3\n"
                          "skid wave,min slippage,max slippage,,,,,,\n"
                          "tireskid1,0.55,0.65\ntireskid2,0.65,0.75\ntireskid3,0.75,1\n"
                          "surface wave,max speed,min surface volume,max surface volume,min surface pitch,max surface "
                          "pitch,min skid volume,max skid volume,num skid samples\n"
                          "surfacegrass,25,0.35,0.75,0.85,1.25,0.5,0.72,1\n"
                          "skid wave,min slippage,max slippage\nsurfacegrassskid,0.25,1\n";

const char* kImpacts = "***\nBanger name,Num samples,ID\nWALL,3,0\n"
                       "sample name,min volume,max volume,min force,max force,frequency\n"
                       "soft,0.91,0.93,1000,8000,1\nmed,0.92,0.95,8000,20000,1\nhuge,0.94,1,20000,999999,1\n"
                       "***\nBanger name,Num samples,ID\nTREE,2,8\n"
                       "sample name,min volume,max volume,min force,max force,frequency\n"
                       "bush,0.9,1,0,999999,1\nleaves,0.95,1,0,999999,1\n***\nBanger name,Num samples,ID\nENDOFDATA,0,0\n";

CarAudioInputs grounded() {
    CarAudioInputs in;
    for (auto& w : in.wheels)
        w.onGround = true;
    return in;
}

} // namespace

TEST(GameAudio, AngelVolumeIsDecibelLinear) {
    EXPECT_FLOAT_EQ(ageVolumeToGain(1.0f), 1.0f);
    EXPECT_NEAR(ageVolumeToGain(0.9f), 0.31623f, 1e-4f); // -10 dB
    EXPECT_NEAR(ageVolumeToGain(0.8f), 0.1f, 1e-5f);     // -20 dB
    EXPECT_LT(ageVolumeToGain(kSilentVolume), 2e-4f);     // -75 dB
    EXPECT_EQ(ageVolumeToGain(0.0f), 0.0f);
}

TEST(GameAudio, AngelPanAttenuatesTheFarChannelInDecibels) {
    // audSound::SetPan: pan * 10000 hundredths of a decibel on the far channel.
    EXPECT_FLOAT_EQ(agePanToMixer(0.0f), 0.0f);
    EXPECT_NEAR(1.0f - agePanToMixer(0.2f), 0.1f, 1e-5f);   // right: left at -20 dB
    EXPECT_NEAR(1.0f + agePanToMixer(-0.1f), 0.31623f, 1e-4f); // left: right at -10 dB
    EXPECT_NEAR(agePanToMixer(1.0f), 1.0f, 1e-4f);
}

TEST(GameAudio, PitchClampsToTheBufferFrequencyRange) {
    // audSound::SetPitch: 100..100000 Hz, no clamp of the multiplier itself.
    EXPECT_NEAR(clampPitch(2.0f, 22050), 2.0f, 1e-6f);
    EXPECT_NEAR(clampPitch(0.001f, 22050), 100.0f / 22050.0f, 1e-6f);
    EXPECT_NEAR(clampPitch(12.0f, 8000), 12.0f, 1e-5f); // MM1 clamped the multiplier to 10
    EXPECT_NEAR(clampPitch(12.0f, 11025), 100000.0f / 11025.0f, 1e-4f);
}

TEST(GameAudio, EngineCurvesFollowVehEngineSampleWrapper) {
    auto def = parseCarAudio(kDefaultCar);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->horn, "RACECARHORN");
    EXPECT_EQ(def->clutch, "REVERSE");
    ASSERT_EQ(def->engine.size(), 2u);
    const auto& idle = def->engine[0];
    const auto& drive = def->engine[1];

    // CalculateVolume: min volume outside the fades (still audible above 0.25),
    // max between them, linear in the fades.
    EXPECT_FLOAT_EQ(EngineSound::evaluate(idle, 0.0f).volume, 0.65f);
    EXPECT_TRUE(EngineSound::evaluate(idle, 0.0f).audible);
    EXPECT_FLOAT_EQ(EngineSound::evaluate(idle, 1000.0f).volume, 0.96f);
    EXPECT_NEAR(EngineSound::evaluate(idle, 4750.0f).volume, 0.805f, 1e-4f);
    EXPECT_FLOAT_EQ(EngineSound::evaluate(idle, 7000.0f).volume, 0.65f);
    EXPECT_FLOAT_EQ(EngineSound::evaluate(idle, 9000.0f).volume, 0.65f);
    EXPECT_FLOAT_EQ(EngineSound::evaluate(drive, 400.0f).volume, 0.7f);
    EXPECT_NEAR(EngineSound::evaluate(drive, 2000.0f).volume, 0.82f, 1e-5f);

    // CalculatePitch: min + rpm * slope inside the shift range (the slope
    // applies to the whole RPM), so the pitch jumps at the range's start.
    EXPECT_FLOAT_EQ(EngineSound::evaluate(drive, 500.0f).pitch, 0.65f);
    EXPECT_NEAR(EngineSound::evaluate(drive, 600.0f).pitch, 0.65f + 600.0f * 3.85f / 39500.0f, 1e-5f);
    EngineSampleDef high{"HIGH", 0.55f, 0.91f, 3000, 8000, 15000, 15000, 0.65f, 2.25f, 3000, 12000};
    EXPECT_FLOAT_EQ(EngineSound::evaluate(high, 3000.0f).pitch, 0.65f);
    EXPECT_NEAR(EngineSound::evaluate(high, 3001.0f).pitch, 0.65f + 3001.0f * 1.6f / 9000.0f, 1e-4f);
    EXPECT_NEAR(EngineSound::evaluate(high, 11999.0f).pitch, 2.783f, 1e-3f); // above max pitch
    EXPECT_FLOAT_EQ(EngineSound::evaluate(high, 12000.0f).pitch, 2.25f);

    // A table volume below 0.25 stops the sample.
    EngineSampleDef quiet{"Q", 0.2f, 0.9f, 1000, 2000, 3000, 4000, 1, 1, 0, 1};
    EXPECT_FALSE(EngineSound::evaluate(quiet, 500.0f).audible);
    EXPECT_TRUE(EngineSound::evaluate(quiet, 2500.0f).audible);
}

TEST(GameAudio, SkidSamplesAndVolumes) {
    auto table = parseSurfaceTable(kSurfaceDry);
    ASSERT_TRUE(table);
    ASSERT_EQ(table->surfaces.size(), 2u);
    const auto& road = table->surfaces[0];
    EXPECT_FALSE(road.hasSurfaceSound());
    // vehSurfaceAudioData::UpdateSkid: inclusive ranges; 0.65 is in two.
    EXPECT_FALSE(SurfaceSounds::skidInRange(road.skids[0], 0.5f));
    EXPECT_TRUE(SurfaceSounds::skidInRange(road.skids[0], 0.65f));
    EXPECT_TRUE(SurfaceSounds::skidInRange(road.skids[1], 0.65f));
    EXPECT_TRUE(SurfaceSounds::skidInRange(road.skids[2], 1.0f));
    // Volume: min + slip * (max - min).
    EXPECT_NEAR(SurfaceSounds::skidVolumeFor(road, 0.55f), 0.5f + 0.55f * 0.38f, 1e-5f);
    EXPECT_NEAR(SurfaceSounds::skidVolumeFor(road, 1.0f), 0.88f, 1e-5f);
    // Rolling sound: linear in speed up to max speed.
    const auto& grass = table->surfaces[1];
    EXPECT_NEAR(SurfaceSounds::surfaceVolumeFor(grass, 12.5f), 0.55f, 1e-5f);
    EXPECT_FLOAT_EQ(SurfaceSounds::surfaceVolumeFor(grass, 30.0f), 0.75f);
    EXPECT_NEAR(SurfaceSounds::surfacePitchFor(grass, 12.5f), 1.05f, 1e-5f);
}

TEST(GameAudio, SurfaceAndSkidSounds) {
    FakeData data({}, {"tireskid1", "tireskid2", "tireskid3", "surfacegrass", "surfacegrassskid"});
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    SurfaceSounds s;
    s.load(mixer, bank, *parseSurfaceTable(kSurfaceDry), Bus::Effects);

    CarAudioInputs in = grounded();
    in.speed = 10.0f;
    for (auto& w : in.wheels)
        w.slip = 0.3f;
    s.update(in, 0.02f);
    EXPECT_TRUE(s.skidding()); // any slip counts, but no range holds 0.3
    EXPECT_EQ(mixer.activeVoices(), 0);
    in.wheels[2].slip = 0.65f; // the largest wheel decides: two ranges at once
    s.update(in, 0.02f);
    EXPECT_TRUE(s.skidPlaying(0));
    EXPECT_TRUE(s.skidPlaying(1));
    EXPECT_EQ(mixer.activeVoices(), 2);
    for (auto& w : in.wheels)
        w.slip = 0.8f;
    in.speed = 0.5f; // MM2 has no speed threshold for skids
    s.update(in, 0.02f);
    EXPECT_TRUE(s.skidPlaying(2));
    EXPECT_EQ(mixer.activeVoices(), 1);
    for (auto& w : in.wheels)
        w.slip = 0.0f;
    s.update(in, 0.02f);
    EXPECT_FALSE(s.skidding());
    EXPECT_EQ(mixer.activeVoices(), 0);

    // Grass (sound index 1 here): rolling above 2 m/s with two wheels down.
    for (auto& w : in.wheels)
        w.surface = 1;
    in.speed = 10.0f;
    s.update(in, 0.02f);
    EXPECT_EQ(s.currentSurface(), 1);
    EXPECT_EQ(mixer.activeVoices(), 1);
    in.wheels[0].onGround = in.wheels[1].onGround = in.wheels[2].onGround = false;
    s.update(in, 0.02f);
    EXPECT_EQ(mixer.activeVoices(), 0);
}

TEST(GameAudio, AirborneNeedsGroundThreeMetresBelow) {
    FakeData data({}, {"tireskid1"});
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    SurfaceSounds s;
    s.load(mixer, bank, *parseSurfaceTable(kSurfaceDry), Bus::Effects);
    CarAudioInputs in; // no wheel on the ground
    in.groundBelow = 1.0f;
    s.update(in, 0.02f);
    EXPECT_FALSE(s.airborne());
    in.groundBelow = 5.0f;
    s.update(in, 0.02f);
    EXPECT_TRUE(s.airborne());
    in.groundBelow.reset(); // stays set until a wheel lands
    s.update(in, 0.02f);
    EXPECT_TRUE(s.airborne());
    in.wheels[3].onGround = true;
    s.update(in, 0.02f);
    EXPECT_FALSE(s.airborne());
}

TEST(GameAudio, SuspensionThumpUsesTheAverageCompression) {
    FakeData data({}, {"thump"});
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    SurfaceSounds s;
    s.load(mixer, bank, *parseSurfaceTable(kSurfaceDry), Bus::Effects);
    s.loadSuspension(mixer, bank, {"thump", 2, 3, 0.85f, 0.9f, 1.0f / 3.0f}, Bus::Effects);
    CarAudioInputs in = grounded();
    in.wheels[0].suspensionSpeed = 6.0f; // one wheel: average 1.5 < 2
    s.update(in, 0.02f);
    EXPECT_EQ(mixer.activeVoices(), 0);
    for (auto& w : in.wheels)
        w.suspensionSpeed = 2.5f;
    s.update(in, 0.02f);
    EXPECT_EQ(mixer.activeVoices(), 1);
}

TEST(GameAudio, TireWobbleThumpsOncePerRevolution) {
    FakeData data({}, {"wobble"}, 200);
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    SurfaceSounds s;
    s.load(mixer, bank, *parseSurfaceTable(kSurfaceDry), Bus::Effects);
    s.loadTireWobble(mixer, bank, {"wobble", 0.97f, 1, 0.75f, 1.5f, 1.0f / 15.0f}, Bus::Effects);
    CarAudioInputs in = grounded();
    in.speed = 10.0f;
    in.wheelRadius = 0.3f; // 1.885 m per revolution
    in.tireWobble = 0.05f; // not past the threshold
    s.update(in, 0.5f);
    EXPECT_EQ(mixer.activeVoices(), 0);
    in.tireWobble = 0.5f;
    s.update(in, 0.1f); // 1 m
    EXPECT_EQ(mixer.activeVoices(), 0);
    s.update(in, 0.1f); // 2 m: a thump
    EXPECT_EQ(mixer.activeVoices(), 1);
}

TEST(GameAudio, ImpactsByTableIndexAndForce) {
    auto table = parseImpactTable(kImpacts);
    ASSERT_TRUE(table);
    ASSERT_EQ(table->bangers.size(), 2u);
    EXPECT_EQ(table->byIndex(1)->name, "TREE");
    EXPECT_EQ(table->byIndex(1000)->name, "WALL"); // out of range: WALL
    // PlaySample: min volume + force * slope.
    const auto& soft = table->byIndex(0)->samples[0];
    EXPECT_NEAR(ImpactSounds::volumeFor(soft, 1000), 0.91f + 1000 * 0.02f / 7000, 1e-5f);
    EXPECT_NEAR(ImpactSounds::volumeFor(soft, 8000), 0.91f + 8000 * 0.02f / 7000, 1e-5f);

    FakeData data({}, {"soft", "med", "huge", "bush", "leaves"});
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    ImpactSounds impacts;
    impacts.load(mixer, bank, *table, Bus::Effects);
    impacts.play({500.0f, 0, {}});
    EXPECT_EQ(mixer.activeVoices(), 0); // below the softest sample
    impacts.play({8000.0f, 0, {}});     // both ends inclusive: soft and med
    EXPECT_EQ(mixer.activeVoices(), 2);
    impacts.play({12000.0f, 0, {}}); // med is still playing
    EXPECT_EQ(mixer.activeVoices(), 2);
    impacts.play({100.0f, 1, {}}); // the tree, by position in the table
    EXPECT_EQ(mixer.activeVoices(), 4);
    EXPECT_EQ(impacts.lastPlayed(), 1);
    EXPECT_FLOAT_EQ(impactStrength({-3, 4, 0.5f}), 7.5f);
}

TEST(GameAudio, SirenStepsThroughItsTable) {
    const char* text = "Explosion sample,volume\nexplosion,0.95\nSample name,\nsirena,0.95\nplay time,next index\n1,1\n"
                       "play time,next index\n2,1\nSample name,\nsirenb,0.95\nplay time,next index\n0.5,0\n";
    auto table = parseSirenTable(text);
    ASSERT_TRUE(table);
    ASSERT_EQ(table->samples.size(), 2u);
    ASSERT_EQ(table->samples[0].steps.size(), 2u);
    FakeData data({}, {"sirena", "sirenb", "explosion"}, 22050);
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    SirenPlayer siren;
    siren.load(mixer, bank, *table, Bus::Effects);
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 0);
    siren.start(true);
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 1);
    EXPECT_EQ(siren.currentSample(), 0);
    // FluctuateSiren: a's first entry (1 s) -> b (0.5 s) -> a's second entry
    // (2 s) -> b -> a's first entry again.
    std::vector<int> seen;
    for (int i = 0; i < 90; ++i) {
        siren.update(0.05f);
        if (seen.empty() || seen.back() != siren.currentSample())
            seen.push_back(siren.currentSample());
    }
    EXPECT_EQ(seen, (std::vector<int>{0, 1, 0, 1, 0}));
    EXPECT_EQ(mixer.activeVoices(), 1);
    siren.stop();
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 0);
    EXPECT_EQ(mixer.activeVoices(), 0);

    // PerpEscapes counts an exploding cop twice.
    SirenPlayer other;
    other.load(mixer, bank, *table, Bus::Effects); // resets the count, like MM2's constructor
    siren.start(true);
    other.start(true);
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 2);
    siren.explode(true, 1.0f);
    siren.stop();
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 0);
    EXPECT_TRUE(siren.explosionPlaying());
    other.stopAll();
    siren.stopAll();
}

TEST(GameAudio, PositionedSoundsAttenuateWithSquaredDistance) {
    Audio3D a;
    a.setDropOffs(0.0f, 150.0f);
    Mat34 listener = Mat34::identity();
    ASSERT_TRUE(a.withinMaxDistance({75, 0, 0}, listener.m3));
    EXPECT_NEAR(a.attenuation(), 0.75f, 1e-5f); // 1 - 75^2 / 150^2
    // Pan: 0.2 * x / (|dx| + |dy| + |dz|).
    EXPECT_NEAR(a.pan(listener, {75, 0, 0}), 0.2f, 1e-6f);
    a.updateDistance({30, 0, -30}, listener.m3);
    EXPECT_NEAR(a.pan(listener, {30, 0, -30}), 0.1f, 1e-6f);
    // Doppler: the pseudo distance closed since the last update.
    EXPECT_FALSE(a.pastMaxDistance({20, 0, -30}, listener.m3));
    EXPECT_NEAR(a.doppler(1.0f / kDopplerSpeed, 0.5f), 1.0f + 10.0f / kDopplerSpeed * 0.5f, 1e-5f);
    EXPECT_TRUE(a.pastMaxDistance({200, 0, 0}, listener.m3));
    a.alwaysAudible = true; // a siren keeps the slot
    EXPECT_FALSE(a.pastMaxDistance({200, 0, 0}, listener.m3));
}

namespace {
struct TestClient : Object3DManager::Client {
    float d2 = 0;
    int priority = 9;
    bool lost = false;
    float slotDistance2() const override { return d2; }
    int slotPriority() const override { return priority; }
    void slotLost() override { lost = true; }
};
} // namespace

TEST(GameAudio, SlotManagerKeepsTheNearestAndHighestPriority) {
    Object3DManager m(3);
    TestClient a, b, c, d, e;
    a.d2 = 100, b.d2 = 400, c.d2 = 900, d.d2 = 1600, e.d2 = 50;
    EXPECT_TRUE(m.add(&a));
    EXPECT_TRUE(m.add(&b));
    EXPECT_TRUE(m.add(&c));
    EXPECT_FALSE(m.add(&d)); // equal priority and farther than everyone
    EXPECT_TRUE(m.add(&e));  // equal priority but closer than the farthest (c)
    EXPECT_TRUE(c.lost);
    EXPECT_FALSE(m.holds(&c));
    d.priority = 10; // a siren: higher priority wins even from far away
    EXPECT_TRUE(m.add(&d));
    EXPECT_TRUE(b.lost);
    EXPECT_EQ(m.used(), 3);
}

TEST(GameAudio, AmbientEnginePitchBands) {
    auto engine = parseAmbientEngine("Engine sample,engine volume,,\nENGINEFERRARI1,0.97,,\nmin speed,max speed,min pitch,max "
                                     "pitch\n0,15,0.27,1\n15,35,1,1.25\n35,500,1.25,1.5\n0,500,0.27,24.603\n");
    ASSERT_TRUE(engine);
    EXPECT_EQ(engine->bands.size(), 4u);
    EXPECT_NEAR(*AmbientCarAudio::pitchFor(*engine, 0.0f), 0.27f, 1e-5f);
    EXPECT_NEAR(*AmbientCarAudio::pitchFor(*engine, 7.5f), 0.635f, 1e-4f);
    EXPECT_NEAR(*AmbientCarAudio::pitchFor(*engine, 25.0f), 1.125f, 1e-4f);
    EXPECT_FALSE(AmbientCarAudio::pitchFor(*engine, 600.0f)); // no band: the pitch stays
    // Slowing down: the last band.
    EXPECT_NEAR(*AmbientCarAudio::pitchFor(*engine, 10.0f, true), 0.27f + 10.0f * 24.333f / 500.0f, 1e-4f);
    auto horn = parseHorn("Horn sample,horn volume,horn pitch,min stuck horn impact force\nBUSHORN,0.97,0.9,5500\n"
                          "horn play duration,horn pause duration,,\n0.5,0.2,,\n1,0,,\n"
                          "horn play duration,horn pause duration,,\n0,0.15,,\n3,0,,\n");
    ASSERT_TRUE(horn);
    EXPECT_EQ(horn->patterns.size(), 2u);
    EXPECT_EQ(horn->patterns[0].beeps.size(), 2u);
    EXPECT_FLOAT_EQ(horn->stuckImpactForce, 5500.0f);
}

TEST(GameAudio, AmbientHornPatternTiming) {
    FakeData data({{"aud/cardata/ambient/default_engine.csv",
                    "Engine sample,engine volume,,\nengine,0.98,,\nmin speed,max speed,min pitch,max pitch\n"
                    "0,33,0.317,1\n33,500,1,1.25\n0,500,0.317,13.977\n"},
                   {"aud/cardata/ambient/default_horn.csv",
                    "Horn sample,horn volume,horn pitch,min stuck horn impact force\nhorn,0.97,1,5500\n"
                    "horn play duration,horn pause duration,,\n0.5,0.2,,\n1,0,,\n"}},
                  {"engine", "horn"}, 22050);
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    AmbientCarAudio car;
    ASSERT_TRUE(car.load(data.vfs, bank, mixer, "va_test"));
    Mat34 at = Mat34::identity();
    at.m3 = {10, 0, 0};
    const Mat34 listener = Mat34::identity();
    EXPECT_FALSE(car.honk(0)); // no slot yet
    car.update(12.0f, at, {}, 0.05f, listener);
    EXPECT_TRUE(car.audible());
    EXPECT_EQ(mixer.activeVoices(), 1);
    EXPECT_TRUE(car.honk(0));
    EXPECT_EQ(mixer.activeVoices(), 2);
    EXPECT_FALSE(car.honk(0)); // already honking
    // 0.5 s on, 0.2 s off, 1 s on.
    int voices = 0;
    for (int i = 0; i < 12; ++i) { // 0.6 s
        car.update(12.0f, at, {}, 0.05f, listener);
        voices = mixer.activeVoices();
    }
    EXPECT_EQ(voices, 1); // pausing
    for (int i = 0; i < 6; ++i)
        car.update(12.0f, at, {}, 0.05f, listener);
    EXPECT_EQ(mixer.activeVoices(), 2); // second beep
    for (int i = 0; i < 30; ++i)
        car.update(12.0f, at, {}, 0.05f, listener);
    EXPECT_EQ(mixer.activeVoices(), 1); // done
    at.m3 = {150, 0, 0};                // beyond 100 m
    car.update(12.0f, at, {}, 0.05f, listener);
    EXPECT_FALSE(car.audible());
    EXPECT_EQ(mixer.activeVoices(), 0);
}

TEST(GameAudio, CreatureVoicesAnswerNearMisses) {
    auto def = parseCreatureVoice("Min speed,Max speed,min time in range,max time out of range\n0,20,5,1\n"
                                  "sample name,volume,,\nline1,0.98,,\nline2,0.98,,\n"
                                  "min impact force,,,\n5500,,,\nsample name,volume,play after seconds,\nouch,0.98,1,\n");
    ASSERT_TRUE(def);
    ASSERT_EQ(def->triggers.size(), 1u);
    EXPECT_EQ(def->triggers[0].lines.size(), 2u);
    EXPECT_EQ(def->impactLines.size(), 1u);
    FakeData data({}, {"line1", "line2", "ouch"}, 22050);
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    CreatureVoice::resetGlobals();
    CreatureVoice voice;
    voice.load(mixer, bank, *def);
    // The container's UpdateAudio: attenuation, pan, squared distance (10 m).
    voice.updateAttenuation(0.9f, 0.0f, 100.0f);
    // Nothing is said without a near miss, however long the speed stays in range.
    for (int i = 0; i < 100; ++i)
        voice.update(10, 0.1f);
    EXPECT_FALSE(voice.speaking());
    // A near miss queues a line half of the time (RandomizeNumber(2n) < n).
    bool spoke = false;
    for (int seed = 1; seed < 20 && !spoke; ++seed) {
        setRandomizeSeedSource([seed] { return seed; });
        voice.avoid();
        voice.update(10, 0.05f);
        spoke = voice.speaking();
    }
    setRandomizeSeedSource({});
    EXPECT_TRUE(spoke);

    // Impact lines: none during the first minute (the shared clock starts at 0).
    CreatureVoice hit;
    hit.load(mixer, bank, *def);
    hit.updateAttenuation(0.9f, 0.0f, 100.0f);
    hit.impact(9000.0f);
    for (int i = 0; i < 30; ++i)
        hit.update(10, 0.1f);
    EXPECT_FALSE(hit.speaking());
    CreatureVoice::advanceClock(60.0f);
    hit.impact(1000.0f); // too soft
    hit.update(10, 2.0f);
    EXPECT_FALSE(hit.speaking());
    hit.impact(9000.0f);
    hit.update(10, 0.5f); // the line waits 1 s
    EXPECT_FALSE(hit.speaking());
    hit.update(10, 0.6f);
    EXPECT_TRUE(hit.speaking());
}

TEST(GameAudio, AnnouncerLineNamesAndChoice) {
    EXPECT_EQ(Announcer::lineName("AL1PRE", 1), "AL1PRE01");
    EXPECT_EQ(Announcer::lineName("AL1PRE", 11), "AL1PRE11");
    auto t = parseSpeechTable("Name prefix/type header,end sufix value,sufix add value,num used\n"
                              "BLUETEAMHASGOLD header,,,\nAL1\\AL1ROBROB ,1,0,1\nROBGETLOOT header,,,\nAL1\\AL1COPS,4,0,4\n");
    ASSERT_TRUE(t);
    ASSERT_EQ(t->rows.size(), 4u);
    EXPECT_TRUE(t->rows[0].header());
    EXPECT_EQ(t->rows[0].eventName(), "BLUETEAMHASGOLD");
    EXPECT_TRUE(t->rows[2].headerIs("robget")); // event names are prefixes
    EXPECT_EQ(t->rows[1].name, "AL1\\AL1ROBROB "); // strtok keeps the space
    EXPECT_FLOAT_EQ(t->rows[3].numUsed, 4.0f);
    // AudSpeechData::GetRandomName: (add, end], the last number moves up one
    // and wraps to 1, not to add + 1.
    EXPECT_EQ(Announcer::pickLine(10, 8, -1, 9.0), 9);
    EXPECT_EQ(Announcer::pickLine(10, 8, -1, 10.98), 10);
    EXPECT_EQ(Announcer::pickLine(10, 8, 9, 9.2), 10);
    EXPECT_EQ(Announcer::pickLine(10, 8, 10, 10.5), 1);
    EXPECT_EQ(Announcer::pickLine(1, 0, 1, 1.5), 1); // one line repeats
}

TEST(GameAudio, AnnouncerPreRaceWaitsAndEventsInterrupt) {
    const char* blitz = "Name prefix/type header,end sufix value,sufix add value\nPRERACE header,,\nPRE,2,0\n"
                        "FINALCHECKPOINT header,,\nRACECHECK,1,0\nRESULTSWIN header,,\nRESULTWIN,1,0\n";
    FakeData data({{"aud/spchdata/london.csv", "Num announcers\n1\nprefix\nAL\n"}, {"aud/spchdata/al1/blitz.csv", blitz}},
                  {"al1pre01", "al1pre02", "al1racecheck01", "al1resultwin01"}, 22050);
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    Announcer a;
    ASSERT_TRUE(a.load(data.vfs, bank, mixer, "london"));
    EXPECT_EQ(a.announcerId(), "al1");
    a.beginRace(AnnouncerMode::Blitz, {}, 1, 4); // snow: no weather lines; no time-of-day table
    std::string line;
    // The draws are seeded with the clock's second (AudManagerBase::RandomizeNumber).
    for (int i = 0; i < 40 && line.empty(); ++i) {
        setRandomizeSeedSource([i] { return 1000 + i; });
        line = a.playPreRace(); // the time-of-day / weather branches play nothing
    }
    setRandomizeSeedSource({});
    ASSERT_EQ(line, "al1pre") << line; // queued: the number is drawn when it starts
    EXPECT_TRUE(a.speaking());
    a.update(1.0f);
    EXPECT_EQ(mixer.activeVoices(), 0); // waits 1.5 s
    a.update(0.6f);
    EXPECT_EQ(mixer.activeVoices(), 1);
    EXPECT_EQ(a.playFinalCheckpoint(), "al1racecheck01"); // cuts the pre-race line off
    EXPECT_EQ(mixer.activeVoices(), 1);
    EXPECT_EQ(a.playResults(1, 4), "al1resultwin01");
    EXPECT_EQ(a.playResults(2, 4), ""); // blitz has no "mid" lines: nothing
}

TEST(GameAudio, RainThunderAtNight) {
    FakeData data({}, {"Rainexterior", "Raininterior", "Thunder"}, 22050);
    SoundBank bank(data.vfs);
    Mixer mixer(48000);
    RainAudio day;
    day.load(bank, mixer, false);
    for (int i = 0; i < 400; ++i)
        day.update(true, false, false, 0.1f);
    EXPECT_EQ(mixer.activeVoices(), 1); // just the rain loop
    day.stop();

    RainAudio night;
    night.load(bank, mixer, true);
    double flash = -1, firstClap = -1, secondClap = -1;
    int voices = 1;
    for (int i = 1; i <= 200; ++i) {
        night.update(true, false, false, 0.1f);
        const double t = i * 0.1;
        if (night.lightningFlash() && flash < 0)
            flash = t;
        const int v = mixer.activeVoices();
        if (v > voices && firstClap < 0)
            firstClap = t;
        else if (v > voices && firstClap >= 0 && secondClap < 0)
            secondClap = t;
        voices = v;
    }
    EXPECT_NEAR(flash, 13.2, 0.15);
    EXPECT_NEAR(firstClap, 15.1, 0.15);
    EXPECT_NEAR(secondClap, 16.2, 0.15);
}

// --- Retail data ---------------------------------------------------------------

TEST(GameAudioRetail, EveryTableParses) {
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    int cars = 0, impacts = 0, surfaces = 0, sirens = 0, ambient = 0, creatures = 0, speech = 0, horns = 0, engines = 0;
    for (const auto& e : v.listFiles()) {
        const std::string& p = e.path;
        if (!p.ends_with(".csv") || p.find("copy of") != std::string::npos || p.find(".wrk.") != std::string::npos)
            continue;
        auto bytes = v.readAll(p);
        ASSERT_TRUE(bytes) << p;
        const auto text = asText(*bytes);
        const auto name = p.substr(p.rfind('/') + 1);
        if (p.starts_with("aud/cardata/player/") || p.starts_with("aud/cardata/opponent/")) {
            if (name == "default_impacts.csv") {
                EXPECT_TRUE(parseImpactTable(text)) << p;
                ++impacts;
            } else if (name.starts_with("default_surface")) {
                auto t = parseSurfaceTable(text);
                EXPECT_TRUE(t) << p;
                if (t) {
                    EXPECT_GE(t->surfaces.size(), 5u) << p;
                }
                ++surfaces;
            } else if (name.find("policesiren") != std::string::npos) {
                EXPECT_TRUE(parseSirenTable(text)) << p;
                ++sirens;
            } else if (name == "suspensionaudio.csv") {
                EXPECT_TRUE(parseSuspension(text)) << p;
            } else if (name == "tirewobble.csv") {
                EXPECT_TRUE(parseTireWobble(text)) << p;
            } else if (name.starts_with("vp") || name == "default.csv") {
                auto def = parseCarAudio(text);
                EXPECT_TRUE(def) << p;
                ++cars;
            }
        } else if (p.starts_with("aud/cardata/ambient/")) {
            if (name.ends_with("_engine.csv")) {
                EXPECT_TRUE(parseAmbientEngine(text)) << p;
                ++engines;
            } else if (name.ends_with("_horn.csv")) {
                auto h = parseHorn(text);
                EXPECT_TRUE(h) << p;
                if (h) {
                    EXPECT_FALSE(h->patterns.empty()) << p;
                }
                ++horns;
            }
        } else if (p.starts_with("aud/ambient/") && !name.ends_with("container.csv")) {
            EXPECT_TRUE(parseAmbientSoundSet(name, text)) << p;
            ++ambient;
        } else if (p.starts_with("aud/creaturedata/") && !name.starts_with("num")) {
            EXPECT_TRUE(parseCreatureVoice(text)) << p;
            ++creatures;
        } else if (p.starts_with("aud/spchdata/") && p.find('/', 13) != std::string::npos &&
                   name.find("index") == std::string::npos) {
            EXPECT_TRUE(parseSpeechTable(text)) << p;
            ++speech;
        }
    }
    EXPECT_GE(cars, 40);
    EXPECT_EQ(impacts, 2);
    EXPECT_EQ(surfaces, 6);
    EXPECT_GE(sirens, 3);
    EXPECT_GE(engines, 8);
    EXPECT_GE(horns, 8);
    EXPECT_GE(ambient, 12);
    EXPECT_GE(creatures, 15);
    EXPECT_GE(speech, 400);
}

TEST(GameAudioRetail, EveryReferencedSampleExists) {
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    SoundBank bank(v);
    std::vector<std::string> missing;
    auto need = [&](const std::string& wave, const std::string& where) {
        if (!wave.empty() && !str::iequals(wave, "NOSOUND") && bank.resolve(wave).empty())
            missing.push_back(wave + " (" + where + ")");
    };
    for (const char* folder : {"player", "opponent"}) {
        for (const auto& e : v.listFiles()) {
            const std::string prefix = std::string("aud/cardata/") + folder + "/vp";
            if (!e.path.starts_with(prefix) || !e.path.ends_with(".csv") || e.path.find("copy") != std::string::npos ||
                e.path.find(".wrk.") != std::string::npos)
                continue;
            auto def = parseCarAudio(asText(*v.readAll(e.path)));
            ASSERT_TRUE(def) << e.path;
            need(def->horn, e.path);
            need(def->clutch, e.path);
            for (const auto& s : def->engine)
                need(s.wave, e.path);
        }
        for (const char* t : {"default_impacts.csv"}) {
            auto table = parseImpactTable(asText(*v.readAll(std::string("aud/cardata/") + folder + "/" + t)));
            for (const auto& b : table->bangers)
                for (const auto& s : b.samples)
                    need(s.wave, t);
        }
    }
    // Reported, not failed: the shipped data may reference unused samples.
    for (const auto& m : missing)
        std::printf("missing sample: %s\n", m.c_str());
    EXPECT_LT(missing.size(), 8u);
}

TEST(GameAudioRetail, PlayerCarSweepSkidAndImpact) {
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    SoundBank bank(v);
    Mixer mixer(48000);
    PlayerCarAudio car;
    std::string err;
    ASSERT_TRUE(car.load(v, bank, mixer, "vpbug", {}, &err)) << err;
    ASSERT_EQ(car.engine().sampleCount(), 4u); // VWIDLE, VWDRIVE, VWMID, VWHIGH
    EXPECT_FALSE(car.police());

    CarAudioInputs in = grounded();
    in.rpm = 0; // at rest: the samples play at idle RPM
    car.update(in, 0.016f);
    // Every vpbug sample has a minimum volume of 0.55: all four loop at once.
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_TRUE(car.engine().state(i).audible) << i;
    float lastPitch = 0;
    for (float rpm = 800; rpm <= 8500; rpm += 100) {
        in.rpm = rpm;
        car.update(in, 0.016f);
        EXPECT_GE(car.engine().state(1).pitch, lastPitch - 1e-4f);
        lastPitch = car.engine().state(1).pitch;
    }
    const int engineVoices = mixer.activeVoices();
    EXPECT_EQ(engineVoices, 4);

    in.speed = 15.0f;
    for (auto& w : in.wheels)
        w.slip = 0.9f;
    car.update(in, 0.016f);
    EXPECT_TRUE(car.surfaces().skidding());
    EXPECT_EQ(mixer.activeVoices(), engineVoices + 1);

    in.impacts = {{25000.0f, 0, {}}};
    car.update(in, 0.016f);
    EXPECT_EQ(mixer.activeVoices(), engineVoices + 2);
}

TEST(GameAudioRetail, PoliceAndFireTruckHornsToggleTheSiren) {
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    SoundBank bank(v);
    Mixer mixer(48000);
    for (const char* name : {"vpcop", "vpsemi"}) {
        PlayerCarAudio car;
        std::string err;
        ASSERT_TRUE(car.load(v, bank, mixer, name, {}, &err)) << err;
        EXPECT_TRUE(car.police()) << name; // vehtypes.csv lists both as police
        CarAudioInputs in = grounded();
        in.rpm = 2000;
        car.update(in, 0.02f);
        in.horn = true;
        car.update(in, 0.02f);
        EXPECT_TRUE(car.sirenOn()) << name;
        car.update(in, 0.02f); // held: no change
        EXPECT_TRUE(car.sirenOn()) << name;
        in.horn = false;
        car.update(in, 0.02f);
        in.horn = true;
        car.update(in, 0.02f);
        EXPECT_FALSE(car.sirenOn()) << name;
        car.stop();
    }
}

TEST(GameAudioRetail, AnnouncerAndAmbience) {
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    SoundBank bank(v);
    Mixer mixer(48000);
    for (const char* city : {"london", "sf"}) {
        Announcer a;
        ASSERT_TRUE(a.load(v, bank, mixer, city)) << city;
        EXPECT_GE(a.announcerCount(), 5);
        for (int i = 1; i <= a.announcerCount(); ++i) {
            if (!v.exists("aud/spchdata/" + std::string(city == std::string("sf") ? "as" : "al") + std::to_string(i) +
                          "/cruise.csv"))
                continue; // SF lists five announcers but ships four
            a.beginSession(i);
            a.beginRace(AnnouncerMode::Blitz, "vpbug", 1, 0);
            std::string line;
            for (int k = 0; k < 30 && line.empty(); ++k)
                line = a.playPreRace();
            EXPECT_FALSE(line.empty()) << city << " announcer " << i;
            a.stop();
            a.beginRace(AnnouncerMode::Circuit, {}, 1, 0);
            EXPECT_FALSE(a.playResults(RaceOutcome::Win).empty()) << city << i;
            a.stop();
        }
        a.beginSession(1);
        ASSERT_TRUE(a.beginCrashCourse(0)) << city;
        EXPECT_FALSE(a.playCrashCoursePreRace().empty()) << city;
        a.stop();
        a.beginCopsAndRobbers();
        EXPECT_FALSE(a.playCopsAndRobbers("ROBGETLOOT").empty()) << city;
        a.stop();

        CityAmbience amb;
        ASSERT_TRUE(amb.load(v, bank, mixer, city)) << city;
        EXPECT_GE(amb.setCount(), 2u);
    }
}

TEST(GameAudioRetail, WreckedCopExplodesAgainUntilReset) {
    // aiPoliceOfficer::Update calls PerpEscapes(true) on every update while
    // the driver's wrecked flag is set, siren or not: PlayExplosion starts the
    // explosion again once the last one has finished.
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    SoundBank bank(v);
    Mixer mixer(48000);
    const Mat34 listener = Mat34::identity();
    OpponentCarAudio cop;
    std::string err;
    ASSERT_TRUE(cop.load(v, bank, mixer, "vpcop", true, {}, &err)) << err;
    CarAudioInputs in = grounded();
    in.rpm = 3000;
    in.transform.m3 = {10, 0, 0};
    cop.update(in, 0.02f, listener); // takes a sound slot (PlayExplosion needs one)
    in.wrecked = true;
    cop.update(in, 0.02f, listener);
    EXPECT_TRUE(cop.explosionPlaying());
    EXPECT_FALSE(cop.sirenOn());
    auto playOut = [&] {
        std::vector<float> out(2 * 4800);
        for (int i = 0; i < 300 && cop.explosionPlaying(); ++i)
            mixer.mix(out.data(), 4800);
    };
    playOut();
    ASSERT_FALSE(cop.explosionPlaying());
    cop.update(in, 0.02f, listener); // still wrecked: again
    EXPECT_TRUE(cop.explosionPlaying());
    playOut();
    in.wrecked = false; // Reset clears the flag
    cop.update(in, 0.02f, listener);
    EXPECT_FALSE(cop.explosionPlaying());
    cop.stop();
}

TEST(GameAudioRetail, OpponentAmbientAndCityEmitters) {
    MM2_REQUIRE_GAME_DATA();
    const auto& v = *test::gameData();
    SoundBank bank(v);
    Mixer mixer(48000);
    const Mat34 listener = Mat34::identity();

    OpponentCarAudio cop;
    std::string err;
    ASSERT_TRUE(cop.load(v, bank, mixer, "vpcop", true, {}, &err)) << err;
    CarAudioInputs in = grounded();
    in.rpm = 3000;
    in.transform.m3 = {10, 0, 0};
    cop.update(in, 0.02f, listener);
    EXPECT_GE(mixer.activeVoices(), 1); // the engine
    in.siren = true;
    cop.update(in, 0.02f, listener);
    EXPECT_TRUE(cop.sirenOn());
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 1);
    const int withSiren = mixer.activeVoices(); // the siren replaces the engine
    EXPECT_GE(withSiren, 1);
    in.transform.m3 = {1000, 0, 0}; // a siren stays audible at any distance
    cop.update(in, 0.02f, listener);
    EXPECT_TRUE(cop.audible());
    EXPECT_GE(mixer.activeVoices(), 1);
    in.siren = false;
    cop.update(in, 0.02f, listener);
    EXPECT_FALSE(cop.audible());
    EXPECT_EQ(mixer.activeVoices(), 0);
    EXPECT_EQ(SirenPlayer::copsPursuingPlayer(), 0);

    AmbientCarAudio sedan; // va_sedans_s has no files of its own: the default engine and horn
    ASSERT_TRUE(sedan.load(v, bank, mixer, "va_sedans_s"));
    Mat34 at = Mat34::identity();
    at.m3 = {5, 0, 0};
    sedan.update(12.0f, at, {}, 0.02f, listener);
    EXPECT_EQ(mixer.activeVoices(), 1);
    EXPECT_TRUE(sedan.honk(0));
    sedan.update(12.0f, at, {}, 0.02f, listener);
    EXPECT_EQ(mixer.activeVoices(), 2);
    sedan.stop();

    // London: the river emitters are audible next to the Thames.
    CityAmbience amb;
    ASSERT_TRUE(amb.load(v, bank, mixer, "london"));
    const auto* river = amb.set("londonriver");
    ASSERT_TRUE(river);
    ASSERT_FALSE(river->points.empty());
    Mat34 there = Mat34::identity();
    there.m3 = river->points.front();
    for (int i = 0; i < 30 * 50; ++i) // 30 s next to the first river point
        amb.update(there, 0.02f);
    EXPECT_TRUE(amb.audible("londonriver"));
    EXPECT_GE(mixer.activeVoices(), 1);
    // The tube voices are only heard underground.
    const auto* tube = amb.set("tubevoices");
    ASSERT_TRUE(tube);
    EXPECT_EQ(tube->audibleArea, 1);
    there.m3 = tube->points.front();
    amb.update(there, 0.02f, false);
    EXPECT_FALSE(amb.audible("tubevoices"));
    amb.update(there, 0.02f, true);
    EXPECT_TRUE(amb.audible("tubevoices"));
}

// OpenMM2 Enhanced: MM1's tune/<car>.mmplayercaraudio.
TEST(Mm1CarAudio, PlayerTuneAndEngineFormula) {
    const char* text = R"(mmPlayerCarAudio :01f9b158 {
    m_pcHornWaveName "Mustanghorn"
    m_bFlags 0
    m_fHornVolume .95
    m_EngineAudio EngineAudio :01f9b1a4 {
            m_pcHighRpmWave ""
            m_pcLowRpmWave "Enginesedan1"
            m_pcClutchWave "Reverse"
            m_fMinVolume[kHiRpmIndex] 0.0
            m_fMaxVolume[kHiRpmIndex] 0.0
            m_fVolumeMultiplier[kHiRpmIndex] 0.000
            m_fMinVolume[kLowRpmIndex] 0.89
            m_fMaxVolume[kLowRpmIndex] 0.89
            m_fVolumeMultiplier[kLowRpmIndex] 0.000181818
            m_fMinPitch[kHiRpmIndex] 0.0
            m_fMaxPitch[kHiRpmIndex] 0
            m_fPitchMultiplier[kHiRpmIndex] 0.0
            m_fMinPitch[kLowRpmIndex] 0.25
            m_fMaxPitch[kLowRpmIndex] 1.20
            m_fPitchMultiplier[kLowRpmIndex] 0.000181818
            m_fClutchVolume 0.93
        }
})";
    auto def = parseMm1PlayerCarAudio(text);
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(def->horn, "mm1:mustanghorn");
    EXPECT_NEAR(def->hornVolume, 0.95f, 1e-5f);
    EXPECT_EQ(def->clutch, "mm1:reverse");
    EXPECT_NEAR(def->clutchVolume, 0.93f, 1e-5f);
    ASSERT_EQ(def->engine.size(), 1u); // the empty high-rpm wave has no sample
    const auto& e = def->engine[0];
    EXPECT_EQ(e.wave, "mm1:enginesedan1");
    // Constant volume 0.89, pitch rpm / 5500 within 0.25..1.2.
    EXPECT_NEAR(EngineSound::evaluate(e, 1000.0f).volume, 0.89f, 1e-5f);
    EXPECT_NEAR(EngineSound::evaluate(e, 1000.0f).pitch, 0.25f, 1e-5f);
    EXPECT_NEAR(EngineSound::evaluate(e, 3300.0f).pitch, 0.6f, 1e-3f);
    EXPECT_NEAR(EngineSound::evaluate(e, 9000.0f).pitch, 1.2f, 1e-5f);
}
