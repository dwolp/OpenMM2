#pragma once

// Car sounds: engine, gear change, horn, siren, tyres on surfaces, skids,
// suspension, tyre wobble, impacts, and ambient traffic. Ported from MM2's
// vehCarAudioContainer, vehCarAudio, vehEngineAudio, vehEngineSampleWrapper,
// vehSurfaceAudio, vehSurfaceAudioData, vehPoliceCarAudio, vehSemiCarAudio,
// AudImpact, AudImpactData, aiAmbientVehicleAudio, aiEngineAudio, vehHornAudio
// and vehHornAudioTiming, driven by the CSV tables in aud/cardata (see
// docs/audio.md).

#include "audio/game/AudioTables.h"
#include "audio/game/Object3D.h"
#include "audio/game/SoundSlot.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace mm2::audio::game {

class CreatureVoice; // audio/game/Voices.h

// --- Inputs the game supplies every update ---------------------------------------

struct WheelAudioInput {
    bool onGround = false;
    // vehWheel's skid amount (vehSurfaceAudio::UpdateSkid takes the largest
    // of the four): the slip percentage of the direction the tyre slides in, 0
    // while it grips; ~1 fully sliding.
    float slip = 0.0f;
    // Surface sound index: the wheel's material "sound:" value in
    // city/materials.mtl (vehWheel::GetSurfaceSound): 0 road, 1 water, 2 grass.
    int surface = 0;
    // Suspension compression speed (m/s, compressing > 0, clamped to +-10 by vehWheel).
    float suspensionSpeed = 0.0f;
    // vehWheel BrakeCoef (tuning). vehSurfaceAudio::IsBrakeing tests the
    // front pair against 0.5, not the brake pedal.
    float brakeCoef = 0.0f;
};

struct ImpactInput {
    // vehCarDamage::ApplyImpact passes |x| + |y| + |z| of the impulse vector
    // (N s); see impactStrength().
    float force = 0.0f;
    // Index into the impact table: the AudioId of the banger hit (0 = WALL:
    // buildings, ground and other cars; out-of-range values also use WALL).
    int audioId = 0;
    Vec3 position;
};

// vehCarDamage::ApplyImpact / aiVehicleActive: the impact strength handed to
// AudImpact::Play, |z| + |y| + |x| summed in that order.
float impactStrength(const Vec3& impulse);

struct CarAudioInputs {
    // OpenMM2 Enhanced, MM1 cars: mmSurfaceAudio::UpdateSkidClear's own skid
    // volume (>= 0), used instead of MM2's slip-to-volume curve.
    float skidVolume = -1.0f;
    // vehEngine's current RPM (vehCarSim +0x2c4), passed to the engine samples
    // as is.
    float rpm = 0.0f;
    float throttle = 0.0f; // 0..1
    float brake = 0.0f;    // 0..1
    float speed = 0.0f;    // m/s (vehCarSim speed)
    int gear = 1;          // -1 reverse, 0 neutral, 1.. forward
    std::array<WheelAudioInput, 4> wheels{}; // front left, front right, rear left, rear right
    std::vector<ImpactInput> impacts; // impacts since the previous update
    bool horn = false;                // horn button held
    bool siren = false;               // police: siren on (AI police: pursuing)
    // aiPoliceOfficer::StartSiren passes whether the suspect is the player
    // (vehCarAudioContainer::IsPlayer); only those sirens count for the cop
    // chase music.
    bool sirenPursuingPlayer = true;
    bool wrecked = false;             // police: destroyed while pursuing (explosion)
    // vehSurfaceAudio::UpdateTireWobble: (damage - MedDamage) / (MaxDamage -
    // MedDamage) from vehCarDamage; the thumps start above 0.05.
    float tireWobble = 0.0f;
    // Radius of the rear left wheel: one wobble thump per revolution.
    float wheelRadius = 0.3f;
    // Distance from the car down to the ground below it, if any within 33 m
    // (vehSurfaceAudio::UpdateAir's probe; player only). MM2 probes ten
    // segments from 3 to 33 m below the car, so a surface closer than 3 m is
    // skipped: the caller should report the first surface between 3 and 33 m.
    std::optional<float> groundBelow;
    Mat34 transform; // car placement (positioned cars)
    Vec3 velocity;
    // The listener is in a tunnel (Aud3DObjectManager's echo flag): the
    // car's sounds get their echo and its rolling sound uses the table's
    // tunnel entry. A car with an Object3DManager also follows the manager's
    // flag (Object3DManager::setTunnel); either one counts.
    bool inTunnel = false;
};

