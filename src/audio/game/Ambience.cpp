// Positioned ambient sounds and rain, ported from MM2 (Aud3DAmbientObject,
// Aud3DAmbObjContainer, mmBridgeAudio, aiSubwayAudio, aiCableCarAudio,
// aiCableCarAudioData, mmRainAudio). See docs/audio.md.
#include "audio/game/Ambience.h"

#include "audio/AngelRandom.h"
#include "core/Log.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::audio::game {
namespace {

// aiSubwayAudio::Update: the running loop from 1 m/s.
constexpr float kSubwayRunningSpeed = 1.0f;
// aiCableCarAudioData::UpdateState thresholds (m/s).
constexpr float kCableCarStill = 0.001f, kCableCarStart = 0.1f, kCableCarStop = 0.5f;
// aiCableCarAudio::UpdateAudio: every sample plays at 0.98 of the attenuation.
constexpr float kCableCarVolume = 0.98f;
// aiCableCarAudio::Init.
constexpr float kCableCarMaxDistance = 100.0f;

// SetEffect(1), SetDelayTime(delay), SetEchoAttenuation(0.96).
void sampleEchoOn(SoundSlot& s, float delay) {
    s.enableEcho();
    s.setEchoDelay(delay);
    s.setEchoAttenuation(kEchoAttenuation);
}

} // namespace

// --- AmbientObject ------------------------------------------------------------------

AmbientObject::~AmbientObject() = default;

Vec3 AmbientObject::nearestPoint(const AmbientSoundSet& set, const Vec3& p) {
    if (set.points.empty())
        return p;
    auto pseudo = [&](const Vec3& q) {
        return std::abs(q.x - p.x) + std::abs(q.y - p.y) + std::abs(q.z - p.z);
    };
    Vec3 best = set.points.front();
    float bestD = pseudo(best);
    for (std::size_t i = 1; i < set.points.size(); ++i) {
        if (const float d = pseudo(set.points[i]); d < bestD) {
            bestD = d;
            best = set.points[i];
        }
    }
    return best;
}

bool AmbientObject::load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view name,
                         Object3DManager* manager, std::string* error) {
    const std::string path = std::format("aud/ambient/{}.csv", str::lower(name));
    auto text = readText(vfs, path);
    if (!text) {
        if (error)
            *error = std::format("{} not found", path);
        return false;
    }
    std::string parseError;
    if (!loadDefinition(bank, mixer, name, *text, manager, &parseError)) {
        if (error)
            *error = std::format("{}: {}", path, parseError);
        return false;
    }
    return true;
}

bool AmbientObject::loadDefinition(SoundBank& bank, Mixer& mixer, std::string_view name, std::string_view csv,
                                   Object3DManager* manager, std::string* error) {
    stop();
    setManager(manager);
    m_samples.clear();
    m_echo = false;
    auto def = parseAmbientSoundSet(name, csv, error);
    if (!def)
        return false;
    m_def = std::move(*def);
    m_audio = Audio3D();
    m_audio.setDropOffs(m_def.minDistance, m_def.maxDistance);
    m_samples.resize(m_def.samples.size());
    m_positional = false;
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        const auto& d = m_def.samples[i];
        m_samples[i].slot.load(mixer, bank, d.wave, Bus::Effects);
        m_samples[i].timer = 0.0f;
        m_samples[i].active = d.active;
        // ReadSoundData: any Loop, Interval or Triggered sample makes the set positional.
        if (d.type != AmbientSampleType::RandomOnce)
            m_positional = true;
    }
    if (!m_def.points.empty())
        m_position = m_def.points.front();
    m_attenuation = m_pan = 0.0f;
    m_doppler = 1.0f;
    m_speed = 0.0f;
    return true;
}

float AmbientObject::interval(const AmbientSampleDef& d) const {
    // PendOneShot: RandomizeNumber(low, high), or exactly high when equal.
    if (d.intervalLow == d.intervalHigh)
        return d.intervalHigh;
    return static_cast<float>(randomizeNumber(d.intervalLow, d.intervalHigh));
}

void AmbientObject::echoOn(float delay) {
    for (auto& s : m_samples)
        sampleEchoOn(s.slot, delay);
    m_echo = true;
}

void AmbientObject::echoOff() {
    for (auto& s : m_samples)
        s.slot.disableEcho();
    m_echo = false;
}

void AmbientObject::slotLost() {
    // UnAssignSounds: the echo goes off and every sample stops.
    if (m_echo)
        echoOff();
    for (auto& s : m_samples)
        if (s.slot.playing())
            s.slot.stop();
}

