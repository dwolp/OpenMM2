#pragma once

// Positioned ambient sounds and rain. Ported from MM2's Aud3DAmbientObject
// (with its owners: Aud3DAmbObjContainer for the city's emitters,
// mmBridgeAudio for drawbridges, aiSubwayAudio for trains, gizFerry), the
// cable cars' aiCableCarAudio / aiCableCarAudioData, and mmRainAudio.

#include "audio/game/AudioTables.h"
#include "audio/game/Object3D.h"
#include "audio/game/SoundSlot.h"

#include <memory>
#include <string>
#include <vector>

namespace mm2::audio::game {

// Aud3DAmbientObject: a positioned object playing the samples of one
// aud/ambient/<name>.csv set: loops (sample type 0), one-shots at random
// volume and pan (1) or at the object's attenuation and pan (2) at random
// intervals, and samples only its owner plays (3). It sounds only while it
// holds a slot of the Object3DManager (the file's priority), and only within
// its audible area (the file's: 0 anywhere, 1 underground, 2 above ground;
// underground is the tunnel echo state). Samples play only while active
// (the file's "active" column, then ActivateSound / DeactivateSound).
class AmbientObject : private SlotHolder {
public:
    AmbientObject() = default;
    ~AmbientObject() override;

    // Aud3DAmbientObject::Init / Load / ReadSoundData: aud/ambient/<name>.csv.
    bool load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view name,
              Object3DManager* manager = nullptr, std::string* error = nullptr);
    // OpenMM2: the same from a definition given as text (the CSV's layout),
    // for sound objects the game data has no file for (MM1's El and plane).
    bool loadDefinition(SoundBank& bank, Mixer& mixer, std::string_view name, std::string_view csv,
                        Object3DManager* manager = nullptr, std::string* error = nullptr);
    // Aud3DObject::SetPositionPtr: where the owner is. A set with VECTORPOINTS
    // sounds from its point nearest the listener instead, chosen while it has
    // no slot (Aud3DObject::SetClosestPositionPtr).
    void setPosition(const Vec3& position) { m_position = position; }
    // Aud3DAmbientObject::Update(speed), the owner's call each frame (the
    // samples whose speed range contains `speed` play), followed by what
    // Aud3DObjectManager::Update does for a slot holder (UpdateAudio: the
    // echo, attenuation, pan and doppler, the samples). `inTunnel` as
    // CarAudioInputs::inTunnel (the manager's echo flag also counts).
    void update(const Mat34& listener, float speed, float dt, bool inTunnel = false);
    // ActivateSound: sample `index` plays from the next update.
    void activate(int index);
    // DeactivateSound: sample `index` stops playing; a loop stops at once,
    // a one-shot plays out.
    void deactivate(int index);
    // PlayOneShot(int): sample `index` at the object's attenuation, pan and
    // doppler (types 2 and 3) or at a random volume and pan (others), unless
    // it is playing. Only with a slot (the samples are assigned to slots).
    // Nothing in MM2 calls it from outside the object.
    void playOneShot(int index);
    // GetSoundIndex: the sample with this name (exact case), or -1.
    int soundIndex(std::string_view wave) const;
    // Aud3DAmbientObject::Reset (Aud3DObject::Reset), which the owners call
    // from their own Reset (gizBridge, gizFerry, gizTrain, aiSubway): an
    // object holding a slot gives it up (UnAssignSounds: the echo goes off,
    // every sample stops) and its distance history is forgotten. The samples'
    // active flags and one-shot timers stay.
    void reset();
    void stop();

    const AmbientSoundSet& definition() const { return m_def; }
    const std::string& name() const { return m_def.name; }
    bool audible() const { return hasSlot(); }
    bool active(int index) const;
    bool samplePlaying(int index) const;
    const Vec3& position() const { return m_position; }
    bool echoOn() const { return m_echo; }

    // Aud3DObject::GetClosestPositionPtr: the VECTORPOINTS entry with the
    // smallest |dx| + |dy| + |dz| to `p` (the first on ties); `p` itself when
    // the set has no points.
    static Vec3 nearestPoint(const AmbientSoundSet& set, const Vec3& p);

private:
    struct Sample {
        SoundSlot slot;
        float timer = 0;     // seconds to the next one-shot; 0 plays at once
        bool active = false; // tagAud3DAmbientSoundData +0x24
    };
    float slotDistance2() const override { return m_audio.distance2(); }
    int slotPriority() const override { return m_def.priority; }
    void slotLost() override; // RemoveFrom3DMgr -> UnAssignSounds
    void lose();
    float interval(const AmbientSampleDef& d) const;
    void playOneShot(std::size_t index);
    void updateAudio(const Mat34& listener, float dt, bool tunnel);
    void updateSoundData(float dt);
    void echoOn(float delay);
    void echoOff();

    AmbientSoundSet m_def;
    std::vector<Sample> m_samples;
    bool m_positional = false; // +0x69: a Loop, Interval or Triggered sample
    Audio3D m_audio;
    Vec3 m_position;
    float m_attenuation = 0.0f, m_pan = 0.0f, m_doppler = 1.0f; // +0x6c, +0x74, +0x70
    float m_speed = 0.0f;                                        // +0x78
    bool m_echo = false;                                         // +0x68
};