enum class SurfaceWeather { Dry, Wet, Snow };

// The surface sound index of a wheel's material (vehWheel::GetSurfaceSound):
// its "sound:" value as a short, with -1 (the default material's) as 0; a
// wheel without a material also gets 0 (the caller's part). "none" is
// already 0 (lvlMaterial::Load). Other values are used as they are; the
// surface table index is checked by the caller. The name is not used:
// cobblestone, for example, has sound 0 and sounds like road.
int surfaceSoundIndex(std::string_view materialName, int mtlSound);

// --- Engine -------------------------------------------------------------------------

// vehEngineAudio: every sample of the car's table loops while its volume at
// the current RPM is at least kSilentVolume.
class EngineSound {
public:
    struct Evaluation {
        float volume = 0; // Angel volume units
        float pitch = 1;
        bool audible = false;
    };
    // vehEngineSampleWrapper::CalculateVolume / CalculatePitch:
    //   volume = min volume at or below the fade-in start and at or past the
    //            fade-out end, rising linearly to max volume over the fade-in
    //            range, max between the fades, falling linearly over the fade-out
    //   pitch  = min pitch up to the shift start, max pitch from the shift end,
    //            and min + rpm * (max - min) / (end - start) in between (the
    //            slope is applied to the whole RPM, not to rpm - start, so the
    //            pitch jumps at both ends of the range unless it starts at 0)
    // `silenced` is vehEngineSampleWrapper::Silence: min and max volume are 0
    // but the fade slopes keep their table values (ParseCSVBuffer computed
    // them once), so the fades still rise from 0 by up to max - min.
    static Evaluation evaluate(const EngineSampleDef& def, float rpm, bool silenced = false);

    void load(Mixer& mixer, SoundBank& bank, const std::vector<EngineSampleDef>& samples, Bus bus);
    // vehEngineSampleWrapper::UpdateRPM(rpm): the player's car. A sample whose
    // table volume is below 0.25 stops; the others get volume and frequency and
    // loop.
    // Each sample then updates its echo (`dt` is the frame time).
    void update(float rpm, float dt = 0.0f);
    // UpdateRPM(rpm, volume, frequency, pan): positioned cars; the cut-off uses
    // the table volume, then volume and pitch are scaled by the attenuation
    // and the doppler factor.
    void update3D(float rpm, float attenuation, float doppler, float pan, float dt = 0.0f);
    // vehEngineAudio::EchoOn / vehEngineSampleWrapper::EchoOn: every sample's
    // echo, `delay` seconds late at 0.96 volume. EchoOff turns them off.
    void echoOn(float delay);
    void echoOff();
    // vehEngineAudio::Silence: zero every sample's volume range (the engine
    // stops), or restore it.
    void silence(bool on);
    void stop();
    std::size_t sampleCount() const { return m_samples.size(); }
    const Evaluation& state(std::size_t i) const { return m_states[i]; }

private:
    std::vector<EngineSampleDef> m_defs;
    std::vector<SoundSlot> m_samples;
    std::vector<Evaluation> m_states;
    bool m_silenced = false;
};

// --- Surfaces, skids, suspension, tyre wobble ------------------------------------------