void AmbientObject::lose() {
    // RemoveFrom3DMgr.
    releaseSlot();
    slotLost();
}

void AmbientObject::reset() {
    // Aud3DAmbientObject::Reset is Aud3DObject::Reset: RemoveFrom3DMgr for a
    // slot holder, then the distance values. Aud3DAmbientObject's own
    // attenuation, pan and doppler (+0x6c, +0x74, +0x70) are not touched.
    if (hasSlot())
        lose();
    m_audio.reset();
}

void AmbientObject::stop() {
    releaseSlot();
    if (m_echo)
        echoOff();
    for (auto& s : m_samples)
        s.slot.stop();
}

bool AmbientObject::active(int index) const {
    return 0 <= index && static_cast<std::size_t>(index) < m_samples.size() &&
           m_samples[static_cast<std::size_t>(index)].active;
}

bool AmbientObject::samplePlaying(int index) const {
    return 0 <= index && static_cast<std::size_t>(index) < m_samples.size() &&
           m_samples[static_cast<std::size_t>(index)].slot.playing();
}

void AmbientObject::activate(int index) {
    if (0 <= index && static_cast<std::size_t>(index) < m_samples.size())
        m_samples[static_cast<std::size_t>(index)].active = true;
}

void AmbientObject::deactivate(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_samples.size())
        return;
    auto& s = m_samples[static_cast<std::size_t>(index)];
    s.active = false;
    // A loop stops; the samples are only assigned while the object has a slot.
    if (m_def.samples[static_cast<std::size_t>(index)].type == AmbientSampleType::Loop && hasSlot() &&
        s.slot.playing())
        s.slot.stop();
}

int AmbientObject::soundIndex(std::string_view wave) const {
    for (std::size_t i = 0; i < m_def.samples.size(); ++i)
        if (m_def.samples[i].wave == wave)
            return static_cast<int>(i);
    return -1;
}

void AmbientObject::playOneShot(int index) {
    if (hasSlot() && 0 <= index && static_cast<std::size_t>(index) < m_samples.size())
        playOneShot(static_cast<std::size_t>(index));
}

void AmbientObject::playOneShot(std::size_t index) {
    // Aud3DAmbientObject::PlayOneShot(tagAud3DAmbientSoundData*).
    const auto& d = m_def.samples[index];
    auto& slot = m_samples[index].slot;
    float volume, pan, pitch;
    if (d.type == AmbientSampleType::Interval || d.type == AmbientSampleType::Triggered) {
        volume = m_attenuation;
        pan = m_pan;
        pitch = m_doppler;
    } else {
        // Two draws in the same second: the volume and the pan move together.
        volume = static_cast<float>(randomizeNumber(0.75f, 1.0f));
        pan = static_cast<float>(randomizeNumber(-1.0f, 1.0f));
        pitch = 1.0f;
    }
    slot.setVolume(d.volume * volume);
    slot.setPan(pan);
    if (d.doppler)
        slot.setPitch(pitch);
    if (!slot.playing())
        slot.playOnce();
}

void AmbientObject::updateSoundData(float dt) {
    // UpdateSoundData: only active samples.
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        const auto& d = m_def.samples[i];
        auto& smp = m_samples[i];
        if (!smp.active)
            continue;
        if (d.type != AmbientSampleType::RandomOnce) {
            // UpdateDoppler.
            smp.slot.setVolume(m_attenuation * d.volume);
            if (d.doppler)
                smp.slot.setPitch(m_doppler);
            smp.slot.setPan(m_pan);
        }
        const bool inSpeedRange = d.minSpeed <= m_speed && m_speed <= d.maxSpeed;
        switch (d.type) {
        case AmbientSampleType::Loop: // UpdateLoop
            if (!inSpeedRange)
                smp.slot.stop();
            else if (!smp.slot.playing())
                smp.slot.playLoop();
            break;
        case AmbientSampleType::RandomOnce:
        case AmbientSampleType::Interval: // UpdateOneShot
            if (inSpeedRange) {
                if (smp.timer <= 0.0f) {
                    playOneShot(i);
                    smp.timer = interval(d);
                }
                smp.timer -= dt;
            }
            break;
        case AmbientSampleType::Triggered: break;
        }
    }
}