// The city's ambient emitters (Aud3DAmbObjContainer): the sets listed in
// aud/ambient/<city>ambientcontainer.csv, each an AmbientObject that stays
// where its file puts it.
class CityAmbience {
public:
    // Aud3DAmbObjContainer::Init.
    bool load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view city,
              Object3DManager* manager = nullptr);
    // Aud3DAmbObjContainer::Update: every object updates with speed 0
    // (Aud3DObjectManager::Update(0)).
    void update(const Mat34& listener, float dt, bool inTunnel = false);
    void update(const Vec3& listener, float dt, bool inTunnel = false);
    void stop();

    std::size_t setCount() const { return m_objects.size(); }
    const AmbientSoundSet* set(std::string_view name) const;
    AmbientObject* object(std::string_view name);
    // Whether a set currently holds a sound slot.
    bool audible(std::string_view name) const;

    static Vec3 nearestPoint(const AmbientSoundSet& set, const Vec3& p) {
        return AmbientObject::nearestPoint(set, p);
    }

private:
    std::vector<std::unique_ptr<AmbientObject>> m_objects;
};

// mmBridgeAudio: a drawbridge's sounds (gizBridge loads "drawbridge": the
// moving loop and the bell). gizBridge activates both when the bridge starts
// to move and deactivates them when it stops, and updates the object with
// speed 0 every frame.
class BridgeAudio : public AmbientObject {
public:
    // Activate / Deactivate: -1 means samples 0 and 1.
    void activate(int index);
    void deactivate(int index);
};

// aiSubwayAudio: a train's sounds (aiSubway and gizTrain load "subwaycar":
// sample 0 the running loop, sample 1 the stopped sound, "NOTHING" in the
// retail file).
class SubwayAudio : public AmbientObject {
public:
    // Activate / Deactivate: -1 means samples 0 and 1.
    void activate(int index);
    void deactivate(int index);
    // Update(speed): at 1 m/s or more sample 0 plays, below it sample 1;
    // the switch deactivates the other one. The object then updates with
    // speed 0.
    void update(const Mat34& listener, float speed, float dt, bool inTunnel = false);
    bool stopped() const { return m_state == 1; }

private:
    int m_state = 0; // +0x80: 0 running, 1 stopped
};

// aiCableCarAudio / aiCableCarAudioData: a cable car's bell, start, running
// loop and stop sounds, chosen by its speed. A positioned object (0..100 m,
// priority 8) that sounds only while it holds a slot.
class CableCarAudio : private SlotHolder {
public:
    enum State : int { Stopped = 0, Stopping = 1, Starting = 2, Running = 3 };

    // aiCableCarAudio::Init with the car's speed now; the samples are
    // CABLECARGOBELL, CABLECARSTOP, CABLECAR, CABLECARSTART and STREETCABLE
    // (the last one is assigned but never played).
    bool load(SoundBank& bank, Mixer& mixer, Object3DManager* manager = nullptr, float speed = 0.0f);
    // aiCableCar::Update: the car's position and speed, then Aud3DObject::
    // Update and, while it holds a slot, UpdateAudio.
    void update(const Mat34& listener, const Vec3& position, float speed, float dt);
    // aiCableCarAudio::Reset (Aud3DObject::Reset, from aiCableCar::Reset): a
    // car holding a slot gives it up (UnAssignSounds: the loop and the start
    // sound stop) and its distance history is forgotten. The state and the
    // previous speed stay.
    void reset();
    void stop();
    bool audible() const { return hasSlot(); }
    State state() const { return static_cast<State>(m_state); }

    // aiCableCarAudioData::UpdateState: stopped below 0.001 m/s (now and
    // before), starting when the speed crosses 0.1 m/s upwards, stopping when
    // it drops to 0.5 m/s or below, running once the start sample is over.
    static int nextState(int state, float speed, float previousSpeed, bool startPlaying);

private:
    float slotDistance2() const override { return m_audio.distance2(); }
    int slotPriority() const override { return 8; }
    void slotLost() override; // UnAssignSounds -> aiCableCarAudioData::Stop
    void updatePlay(float volume, float pan, float doppler);

    SoundSlot m_go, m_stop, m_loop, m_start, m_streetCable;
    Audio3D m_audio;
    Vec3 m_position;
    float m_previousSpeed = 0.0f; // +0x64
    int m_state = Stopped;        // +0x2c
    int m_lastState = Stopped;    // +0x30
};

// mmRainAudio: the rain loop (exterior, or interior while the camera is in
// the car), quieter under shelter (in tunnels), and at night thunder with a
// lightning flash.
class RainAudio {
public:
    // mmRainAudio::mmRainAudio: thunder only exists at night.
    void load(SoundBank& bank, Mixer& mixer, bool night = false);
    // mmRainAudio::Update / SetInterior; `sheltered` is the tunnel echo state.
    void update(bool raining, bool interior, bool sheltered, float dt);
    void stop();
    // mmRainAudio +2: set for one update 13 s into each cycle. MM2 tracks it
    // but nothing reads it: no lightning flash is drawn.
    bool lightningFlash() const { return m_flash; }

private:
    void shelter(bool on);

    SoundSlot m_exterior, m_interior, m_thunder, m_thunder2;
    bool m_night = false;
    bool m_interiorOn = false;
    bool m_sheltered = false;
    float m_timer = 0;
    int m_flashState = 0;
    bool m_thunderState = false; // the first clap has played, the second is due
    bool m_flash = false;
};

} // namespace mm2::audio::game
