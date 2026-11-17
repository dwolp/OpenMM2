#pragma once

// Spoken lines: the announcer (MM2's mmRaceSpeech, mmCNRSpeech and mmCCSpeech
// on AudSpeech / AudSpeechData, driven by the aud/spchdata tables) and
// "creature" voices (ambient drivers and pedestrians: AudCreature,
// AudCreatureAvoid, AudCreatureImpact with the aud/creaturedata tables).

#include "audio/game/AudioTables.h"
#include "audio/game/Object3D.h"
#include "audio/game/SoundSlot.h"

#include <map>
#include <string>

namespace mm2::audio::game {

enum class AnnouncerMode { Cruise, Blitz, Checkpoint, Circuit };
enum class RaceOutcome { Poor, Mid, Win };

// mmSpeechContainer with its AudSpeech: speech groups ("AL3PRE": lines
// AL3PRE01..AL3PRE11), a small queue of delayed plays and the line playing.
// The play methods return the line started at once, the group's name prefix
// for a delayed play (its number is drawn when it starts), or "" when nothing
// plays.
class Announcer {
public:
    // Reads aud/spchdata/<city>.csv ("Num announcers", "prefix"). MM2 only has
    // announcers for "sf" and "london" (mmRaceSpeech::LoadCityInfo).
    bool load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view city);
    // OpenMM2 Enhanced: MM1's commentary (aud/aud11/vcaud, indexed as "mm1:<name>"), used for Midtown
    // Madness 1's cities. Its groups are the numbered lines of one name ("aprerace01", "aresultwin07").
    // MM1 chooses and times its lines in code that is not decoded: the groups, which events play them
    // and with what odds follow MM2's announcer (inferred); MM1's file names give the rest.
    bool loadMm1(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer);
    // The Mm1 counterpart of beginRace. raceIndex is the blitz or circuit's number (0-9).
    void beginMm1Race(AnnouncerMode mode, int raceIndex, std::string_view car, int timeOfDay, int weather);
    // MM2 picks the announcer again for every race (LoadCityInfo, from
    // mmSpeechContainer::InitRace): RandomizeNumber(1, count + 0.99), and in
    // SF a 3 becomes 4 (SF lists five announcers but ships as1, as2, as4 and
    // as5, so as4 is picked twice as often). index > 0 fixes the announcer
    // instead (tools, tests); index < 0 restores the random pick.
    void beginSession(int index = -1);
    std::string announcerId() const; // "al3"
    int announcerCount() const { return static_cast<int>(m_count); }

    // mmSpeechContainer::InitRace + mmPlayer::InitSpeechAudio: the groups for
    // a race: the mode's table, the weather's pre-race lines (none in snow),
    // the time of day's (evening only in clear weather), the car's pre-race and
    // results lines, the final lap and the damage penalty (cruise or race).
    // weather: 0 clear, 1 cloudy, 2 fog, 3 rain, 4 snow; timeOfDay: 0 morning,
    // 1 noon, 2 evening, 3 night.
    void beginRace(AnnouncerMode mode, std::string_view car, int timeOfDay, int weather);
    // mmRaceSpeech::LoadVehicleUnlock / LoadTextureUnlock: more groups.
    bool loadVehicleUnlock(std::string_view car);
    bool loadTextureUnlock(std::string_view car);

    // mmRaceSpeech. The pre-race line waits 1.5 s; the unlock-race and
    // unlock-vehicle lines wait 0.1 s in the queue; every other event stops
    // the line playing and empties the queue first.
    std::string playPreRace();
    std::string playFinalCheckpoint();
    std::string playFinalLap();
    std::string playDamagePenalty();
    std::string playRaceProgress();
    // PlayResults(position, racers): 1st wins, a place above half the field
    // is "mid", anything else "poor".
    std::string playResults(int position, int racers);
    std::string playResults(RaceOutcome outcome);
    std::string playUnlockRace();
    std::string playUnlockVehicle();
    std::string playUnlockTexture();