void AmbientObject::updateAudio(const Mat34& listener, float dt, bool tunnel) {
    // Aud3DAmbientObject::UpdateAudio(): the echo follows the manager's flag;
    // a set heard only underground (or only above ground) gives up its slot
    // when that changes.
    if (!m_echo) {
        if (tunnel)
            echoOn(tunnelEchoDelay(manager()));
    } else if (!tunnel) {
        echoOff();
    }
    if (m_def.audibleArea == 1) {
        if (!m_echo) {
            lose();
            return;
        }
        for (auto& s : m_samples)
            s.slot.updateEcho(dt);
    } else if (m_def.audibleArea == 2) {
        if (m_echo) {
            lose();
            return;
        }
    } else if (m_echo) {
        for (auto& s : m_samples)
            s.slot.updateEcho(dt);
    }
    // UpdateAudio(doppler factor).
    if (m_audio.pastMaxDistance(m_position, listener.m3)) {
        lose();
        return;
    }
    if (m_positional) {
        m_attenuation = m_audio.attenuation();
        m_pan = m_audio.pan(listener, m_position);
        m_doppler = m_audio.doppler(1.0f / kDopplerSpeed, dt);
    }
    updateSoundData(dt);
}

void AmbientObject::update(const Mat34& listener, float speed, float dt, bool inTunnel) {
    // Aud3DAmbientObject::Update(speed): the slot request only within the
    // audible area.
    m_speed = speed;
    const bool tunnel = inTunnel || (manager() && manager()->echo());
    const int area = m_def.audibleArea;
    const bool inArea = area == 1 ? tunnel : area == 2 ? !tunnel : true;
    if (inArea && !hasSlot()) {
        // Aud3DObject::Update: a set with points sounds from the one nearest
        // the listener, chosen while it has no slot.
        if (!m_def.points.empty())
            m_position = nearestPoint(m_def, listener.m3);
        acquireSlot(m_audio.withinMaxDistance(m_position, listener.m3));
    }
    // Aud3DObjectManager::Update: UpdateAudio for a slot holder.
    if (hasSlot())
        updateAudio(listener, dt, tunnel);
}

// --- CityAmbience -----------------------------------------------------------------

bool CityAmbience::load(const vfs::Vfs& vfs, SoundBank& bank, Mixer& mixer, std::string_view city,
                        Object3DManager* manager) {
    stop();
    m_objects.clear();
    auto text = readText(vfs, std::format("aud/ambient/{}ambientcontainer.csv", str::lower(city)));
    if (!text)
        return false;
    for (const auto& name : parseAmbientContainer(*text)) {
        // Aud3DAmbObjContainer::CreateAmbientObject (FileValid first).
        auto object = std::make_unique<AmbientObject>();
        std::string error;
        if (object->load(vfs, bank, mixer, name, manager, &error))
            m_objects.push_back(std::move(object));
        else
            log::debug("ambience: {}", error);
    }
    return !m_objects.empty();
}

void CityAmbience::update(const Vec3& listener, float dt, bool inTunnel) {
    Mat34 l = Mat34::identity();
    l.m3 = listener;
    update(l, dt, inTunnel);
}

void CityAmbience::update(const Mat34& listener, float dt, bool inTunnel) {
    for (auto& o : m_objects)
        o->update(listener, 0.0f, dt, inTunnel);
}

AmbientObject* CityAmbience::object(std::string_view name) {
    for (auto& o : m_objects)
        if (str::iequals(o->name(), name))
            return o.get();
    return nullptr;
}

const AmbientSoundSet* CityAmbience::set(std::string_view name) const {
    for (const auto& o : m_objects)
        if (str::iequals(o->name(), name))
            return &o->definition();
    return nullptr;
}

bool CityAmbience::audible(std::string_view name) const {
    for (const auto& o : m_objects)
        if (str::iequals(o->name(), name))
            return o->audible();
    return false;
}

void CityAmbience::stop() {
    for (auto& o : m_objects)
        o->stop();
}

// --- BridgeAudio, SubwayAudio --------------------------------------------------------

void BridgeAudio::activate(int index) {
    // mmBridgeAudio::Activate.
    if (index == -1) {
        for (int i = 0; i < 2; ++i)
            AmbientObject::activate(i);
        return;
    }
    AmbientObject::activate(index);
}

void BridgeAudio::deactivate(int index) {
    if (index == -1) {
        for (int i = 0; i < 2; ++i)
            AmbientObject::deactivate(i);
        return;
    }
    AmbientObject::deactivate(index);
}

void SubwayAudio::activate(int index) {
    // aiSubwayAudio::Activate.
    if (index == -1) {
        for (int i = 0; i < 2; ++i)
            AmbientObject::activate(i);
        return;
    }
    AmbientObject::activate(index);
}

void SubwayAudio::deactivate(int index) {
    if (index == -1) {
        for (int i = 0; i < 2; ++i)
            AmbientObject::deactivate(i);
        return;
    }
    AmbientObject::deactivate(index);
}