// vehSurfaceAudio with its vehSurfaceAudioData entries.
class SurfaceSounds {
public:
    void load(Mixer& mixer, SoundBank& bank, const SurfaceTable& table, Bus bus);
    void loadSuspension(Mixer& mixer, SoundBank& bank, const SuspensionDef& def, Bus bus);
    void loadTireWobble(Mixer& mixer, SoundBank& bank, const TireWobbleDef& def, Bus bus);

    // vehSurfaceAudio::Update: suspension, rolling surface, skid, tyre wobble.
    // The player's car (not positioned) also tracks the airborne state.
    void update(const CarAudioInputs& in, float dt);
    // The positioned variant: volumes scaled by `attenuation`, pan applied,
    // no wobble pitch and no airborne tracking.
    void update3D(const CarAudioInputs& in, float dt, float attenuation, float pan);
    // vehSurfaceAudio::UnAssignSounds (the car lost its sound slot): the
    // current surface and skid sounds stop; a suspension or wobble thump plays
    // out.
    void silence();
    void stop();
    // The car's tunnel state for the next updates (its inputs' inTunnel or the
    // manager's echo flag): the table's tunnel entry is the surface.
    void setTunnel(bool inTunnel) { m_inTunnel = inTunnel; }
    // vehSurfaceAudio::EchoOn / vehSurfaceAudioData::EchoOn: the echo of every
    // entry's surface and skid samples (the suspension and wobble thumps have
    // none). UpdateEcho updates the current entry's.
    void echoOn(float delay);
    void echoOff();
    void updateEcho(float dt);

    int currentSurface() const { return m_surface; }
    bool skidding() const { return m_skidding; }
    // Whether skid sample k of the current surface is playing.
    bool skidPlaying(int k) const;
    // vehSurfaceAudio::UpdateAir: all four wheels off the ground with ground
    // 3 to 33 m below (the "big air" the music reacts to); cleared on landing.
    bool airborne() const { return m_airborne; }

    // vehSurfaceAudioData::UpdateSkid: every skid sample whose slip range
    // contains the slip (bounds inclusive) plays, the others stop; the volume
    // is min skid volume + slip * (max - min).
    static bool skidInRange(const SkidSampleDef& skid, float slip);
    static float skidVolumeFor(const SurfaceSoundDef& def, float slip);
    // vehSurfaceAudioData::UpdateSurface: linear in speed up to max speed.
    static float surfaceVolumeFor(const SurfaceSoundDef& def, float speed);
    static float surfacePitchFor(const SurfaceSoundDef& def, float speed);

private:
    struct Entry {
        SurfaceSoundDef def;
        SoundSlot surface;
        std::vector<SoundSlot> skids;
    };
    bool surfaceChanged(const CarAudioInputs& in) const;
    void selectSurface(const CarAudioInputs& in, bool positioned);
    void stopSurface(int index);
    void stopSkid(int index);
    bool skidPlayingOn(int index) const;
    void updateSurface(const CarAudioInputs& in, bool positioned, float attenuation, float pan);
    void updateSkid(const CarAudioInputs& in, float attenuation, float pan);
    void updateSuspension(const CarAudioInputs& in, bool positioned, float attenuation, float pan);
    void updateTireWobble(const CarAudioInputs& in, float dt, bool positioned, float attenuation, float pan);
    Entry* entry(int index);

    std::vector<Entry> m_entries;
    bool m_inTunnel = false;
    int m_tunnelIndex = 0;
    int m_surface = 0;
    int m_previous = -1;
    std::size_t m_skidVariant = 0; // cycleSkids: the sample of the current skid
    bool m_skidding = false;
    bool m_airborne = false;
    std::optional<SuspensionDef> m_suspensionDef;
    SoundSlot m_suspension;
    std::optional<TireWobbleDef> m_wobbleDef;
    SoundSlot m_wobble;
    float m_wobbleDistance = 0.0f;
    float m_dt = 0.0f;           // the frame time of the current update
    float m_cobbleDistance = 0.0f; // MM1 PlayCobble's accumulator
    bool m_mm1 = false;
};