    // mmSpeechContainer::InitCNR: bullshit.csv in SF (sic), cnrlondon.csv
    // anywhere else. Its rows name the announcer themselves
    // ("AL1\AL1ROBROB ,1,0,1"); a fourth "num used" column picks a random
    // window of that many lines when it is smaller than the range.
    void beginCopsAndRobbers();
    // mmCNRSpeech::Play(event): a random group of the event, after 0.01 s.
    std::string playCopsAndRobbers(std::string_view event);

    // mmSpeechContainer::InitCC (crash course lesson 0..12): aud/spchdata/ccl
    // in London, ccs elsewhere, plus the checkpoint location lines for lesson 4.
    bool beginCrashCourse(int lesson);
    // mmCCSpeech::PlayPreRace: the intro after 1.5 s, then checkpoint 0's
    // location line after 1.51 s.
    std::string playCrashCoursePreRace();
    // mmCCSpeech::PlayCheckPoint: the location line cc_cpoint_indexinfo gives
    // for checkpoint `index` (none for -1), after `delay` seconds.
    std::string playCrashCourseCheckPoint(int index, float delay);
    std::string playCrashCourseResults(bool passed);
    std::string playCrashCourseUnlock();

    // AudSpeech::Update: every queued play counts down; the first one due
    // starts once nothing is playing.
    void update(float dt);
    bool speaking() const;
    void stop(); // AudSpeech::Stop: empties the queue too

    // AudSpeechData::GetRandomName: `drawn` is RandomizeNumber(add + 1,
    // end + 0.99); the number just used moves up one, wrapping to 1 (not to
    // add + 1), unless the group has a single line.
    static int pickLine(float end, float add, int last, double drawn);
    // The name of line `n` of a group: the prefix and two digits (more for
    // 100 and up).
    static std::string lineName(std::string_view prefix, int n);

private:
    struct Group {
        std::string prefix; // file name before the number ("AL3PRE")
        float end = 0, add = 0;
        int last = 0; // AudSpeechData +0x10
    };
    struct Queued {
        int group = -1;
        int line = -1; // a fixed line number (mmCCSpeech checkpoints), else -1
        float delay = 0;
    };
    enum class Kind { None, Race, CopsAndRobbers, CrashCourse };
    // loadMm1: the longest run of numbered lines `prefix`NN, as a group (-1 when there is none).
    int addMm1Group(std::string_view prefix);
    bool m_mm1 = false;

    void reset(Kind kind);
    bool loadCityInfo();
    const SpeechTable* table(const std::string& path);
    // mmRaceSpeech::LoadGroup / SetReadState for aud/spchdata/<as|al>N/<name>.csv.
    bool loadRaceGroup(std::string_view name);
    // mmCCSpeech::LoadGroup / SetReadState.
    bool loadCrashCourseGroup(const std::string& name);
    int addGroup(std::string prefix, float end, float add);
    void allocateQueueSlot() { m_queue.emplace_back(); }
    // AudSpeech::Play(group, delay) and Play(group, line, delay).
    std::string play(int group, float delay, int line = -1);
    bool putInQueue(int group, int line, float delay);
    std::string start(int group, int line);
    // RandomizeNumber(first, last + 0.99) truncated: a group of a range.
    static int randomGroup(float first, float last);

    const vfs::Vfs* m_vfs = nullptr;
    SoundBank* m_bank = nullptr;
    Mixer* m_mixer = nullptr;
    std::string m_city;
    std::string m_prefix; // "al" / "as" from <city>.csv
    float m_count = 0;
    int m_forcedIndex = -1;
    int m_index = 1;
    int m_cityKind = 0; // mmRaceSpeech +0xcc: 1 SF, 2 London, 0 none
    std::map<std::string, std::optional<SpeechTable>> m_tables;