void SubwayAudio::update(const Mat34& listener, float speed, float dt, bool inTunnel) {
    // aiSubwayAudio::Update(speed).
    if (kSubwayRunningSpeed <= speed) {
        if (m_state == 1) {
            deactivate(1);
            activate(0);
            m_state = 0;
        }
    } else if (m_state == 0) {
        deactivate(0);
        activate(1);
        m_state = 1;
    }
    AmbientObject::update(listener, 0.0f, dt, inTunnel);
}

// --- CableCarAudio -------------------------------------------------------------------

int CableCarAudio::nextState(int state, float speed, float previousSpeed, bool startPlaying) {
    // aiCableCarAudioData::UpdateState(speed, previous speed).
    if (previousSpeed < kCableCarStill && speed < kCableCarStill)
        return Stopped;
    if (previousSpeed < kCableCarStart && kCableCarStart <= speed)
        return Starting;
    if (kCableCarStop < previousSpeed && speed <= kCableCarStop)
        return Stopping;
    if (state == Starting && !startPlaying)
        return Running;
    return state;
}

bool CableCarAudio::load(SoundBank& bank, Mixer& mixer, Object3DManager* manager, float speed) {
    stop();
    setManager(manager);
    // aiCableCarAudioData: the samples (Aud3DObjectManager::AllocateSample).
    bool any = m_go.load(mixer, bank, "CABLECARGOBELL", Bus::Effects);
    any = m_stop.load(mixer, bank, "CABLECARSTOP", Bus::Effects) || any;
    any = m_loop.load(mixer, bank, "CABLECAR", Bus::Effects) || any;
    any = m_start.load(mixer, bank, "CABLECARSTART", Bus::Effects) || any;
    any = m_streetCable.load(mixer, bank, "STREETCABLE", Bus::Effects) || any;
    // aiCableCarAudio::Init: drop-offs 0..100 m, priority 8, the speed now.
    m_audio = Audio3D();
    m_audio.setDropOffs(0.0f, kCableCarMaxDistance);
    m_previousSpeed = speed;
    m_state = m_lastState = Stopped;
    return any;
}

void CableCarAudio::slotLost() {
    // aiCableCarAudioData::UnAssignSounds -> Stop: the loop and the start
    // sound stop; the bell and the stop sound play out.
    if (m_loop.playing())
        m_loop.stop();
    if (m_start.playing())
        m_start.stop();
}

void CableCarAudio::reset() {
    // aiCableCarAudio::Reset is Aud3DObject::Reset: RemoveFrom3DMgr for a
    // slot holder, then the distance values.
    if (hasSlot()) {
        releaseSlot();
        slotLost();
    }
    m_audio.reset();
}

void CableCarAudio::stop() {
    releaseSlot();
    for (auto* s : {&m_go, &m_stop, &m_loop, &m_start, &m_streetCable})
        s->stop();
}

void CableCarAudio::updatePlay(float volume, float pan, float doppler) {
    // aiCableCarAudioData::UpdatePlay(volume, pan, frequency).
    switch (m_state) {
    case Stopped:
        if (m_loop.playing())
            m_loop.stop();
        break;
    case Stopping:
        if (m_loop.playing())
            m_loop.stop();
        m_stop.setVolume(volume);
        m_stop.setPan(pan);
        m_stop.setPitch(doppler);
        if (!m_stop.playing())
            m_stop.playOnce();
        break;
    case Starting:
        m_start.setVolume(volume);
        m_start.setPan(pan);
        m_start.setPitch(doppler);
        if (!m_start.playing())
            m_start.playOnce();
        // The bell rings once as the car sets off, at the start's volume
        // with no pan or doppler.
        m_go.setVolume(volume);
        if (m_lastState != Starting && !m_go.playing())
            m_go.playOnce();
        break;
    case Running:
        m_loop.setVolume(volume);
        m_loop.setPan(pan);
        m_loop.setPitch(doppler);
        if (!m_loop.playing())
            m_loop.playLoop();
        break;
    default: break;
    }
    m_lastState = m_state;
}

void CableCarAudio::update(const Mat34& listener, const Vec3& position, float speed, float dt) {
    m_position = position;
    // Aud3DObject::Update: a slot while within 100 m.
    if (!hasSlot() && !acquireSlot(m_audio.withinMaxDistance(position, listener.m3)))
        return;
    // aiCableCarAudio::UpdateAudio(): then the speed becomes the previous one.
    if (m_audio.pastMaxDistance(position, listener.m3)) {
        releaseSlot();
        slotLost();
    } else {
        const float attenuation = m_audio.attenuation();
        const float pan = m_audio.pan(listener, position);
        // _fCableCarUpdate is 1 / 56.7166, the usual doppler factor.
        const float doppler = m_audio.doppler(1.0f / kDopplerSpeed, dt);
        m_state = nextState(m_state, speed, m_previousSpeed, m_start.playing());
        updatePlay(kCableCarVolume * attenuation, pan, doppler);
    }
    m_previousSpeed = speed;
}

