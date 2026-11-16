// Car sounds, ported from MM2 (vehCarAudioContainer, vehCarAudio,
// vehEngineAudio, vehEngineSampleWrapper, vehSurfaceAudio,
// vehSurfaceAudioData, vehPoliceCarAudio, vehSemiCarAudio, AudImpact,
// AudImpactData, aiAmbientVehicleAudio, aiEngineAudio, vehHornAudio,
// vehHornAudioTiming). See docs/audio.md.
#include "audio/game/CarAudio.h"

#include "audio/AngelRandom.h"
#include "audio/game/Voices.h"
#include "core/Log.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::audio::game {
namespace {

// vehSurfaceAudio::UpdateSurface: no rolling sound at or below 2 m/s.
constexpr float kSurfaceMinSpeed = 2.0f;
// vehSurfaceAudio::UpdateAir: the airborne probe covers 3 to 33 m below.
constexpr float kAirProbeStart = 3.0f, kAirProbeEnd = 33.0f;
// vehSemiCarAudio::UpdateAirBlow: the air brake hisses once the truck is
// (almost) stopped.
constexpr float kAirBlowMaxSpeed = 0.04f;
// vehPoliceCarAudio::UpdateSiren: positioned sirens never drop below this.
constexpr float kSirenMinVolume = 0.75f;
// vehPoliceCarAudio::UpdateExplosion.
constexpr float kExplosionMinVolume = 0.85f;
// vehPoliceCarAudio constructor: the explosion's volume (+0x140).
constexpr float kExplosionVolume = 1.0f;
// aiAmbientVehicleAudio::UpdateAudio: a speed drop above this per update is
// a crash, not braking; the engine pitch then decays by 5% per update.
constexpr float kAmbientSpeedDrop = 4.0f;
// vehHornAudio::PlayImpact: RandomizeNumber(10) must reach 7.5.
constexpr float kImpactHornChance = 7.5f;
// vehSurfaceAudio::SetWheelPointers: the wobble distance is the rear left
// wheel's circumference with this value of 2 pi.
constexpr float kTwoPi = 6.28318f;

// vehSemiCarAudio's air brake latch is a single global in MM2, shared by
// every semi.
bool g_airBlown = false;
// vehPoliceCarAudio::s_iNumCopsPursuingPlayer.
int g_copsPursuingPlayer = 0;

// vehCarAudio::EchoOn: the horn's echo follows 0.05 s behind at 0.997 of the
// sample rate, whatever the manager's delay.
constexpr float kHornEchoDelay = 0.05f, kHornEchoFrequency = 0.997f;

// The car is in a tunnel: its inputs say so or its manager's echo is on.
bool inTunnel(const CarAudioInputs& in, const Object3DManager* manager) {
    return in.inTunnel || (manager && manager->echo());
}

// SetEffect(1), SetDelayTime(delay), SetEchoAttenuation(0.96): the EchoOn
// of one sample.
void sampleEchoOn(SoundSlot& s, float delay) {
    s.enableEcho();
    s.setEchoDelay(delay);
    s.setEchoAttenuation(kEchoAttenuation);
}

// The clamps MM2 writes as "low if below, else high if above": when high is
// below low a value above high gives high.
float mm2Clamp(float v, float low, float high) {
    if (!(low <= v))
        return low;
    return high < v ? high : v;
}

std::optional<CarAudioDef> loadCarDef(const vfs::Vfs& vfs, const std::string& path, std::string* error) {
    auto text = readText(vfs, path);
    if (!text) {
        if (error)
            *error = std::format("{} not found", path);
        return std::nullopt;
    }
    return parseCarAudio(*text, error);
}

// vehCarAudio::Init: Load(car), else Load("default").
std::optional<CarAudioDef> loadCarOrDefault(const vfs::Vfs& vfs, std::string_view folder,
                                            std::string_view car, std::string* error) {
    if (auto def = loadCarDef(vfs, std::format("aud/cardata/{}/{}.csv", folder, str::lower(car)), error))
        return def;
    return loadCarDef(vfs, std::format("aud/cardata/{}/default.csv", folder), error);
}

template <class T, class Fn>
std::optional<T> loadTable(const vfs::Vfs& vfs, const std::string& path, Fn parse) {
    auto text = readText(vfs, path);
    if (!text) {
        log::debug("audio: {} not found", path);
        return std::nullopt;
    }
    return parse(*text);
}

// vehCarAudio::Init: rain picks the wet table, every other weather (snow
// included) the dry one; default_surfaceice.csv is never loaded. The tables
// always come from aud/cardata/player, for opponents too.
const char* surfaceFile(SurfaceWeather w) {
    return w == SurfaceWeather::Wet ? "aud/cardata/player/default_surfacewet.csv"
                                    : "aud/cardata/player/default_surfacedry.csv";
}

// mmGame::Init: "londonpolicesiren" in London, "sfpolicesiren" anywhere else.
std::string sirenFile(std::string_view city) {
    const char* table = str::iequals(city, "london") ? "london" : "sf";
    return std::format("aud/cardata/player/{}policesiren.csv", table);
}

VehicleTypes loadVehicleTypes(const vfs::Vfs& vfs) {
    if (auto text = readText(vfs, "aud/cardata/shared/vehtypes.csv"))
        return parseVehicleTypes(*text);
    return {};
}

// OpenMM2 Enhanced: MM1's banger impact sounds (mmImpactAudio::GetBangerPtrs
// by the prop's AudioId 3..27, volumes from mmGame's SetBangerVolumeRange
// calls: [min, max, divisor], default 0.9..1.0 / 35). Ids MM1 has no sound for
// (0, 1, 2, 9, 21 and beyond) keep MM2's table.
ImpactTable mm1ImpactTable(ImpactTable base) {
    struct Rule {
        int id;
        std::vector<std::pair<const char*, float>> samples; // wave, frequency
        float lo, hi, div;
        bool alternate;
    };
    const Rule rules[] = {
        {3, {{"trafficlightimpact", 1.0f}}, 0.85f, 0.88f, 35.0f, false},
        {4, {{"signimpact2", 1.0f}, {"signimpact3", 1.0f}}, 0.81f, 0.83f, 35.0f, true},
        {5, {{"mailimpact1", 1.0f}}, 0.85f, 0.87f, 35.0f, false},
        {6, {{"parkingmeter6", 1.0f}}, 0.89f, 0.91f, 35.0f, false},
        {7, {{"trashimpact", 1.0f}}, 0.87f, 0.89f, 35.0f, false},
        {8, {{"parkbenchimpact", 1.0f}}, 0.87f, 0.89f, 35.0f, false},
        {10, {{"bushimpact", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {11, {{"carimpactmed1", 1.0f}, {"bushimpact", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {12, {{"billboardimpact", 1.0f}}, 0.9f, 0.92f, 35.0f, false},
        {13, {{"roadworksawhorse", 1.0f}}, 0.94f, 0.96f, 35.0f, false},
        {14, {{"fountainsplash", 0.5f}}, 0.9f, 1.0f, 35.0f, false},
        {15, {{"trashimpact", 1.4967f}}, 0.9f, 1.0f, 35.0f, false},
        {16, {{"mailimpact1", 1.26f}}, 0.9f, 1.0f, 35.0f, false},
        {17, {{"carimpactmed1", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {18, {{"carimpactsoft3", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {19, {{"carimpactsoft3", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {20, {{"carimpacthard3", 1.0f}, {"parkbenchimpact", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {22, {{"caroncobble", 1.0f}}, 0.93f, 1.0f, 35.0f, false},
        {23, {{"carimpacthard3", 1.0f}}, 0.9f, 1.0f, 35.0f, false},
        {24, {{"trafficlightimpact", 1.5f}}, 0.9f, 0.93f, 35.0f, false},
        {25, {{"mailimpact1", 1.5f}}, 0.9f, 0.95f, 35.0f, false},
        {26, {{"fountainsplash", 0.5f}}, 0.98f, 1.0f, 35.0f, false},
        {27, {{"glassbreak", 1.0f}}, 0.94f, 0.96f, 15.0f, false},
    };
    if (base.bangers.size() < 28)
        base.bangers.resize(28);
    for (const Rule& r : rules) {
        BangerSoundDef& b = base.bangers[static_cast<std::size_t>(r.id)];
        b.name = "MM1";
        b.id = r.id;
        b.mm1Alternate = r.alternate;
        b.samples.clear();
        for (const auto& [wave, freq] : r.samples) {
            ImpactSampleDef d;
            d.wave = std::string("mm1:") + wave;
            d.minVolume = r.lo;
            d.maxVolume = r.hi;
            d.frequency = freq;
            d.mm1Divisor = r.div;
            b.samples.push_back(d);
        }
    }
    return base;
}

// OpenMM2 Enhanced: MM1's skid sample per surface sound index (mmCarAudio's
// sample sets: clear {Surfacegrassskid, Skidcobble, Skidflagstone1, Tireskid1},
// rain {Grassskidwet, Skidcobble, Skidflagstone1, Wetskid}, snow {Snowskid,
// Tireskid1}; mmSurfaceAudio::UpdateSkidClear picks by the surface: 2 grass,
// 3 cobble, 4 flagstone, the rest the tyre sample). The variant cycling on
// the road samples (Tireskid1..3) is not ported.
std::string mm1SkidSample(int surface, SurfaceWeather w) {
    if (w == SurfaceWeather::Snow)
        return surface == 0 || surface == 1 ? "mm1:tireskid1" : "mm1:snowskid";
    const bool wet = w == SurfaceWeather::Wet;
    switch (surface) {
    case 2: return wet ? "mm1:grassskidwet" : "mm1:surfacegrassskid";
    case 3: return "mm1:skidcobble";
    case 4: return "mm1:skidflagstone1";
    default: return wet ? "mm1:wetskid" : "mm1:tireskid1";
    }
}

void loadSurfaces(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, SurfaceSounds& surfaces,
                  SurfaceWeather w, bool mm1Sounds) {
    auto parse = [](std::string_view s) { return parseSurfaceTable(s); };
    if (auto t = loadTable<SurfaceTable>(vfs, surfaceFile(w), parse)) {
        if (mm1Sounds) {
            for (std::size_t i = 0; i < t->surfaces.size(); ++i) {
                auto& def = t->surfaces[i];
                const std::string first = mm1SkidSample(static_cast<int>(i), w);
                def.skids = {{first, 0.0f, 100.0f}};
                // Road: skids[0] up to 35 m/s (the first AudSound, always variant 3,
                // Tireskid1 / Wetskid: its counter clamps there), above it the
                // second one's variants in turn (UpdateSkidClear; rain has two).
                def.cycleAboveSpeed = -1.0f;
                if (first == "mm1:tireskid1" && w == SurfaceWeather::Dry) {
                    def.skids = {{"mm1:tireskid1", 0.0f, 100.0f}, {"mm1:tireskid1", 0.0f, 100.0f},
                                 {"mm1:tireskid2", 0.0f, 100.0f}, {"mm1:tireskid3", 0.0f, 100.0f}};
                    def.cycleAboveSpeed = 35.0f;
                } else if (first == "mm1:wetskid") {
                    def.skids = {{"mm1:wetskid", 0.0f, 100.0f}, {"mm1:wetskid", 0.0f, 100.0f},
                                 {"mm1:wetskid2", 0.0f, 100.0f}};
                    def.cycleAboveSpeed = 35.0f;
                }
            }
        }
        surfaces.load(mixer, bank, *t, Bus::Effects);
    }
    // vehCarAudio::Init loads suspension and tyre wobble for every car.
    if (auto d = loadTable<SuspensionDef>(vfs, "aud/cardata/player/suspensionaudio.csv", parseSuspension))
        surfaces.loadSuspension(mixer, bank, *d, Bus::Effects);
    if (auto d = loadTable<TireWobbleDef>(vfs, "aud/cardata/player/tirewobble.csv", parseTireWobble))
        surfaces.loadTireWobble(mixer, bank, *d, Bus::Effects);
}

int mm2Gear(int gear) { return gear + 1; } // MM2: 0 reverse, 1 neutral, 2 first

int groundedWheels(const CarAudioInputs& in) {
    int n = 0;
    for (const auto& w : in.wheels)
        n += w.onGround ? 1 : 0;
    return n;
}

} // namespace

float impactStrength(const Vec3& impulse) {
    return std::abs(impulse.z) + std::abs(impulse.y) + std::abs(impulse.x);
}

int surfaceSoundIndex(std::string_view, int mtlSound) {
    // The material keeps the sound as a short (+0x26).
    const auto sound = static_cast<std::int16_t>(mtlSound);
    return sound == -1 ? 0 : sound;
}

std::string carAudioPath(const vfs::Vfs& vfs, std::string_view folder, std::string_view car) {
    const std::string specific = std::format("aud/cardata/{}/{}.csv", folder, str::lower(car));
    if (vfs.exists(specific))
        return specific;
    return std::format("aud/cardata/{}/default.csv", folder);
}

// --- EngineSound --------------------------------------------------------------------

EngineSound::Evaluation EngineSound::evaluate(const EngineSampleDef& d, float rpm, bool silenced) {
    Evaluation e;
    if (d.mm1) {
        // mmEngineAudio::UpdateRPM: v = clamp(x, min, max) as written there
        // (<= min gives min, >= max gives max); pitch likewise. The "low"
        // sample's volume runs on (0 - rpm): the field the asm subtracts from
        // (+0x30) is never set.
        const auto clamp = [](float v, float lo, float hi) { return v <= lo ? lo : (v < hi ? v : hi); };
        const float x = d.mm1Low ? (0.0f - rpm) * d.mm1VolumeMultiplier : rpm * d.mm1VolumeMultiplier;
        e.volume = clamp(x, d.minVolume, d.maxVolume);
        e.pitch = clamp(rpm * d.mm1PitchMultiplier, d.minPitch, d.maxPitch);
        e.audible = !(e.volume < kSilentVolume);
        return e;
    }
    // ParseCSVBuffer precomputes the slopes (0 for an empty range) from the
    // table; Silence later zeroes only the minimum and maximum.
    const float inRange = d.fadeInEndRpm - d.fadeInStartRpm;
    const float outRange = d.fadeOutEndRpm - d.fadeOutStartRpm;
    const float inSlope = (d.maxVolume - d.minVolume) * (inRange != 0.0f ? 1.0f / inRange : 0.0f);
    const float outSlope = (d.maxVolume - d.minVolume) * (outRange != 0.0f ? 1.0f / outRange : 0.0f);
    const float minVolume = silenced ? 0.0f : d.minVolume;
    const float maxVolume = silenced ? 0.0f : d.maxVolume;
    if (d.oldLayout) {
        // CalculateVolumeOld: rpm / divisor below the cut RPM, divisor / rpm
        // from it; the minimum first, then the maximum. A divisor of 0 (or 0
        // RPM at the cut) divides by zero in MM2; OpenMM2 keeps a NaN out of
        // the mixer (malformed data only).
        float v = rpm < d.cutRpm ? rpm / d.volumeDivisor : d.volumeDivisor / rpm;
        if (std::isnan(v))
            v = 0.0f;
        if (v < minVolume)
            v = minVolume;
        else if (maxVolume < v)
            v = maxVolume;
        e.volume = v;
    } else if (rpm <= d.fadeInStartRpm || d.fadeOutEndRpm <= rpm)
        e.volume = minVolume;
    else if (rpm < d.fadeInEndRpm)
        e.volume = (rpm - d.fadeInStartRpm) * inSlope + minVolume;
    else if (rpm <= d.fadeOutStartRpm)
        e.volume = maxVolume;
    else
        e.volume = (d.fadeOutEndRpm - rpm) * outSlope + minVolume;
    // CalculatePitch.
    const float pitchRange = d.pitchEndRpm - d.pitchStartRpm;
    const float pitchSlope = (d.maxPitch - d.minPitch) * (pitchRange != 0.0f ? 1.0f / pitchRange : 0.0f);
    if (rpm <= d.pitchStartRpm)
        e.pitch = d.minPitch;
    else if (d.pitchEndRpm <= rpm)
        e.pitch = d.maxPitch;
    else
        e.pitch = rpm * pitchSlope + d.minPitch;
    e.audible = !(e.volume < kSilentVolume);
    return e;
}

void EngineSound::load(Mixer& mixer, SoundBank& bank, const std::vector<EngineSampleDef>& samples, Bus bus) {
    stop();
    m_defs = samples;
    m_samples.clear();
    m_samples.resize(samples.size());
    m_states.assign(samples.size(), {});
    m_silenced = false;
    for (std::size_t i = 0; i < samples.size(); ++i)
        m_samples[i].load(mixer, bank, samples[i].wave, bus);
}

void EngineSound::silence(bool on) { m_silenced = on; }

void EngineSound::update(float rpm, float dt) {
    // vehEngineSampleWrapper::UpdateRPM(rpm), ending with the sample's echo
    // update (its +0x40 echo flag).
    for (std::size_t i = 0; i < m_defs.size(); ++i) {
        const Evaluation e = evaluate(m_defs[i], rpm, m_silenced);
        m_states[i] = e;
        auto& s = m_samples[i];
        if (!e.audible) {
            if (s.playing())
                s.stop();
        } else {
            s.setVolume(e.volume);
            s.setPitch(e.pitch);
            if (!s.playing())
                s.playLoop();
        }
        s.updateEcho(dt);
    }
}

void EngineSound::update3D(float rpm, float attenuation, float doppler, float pan, float dt) {
    // vehEngineSampleWrapper::UpdateRPM(rpm, volume, frequency, pan).
    for (std::size_t i = 0; i < m_defs.size(); ++i) {
        const Evaluation e = evaluate(m_defs[i], rpm, m_silenced);
        m_states[i] = e;
        auto& s = m_samples[i];
        if (!e.audible) {
            if (s.playing())
                s.stop();
        } else {
            s.setVolume(e.volume * attenuation);
            s.setPitch(e.pitch * doppler);
            s.setPan(pan);
            if (!s.playing())
                s.playLoop();
        }
        s.updateEcho(dt);
    }
}

void EngineSound::echoOn(float delay) {
    for (auto& s : m_samples)
        sampleEchoOn(s, delay);
}

void EngineSound::echoOff() {
    for (auto& s : m_samples)
        s.disableEcho();
}

void EngineSound::stop() {
    for (auto& s : m_samples)
        s.stop();
}

// --- SurfaceSounds ------------------------------------------------------------------

bool SurfaceSounds::skidInRange(const SkidSampleDef& skid, float slip) {
    return !(slip < skid.min || skid.max < slip);
}

float SurfaceSounds::skidVolumeFor(const SurfaceSoundDef& def, float slip) {
    return slip * (def.maxSkidVolume - def.minSkidVolume) + def.minSkidVolume;
}

float SurfaceSounds::surfaceVolumeFor(const SurfaceSoundDef& def, float speed) {
    if (def.maxSpeed <= speed)
        return def.maxVolume;
    const float inv = def.maxSpeed != 0.0f ? 1.0f / def.maxSpeed : 0.0f;
    return speed * ((def.maxVolume - def.minVolume) * inv) + def.minVolume;
}

float SurfaceSounds::surfacePitchFor(const SurfaceSoundDef& def, float speed) {
    if (def.maxSpeed <= speed)
        return def.maxPitch;
    const float inv = def.maxSpeed != 0.0f ? 1.0f / def.maxSpeed : 0.0f;
    return speed * ((def.maxPitch - def.minPitch) * inv) + def.minPitch;
}

void SurfaceSounds::load(Mixer& mixer, SoundBank& bank, const SurfaceTable& table, Bus bus) {
    stop();
    m_entries.clear();
    m_entries.resize(table.surfaces.size());
    for (std::size_t i = 0; i < table.surfaces.size(); ++i) {
        Entry& e = m_entries[i];
        e.def = table.surfaces[i];
        // vehSurfaceAudioData::ParseCSVBuffer: NOSOUND (exact case) has no sample.
        if (e.def.hasSurfaceSound())
            e.surface.load(mixer, bank, e.def.wave, bus);
        e.skids.resize(e.def.skids.size());
        for (std::size_t k = 0; k < e.def.skids.size(); ++k)
            if (e.def.skids[k].wave != "NOSOUND")
                e.skids[k].load(mixer, bank, e.def.skids[k].wave, bus);
    }
    m_tunnelIndex = table.tunnelIndex;
    m_surface = 0;
    m_previous = -1;
    m_skidding = false;
    m_airborne = false;
}

void SurfaceSounds::loadSuspension(Mixer& mixer, SoundBank& bank, const SuspensionDef& def, Bus bus) {
    m_suspensionDef = def;
    m_suspension.load(mixer, bank, def.wave, bus);
}

void SurfaceSounds::loadTireWobble(Mixer& mixer, SoundBank& bank, const TireWobbleDef& def, Bus bus) {
    m_wobbleDef = def;
    m_wobble.load(mixer, bank, def.wave, bus);
    m_wobbleDistance = 0.0f;
}

SurfaceSounds::Entry* SurfaceSounds::entry(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_entries.size())
        return nullptr;
    return &m_entries[static_cast<std::size_t>(index)];
}

void SurfaceSounds::stopSurface(int index) {
    if (Entry* e = entry(index))
        e->surface.stop();
}

void SurfaceSounds::stopSkid(int index) {
    if (Entry* e = entry(index))
        for (auto& s : e->skids)
            s.stop();
}

bool SurfaceSounds::skidPlayingOn(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= m_entries.size())
        return false;
    for (const auto& s : m_entries[static_cast<std::size_t>(index)].skids)
        if (s.playing())
            return true;
    return false;
}

bool SurfaceSounds::skidPlaying(int k) const {
    if (m_surface < 0 || static_cast<std::size_t>(m_surface) >= m_entries.size())
        return false;
    const auto& skids = m_entries[static_cast<std::size_t>(m_surface)].skids;
    return k >= 0 && static_cast<std::size_t>(k) < skids.size() && skids[static_cast<std::size_t>(k)].playing();
}

bool SurfaceSounds::surfaceChanged(const CarAudioInputs& in) const {
    // Either front wheel still on the current surface keeps it.
    return surfaceSoundIndex({}, in.wheels[0].surface) != m_surface &&
           surfaceSoundIndex({}, in.wheels[1].surface) != m_surface;
}

void SurfaceSounds::selectSurface(const CarAudioInputs& in, bool positioned) {
    if (m_inTunnel || in.inTunnel) {
        m_surface = m_tunnelIndex;
        return;
    }
    if (!surfaceChanged(in))
        return;
    if (positioned)
        stopSurface(m_surface);
    m_surface = surfaceSoundIndex({}, in.wheels[0].surface);
    if (!positioned && m_previous != m_surface)
        stopSurface(m_previous);
    if (static_cast<int>(m_entries.size()) <= m_surface)
        m_surface = 0;
}

void SurfaceSounds::silence() {
    stopSurface(m_surface);
    stopSkid(m_surface);
}

void SurfaceSounds::echoOn(float delay) {
    // vehSurfaceAudioData::EchoOn: the skid samples, then the surface sample.
    for (auto& e : m_entries) {
        for (auto& s : e.skids)
            sampleEchoOn(s, delay);
        sampleEchoOn(e.surface, delay);
    }
}

void SurfaceSounds::echoOff() {
    for (auto& e : m_entries) {
        for (auto& s : e.skids)
            s.disableEcho();
        e.surface.disableEcho();
    }
}

void SurfaceSounds::updateEcho(float dt) {
    // vehSurfaceAudio::UpdateEcho: the entry at +0 (the current surface).
    if (Entry* e = entry(m_surface)) {
        for (auto& s : e->skids)
            s.updateEcho(dt);
        e->surface.updateEcho(dt);
    }
}

void SurfaceSounds::stop() {
    for (auto& e : m_entries) {
        e.surface.stop();
        for (auto& s : e.skids)
            s.stop();
    }
    m_suspension.stop();
    m_wobble.stop();
    m_skidding = false;
}

void SurfaceSounds::updateSurface(const CarAudioInputs& in, bool positioned, float attenuation, float pan) {
    const int grounded = groundedWheels(in);
    if (grounded < 2) {
        stopSurface(m_surface);
        if (positioned)
            return;
        if (grounded == 0) {
            // UpdateAir: "big air" once the probe from 3 to 33 m below finds
            // something; nothing else clears it.
            if (in.groundBelow && kAirProbeStart <= *in.groundBelow && *in.groundBelow <= kAirProbeEnd)
                m_airborne = true;
        } else {
            m_airborne = false;
        }
        return;
    }
    if (!positioned)
        m_airborne = false;
    if (kSurfaceMinSpeed < in.speed && !skidPlayingOn(m_surface)) {
        selectSurface(in, positioned);
        Entry* e = entry(m_surface);
        if (e && e->surface.valid()) {
            // vehSurfaceAudioData::UpdateSurface: frequency, volume, pan, play.
            e->surface.setPitch(surfacePitchFor(e->def, in.speed));
            e->surface.setVolume(surfaceVolumeFor(e->def, in.speed) * attenuation);
            if (positioned)
                e->surface.setPan(pan);
            if (!e->surface.playing())
                e->surface.playLoop();
            m_previous = m_surface;
            return;
        }
    }
    stopSurface(m_surface);
}

void SurfaceSounds::updateSkid(const CarAudioInputs& in, float attenuation, float pan) {
    float slip = 0.0f;
    if (0.0f < in.wheels[0].slip)
        slip = in.wheels[0].slip;
    for (std::size_t w = 1; w < in.wheels.size(); ++w)
        if (slip < in.wheels[w].slip)
            slip = in.wheels[w].slip;
    if (m_inTunnel || in.inTunnel) {
        m_surface = m_tunnelIndex;
    } else if (surfaceChanged(in)) {
        stopSkid(m_surface);
        m_surface = surfaceSoundIndex({}, in.wheels[0].surface);
        if (static_cast<int>(m_entries.size()) <= m_surface)
            m_surface = 0;
    }
    Entry* e = entry(m_surface);
    if (0.0f < slip) {
        // vehSurfaceAudioData::UpdateSkid.
        if (e) {
            for (std::size_t k = 0; k < e->skids.size(); ++k) {
                auto& s = e->skids[k];
                if (e->def.cycleAboveSpeed >= 0.0f && e->skids.size() > 1 &&
                    k != (in.speed > e->def.cycleAboveSpeed ? 1 + m_skidVariant % (e->skids.size() - 1) : 0)) {
                    if (s.playing())
                        s.stop();
                    continue;
                }
                if (!skidInRange(e->def.skids[k], slip)) {
                    if (s.playing())
                        s.stop();
                    continue;
                }
                s.setVolume((in.skidVolume >= 0.0f ? in.skidVolume : skidVolumeFor(e->def, slip)) * attenuation);
                s.setPan(pan);
                if (!s.playing())
                    s.playLoop();
            }
        }
        m_skidding = true;
        m_previous = m_surface;
        return;
    }
    if (m_skidding) {
        stopSkid(m_surface);
        m_skidding = false;
        ++m_skidVariant; // the next sample for the next skid (cycleSkids)
    }
    m_previous = m_surface;
}

void SurfaceSounds::updateSuspension(const CarAudioInputs& in, bool positioned, float attenuation, float pan) {
    if (!m_suspensionDef || !m_suspension.valid())
        return;
    const auto& d = *m_suspensionDef;
    const float average = (in.wheels[0].suspensionSpeed + in.wheels[1].suspensionSpeed +
                           in.wheels[2].suspensionSpeed + in.wheels[3].suspensionSpeed) *
                          0.25f;
    if (!(std::abs(d.minVelocity) <= average) || groundedWheels(in) < 2 || m_suspension.playing())
        return;
    const float volume = mm2Clamp(average * d.volumeScale, d.minVolume, d.maxVolume);
    m_suspension.setVolume(volume * attenuation);
    if (positioned)
        m_suspension.setPan(pan);
    m_suspension.playOnce();
}

void SurfaceSounds::updateTireWobble(const CarAudioInputs& in, float dt, bool positioned, float attenuation,
                                     float pan) {
    if (!m_wobbleDef || !m_wobble.valid())
        return;
    float damage = in.tireWobble;
    if (!(0.0f <= damage))
        return;
    if (damage <= 1.0f) {
        if (damage <= 0.05f)
            return;
    } else {
        damage = 1.0f;
    }
    // One thump per revolution of the rear left wheel.
    float travelled = in.speed * dt + m_wobbleDistance;
    if (in.wheelRadius * kTwoPi <= travelled) {
        if (m_wobble.playing())
            return; // the distance is not stored: the thump comes when the last one ends
        const auto& d = *m_wobbleDef;
        const float volume = mm2Clamp(damage, d.minVolume, d.maxVolume);
        if (positioned) {
            m_wobble.setVolume(volume * attenuation);
            m_wobble.setPan(pan);
        } else {
            m_wobble.setVolume(volume);
            m_wobble.setPitch(mm2Clamp(in.speed * d.pitchScale, d.minPitch, d.maxPitch));
        }
        m_wobble.playOnce();
        travelled = 0.0f;
    }
    m_wobbleDistance = travelled;
}

void SurfaceSounds::update(const CarAudioInputs& in, float dt) {
    if (m_entries.empty())
        return;
    updateSuspension(in, false, 1.0f, 0.0f);
    updateSurface(in, false, 1.0f, 0.0f);
    updateSkid(in, 1.0f, 0.0f);
    updateTireWobble(in, dt, false, 1.0f, 0.0f);
}

void SurfaceSounds::update3D(const CarAudioInputs& in, float dt, float attenuation, float pan) {
    if (m_entries.empty())
        return;
    updateSuspension(in, true, attenuation, pan);
    updateSurface(in, true, attenuation, pan);
    updateSkid(in, attenuation, pan);
    updateTireWobble(in, dt, true, attenuation, pan);
}

// --- ImpactSounds ---------------------------------------------------------------------

float ImpactSounds::volumeFor(const ImpactSampleDef& s, float force) {
    const float range = s.maxForce - s.minForce;
    const float slope = (s.maxVolume - s.minVolume) * (range != 0.0f ? 1.0f / range : 0.0f);
    return slope * force + s.minVolume;
}

void ImpactSounds::load(Mixer& mixer, SoundBank& bank, const ImpactTable& table, Bus bus) {
    stop();
    m_bangers.clear();
    for (const auto& b : table.bangers) {
        Banger out;
        out.defs = b.samples;
        out.alternate = b.mm1Alternate;
        out.slots.resize(b.samples.size());
        out.volumes.assign(b.samples.size(), 0.0f);
        for (std::size_t i = 0; i < b.samples.size(); ++i) {
            out.slots[i].load(mixer, bank, b.samples[i].wave, bus);
            // AudImpactData::AssignSounds sets the frequency once.
            if (b.samples[i].frequency != 1.0f)
                out.slots[i].setPitch(b.samples[i].frequency);
        }
        m_bangers.push_back(std::move(out));
    }
    m_last = -1;
}

void ImpactSounds::play(const ImpactInput& impact, float attenuation, float pan) {
    // AudImpact::Play(force, index): -1 plays nothing.
    if (impact.audioId == -1 || m_bangers.empty())
        return;
    // The index is kept as given; GetAudImpactDataPtr maps an out-of-range one
    // to the first banger.
    m_last = impact.audioId;
    const bool inRange = impact.audioId >= 0 && static_cast<std::size_t>(impact.audioId) < m_bangers.size();
    const std::size_t index = inRange ? static_cast<std::size_t>(impact.audioId) : 0;
    Banger& b = m_bangers[index];
    // MM1's alternating sign hits: the counter runs 0..10, the first sample up
    // to 5, the second after (GetBangerPtrs).
    std::size_t only = b.defs.size();
    if (b.alternate && b.defs.size() > 1) {
        b.counter = b.counter + 1 > 10 ? 0 : b.counter + 1;
        only = b.counter > 5 ? 1 : 0;
    }
    for (std::size_t i = 0; i < b.defs.size(); ++i) {
        const auto& d = b.defs[i];
        if (only != b.defs.size() && i != only)
            continue;
        if (d.mm1Divisor > 0.0f) {
            // mmImpactAudio::PlayBanger: nothing under strength 3.
            if (impact.force < 3.0f)
                continue;
        } else if (impact.force < d.minForce || d.maxForce < impact.force) {
            continue;
        }
        // AudImpactData::PlaySample.
        auto& slot = b.slots[i];
        if (!slot.valid() || slot.playing())
            continue;
        const float volume = d.mm1Divisor > 0.0f
                                 ? std::clamp(impact.force / d.mm1Divisor, d.minVolume, d.maxVolume)
                                 : volumeFor(d, impact.force);
        slot.setVolume(volume * attenuation);
        slot.setPan(pan);
        slot.playOnce();
        b.volumes[i] = volume;
    }
}

void ImpactSounds::updateAttenuation(float attenuation, float pan) {
    if (m_last == -1 || m_bangers.empty())
        return;
    const std::size_t index = m_last < 0 || static_cast<std::size_t>(m_last) >= m_bangers.size() ? 0 : m_last;
    Banger& b = m_bangers[index];
    for (std::size_t i = 0; i < b.slots.size(); ++i) {
        if (!b.slots[i].playing())
            continue;
        b.slots[i].setVolume(attenuation * b.volumes[i]);
        b.slots[i].setPan(pan);
    }
}

void ImpactSounds::stop() {
    for (auto& b : m_bangers)
        for (auto& s : b.slots)
            s.stop();
}

// --- SirenPlayer ------------------------------------------------------------------------

int SirenPlayer::copsPursuingPlayer() { return g_copsPursuingPlayer; }
void SirenPlayer::resetPursuitCount() { g_copsPursuingPlayer = 0; }

void SirenPlayer::load(Mixer& mixer, SoundBank& bank, const SirenTable& table, Bus bus) {
    stopAll();
    m_samples.clear();
    m_samples.resize(table.samples.size());
    for (std::size_t i = 0; i < table.samples.size(); ++i) {
        m_samples[i].def = table.samples[i];
        m_samples[i].slot.load(mixer, bank, table.samples[i].wave, bus);
        m_samples[i].slot.setVolume(table.samples[i].volume); // AssignSounds
        m_samples[i].step = 0;
    }
    m_explosion.load(mixer, bank, table.explosion, bus);
    m_explosion.setVolume(kExplosionVolume);
    m_current = 0;
    m_state = 0;
    m_timer = 0;
    m_damaged = false;
    m_damageTime = 0.01f;
    // ReadSirenData leaves the last sample's volume in +0x144.
    m_volume = m_samples.empty() ? 0.0f : m_samples.back().def.volume;
    // Every vehPoliceCarAudio constructor resets the pursuit count.
    g_copsPursuingPlayer = 0;
}

void SirenPlayer::start(bool pursuingPlayer, bool audible, float attenuation, float doppler) {
    if (m_state != 0 || m_samples.empty())
        return;
    m_state = pursuingPlayer ? 1 : 2;
    Sample& s = current();
    m_volume = s.def.volume;
    if (m_state == 1)
        ++g_copsPursuingPlayer;
    if (audible && !s.slot.playing()) {
        // The player's car passes attenuation 1 and frequency 1.
        s.slot.setVolume(attenuation * s.def.volume);
        s.slot.setPitch(doppler);
        s.slot.playLoop();
    }
}

void SirenPlayer::stop() {
    if (g_copsPursuingPlayer > 0 && m_state == 1)
        --g_copsPursuingPlayer;
    m_state = 0;
    if (!m_samples.empty() && current().slot.playing())
        current().slot.stop();
}

void SirenPlayer::silence() {
    for (auto& s : m_samples)
        if (s.slot.playing())
            s.slot.stop();
}

void SirenPlayer::stopAll() {
    stop();
    silence();
    m_explosion.stop();
}

void SirenPlayer::echoOn(float delay) {
    for (auto& s : m_samples)
        sampleEchoOn(s.slot, delay);
}

void SirenPlayer::echoOff() {
    for (auto& s : m_samples)
        s.slot.disableEcho();
}

void SirenPlayer::updateEcho(float dt) {
    for (auto& s : m_samples)
        s.slot.updateEcho(dt);
}

void SirenPlayer::fluctuate(float dt) {
    // FluctuateSiren.
    if (m_samples.size() == 1)
        return;
    Sample& s = current();
    if (s.def.steps.empty())
        return;
    const SirenStep step = s.def.steps[s.step];
    if (step.playTime <= m_timer) {
        if (++s.step >= s.def.steps.size())
            s.step = 0;
        if (s.slot.playing())
            s.slot.stop();
        // OpenMM2 keeps an out-of-range "next index" inside the table.
        m_current = std::clamp(step.next, 0, static_cast<int>(m_samples.size()) - 1);
        current().slot.playLoop();
        m_timer = 0.0f;
    }
    m_timer += dt;
}

void SirenPlayer::damage(float attenuation, float doppler) {
    // DamageSiren.
    Sample& s = current();
    float pitch;
    if (m_damageTime > 1.0f) {
        if (m_damageTime > 1.75f) {
            if (m_volume > 0.2f) {
                const float old = m_volume;
                m_volume = old * 0.5f;
                s.slot.setVolume(attenuation * old * 0.5f);
                return;
            }
            s.slot.stop();
            m_damaged = false;
            m_state = 0;
            return;
        }
        pitch = 0.45f;
    } else {
        pitch = 0.5f;
    }
    s.slot.setVolume(attenuation * s.def.volume);
    s.slot.setPitch(doppler * pitch);
    m_damageTime += m_dt;
}

void SirenPlayer::update(float dt) {
    // vehPoliceCarAudio::UpdateAudioNon3D: UpdateSiren() while the siren is on.
    if (m_state == 0 || m_samples.empty())
        return;
    m_dt = dt;
    fluctuate(dt);
    // A non-positioned object's attenuation (+0xc) stays 0; the player's car
    // never explodes anyway (only aiPoliceOfficer::PerpEscapes calls
    // PlayExplosion).
    if (m_damaged)
        damage(0.0f, 1.0f);
    if (!current().slot.playing())
        current().slot.playLoop();
}

void SirenPlayer::update3D(float dt, float attenuation, float doppler, float pan) {
    // UpdateSiren(volume, frequency, pan), then UpdateExplosion. Its fading
    // branch (+0x152) is unreachable: PerpEscapes sets the flag and the
    // StopSiren that follows clears it.
    if (m_state == 0 || m_samples.empty())
        return;
    m_dt = dt;
    fluctuate(dt);
    Sample& s = current();
    if (!m_damaged) {
        float volume = s.def.volume * attenuation;
        if (volume < kSirenMinVolume)
            volume = kSirenMinVolume;
        s.slot.setVolume(volume);
        s.slot.setPitch(doppler);
    } else {
        damage(attenuation, doppler);
    }
    if (!s.slot.playing())
        s.slot.playLoop();
    s.slot.setPan(pan);
    if (m_explosion.playing()) {
        float volume = attenuation * kExplosionVolume;
        if (volume < kExplosionMinVolume)
            volume = kExplosionMinVolume;
        m_explosion.setVolume(volume);
        m_explosion.setPan(pan);
    }
}

void SirenPlayer::explode(bool audible, float attenuation, float doppler) {
    // PlayExplosion.
    if (audible && m_explosion.valid() && !m_explosion.playing()) {
        m_explosion.setVolume(attenuation * kExplosionVolume);
        m_explosion.playOnce();
    }
    if (m_samples.empty())
        return;
    m_volume = current().def.volume;
    // The pursuit count drops here and again in the StopSiren that follows
    // (aiPoliceOfficer::PerpEscapes): MM2 counts an exploding cop twice.
    if (g_copsPursuingPlayer > 0 && m_state == 1)
        --g_copsPursuingPlayer;
    if (audible && current().slot.valid()) {
        m_damaged = true;
        m_damageTime = 0.01f;
        damage(attenuation, doppler);
    } else {
        m_state = 0;
    }
}

// --- PlayerCarAudio ---------------------------------------------------------------------

bool PlayerCarAudio::load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view car,
                          const CarAudioOptions& options, std::string* error) {
    stop();
    // OpenMM2 Enhanced: an MM1 city's cars sound from MM1's own tune file
    // (tune/<car>.mmplayercaraudio, else default.mmplayercaraudio).
    std::optional<CarAudioDef> def;
    if (options.mm1Sounds) {
        for (const std::string& path : {std::format("mm1/tune/{}.mmplayercaraudio", str::lower(car)),
                                        std::string("mm1/tune/default.mmplayercaraudio")}) {
            if (auto text = readText(vfs, path)) {
                std::string err;
                if ((def = parseMm1PlayerCarAudio(*text, &err)))
                    break;
                log::warn("audio: {}: {}", path, err);
            }
        }
    }
    if (!def)
        def = loadCarOrDefault(vfs, "player", car, error);
    if (!def)
        return false;
    m_def = std::move(*def);
    m_engine.load(mixer, bank, m_def.engine, Bus::Engine);
    m_horn.load(mixer, bank, m_def.horn, Bus::Effects);
    m_clutch.load(mixer, bank, m_def.clutch, Bus::Engine);
    // vehCarAudio::SetNon3DParams.
    m_horn.setVolume(m_def.hornVolume);
    m_horn.setPitch(1.0f);
    m_clutch.setVolume(m_def.clutchVolume);
    loadSurfaces(vfs, bank, mixer, m_surfaces, options.weather, options.mm1Sounds);
    if (auto t = loadTable<ImpactTable>(vfs, "aud/cardata/player/default_impacts.csv",
                                        [](std::string_view s) { return parseImpactTable(s); }))
        m_impacts.load(mixer, bank, options.mm1Sounds ? mm1ImpactTable(std::move(*t)) : *t, Bus::Effects);

    // vehCarAudioContainer: the sound class comes from vehtypes.csv, semis first.
    const VehicleTypes types = loadVehicleTypes(vfs);
    m_semi.reset();
    m_siren.reset();
    m_reverseBeep = SoundSlot();
    m_airBlow = SoundSlot();
    if (types.isFreight(car)) {
        m_semi = loadTable<SemiDef>(vfs, "aud/cardata/shared/semidata.csv", parseSemiData);
        if (m_semi) {
            m_reverseBeep.load(mixer, bank, m_semi->reverse, Bus::Effects);
            m_airBlow.load(mixer, bank, m_semi->airBlow, Bus::Effects);
            // vehSemiCarAudio::SetNon3DParams gives the air brake the reverse
            // beeper's volume.
            m_reverseBeep.setVolume(m_semi->reverseVolume);
            m_reverseBeep.setPitch(1.0f);
            m_airBlow.setVolume(m_semi->reverseVolume);
            m_airBlow.setPitch(1.0f);
        }
    } else if (types.isPolice(car)) {
        if (auto t = loadTable<SirenTable>(vfs, sirenFile(options.city),
                                           [](std::string_view s) { return parseSirenTable(s); })) {
            m_siren.emplace();
            m_siren->load(mixer, bank, *t, Bus::Effects);
        }
    }
    m_prevGear = -1;
    m_hornPressed = false;
    g_airBlown = false;
    m_manager = options.manager;
    m_echo = false;
    return true;
}

void PlayerCarAudio::echoOn(float delay) {
    // vehPoliceCarAudio::EchoOn (the sirens) or vehSemiCarAudio::EchoOn (the
    // reverse beeper and air brake), then vehCarAudio::EchoOn.
    if (m_siren)
        m_siren->echoOn(delay);
    if (m_semi)
        for (auto* s : {&m_reverseBeep, &m_airBlow})
            sampleEchoOn(*s, delay);
    m_engine.echoOn(delay);
    m_surfaces.echoOn(delay);
    sampleEchoOn(m_horn, kHornEchoDelay);
    m_horn.setEchoFrequency(kHornEchoFrequency);
    sampleEchoOn(m_clutch, delay);
    m_echo = true;
}

void PlayerCarAudio::echoOff() {
    if (m_siren)
        m_siren->echoOff();
    if (m_semi)
        for (auto* s : {&m_reverseBeep, &m_airBlow})
            s->disableEcho();
    m_engine.echoOff();
    m_surfaces.echoOff();
    m_horn.disableEcho();
    m_clutch.disableEcho();
    m_echo = false;
}

void PlayerCarAudio::updateEcho(float dt) {
    // The sirens or semi samples, then vehCarAudio::UpdateEcho: horn, clutch
    // and the current surface. The engine samples update theirs in UpdateRPM.
    if (m_siren)
        m_siren->updateEcho(dt);
    if (m_semi)
        for (auto* s : {&m_reverseBeep, &m_airBlow})
            s->updateEcho(dt);
    m_horn.updateEcho(dt);
    m_clutch.updateEcho(dt);
    m_surfaces.updateEcho(dt);
}

void PlayerCarAudio::updateEchoState(bool tunnel, float dt) {
    if (m_siren || m_semi) {
        // vehPoliceCarAudio / vehSemiCarAudio::UpdateAudio: no echo update on
        // the update that turns the echo on.
        if (!m_echo) {
            if (tunnel)
                echoOn(tunnelEchoDelay(m_manager));
        } else {
            if (!tunnel)
                echoOff();
            updateEcho(dt);
        }
        return;
    }
    // vehCarAudio::UpdateAudio.
    if (!m_echo && tunnel)
        echoOn(tunnelEchoDelay(m_manager));
    else if (m_echo && !tunnel)
        echoOff();
    if (m_echo)
        updateEcho(dt);
}

void PlayerCarAudio::updateHorn(bool pressed) {
    // mmGame::UpdateHorn. A car with siren lights (every police-list car in
    // the retail data: vpcop, vpsemi) toggles its siren on each press; the
    // container never plays a police car's horn.
    if (!pressed) {
        if (!m_siren && m_hornPressed && m_horn.playing())
            m_horn.stop(); // vehCarAudio::StopHorn
        m_hornPressed = false;
        return;
    }
    if (!m_hornPressed) {
        if (m_siren) {
            if (m_siren->on())
                m_siren->stop();
            else
                m_siren->start(false);
        } else if (m_horn.valid() && !m_horn.playing()) {
            m_horn.playLoop(); // vehCarAudio::PlayHorn
        }
    }
    m_hornPressed = true;
}

void PlayerCarAudio::update(const CarAudioInputs& in, float dt) {
    // vehCarDamage::ApplyImpact plays the impacts during the simulation, before
    // the audio update.
    for (const auto& impact : in.impacts)
        m_impacts.play(impact);

    // vehCarAudio::UpdateAudio: the tunnel echo, then UpdateAudioNon3D (or the
    // police / semi variant).
    const bool tunnel = inTunnel(in, m_manager);
    m_surfaces.setTunnel(tunnel);
    updateEchoState(tunnel, dt);

    // UpdateGear: the clutch sample plays whenever the gear changes into or out
    // of reverse (MM2 gear 0). The first update counts as a change from "no
    // gear" (-1), so starting in reverse plays it too.
    const int gear = mm2Gear(in.gear);
    // MM1 (mmEngineAudio::UpdateRPM): only between reverse (0) and first (2); it
    // starts in state 0.
    const bool clutchChange = m_def.mm1 ? ((m_prevGear < 0 ? 0 : m_prevGear) == 0 && gear == 2) ||
                                              ((m_prevGear < 0 ? 0 : m_prevGear) == 2 && gear == 0)
                                        : ((m_prevGear == 0) != (gear == 0));
    if (m_clutch.valid() && clutchChange)
        m_clutch.playOnce();
    m_prevGear = gear;

    m_engine.update(in.rpm, dt);
    m_surfaces.update(in, dt);

    if (m_semi) {
        // vehSemiCarAudio::UpdateReverse.
        if (gear == 0) {
            if (!m_reverseBeep.playing())
                m_reverseBeep.playLoop();
        } else if (m_reverseBeep.playing()) {
            m_reverseBeep.stop();
        }
        // UpdateAirBlow: "braking" is vehSurfaceAudio::IsBrakeing, which tests
        // the front wheels' BrakeCoef tuning (> 0.5), not the pedal. Every
        // retail semi has 0.5, so the air brake never sounds.
        const bool braking = 0.5f < in.wheels[0].brakeCoef && 0.5f < in.wheels[1].brakeCoef;
        if (kAirBlowMaxSpeed < in.speed) {
            if (!braking)
                g_airBlown = false;
        } else if (braking) {
            if (!g_airBlown) {
                m_airBlow.playOnce();
                g_airBlown = true;
            }
        } else {
            g_airBlown = false;
        }
    }
    if (m_siren)
        m_siren->update(dt);

    // mmGame::UpdateHorn follows the controls.
    updateHorn(in.horn);
}

void PlayerCarAudio::stop() {
    if (m_echo)
        echoOff();
    m_engine.stop();
    m_surfaces.stop();
    m_impacts.stop();
    for (auto* s : {&m_horn, &m_clutch, &m_reverseBeep, &m_airBlow})
        s->stop();
    if (m_siren)
        m_siren->stopAll();
}

// --- OpponentCarAudio ---------------------------------------------------------------------

bool OpponentCarAudio::load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view car, bool police,
                            const CarAudioOptions& options, std::string* error) {
    stop();
    setManager(options.manager);
    // OpenMM2 Enhanced: an MM1 city's AI cars sound from MM1's tune files
    // (tune/<car>_opp / _cop.mmopponentcaraudio, a network player's
    // <car>.mmnetworkcaraudio, else the default).
    std::optional<CarAudioDef> def;
    if (options.mm1Sounds) {
        const std::string lc = str::lower(car);
        std::vector<std::string> paths;
        if (options.horn)
            paths.push_back(std::format("mm1/tune/{}.mmnetworkcaraudio", lc));
        else
            paths.push_back(std::format("mm1/tune/{}{}.mmopponentcaraudio", lc, police ? "_cop" : "_opp"));
        paths.push_back(std::format("mm1/tune/{}.mmopponentcaraudio", lc));
        paths.push_back(options.horn ? "mm1/tune/default.mmnetworkcaraudio" : "mm1/tune/default.mmopponentcaraudio");
        for (const std::string& path : paths) {
            if (auto text = readText(vfs, path)) {
                std::string err;
                if ((def = parseMm1OpponentCarAudio(*text, &err)))
                    break;
                log::warn("audio: {}: {}", path, err);
            }
        }
    }
    if (!def)
        def = loadCarOrDefault(vfs, "opponent", car, error);
    if (!def)
        return false;
    m_def = std::move(*def);
    m_engine.load(mixer, bank, m_def.engine, Bus::Engine);
    // vehCarAudio::Load: positioned cars have no clutch sample, and a horn
    // only in vehCarAudioContainer mode 0 (network players).
    m_hasHorn = options.horn;
    m_horn = SoundSlot();
    if (m_hasHorn)
        m_horn.load(mixer, bank, m_def.horn, Bus::Effects);
    loadSurfaces(vfs, bank, mixer, m_surfaces, options.weather, options.mm1Sounds);
    if (auto t = loadTable<ImpactTable>(vfs, "aud/cardata/opponent/default_impacts.csv",
                                        [](std::string_view s) { return parseImpactTable(s); }))
        m_impacts.load(mixer, bank, options.mm1Sounds ? mm1ImpactTable(std::move(*t)) : *t, Bus::Effects);

    // vehCarAudioContainer: semis first, then police (InitPolice), for the
    // names in vehtypes.csv. `police` is OpenMM2's flag for AI police cars
    // whose model is not in the list; MM2 goes by the name alone.
    m_siren.reset();
    m_semi.reset();
    m_reverseBeep = SoundSlot();
    m_airBlow = SoundSlot();
    const VehicleTypes types = loadVehicleTypes(vfs);
    if (types.isFreight(car)) {
        m_semi = loadTable<SemiDef>(vfs, "aud/cardata/shared/semidata.csv", parseSemiData);
        if (m_semi) {
            m_reverseBeep.load(mixer, bank, m_semi->reverse, Bus::Effects);
            m_airBlow.load(mixer, bank, m_semi->airBlow, Bus::Effects);
        }
    } else if (police || types.isPolice(car)) {
        if (auto t = loadTable<SirenTable>(vfs, sirenFile(options.city),
                                           [](std::string_view s) { return parseSirenTable(s); })) {
            m_siren.emplace();
            m_siren->load(mixer, bank, *t, Bus::Effects);
        }
    }
    // vehCarAudio::Init priority 9, vehPoliceCarAudio::Init 7.
    m_priority = m_siren ? 7 : 9;
    m_3d = Audio3D();
    m_3d.setDropOffs(0.0f, kMaxDistance);
    m_attenuation = 0.0f;
    m_doppler = 1.0f;
    m_pan = 0.0f;
    m_hornPressed = m_prevSiren = false;
    return true;
}

void OpponentCarAudio::echoOn(float delay) {
    // As PlayerCarAudio::echoOn; a positioned car has no clutch sample, and a
    // horn only as a network player's car.
    if (m_siren)
        m_siren->echoOn(delay);
    if (m_semi)
        for (auto* s : {&m_reverseBeep, &m_airBlow})
            sampleEchoOn(*s, delay);
    m_engine.echoOn(delay);
    m_surfaces.echoOn(delay);
    sampleEchoOn(m_horn, kHornEchoDelay);
    m_horn.setEchoFrequency(kHornEchoFrequency);
    m_echo = true;
}

void OpponentCarAudio::echoOff() {
    if (m_siren)
        m_siren->echoOff();
    if (m_semi)
        for (auto* s : {&m_reverseBeep, &m_airBlow})
            s->disableEcho();
    m_engine.echoOff();
    m_surfaces.echoOff();
    m_horn.disableEcho();
    m_echo = false;
}

void OpponentCarAudio::updateEcho(float dt) {
    if (m_siren)
        m_siren->updateEcho(dt);
    if (m_semi)
        for (auto* s : {&m_reverseBeep, &m_airBlow})
            s->updateEcho(dt);
    m_horn.updateEcho(dt);
    m_surfaces.updateEcho(dt);
}

void OpponentCarAudio::updateEchoState(bool tunnel, float dt) {
    if (m_siren || m_semi) {
        // vehPoliceCarAudio / vehSemiCarAudio::UpdateAudio.
        if (!m_echo) {
            if (tunnel)
                echoOn(tunnelEchoDelay(manager()));
        } else {
            if (!tunnel)
                echoOff();
            updateEcho(dt);
        }
        return;
    }
    if (!m_echo && tunnel)
        echoOn(tunnelEchoDelay(manager()));
    else if (m_echo && !tunnel)
        echoOff();
    if (m_echo)
        updateEcho(dt);
}

void OpponentCarAudio::silence() {
    // vehCarAudio::UnAssignSounds (and the semi / police variants): the echo
    // goes off, the horn, engine, surface and siren stop; impacts, thumps and
    // an explosion play out.
    if (m_echo)
        echoOff();
    m_engine.stop();
    m_surfaces.silence();
    if (m_horn.playing())
        m_horn.stop();
    if (m_siren)
        m_siren->silence();
    if (m_reverseBeep.playing())
        m_reverseBeep.stop();
    if (m_airBlow.playing())
        m_airBlow.stop();
}

void OpponentCarAudio::update(const CarAudioInputs& in, float dt, const Vec3& listener) {
    Mat34 l = Mat34::identity();
    l.m3 = listener;
    update(in, dt, l);
}

void OpponentCarAudio::update(const CarAudioInputs& in, float dt, const Mat34& listener) {
    const Vec3 position = in.transform.m3;

    // vehCarDamage::ApplyImpact during the simulation: only a car holding a
    // slot has its AudImpact; the sounds use the last attenuation and pan.
    if (hasSlot())
        for (const auto& impact : in.impacts)
            m_impacts.play(impact, m_attenuation, m_pan);

    // aiPoliceOfficer::StartSiren / StopSiren / PerpEscapes change the siren
    // state whether or not the car has a sound slot; only a car with a slot
    // starts the samples.
    if (m_siren) {
        const bool audible = hasSlot();
        if (in.siren && !m_prevSiren && !m_siren->on()) {
            m_priority = 10;
            m_3d.alwaysAudible = true;
            m_siren->start(in.sirenPursuingPlayer, audible, m_attenuation, m_doppler);
            if (audible)
                m_engine.stop(); // StartSiren silences a positioned car's engine
        }
        // aiPoliceOfficer::Update calls PerpEscapes(true) on every update
        // while the driver's wrecked flag is set (aiVehiclePhysics +0x9686):
        // PlayExplosion, which starts the explosion again once the last one
        // has finished, then StopSiren.
        if (in.wrecked) {
            m_siren->explode(audible, m_attenuation, m_doppler);
            m_siren->stop();
            m_priority = 7;
            m_3d.alwaysAudible = false;
        } else if (!in.siren && m_prevSiren) {
            m_siren->stop();
            m_priority = 7;
            m_3d.alwaysAudible = false;
        }
    }
    m_prevSiren = in.siren;

    // A network player's horn: vehCarAudioContainer::PlayHorn / StopHorn,
    // latched.
    if (m_hasHorn) {
        if (in.horn && !m_hornPressed) {
            if (hasSlot() && m_horn.valid() && !m_horn.playing())
                m_horn.playLoop();
            m_hornPressed = true;
        } else if (!in.horn && m_hornPressed) {
            if (m_horn.playing())
                m_horn.stop();
            m_hornPressed = false;
        }
    }

    // Aud3DObject::Update: ask for a slot while within the maximum distance.
    if (!hasSlot() && !acquireSlot(m_3d.withinMaxDistance(position, listener.m3)))
        return;

    // vehCarAudio::UpdateAudio (Aud3DObjectManager::Update, for slot
    // holders): the tunnel echo, then UpdateAudio3D(doppler factor).
    const bool tunnel = inTunnel(in, manager());
    m_surfaces.setTunnel(tunnel);
    updateEchoState(tunnel, dt);

    if (m_3d.pastMaxDistance(position, listener.m3)) {
        // vehPoliceCarAudio::UpdateAudio3D keeps the slot (and the values of the
        // last update) while an explosion plays.
        if (!(m_siren && m_siren->explosionPlaying())) {
            releaseSlot();
            silence();
            return;
        }
    } else {
        // The 25 m "amplification" multiplies by 1 + 0.01 * a speed field nothing
        // sets, i.e. by 1, and only for the horn and clutch: not ported.
        m_attenuation = m_3d.attenuation();
        m_pan = m_3d.pan(listener, position);
        m_doppler = m_3d.doppler(1.0f / kDopplerSpeed, dt);
        m_impacts.updateAttenuation(m_attenuation, m_pan);
        if (m_hasHorn && m_horn.valid()) {
            m_horn.setVolume(m_attenuation * m_def.hornVolume);
            m_horn.setPitch(m_doppler);
            m_horn.setPan(m_pan);
        }
        if (m_semi) {
            // vehSemiCarAudio::UpdateAudio3D.
            for (auto* s : {&m_reverseBeep, &m_airBlow})
                s->setPan(m_pan);
            m_reverseBeep.setVolume(m_attenuation * m_semi->reverseVolume);
            m_reverseBeep.setPitch(m_doppler);
            m_airBlow.setVolume(m_attenuation * m_semi->airBlowVolume);
            m_airBlow.setPitch(m_doppler);
        }
    }

    // vehCarAudio / vehPoliceCarAudio::UpdateAudio3D(): surfaces, then the
    // engine or (police with the siren on) the siren.
    if (m_siren) {
        m_surfaces.update3D(in, dt, m_attenuation, m_pan);
        if (m_siren->on())
            m_siren->update3D(dt, m_attenuation, m_doppler, m_pan);
        else
            m_engine.update3D(in.rpm, m_attenuation, m_doppler, m_pan, dt);
        return;
    }
    m_engine.update3D(in.rpm, m_attenuation, m_doppler, m_pan, dt);
    m_surfaces.update3D(in, dt, m_attenuation, m_pan);
    if (m_semi) {
        // vehSemiCarAudio::UpdateReverse / UpdateAirBlow.
        const int gear = mm2Gear(in.gear);
        if (gear == 0) {
            if (!m_reverseBeep.playing())
                m_reverseBeep.playLoop();
        } else if (m_reverseBeep.playing()) {
            m_reverseBeep.stop();
        }
        const bool braking = 0.5f < in.wheels[0].brakeCoef && 0.5f < in.wheels[1].brakeCoef;
        if (kAirBlowMaxSpeed < in.speed) {
            if (!braking)
                g_airBlown = false;
        } else if (braking) {
            if (!g_airBlown) {
                m_airBlow.playOnce();
                g_airBlown = true;
            }
        } else {
            g_airBlown = false;
        }
    }
}

void OpponentCarAudio::reset() {
    // Aud3DObject::Reset for a positioned object: RemoveFrom3DMgr when it
    // holds a slot, then the distance values.
    if (hasSlot()) {
        releaseSlot();
        silence();
    }
    m_3d.reset();
    m_attenuation = 0.0f; // +0xc
    m_pan = 0.0f;         // +4
    m_doppler = 1.0f;     // +0x10
}

void OpponentCarAudio::stop() {
    releaseSlot();
    if (m_echo)
        echoOff();
    m_engine.stop();
    m_surfaces.stop();
    m_impacts.stop();
    for (auto* s : {&m_horn, &m_reverseBeep, &m_airBlow})
        s->stop();
    if (m_siren)
        m_siren->stopAll();
}

// --- AmbientCarAudio ----------------------------------------------------------------------

std::optional<float> AmbientCarAudio::pitchFor(const AmbientEngineDef& def, float speed, bool slowing) {
    if (def.bands.empty())
        return std::nullopt;
    auto slope = [](const SpeedBand& b) {
        const float range = b.maxSpeed - b.minSpeed;
        return (b.maxPitch - b.minPitch) * (range != 0.0f ? 1.0f / range : 0.0f);
    };
    if (slowing) {
        const SpeedBand& last = def.bands.back();
        return speed * slope(last) + last.minPitch;
    }
    for (std::size_t i = 0; i + 1 < def.bands.size(); ++i) {
        const SpeedBand& b = def.bands[i];
        if (!(speed < b.minSpeed || b.maxSpeed < speed))
            return (speed - b.minSpeed) * slope(b) + b.minPitch;
    }
    return std::nullopt;
}

bool AmbientCarAudio::load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view type,
                           Object3DManager* manager) {
    stop();
    setManager(manager);
    // aiAmbientVehicleAudio::Init: <type>_engine / <type>_horn, else default_.
    auto find = [&](std::string_view suffix) {
        const std::string specific = std::format("aud/cardata/ambient/{}_{}.csv", str::lower(type), suffix);
        return vfs.exists(specific) ? specific : std::format("aud/cardata/ambient/default_{}.csv", suffix);
    };
    m_engineDef = {};
    if (auto text = readText(vfs, find("engine")))
        if (auto def = parseAmbientEngine(*text))
            m_engineDef = std::move(*def);
    m_hornDef.reset();
    if (auto text = readText(vfs, find("horn")))
        m_hornDef = parseHorn(*text);
    m_engine.load(mixer, bank, m_engineDef.wave, Bus::Engine);
    m_horn = SoundSlot();
    if (m_hornDef)
        m_horn.load(mixer, bank, m_hornDef->wave, Bus::Effects);
    if (auto t = loadTable<ImpactTable>(vfs, "aud/cardata/opponent/default_impacts.csv",
                                        [](std::string_view s) { return parseImpactTable(s); }))
        m_impacts.load(mixer, bank, *t, Bus::Effects);
    m_3d = Audio3D();
    m_3d.setDropOffs(0.0f, kMaxDistance);
    m_pitch = 0.0f;
    m_speed = m_prevSpeed = 0.0f;
    m_attenuation = 1.0f;
    m_pan = 0.0f;
    m_hornState = 2;
    m_pattern = 0;
    m_beeps.assign(m_hornDef ? m_hornDef->patterns.size() : 0, 0);
    m_hornAttenuation = m_hornDoppler = 1.0f;
    m_hornPan = 0.0f;
    return m_engine.valid();
}

void AmbientCarAudio::startPattern(std::size_t index) {
    // vehHornAudio::UpdateDoppler with the stored values (nothing is set while
    // the current pattern is idle), then vehHornAudioTiming::Play: the horn
    // loops with whatever volume and frequency it had, from the pattern's
    // current beep.
    m_pattern = index;
    m_hornTimer = 0.0f;
    m_hornState = 0;
    if (m_horn.valid())
        m_horn.playLoop();
}

bool AmbientCarAudio::honk(int pattern) {
    // vehHornAudio::PlayAvoidance: only a car holding a sound slot has its horn
    // assigned, and only an idle horn honks.
    if (!hasSlot() || !m_hornDef || !m_horn.valid() || m_hornState != 2)
        return false;
    // +0xc: the index of the last pattern.
    const int last = static_cast<int>(m_hornDef->patterns.size()) - 1;
    int choice;
    if (pattern >= 0) {
        choice = std::min(pattern, std::max(last, 0));
    } else {
        choice = static_cast<int>(randomizeNumber(static_cast<float>(last * 2) - 0.01f));
        if (last <= choice)
            return false;
    }
    if (choice < 0 || static_cast<std::size_t>(choice) >= m_hornDef->patterns.size())
        return false;
    startPattern(static_cast<std::size_t>(choice));
    return true;
}

void AmbientCarAudio::impact(const ImpactInput& impact) {
    // aiVehicleActive: only a car holding a sound slot has its impact and
    // horn sounds assigned; they play with the last attenuation and pan.
    if (!hasSlot())
        return;
    m_impacts.play(impact, m_attenuation, m_pan);
    // vehHornAudio::PlayImpact: the last pattern, one time in four.
    if (!m_hornDef || m_hornDef->patterns.empty() || !m_horn.valid() || impact.force < m_hornDef->stuckImpactForce)
        return;
    if (randomizeNumber(10.0f) < kImpactHornChance)
        return;
    const std::size_t last = m_hornDef->patterns.size() - 1;
    if (m_hornState != 2) {
        if (m_pattern == last)
            return;
        // vehHornAudioTiming::Stop rewinds the interrupted pattern.
        m_beeps[m_pattern] = 0;
        m_hornState = 2;
        if (m_horn.playing())
            m_horn.stop();
    }
    startPattern(last);
}

void AmbientCarAudio::updateHorn(float dt) {
    // vehHornAudioTiming::Update for the current pattern.
    if (!m_hornDef || m_hornDef->patterns.empty())
        return;
    const auto& beeps = m_hornDef->patterns[m_pattern].beeps;
    std::size_t& beep = m_beeps[m_pattern];
    const auto at = [&](std::size_t i) {
        return i < beeps.size() ? beeps[i] : std::pair<float, float>{0.0f, 0.0f};
    };
    if (m_hornState == 0) {
        m_hornTimer += dt;
        if (at(beep).first <= m_hornTimer) {
            m_horn.stop();
            m_hornTimer = 0.0f;
            m_hornState = 1;
            return;
        }
        if (!m_horn.playing())
            m_horn.playLoop();
        return;
    }
    if (m_hornState == 1) {
        m_hornTimer += dt;
        if (at(beep).second <= m_hornTimer) {
            if (beep + 1 < beeps.size()) {
                ++beep;
                m_hornState = 0;
                m_horn.playLoop();
            } else {
                m_hornState = 2;
                beep = 0;
                if (m_horn.playing())
                    m_horn.stop();
            }
            m_hornTimer = 0.0f;
            return;
        }
    }
    if (m_horn.playing())
        m_horn.stop();
}

void AmbientCarAudio::echoOn(float delay) {
    // aiAmbientVehicleAudio::EchoOn: aiEngineAudio::EchoOn, vehHornAudio::
    // EchoOn (the manager's delay, not the player's horn's 0.05 s) and the
    // driver's AudCreature::EchoOn.
    sampleEchoOn(m_engine, delay);
    sampleEchoOn(m_horn, delay);
    if (m_voice)
        m_voice->echoOn(delay);
    m_echo = true;
}

void AmbientCarAudio::echoOff() {
    // aiAmbientVehicleAudio::EchoOff: aiEngineAudio::EchoOff, vehHornAudio::
    // EchoOff, AudCreature::EchoOff.
    m_engine.disableEcho();
    m_horn.disableEcho();
    if (m_voice)
        m_voice->echoOff();
    m_echo = false;
}

void AmbientCarAudio::silence() {
    // aiAmbientVehicleAudio::UnAssignSounds: the echo goes off, the engine
    // and horn stop and the horn pattern goes idle (vehHornAudio::Reset keeps
    // its beep index); impacts play out; the driver's voice drops its queued
    // lines (AudCreature::UnAssignSounds).
    if (m_echo)
        echoOff();
    m_engine.stop();
    if (m_horn.playing())
        m_horn.stop();
    m_hornState = 2;
    if (m_voice)
        m_voice->unassign();
}

void AmbientCarAudio::avoidReaction() {
    // aiAmbientVehicleAudio::PlayAvoidanceReaction: with a slot (+0x44) and a
    // voice.
    if (hasSlot() && m_voice)
        m_voice->avoid();
}

void AmbientCarAudio::impactReaction(float force) {
    // aiAmbientVehicleAudio::PlayImpactReaction.
    if (hasSlot() && m_voice)
        m_voice->impact(force);
}

void AmbientCarAudio::reset() {
    // aiAmbientVehicleAudio::Reset: Aud3DObject::Reset (RemoveFrom3DMgr for a
    // slot holder, the distance values), then +0x80 / +0x84.
    if (hasSlot()) {
        releaseSlot();
        silence();
    }
    m_3d.reset();
    m_speed = m_prevSpeed = 0.0f;
}

void AmbientCarAudio::update(float speed, const Mat34& transform, const Vec3& velocity, float dt,
                             const Vec3& listener, bool inTunnel) {
    Mat34 l = Mat34::identity();
    l.m3 = listener;
    update(speed, transform, velocity, dt, l, inTunnel);
}

void AmbientCarAudio::update(float speed, const Mat34& transform, const Vec3&, float dt,
                             const Mat34& listener, bool inTunnel) {
    // aiVehicleSpline copies its (non-negative) speed into +0x80.
    m_speed = std::abs(speed);
    if (!hasSlot() && !acquireSlot(m_3d.withinMaxDistance(transform.m3, listener.m3)))
        return;
    // aiAmbientVehicleAudio::UpdateAudio(): the tunnel echo, then
    // UpdateAudio(doppler factor).
    const bool tunnel = inTunnel || (manager() && manager()->echo());
    if (!m_echo && tunnel)
        echoOn(tunnelEchoDelay(manager()));
    else if (m_echo && !tunnel)
        echoOff();
    if (m_echo) {
        // aiAmbientVehicleAudio::UpdateEcho: engine, horn, the driver's voice.
        m_engine.updateEcho(dt);
        m_horn.updateEcho(dt);
        if (m_voice)
            m_voice->updateEcho(dt);
    }
    if (m_3d.pastMaxDistance(transform.m3, listener.m3)) {
        releaseSlot();
        silence();
        return;
    }
    const float attenuation = m_3d.attenuation();
    const float pan = m_3d.pan(listener, transform.m3);
    const float doppler = m_3d.doppler(2.0f / kDopplerSpeed, dt);
    m_attenuation = attenuation;
    m_pan = pan;
    // A drop of more than 4 m/s since the last update decays the engine
    // pitch from the previous speed instead of following the new one.
    const bool decaying = !(m_prevSpeed - m_speed <= kAmbientSpeedDrop);
    float pitchSpeed = m_speed;
    if (decaying) {
        m_prevSpeed *= 0.95f;
        pitchSpeed = m_prevSpeed;
    }
    // vehHornAudio::UpdateDoppler: stored; applied only while a pattern runs.
    m_hornAttenuation = attenuation;
    m_hornDoppler = doppler;
    m_hornPan = pan;
    if (m_hornDef && m_horn.valid() && m_hornState != 2) {
        m_horn.setVolume(attenuation * m_hornDef->volume);
        m_horn.setPitch(doppler * m_hornDef->pitch);
        m_horn.setPan(pan);
    }
    // aiEngineAudio::UpdateDoppler uses the pitch computed on the previous
    // update (0 at first: the buffer's lowest frequency, 100 Hz).
    if (m_engine.valid()) {
        m_engine.setVolume(attenuation * m_engineDef.volume);
        m_engine.setPitch(doppler * m_pitch);
        m_engine.setPan(pan);
        if (!m_engine.playing())
            m_engine.playLoop();
    }
    // aiEngineAudio::CalculatePitch: the slow-down curve while decaying or
    // slowing, else the speed bands.
    if (auto p = pitchFor(m_engineDef, pitchSpeed, decaying || pitchSpeed < m_prevSpeed))
        m_pitch = *p;
    // AudCreature::UpdateAttenuation with the squared distance to the
    // listener (GetDistToClosestHead2), before the impacts'.
    if (m_voice)
        m_voice->updateAttenuation(attenuation, pan, m_3d.distance2());
    m_impacts.updateAttenuation(attenuation, pan);
    if (!decaying)
        m_prevSpeed = m_speed;
    updateHorn(dt);
}

void AmbientCarAudio::stop() {
    releaseSlot();
    if (m_echo)
        echoOff();
    m_engine.stop();
    m_horn.stop();
    m_impacts.stop();
    m_hornState = 2;
}

} // namespace mm2::audio::game