// --- Impacts ------------------------------------------------------------------------------

// AudImpact / AudImpactData: one-shot collision sounds chosen by what was hit
// and how hard.
class ImpactSounds {
public:
    void load(Mixer& mixer, SoundBank& bank, const ImpactTable& table, Bus bus);
    // AudImpactData::Play: every sample of the banger whose force range
    // contains `force` (bounds inclusive) and that is not already playing.
    void play(const ImpactInput& impact, float attenuation = 1.0f, float pan = 0.0f);
    // AudImpact::UpdateAttenuation: positioned cars keep the samples of the
    // last banger hit following the car's attenuation and pan.
    void updateAttenuation(float attenuation, float pan);
    void stop();
    int lastPlayed() const { return m_last; }

    // AudImpactData::PlaySample: min volume + force * (max - min) / (max force
    // - min force). The slope is applied to the whole force, not to force -
    // min force, so loud hits can exceed the max volume.
    static float volumeFor(const ImpactSampleDef& s, float force);

private:
    struct Banger {
        std::vector<ImpactSampleDef> defs;
        std::vector<SoundSlot> slots;
        std::vector<float> volumes; // table volume each sample last played at
        bool alternate = false;     // MM1's sign hits
        int counter = 0;
    };
    std::vector<Banger> m_bangers;
    int m_last = -1;
};

// --- Sirens ---------------------------------------------------------------------------------

// vehPoliceCarAudio's siren: sample i loops until its current (play time,
// next) entry runs out, then the next sample starts and the entry index of
// sample i advances, wrapping (FluctuateSiren). After an explosion the siren
// drops in pitch and dies (DamageSiren).
class SirenPlayer {
public:
    void load(Mixer& mixer, SoundBank& bank, const SirenTable& table, Bus bus);
    // StartSiren / StopSiren. `pursuingPlayer` is StartSiren's argument (the
    // cop chases the player; counted by copsPursuingPlayer()). The state
    // changes whether or not the car has a sound slot (`audible`); only a car
    // with a slot starts the sample.
    void start(bool pursuingPlayer, bool audible = true, float attenuation = 1.0f, float doppler = 1.0f);
    void stop();
    // UpdateSiren: the player's car (not positioned).
    void update(float dt);
    // UpdateSiren(volume, frequency, pan): positioned cars. The siren never
    // drops below volume 0.75 however far away the car is.
    void update3D(float dt, float attenuation, float doppler, float pan);
    // PlayExplosion: the explosion one-shot at the car's attenuation (only
    // with a slot); the siren drops to half pitch (DamageSiren). Its callers
    // stop the siren right after (aiPoliceOfficer::PerpEscapes), so in
    // practice the siren just stops.
    void explode(bool audible, float attenuation, float doppler = 1.0f);
    // UnAssignSounds: the siren samples stop (an explosion plays out); the
    // siren state stays.
    void silence();
    void stopAll();
    // vehPoliceCarAudio::EchoOn / EchoOff / UpdateEcho: the siren samples'
    // echo (not the explosion's).
    void echoOn(float delay);
    void echoOff();
    void updateEcho(float dt);

    bool on() const { return m_state != 0; }
    int currentSample() const { return m_state != 0 ? m_current : -1; }
    bool explosionPlaying() const { return m_explosion.playing(); }

    // FluctuateSiren keeps one timer for the whole siren.
    float timer() const { return m_timer; }
    // vehPoliceCarAudio::s_iNumCopsPursuingPlayer: drives the cop chase music.
    static int copsPursuingPlayer();
    static void resetPursuitCount();

private:
    struct Sample {
        SirenSampleDef def;
        SoundSlot slot;
        std::size_t step = 0;
    };
    void fluctuate(float dt);
    void damage(float attenuation, float doppler);
    Sample& current() { return m_samples[static_cast<std::size_t>(m_current)]; }