// --- RainAudio ---------------------------------------------------------------------

namespace {
// mmRainAudio: initial volumes; ShelterOff / ShelterOn volumes; thunder timing.
constexpr float kExteriorVolume = 0.82f, kInteriorVolume = 0.85f;
constexpr float kOpenExteriorVolume = 0.76f, kOpenInteriorVolume = 0.83f;
constexpr float kShelterExteriorVolume = 0.65f, kShelterThunderVolume = 0.85f;
constexpr float kFlashTime = 13.0f, kThunderTime = 15.0f, kSecondThunderDelay = 1.0f;
} // namespace

void RainAudio::load(SoundBank& bank, Mixer& mixer, bool night) {
    m_night = night;
    m_exterior.load(mixer, bank, "Rainexterior", Bus::Effects);
    m_exterior.setVolume(kExteriorVolume);
    m_interior.load(mixer, bank, "Raininterior", Bus::Effects);
    m_interior.setVolume(kInteriorVolume);
    m_thunder = SoundSlot();
    m_thunder2 = SoundSlot();
    if (night) {
        m_thunder.load(mixer, bank, "Thunder", Bus::Effects);
        m_thunder.setVolume(1.0f);
        m_thunder.setPan(-0.2f);
        m_thunder2.load(mixer, bank, "Thunder", Bus::Effects);
        m_thunder2.setVolume(1.0f);
        m_thunder2.setPan(0.2f);
        m_thunder2.setPitch(0.8f);
    }
    m_interiorOn = m_sheltered = false;
    m_timer = 0;
    m_flashState = 0;
    m_thunderState = false;
}

void RainAudio::shelter(bool on) {
    if (on) {
        // ShelterOn.
        m_exterior.setVolume(kShelterExteriorVolume);
        m_interior.setVolume(0.0f);
        m_thunder.setVolume(kShelterThunderVolume);
        m_thunder2.setVolume(kShelterThunderVolume);
        m_thunder.setPan(0.0f);
        m_thunder2.setPan(0.0f);
    } else {
        // ShelterOff. Its thunder pans of -20 and 20 are clamped to -1 and 1
        // (audObject::SetPan), so after the first shelter the claps are hard
        // left and hard right instead of +-0.2.
        m_exterior.setVolume(kOpenExteriorVolume);
        m_interior.setVolume(kOpenInteriorVolume);
        m_thunder.setVolume(1.0f);
        m_thunder2.setVolume(1.0f);
        m_thunder.setPan(-20.0f);
        m_thunder2.setPan(20.0f);
    }
    m_sheltered = on;
}

void RainAudio::update(bool raining, bool interior, bool sheltered, float dt) {
    m_flash = false;
    if (!raining) {
        stop();
        return;
    }
    // SetInterior.
    if (interior != m_interiorOn) {
        SoundSlot& on = interior ? m_interior : m_exterior;
        SoundSlot& off = interior ? m_exterior : m_interior;
        on.playLoop();
        off.stop();
        m_interiorOn = interior;
    }
    // Update.
    if (sheltered != m_sheltered)
        shelter(sheltered);
    SoundSlot& current = m_interiorOn ? m_interior : m_exterior;
    if (!current.playing())
        current.playLoop();
    if (!m_night) {
        m_timer = 0;
        return;
    }
    if (m_flashState == 0 && m_timer > kFlashTime) {
        m_flashState = 1;
        m_flash = true;
    } else if (m_flashState == 1) {
        m_flashState = 2;
    }
    if (kThunderTime <= m_timer && !m_thunderState) {
        if (!m_thunder.playing())
            m_thunder.playOnce();
        m_thunderState = true;
        m_timer = 0;
    }
    if (kSecondThunderDelay < m_timer && m_thunderState) {
        if (!m_thunder2.playing())
            m_thunder2.playOnce();
        m_thunderState = false;
        m_timer = 0;
        m_flashState = 0;
    }
    m_timer += dt;
}

void RainAudio::stop() {
    m_exterior.stop();
    m_interior.stop();
    m_thunder.stop();
    m_thunder2.stop();
    m_timer = 0;
    m_flashState = 0;
    m_thunderState = false;
}

} // namespace mm2::audio::game
