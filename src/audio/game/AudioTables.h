#pragma once

// Data tables for the in-game sound effects (aud/cardata, aud/ambient,
// aud/creaturedata, aud/spchdata in MM2AUD.AR). Each parser follows the MM2
// loader that reads the file line by line: which line holds what, how a block
// ends, and how cells are read (strtok skips empty cells, atof / atoi read a
// numeric prefix; see audio/TextFields.h): vehCarAudio::Load,
// vehEngineAudio::Load, vehEngineSampleWrapper::ParseCSVBuffer,
// vehSurfaceAudio::LoadCSV, vehSurfaceAudioData::ParseCSVBuffer,
// AudImpact::ReadCSV, AudImpactData::ReadCSV, vehPoliceCarAudio::Load /
// ReadSirenData, vehSurfaceAudio::LoadSuspension / LoadTireWobble,
// vehSemiCarAudio::Load, vehCarAudioContainer::RegisterTypes,
// aiEngineAudio::ReadCSV, vehHornAudio::ReadCSV, Aud3DAmbientObject::Load /
// ReadSoundData, Aud3DObject::ReadVectorPoints, Aud3DAmbObjContainer::Init,
// AudCreature::ReadCSV, AudCreatureAvoid / AudCreatureImpact::ParseCSVBuffer,
// mmRaceSpeech::LoadCityInfo / LoadGroup. Where MM2 would read past the end
// of a line (a missing cell is a NULL token there) the parsers use 0.
//
// Volumes in every table are Angel volume units, see ageVolumeToGain().

#include "audio/AngelUnits.h"
#include "core/Math.h"
#include "vfs/Vfs.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::audio::game {

// MM2's volume and pan units (audio/AngelUnits.h).
using audio::ageMasterVolume;
using audio::agePanToMixer;
using audio::ageVolumeToGain;

// vehEngineSampleWrapper::UpdateRPM stops a sample whose table volume falls
// below 0.25 (-75 dB) and restarts it above.
inline constexpr float kSilentVolume = 0.25f;

// --- Cars: aud/cardata/{player,opponent}/<car>.csv -----------------------------

struct EngineSampleDef {
    std::string wave;
    float minVolume = 0, maxVolume = 0;
    float fadeInStartRpm = 0, fadeInEndRpm = 0;
    float fadeOutStartRpm = 0, fadeOutEndRpm = 0;
    float minPitch = 0, maxPitch = 0;
    float pitchStartRpm = 0, pitchEndRpm = 0;
    // The "Volume Divisor" layout (vehEngineSampleWrapper::ParseCSVBufferOld,
    // +0x41): volume = rpm / divisor below cutRpm, divisor / rpm from it,
    // clamped to the min / max volume (CalculateVolumeOld). The fade columns
    // are not used.
    bool oldLayout = false;
    float volumeDivisor = 0; // +0x44
    float cutRpm = 0;        // +0x48
};

// The "flags" column (2 on vpcentury, 4 on vpcop, 8 on vpsemi) and "Num Engine
// Samples" are read and discarded by vehCarAudio::Load: MM2 picks the semi,
// police and nitro sound classes from shared/vehtypes.csv
// (vehCarAudioContainer::RegisterTypes) and reads engine rows up to the end of
// the file (vehEngineAudio::Load). The flags are kept for tools only.
namespace CarAudioFlag {
inline constexpr unsigned Freight = 2;
inline constexpr unsigned Police = 4;
inline constexpr unsigned SirenHorn = 8;
} // namespace CarAudioFlag

struct CarAudioDef {
    std::string horn;
    float hornVolume = 0;
    unsigned flags = 0;
    std::string clutch; // "REVERSE" for cars, "TRUCKGEARSHIFT" for trucks
    float clutchVolume = 0;
    // vehEngineAudio::Load switches to ParseCSVBufferOld / CalculateVolumeOld
    // when the engine header's fourth cell is "Volume Divisor" (any case). No
    // car table MM2 loads has that layout (only the unused "copy of" files).
    bool oldEngineLayout = false;
    std::vector<EngineSampleDef> engine;
};
std::optional<CarAudioDef> parseCarAudio(std::string_view text, std::string* error = nullptr);

// --- Impacts: aud/cardata/{player,opponent}/default_impacts.csv -------------

struct ImpactSampleDef {
    std::string wave;
    float minVolume = 0, maxVolume = 0;
    float minForce = 0, maxForce = 0;
    float frequency = 1; // playback rate multiplier
};
struct BangerSoundDef {
    std::string name; // WALL, LIGHT, SIGN, ...
    int id = 0;       // "ID" column (tools; AudImpact::ReadCSV never reads it)
    std::vector<ImpactSampleDef> samples;
};
struct ImpactTable {
    std::vector<BangerSoundDef> bangers; // file order
    // By the "ID" column (tools).
    const BangerSoundDef* find(int id) const;
    // AudImpact::GetAudImpactDataPtr: bangers are indexed by their position in
    // the file; an index out of range (MM2 passes 1000 for "no banger data")
    // selects the first entry, WALL.
    const BangerSoundDef* byIndex(int index) const;
};
// AudImpact::ReadCSV: blocks of "***", the header, "<name>,<count>,<id>", the
// sample header and <count> sample rows, until a block named ENDOFDATA. A file
// that ends before ENDOFDATA (or inside a block) loses every block: MM2 then
// has no impact sounds.
std::optional<ImpactTable> parseImpactTable(std::string_view text, std::string* error = nullptr);