    std::vector<Sample> m_samples;
    SoundSlot m_explosion;
    int m_current = 0;
    int m_state = 0;         // 0 off, 1 pursuing the player, 2 on
    float m_timer = 0;       // time on the current sample
    float m_volume = 0.95f;  // current siren volume
    bool m_damaged = false;
    float m_damageTime = 0.01f;
    float m_dt = 0.0f;
};

// --- Player car -----------------------------------------------------------------------------

struct CarAudioOptions {
    SurfaceWeather weather = SurfaceWeather::Dry;
    // OpenMM2 Enhanced: play MM1's skid and banger-impact samples (mm1/aud, from
    // AUDIO.AR) instead of MM2's (an MM1 city).
    bool mm1Sounds = false;
    // Police siren table: londonpolicesiren.csv in London, sfpolicesiren.csv in
    // every other city (mmGame::Init, vehCarAudioContainer::SetSirenCSVName).
    std::string city = "london";
    // Positioned cars share this (Aud3DObjectManager); null = every car sounds.
    // The player's car only reads its tunnel echo state.
    Object3DManager* manager = nullptr;
    // A network player's car (vehCarAudioContainer mode 0) keeps its horn; AI
    // opponents and police (mode 1) have none.
    bool horn = false;
};

// The player's car: vehCarAudio (or vehSemiCarAudio / vehPoliceCarAudio by
// shared/vehtypes.csv), not positioned.
class PlayerCarAudio {
public:
    // Loads aud/cardata/player/<car>.csv (default.csv when missing), the
    // surface table for the weather, suspension, tyre wobble and impact tables
    // and, for vehicles listed in vehtypes.csv, the semi or police extras.
    bool load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view car,
              const CarAudioOptions& options = {}, std::string* error = nullptr);
    void update(const CarAudioInputs& in, float dt);
    void stop();
    // vehCarAudioContainer::SilenceEngine (the engine dies after a damage out).
    void silenceEngine(bool on) { m_engine.silence(on); }

    const CarAudioDef& definition() const { return m_def; }
    const EngineSound& engine() const { return m_engine; }
    const SurfaceSounds& surfaces() const { return m_surfaces; }
    const ImpactSounds& impacts() const { return m_impacts; }
    bool police() const { return m_siren.has_value(); }
    bool sirenOn() const { return m_siren && m_siren->on(); }
    bool airborne() const { return m_surfaces.airborne(); }

private:
    void updateHorn(bool pressed);
    // vehCarAudio::UpdateAudio's start (and the police and semi variants'):
    // the echo follows the tunnel state.
    void updateEchoState(bool tunnel, float dt);
    void echoOn(float delay);
    void echoOff();
    void updateEcho(float dt);

    Object3DManager* m_manager = nullptr;
    bool m_echo = false; // vehCarAudio +0x12d
    CarAudioDef m_def;
    EngineSound m_engine;
    SurfaceSounds m_surfaces;
    ImpactSounds m_impacts;
    SoundSlot m_horn, m_clutch, m_reverseBeep, m_airBlow;
    std::optional<SemiDef> m_semi;
    std::optional<SirenPlayer> m_siren;
    int m_prevGear = -1; // MM2 gear index (0 reverse); -1 before the first update
    bool m_hornPressed = false;
};

// --- Opponents / police / network cars (positioned) -----------------------------------------