    Kind m_kind = Kind::None;
    std::vector<Group> m_groups;
    float m_groupCount = 0; // the next group's index (+0xd4 / +0x78 / +0x9c)
    // mmRaceSpeech: ranges as floats (-1 when unset) and single groups.
    float m_preRaceFirst = -1, m_preRaceLast = -1;
    float m_unlockRaceFirst = -1, m_unlockRaceLast = -1;
    float m_unlockVehicleFirst = -1, m_unlockVehicleLast = -1;
    int m_unlockCount = 0; // +0x90: extra queue slots
    int m_finalCheckpoint = -1, m_raceProgress = -1, m_resultsPoor = -1, m_resultsMid = -1, m_resultsWin = -1;
    int m_damagePenalty = -1, m_vehiclePreRace = -1, m_vehicleResultsWin = -1, m_vehicleResultsMid = -1;
    int m_vehicleResultsPoor = -1, m_timeOfDay = -1, m_weather = -1, m_finalLap = -1, m_unlockTexture = -1;
    // mmCNRSpeech: events with their group ranges.
    struct Event {
        std::string name;
        float first = 0, last = 0;
    };
    std::vector<Event> m_events;
    // mmCCSpeech.
    int m_ccPreRace = -1, m_ccUnlock = -1, m_ccCheckPoint = -1, m_ccResultsPoor = -1, m_ccResultsWin = -1;
    int m_ccUnlockCount = 0;
    std::vector<int> m_ccCheckPointLines;

    std::vector<Queued> m_queue; // AudSpeech::AllocateQueuePlayData: one slot to start with
    SoundSlot m_current;
};

// One creature's voice: AudCreature, with its AudCreatureAvoid blocks (the
// near-miss lines) and its AudCreatureImpact (the impact lines). MM2 keeps
// one per voice file and 3D-manager slot; the container holding that slot (a
// pedestrian, PedestrianAudio) gives it its attenuation and pan and is asked
// for its slot again before a line plays.
class CreatureVoice {
public:
    // The container a voice belongs to while it holds the slot
    // (SetAud3DObjectPtr).
    class Owner {
    public:
        virtual ~Owner() = default;
        // Aud3DObject::UpdateNonVirtual: asks for a slot (within range);
        // returns whether the container holds one.
        virtual bool requestSlot() = 0;
    };

    void load(Mixer& mixer, SoundBank& bank, CreatureVoiceDef def);
    void setOwner(Owner* owner) { m_owner = owner; }
    // AudCreature::Update(speed): AudCreatureContainer::UpdateStatics updates
    // every creature every frame with the player's speed. Each block counts
    // the time the speed spends in and out of its range; a queued near-miss
    // line plays within 50 m (or is dropped after 5 s), a queued impact line
    // after its delay.
    void update(float speed, float dt);
    // UpdateAttenuation(attenuation, pan, squared distance): from the
    // container's UpdateAudio; the lines being said follow it.
    void updateAttenuation(float attenuation, float pan, float distance2);
    // AudCreature::PlayAvoidance (a near miss): every eligible block queues
    // one of its lines half of the time.
    void avoid();
    // AudCreature::PlayImpact: a line `delay` seconds after a hit at least as
    // hard as the table's minimum, at most once a minute across all creatures.
    void impact(float force);
    // UnAssignSounds: queued lines are dropped; lines being said play out.
    void unassign();
    // OpenMM2's teardown: unassign() and every line stops.
    void stop();
    // IsPlaying: the chosen line of a block, or the impact line, is playing.
    bool speaking() const;
    // EchoOn / EchoOff / UpdateEcho: every line's echo.
    void echoOn(float delay);
    void echoOff();
    void updateEcho(float dt);

    // AudCreatureImpact::UpdateStatics: the shared impact-line clock.
    static void advanceClock(float dt);
    static void resetGlobals();

private:
    struct Avoid {
        VoiceSpeedTrigger def;
        std::vector<SoundSlot> slots;
        float timeIn = 0, timeOut = 0;
        bool queued = false;
        float queueTime = 0;
        std::size_t line = 0;
    };
    bool eligible(const Avoid& a) const;
    void playAvoid(Avoid& a);
    void playImpact();
    bool ownerHasSlot();

    CreatureVoiceDef m_def;
    std::vector<Avoid> m_avoids;
    std::vector<SoundSlot> m_impactSlots;
    bool m_impactQueued = false;
    float m_impactTime = 0;
    std::size_t m_impactLine = 0;
    Owner* m_owner = nullptr;
    // AudCreatureAvoid / AudCreatureImpact start at attenuation 1, pan 0.
    float m_attenuation = 1.0f, m_pan = 0.0f, m_distance2 = 0.0f;
};

} // namespace mm2::audio::game