// --- Surfaces: aud/cardata/player/default_surface{dry,wet}.csv -----------------

struct SkidSampleDef {
    std::string wave; // "NOSOUND" (exact case) = none
    float min = 0, max = 0; // slippage range
};
struct SurfaceSoundDef {
    std::string wave; // "NOSOUND" (exact case) = none
    float maxSpeed = 0;
    float minVolume = 0, maxVolume = 0;
    float minPitch = 0, maxPitch = 0;
    float minSkidVolume = 0, maxSkidVolume = 0;
    std::vector<SkidSampleDef> skids;
    // OpenMM2 Enhanced (MM1's skids): play one of `skids` per skid, the next
    // after each one ends (mmSurfaceAudio::UpdateSkidClear's variant cycle).
    bool cycleSkids = false;
    bool hasSurfaceSound() const;
};
struct SurfaceTable {
    int tunnelIndex = 0; // entry used for every wheel while the tunnel echo is on
    std::vector<SurfaceSoundDef> surfaces; // indexed by surface sound index
    const SurfaceSoundDef* at(int index) const;
};
// vehSurfaceAudio::LoadCSV: the tunnel index on line 2, then blocks of a
// header line, the surface row, the skid header and the skid rows. The ice
// table (default_surfaceice.csv, never loaded by MM2) has other columns and
// reads as garbage, as it would in MM2.
std::optional<SurfaceTable> parseSurfaceTable(std::string_view text, std::string* error = nullptr);

// --- Police sirens: aud/cardata/player/{london,sf}policesiren.csv ------------
// (opponent/policesiren.csv has the same layout but MM2 never loads it.)

struct SirenStep {
    float playTime = 0;
    int next = 0; // index of the sample that follows (read with atoi)
};
struct SirenSampleDef {
    std::string wave;
    float volume = 0;
    std::vector<SirenStep> steps; // (play time, next) pairs, used in turn
};
struct SirenTable {
    std::string explosion;
    // vehPoliceCarAudio::Load reads only the explosion sample's name: it
    // always plays at volume 1 (times distance attenuation).
    std::vector<SirenSampleDef> samples;
};
std::optional<SirenTable> parseSirenTable(std::string_view text, std::string* error = nullptr);

// --- Small player tables ---------------------------------------------------------

struct SuspensionDef { // player/suspensionaudio.csv (vehSurfaceAudio::LoadSuspension)
    std::string wave;
    float minVelocity = 2; // the wheels' average compression speed must reach |minVelocity|
    float maxVelocity = 3; // read but unused by MM2
    float minVolume = 0.75f, maxVolume = 1;
    float volumeScale = 1.0f / 3.0f; // 1 / "Volume Divisor" (0 for a divisor of 0)
};
std::optional<SuspensionDef> parseSuspension(std::string_view text);

struct TireWobbleDef { // player/tirewobble.csv (vehSurfaceAudio::LoadTireWobble)
    std::string wave;
    float minVolume = 0.95f, maxVolume = 1;  // clamp of the damage fraction
    float minPitch = 0.75f, maxPitch = 0.95f; // clamp of speed * pitchScale
    float pitchScale = 0.0633333f;           // 1 / "pitch divisor" (0 for a divisor of 0)
};
std::optional<TireWobbleDef> parseTireWobble(std::string_view text);

struct SemiDef { // shared/semidata.csv (vehSemiCarAudio::Load)
    std::string reverse, airBlow;
    float reverseVolume = 0.87f, airBlowVolume = 0.9f;
};
std::optional<SemiDef> parseSemiData(std::string_view text);

struct VehicleTypes { // shared/vehtypes.csv (vehCarAudioContainer::RegisterTypes)
    std::vector<std::string> freight; // "Semi or bus": vehSemiCarAudio
    std::vector<std::string> police;  // "Police Car": vehPoliceCarAudio (vpcop, vpsemi, vpeagle)
    bool alwaysNitro = false;         // every other car would use vehNitroCarAudio
    bool isFreight(std::string_view car) const;
    bool isPolice(std::string_view car) const;
};
VehicleTypes parseVehicleTypes(std::string_view text);

// --- Ambient traffic: aud/cardata/ambient/<type>_engine.csv and _horn.csv ----

struct SpeedBand {
    float minSpeed = 0, maxSpeed = 0;
    float minPitch = 0, maxPitch = 0;
};
struct AmbientEngineDef {
    std::string wave;
    float volume = 0;
    // Speed bands (m/s) in file order: every line after the third.
    // aiEngineAudio::CalculatePitch uses the last band (0..500 in every retail
    // file) while the car slows down and the others while it holds or gains
    // speed.
    std::vector<SpeedBand> bands;
};
std::optional<AmbientEngineDef> parseAmbientEngine(std::string_view text);