// vehCarAudio in 3D mode: another car, attenuated by distance (0..150 m) and
// panned like every MM2 positioned sound, sounding only while it holds a slot
// of the Object3DManager. Police (vehPoliceCarAudio) and semis
// (vehSemiCarAudio) by shared/vehtypes.csv, as for the player.
class OpponentCarAudio : private SlotHolder {
public:
    bool load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view car, bool police,
              const CarAudioOptions& options = {}, std::string* error = nullptr);
    void update(const CarAudioInputs& in, float dt, const Mat34& listener);
    void update(const CarAudioInputs& in, float dt, const Vec3& listener);
    // vehCar::Reset (a car put back at its start or reset by its driver) ->
    // vehCarAudioContainer::Reset -> vehCarAudio::Reset, vehPoliceCarAudio::
    // Reset and vehSemiCarAudio::Reset, all Aud3DObject::Reset: a car holding
    // a slot gives it up (UnAssignSounds: the loops stop, impacts and an
    // explosion play out) and its distance history is forgotten, so the
    // update after it asks for a slot again with no doppler shift; the
    // attenuation, pan and doppler become 0, 0 and 1. The siren state stays
    // (aiPoliceOfficer::Reset stops it itself).
    void reset();
    void stop();
    bool audible() const { return hasSlot(); }
    bool sirenOn() const { return m_siren && m_siren->on(); }
    bool explosionPlaying() const { return m_siren && m_siren->explosionPlaying(); }

    static constexpr float kMaxDistance = 150.0f; // vehCarAudio::Init SetDropOffs(0, 150)

private:
    float slotDistance2() const override { return m_3d.distance2(); }
    int slotPriority() const override { return m_priority; }
    void slotLost() override { silence(); }
    void silence();
    void updateEchoState(bool tunnel, float dt);
    void echoOn(float delay);
    void echoOff();
    void updateEcho(float dt);

    bool m_echo = false; // vehCarAudio +0x12d
    CarAudioDef m_def;
    EngineSound m_engine;
    SurfaceSounds m_surfaces;
    ImpactSounds m_impacts;
    SoundSlot m_horn;
    std::optional<SirenPlayer> m_siren;
    std::optional<SemiDef> m_semi;
    SoundSlot m_reverseBeep, m_airBlow;
    Audio3D m_3d;
    int m_priority = 9;
    // The last values UpdateAudio3D computed (Aud3DObject +0xc, +0x10, +4).
    float m_attenuation = 0.0f;
    float m_doppler = 1.0f;
    float m_pan = 0.0f;
    bool m_hasHorn = false;
    bool m_hornPressed = false; // vehCarAudioContainer +0: PlayHorn / StopHorn latch
    bool m_prevSiren = false;
};

// --- Ambient traffic (positioned) -------------------------------------------------------------