struct HornPattern {
    std::vector<std::pair<float, float>> beeps; // (play seconds, pause seconds)
};
struct HornDef {
    std::string wave;
    float volume = 0, pitch = 0;
    // vehHornAudio::PlayImpact: a hit at least this hard plays the last pattern
    // (a long blast in every retail file) one time in four.
    float stuckImpactForce = 0;
    std::vector<HornPattern> patterns;
};
std::optional<HornDef> parseHorn(std::string_view text);

// --- City ambience: aud/ambient/*.csv -----------------------------------------

// Aud3DAmbientObject::UpdateSoundData.
enum class AmbientSampleType : int {
    Loop = 0,       // positional loop
    RandomOnce = 1, // one-shot every [low, high] s at a random volume (0.75..1) and pan; not positional
    Interval = 2,   // positional one-shot every [low, high] s
    Triggered = 3,  // positional, played on request (bridges, subway, pedestrians)
};
struct AmbientSampleDef {
    std::string wave;
    float volume = 0;
    // The "sample type" column. UpdateSoundData aborts the game on any other
    // value; OpenMM2 clamps it to 0..3 instead.
    AmbientSampleType type = AmbientSampleType::Loop;
    float intervalLow = 0, intervalHigh = 0;
    bool active = false;
    float minSpeed = 0, maxSpeed = 0; // of the object the set is attached to
    bool doppler = false;
};
struct AmbientSoundSet {
    std::string name;
    float minDistance = 0, maxDistance = 0;
    int priority = 0; // Aud3DObjectManager priority
    // Aud3DAmbientObject::Update: 0 everywhere, 1 only underground (while the
    // tunnel echo is on), 2 only above ground.
    int audibleArea = 0;
    std::vector<AmbientSampleDef> samples;
    std::vector<Vec3> points; // VECTORPOINTS: the set sounds from the one nearest the listener
};
std::optional<AmbientSoundSet> parseAmbientSoundSet(std::string_view name, std::string_view text,
                                                    std::string* error = nullptr);
// <city>ambientcontainer.csv: names of the sets loaded for a city.
std::vector<std::string> parseAmbientContainer(std::string_view text);

// --- Creature voices: aud/creaturedata/*.csv ---------------------------------

struct VoiceLine {
    std::string wave;
    float volume = 0;
    float delay = 0; // impact lines: seconds after the impact
};
// One AudCreatureAvoid block: lines a driver or pedestrian says when the AI
// reports a near miss (AudCreature::PlayAvoidance). The block is eligible
// unless its speed has been in [minSpeed, maxSpeed) for less than
// minTimeInRange and out of it for longer than maxTimeOutOfRange.
struct VoiceSpeedTrigger {
    float minSpeed = 0, maxSpeed = 0;
    float minTimeInRange = 0;
    float maxTimeOutOfRange = 0;
    std::vector<VoiceLine> lines;
};
struct CreatureVoiceDef {
    std::vector<VoiceSpeedTrigger> triggers;
    // AudCreatureImpact: the last "min impact force" block of the file.
    bool hasImpact = false;
    float minImpactForce = 0;
    std::vector<VoiceLine> impactLines;
};
// AudCreature::ReadCSV: blocks start with a "min speed" or "min impact force"
// header (any case); a file starting with anything else has no blocks.
std::optional<CreatureVoiceDef> parseCreatureVoice(std::string_view text);
// AudCreatureContainer::LoadNumFileChoices: the "Num files" value of
// numambcarvoicefiles_<l|s>.csv and num{fe,}malepedvoicefiles.csv.
std::optional<float> parseNumFileChoices(std::string_view text);

// --- Announcer: aud/spchdata --------------------------------------------------

// One line of a speech table after its first: a header ("<EVENT> header", any
// case) or a line set "<prefix>,<end>,<add>[,<num used>]": candidate files
// <announcer><prefix>NN, NN in (add, end].
struct SpeechRow {
    std::string name;
    float end = 0, add = 0;
    float numUsed = 0; // Cops & Robbers tables only
    // mmRaceSpeech::SetReadState: a first cell ending in "header" (not just
    // "header" itself).
    bool header() const;
    // strnicmp(name, event, strlen(event)): the event names are prefixes.
    bool headerIs(std::string_view event) const;
    // mmCNRSpeech::SetReadState: the event name, up to the last space before
    // "header".
    std::string eventName() const;
};
struct SpeechTable {
    std::vector<SpeechRow> rows;
};
std::optional<SpeechTable> parseSpeechTable(std::string_view text);

// <city>.csv (mmRaceSpeech::LoadCityInfo): "Num announcers" on line 2 (read
// with atof) and the prefix on line 4 (AL -> al1..al6).
struct AnnouncerList {
    float count = 0;
    std::string prefix;
};
std::optional<AnnouncerList> parseAnnouncerList(std::string_view text);

// Reads a text file from the mounted game data (nullopt if missing).
std::optional<std::string> readText(const vfs::Vfs& vfs, std::string_view path);

} // namespace mm2::audio::game