// aiAmbientVehicleAudio: one engine loop pitched by speed band, horn patterns,
// impacts; attenuated over 0..100 m.
class AmbientCarAudio : private SlotHolder {
public:
    // `type` is the ambient vehicle name ("va_sedans_s"); files are
    // aud/cardata/ambient/<type>_engine.csv and _horn.csv, else
    // default_engine.csv / default_horn.csv (aiAmbientVehicleAudio::Init,
    // aiEngineAudio::Load "%s_engine", vehHornAudio::Load "%s_horn"). The
    // va_sedan_s files therefore go unused: the model is va_sedans_s.
    // Impacts come from aud/cardata/opponent/default_impacts.csv.
    bool load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view type,
              Object3DManager* manager = nullptr);
    // `inTunnel` as CarAudioInputs::inTunnel: the echo also follows it.
    void update(float speed, const Mat34& transform, const Vec3& velocity, float dt, const Mat34& listener,
                bool inTunnel = false);
    void update(float speed, const Mat34& transform, const Vec3& velocity, float dt, const Vec3& listener,
                bool inTunnel = false);
    // vehHornAudio::PlayAvoidance: RandomizeNumber(2 * last - 0.01) picks one
    // of the patterns before the last (the long "stuck" blast is kept for
    // impacts), or (about half the time) nothing; returns whether a pattern
    // started. pattern >= 0 forces one.
    bool honk(int pattern = -1);
    // aiVehicleActive's impact: the impact sounds and vehHornAudio::PlayImpact.
    void impact(const ImpactInput& impact);
    // The driver's voice (aiAmbientVehicleAudio +0x78, an AudCreature the
    // owner loads: aiAmbientVehicleAudio::LoadVoices), or null; it must
    // outlive this object's use of it. While the car holds its slot the voice
    // follows the car's attenuation, pan and squared distance
    // (AudCreature::UpdateAttenuation, in UpdateAudio), its echo goes on and
    // off with the car's (EchoOn / EchoOff / UpdateEcho), and losing the slot
    // drops its queued lines (UnAssignSounds).
    void setVoice(CreatureVoice* voice) { m_voice = voice; }
    // PlayAvoidanceReaction (aiGoalAvoidPlayer::Reset, after the avoidance
    // horn started a pattern) and PlayImpactReaction (aiVehicleActive's
    // impact, with the impact strength): the voice's AudCreature::
    // PlayAvoidance / PlayImpact, only while the car holds its slot.
    void avoidReaction();
    void impactReaction(float force);
    // aiAmbientVehicleAudio::Reset (aiVehicleSpline::Reset: the car is put
    // back on the road, or returned to the pool): Aud3DObject::Reset (a slot
    // holder gives its slot up, UnAssignSounds: the engine and horn stop,
    // impacts play out, the voice's queued lines go; the distance history is
    // forgotten) and its speed and previous speed back to 0.
    void reset();
    void stop();
    bool audible() const { return hasSlot(); }
    // The last attenuation, pan and squared listener distance the update
    // worked out, which the driver's voice (AudCreature) follows.
    float attenuation() const { return m_attenuation; }
    float pan() const { return m_pan; }
    float distance2() const { return m_3d.distance2(); }

    // aiEngineAudio::CalculatePitch for a car holding or gaining speed (the
    // band containing the speed; bands are inclusive and the last band is
    // skipped) or slowing down (the last band). Returns nullopt when no band
    // applies (the pitch then stays as it was).
    static std::optional<float> pitchFor(const AmbientEngineDef& def, float speed, bool slowing = false);

    static constexpr float kMaxDistance = 100.0f; // aiAmbientVehicleAudio::Init SetDropOffs(0, 100)

private:
    float slotDistance2() const override { return m_3d.distance2(); }
    int slotPriority() const override { return 8; }
    void slotLost() override { silence(); }
    void silence();
    void updateHorn(float dt);
    void startPattern(std::size_t index);
    // aiAmbientVehicleAudio::EchoOn / EchoOff / UpdateEcho: the engine and
    // horn echo (aiEngineAudio, vehHornAudio).
    void echoOn(float delay);
    void echoOff();

    bool m_echo = false; // aiAmbientVehicleAudio +0x89
    CreatureVoice* m_voice = nullptr; // +0x78

    AmbientEngineDef m_engineDef;
    std::optional<HornDef> m_hornDef;
    SoundSlot m_engine, m_horn;
    ImpactSounds m_impacts;
    Audio3D m_3d;
    // The last attenuation and pan (AudImpact +0x18, +0x1c), used by impacts.
    float m_attenuation = 1.0f, m_pan = 0.0f;
    float m_pitch = 0.0f;
    float m_speed = 0.0f, m_prevSpeed = 0.0f;
    // vehHornAudioTiming of the current pattern; every pattern keeps its own
    // beep index (Reset idles a pattern without rewinding it).
    std::size_t m_pattern = 0;
    std::vector<std::size_t> m_beeps;
    int m_hornState = 2; // 0 sounding, 1 pausing, 2 idle
    float m_hornTimer = 0;
    // vehHornAudio::UpdateDoppler's stored attenuation, doppler and pan.
    float m_hornAttenuation = 1.0f, m_hornDoppler = 1.0f, m_hornPan = 0.0f;
};

// Finds a file in aud/cardata/<folder>/ for a car, falling back to default.csv.
std::string carAudioPath(const vfs::Vfs& vfs, std::string_view folder, std::string_view car);

} // namespace mm2::audio::game
