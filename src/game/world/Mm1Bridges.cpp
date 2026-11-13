#include "game/world/Mm1Bridges.h"

#include "asset/Dlp.h"
#include "core/Libm.h"
#include "core/Log.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace mm2::game::world {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
// mmBridgeMgr::Init: a race's open angle (0.1 pi).
constexpr float kRaceOnGoal = 0.314159274f;
// The cells a part covers are sampled this far apart, half a metre above it.
constexpr float kRoomStep = 4.0f;

std::string_view textOf(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

Vec3 rotate(const Mat34& m, const Vec3& v) {
    return {(m.m0.x * v.x + m.m1.x * v.y) + m.m2.x * v.z, (m.m0.y * v.x + m.m1.y * v.y) + m.m2.y * v.z,
            (m.m0.z * v.x + m.m1.z * v.y) + m.m2.z * v.z};
}

} // namespace

std::string Mm1Bridges::tuningName(const Options& o) {
    std::string name;
    switch (o.mode) {
    case GameMode::Checkpoint:
        name = std::format("race{}", o.raceIndex);
        break;
    case GameMode::Circuit:
        name = std::format("circuit{}", o.raceIndex);
        break;
    case GameMode::Blitz:
        name = std::format("blitz{}", o.raceIndex);
        break;
    case GameMode::CopsAndRobbers:
        name = "copsnrobbers";
        break;
    default:
        name = "roam";
        break;
    }
    if (o.multiplayer)
        name += "m";
    return name;
}

city::Mm1BridgeTuning Mm1Bridges::tuningFor(const city::Mm1City& city, const vfs::Vfs& vfs, const Options& o) {
    city::Mm1BridgeTuning t;
    t.sets.resize(city.bridges.size());
    const bool race = o.mode != GameMode::Cruise && o.mode != GameMode::CrashCourse;
    if (o.multiplayer || !race) {
        // Up, for good unless the tuning says otherwise.
        for (auto& s : t.sets) {
            s.initialPos = 1;
            s.upInterval = std::numeric_limits<float>::infinity();
        }
    } else {
        // Triggered by the player within four times the distance between
        // the first leaf's hinge and the fourth part's (the other leaf) of
        // their midpoint, with no timeout; the leaves open to 0.1 pi.
        for (std::size_t i = 0; i < city.bridges.size(); ++i) {
            const auto& parts = city.bridges[i].parts;
            if (parts.size() < 4)
                continue;
            const Vec3 a = parts[0].position, b = parts[3].position;
            auto& s = t.sets[i];
            s.triggered = true;
            s.triggerPos = {a.x - (a.x - b.x) * 0.5f, a.y - (a.y - b.y) * 0.5f, a.z - (a.z - b.z) * 0.5f};
            const Vec3 d = a - b;
            s.triggerDist2 = (d.x * d.x + d.y * d.y + d.z * d.z) * 16.0f;
            s.triggerTimeout = std::numeric_limits<float>::infinity();
        }
        t.bridgeOnGoal = kRaceOnGoal;
    }
    const std::string path = std::format("mm1/tune/{}.mmbridgemgr", tuningName(o));
    if (auto bytes = vfs.readAll(path)) {
        t = city::parseMm1BridgeTuning(textOf(*bytes), t);
        log::info("mm1 bridges: {}", path);
    }
    return t;
}

Mm1Bridges::Mm1Bridges(const city::Mm1City& city, const vfs::Vfs& vfs, const bangers::BangerDataLibrary& data,
                       const bangers::BangerSet& bounds, const phys::Level* level, const Options& options)
    : m_tuning(tuningFor(city, vfs, options)) {
    std::size_t setIndex = 0;
    int bodies = 0;
    for (const auto& src : city.bridges) {
        Set set;
        set.name = src.name;
        set.tuning = setIndex < m_tuning.sets.size() ? m_tuning.sets[setIndex] : city::Mm1BridgeSetTuning{};
        ++setIndex;
        for (const auto& sp : src.parts) {
            Part p;
            p.model = sp.model;
            // mmAnimDOF::Init: a drawbridge leaf turns about X, anything
            // else (a crossing gate) about Z.
            p.kind = sp.model.starts_with("tpdrawbridge") ? Part::Kind::Leaf : Part::Kind::Gate;
            // The hinge's frame: X along the axis point on the ground, Y up,
            // Z = X x Y, at the point plus the DLP's PIVOT turned by it.
            Vec3 pivot{};
            if (auto bytes = vfs.readAll(std::format("mm1/dlp/{}.dlp", sp.model)))
                if (auto dlp = asset::parseDlp(*bytes))
                    pivot = dlp->point("PIVOT").value_or(Vec3{});
            p.base = sp.matrix();
            p.base.m3 = p.base.m3 + rotate(p.base, pivot);
            const bangers::BangerData* d = data.find(sp.model);
            if (d) {
                p.bodyOffset = d->cg - pivot;
                p.body = std::make_unique<GizmoBody>();
                p.body->model = sp.model;
                p.body->data = d;
                p.body->bounds = &bounds;
                // As MM2's gizmos (Gizmos::makeBody): collidable, with the
                // terrain and the wheels.
                p.body->collidable = true;
                p.body->terrainCollidable = true;
                p.body->wheelCollidable = true;
                p.body->audioId = d->colliderId;
                ++bodies;
            } else {
                log::warn("mm1 bridges: {} has no banger data", sp.model);
            }
            if (p.kind == Part::Kind::Gate) {
                // mmAnimDOF::Init: the gate's post, an unhit banger at the
                // gate's points.
                bangers::PlacedProp base;
                base.model = "tpcrossbase";
                Vec3 ahead{sp.axisPoint.x - sp.position.x, 0.0f, sp.axisPoint.z - sp.position.z};
                ahead = ahead.mag2() > 0.0f ? ahead.normalized() : Vec3{0, 0, -1};
                base.transform.m2 = ahead * -1.0f;
                base.transform.m1 = {0.0f, 1.0f, 0.0f};
                base.transform.m0 = base.transform.m1.cross(base.transform.m2);
                base.transform.m3 = sp.position;
                m_gateBases.push_back(std::move(base));
            }
            set.parts.push_back(std::move(p));
        }
        if (src.parts.size() >= 4) {
            const Vec3 a = src.parts[0].position, b = src.parts[3].position;
            set.centre = (a + b) * 0.5f;
        } else if (!src.parts.empty()) {
            set.centre = src.parts[0].position;
        }
        m_sets.push_back(std::move(set));
    }
    reset();
    // The cells each body covers, down: sampled over the leaf's deck (Size.x
    // wide, Size.z long toward -Z) or along the gate's arm (Size.x toward -X).
    if (level) {
        for (auto& set : m_sets) {
            for (auto& p : set.parts) {
                if (!p.body || !p.body->data)
                    continue;
                const Vec3 size = p.body->data->size;
                std::vector<Vec3> samples;
                if (p.kind == Part::Kind::Leaf) {
                    for (float x = -size.x * 0.5f; x <= size.x * 0.5f; x += kRoomStep)
                        for (float z = 0.0f; z >= -size.z; z -= kRoomStep)
                            samples.push_back({x, 0.5f, z});
                } else {
                    for (float x = 0.0f; x >= -size.x; x -= kRoomStep)
                        samples.push_back({x, 0.0f, 0.0f});
                }
                // The leaves are down at the reset when they start up, so
                // sample their down frame.
                for (const Vec3& s : samples) {
                    const int room = level->findRoom(p.base.transform(s) + Vec3{0.0f, 0.5f, 0.0f}, 0);
                    if (room > 0 && std::ranges::find(p.rooms, room) == p.rooms.end())
                        p.rooms.push_back(room);
                }
                for (int room : p.rooms) {
                    if (m_rooms.size() <= static_cast<std::size_t>(room))
                        m_rooms.resize(static_cast<std::size_t>(room) + 1);
                    m_rooms[static_cast<std::size_t>(room)].push_back(p.body.get());
                }
                if (!p.rooms.empty()) {
                    p.body->room = p.rooms.front();
                    p.body->listed = true;
                }
            }
        }
    }
    log::info("mm1 bridges: {} sets, {} moving parts, tuning {}", m_sets.size(), bodies, tuningName(options));
}

Mm1Bridges::~Mm1Bridges() { stopAudio(); }

void Mm1Bridges::loadAudio(const vfs::Vfs& vfs, audio::SoundBank& bank, audio::Mixer& mixer,
                           audio::game::Object3DManager* manager) {
    for (auto& s : m_sets) {
        s.audio = std::make_unique<audio::game::BridgeAudio>();
        if (!s.audio->load(vfs, bank, mixer, "drawbridge", manager))
            s.audio.reset();
    }
}

void Mm1Bridges::updateAudio(const Mat34& listener, float dt, bool inTunnel) {
    for (auto& s : m_sets) {
        if (!s.audio)
            continue;
        // As gizBridge::Update: activated as the leaves start, deactivated
        // when they stop.
        const bool leavesMoving = std::ranges::any_of(s.parts, [](const Part& p) {
            return p.kind == Part::Kind::Leaf &&
                   (p.state == Part::State::TurningOn || p.state == Part::State::TurningOff) && p.t > 0.0f &&
                   p.t < 1.0f;
        });
        if (leavesMoving != s.sounding) {
            if (leavesMoving)
                s.audio->activate(-1);
            else
                s.audio->deactivate(-1);
            s.sounding = leavesMoving;
        }
        s.audio->setPosition(s.centre);
        s.audio->update(listener, 0.0f, dt, inTunnel);
    }
}

void Mm1Bridges::stopAudio() {
    for (auto& s : m_sets)
        if (s.audio)
            s.audio->stop();
}

void Mm1Bridges::setSwitch(Part& p, bool on) {
    // mmAnimDOF::SetSwitch: start turning from the end it is leaving.
    p.state = on ? Part::State::TurningOn : Part::State::TurningOff;
    p.t = on ? 0.0f : 1.0f;
    p.delay = 0.0f;
}

void Mm1Bridges::setState(Part& p, bool on) {
    // mmAnimDOF::SetState: at rest at one end.
    p.state = on ? Part::State::On : Part::State::Off;
    p.t = on ? 1.0f : 0.0f;
    p.delay = 0.0f;
}

void Mm1Bridges::place(Part& p, const city::Mm1BridgeTuning& t) {
    const bool leaf = p.kind == Part::Kind::Leaf;
    const float off = leaf ? t.bridgeOffGoal : t.gateOffGoal;
    const float on = leaf ? t.bridgeOnGoal : t.gateOnGoal;
    // mmAnimDOF::Update: the progress eased by (cos((t + 1) pi) + 1) / 2,
    // between the goals, turning the hinge's frame (RotateAbs, then Dot).
    const float t01 = std::clamp(p.t, 0.0f, 1.0f);
    const float ease = (libm::cos((t01 + 1.0f) * kPi) + 1.0f) * 0.5f; // fcos
    const float angle = off + (on - off) * ease;
    p.matrix = Mat34::mul(leaf ? Mat34::rotationX(angle) : Mat34::rotationZ(angle), p.base);
    if (p.body) {
        Mat34 m = p.matrix;
        m.m3 = m.m3 + rotate(p.matrix, p.bodyOffset);
        p.body->setMatrix(m);
    }
}

void Mm1Bridges::updatePart(Part& p, const city::Mm1BridgeTuning& t, float dt) {
    // mmAnimDOF::Update.
    const bool leaf = p.kind == Part::Kind::Leaf;
    const bool turningOn = p.state == Part::State::TurningOn;
    if (turningOn || p.state == Part::State::TurningOff) {
        const float delta = leaf ? t.bridgeDelta : t.gateDelta;
        const float delay = leaf ? (turningOn ? t.bridgeOnDelay : t.bridgeOffDelay)
                                 : (turningOn ? t.gateOnDelay : t.gateOffDelay);
        if (delay > p.delay) {
            p.delay = dt + p.delay;
        } else {
            p.t = turningOn ? dt * delta + p.t : p.t - dt * delta;
            p.t = std::clamp(p.t, 0.0f, 1.0f);
            if (p.t == 1.0f)
                p.state = Part::State::On;
            else if (p.t == 0.0f)
                p.state = Part::State::Off;
        }
    }
    place(p, t);
}

void Mm1Bridges::reset() {
    m_time = 0.0f;
    for (auto& s : m_sets) {
        // mmBridgeSet::Reset: the switch at the initial position, waiting
        // for the trigger when it has one; every part at rest there.
        s.switchOn = s.tuning.initialPos;
        s.state = s.tuning.triggered ? 3 : s.tuning.initialPos;
        // OpenMM2: the timers start over too (MM1's Reset leaves them).
        s.downTimer = 0.0f;
        s.upTimer = 0.0f;
        if (s.audio) {
            s.audio->reset();
            s.sounding = false;
        }
        for (auto& p : s.parts) {
            setState(p, s.tuning.initialPos != 0);
            place(p, m_tuning);
        }
    }
}

void Mm1Bridges::updateSet(Set& s, float dt, const std::optional<Vec3>& player) {
    // mmBridgeSet::Update.
    if (s.state == 0) {
        if (s.downTimer < s.tuning.downInterval)
            s.downTimer = dt + s.downTimer;
    } else if (s.state == 1) {
        if (s.upTimer < s.tuning.upInterval)
            s.upTimer = dt + s.upTimer;
    } else if (s.state == 3) {
        // mmAnimTrigger::CheckObjectDistance (the player within the
        // distance), or the timeout passed.
        const bool near = player && (*player - s.tuning.triggerPos).mag2() < s.tuning.triggerDist2;
        if (!near && !(m_time > s.tuning.triggerTimeout)) {
            for (auto& p : s.parts)
                updatePart(p, m_tuning, dt);
            return;
        }
        s.switchOn = s.tuning.initialPos == 0 ? 1 : 0;
        for (auto& p : s.parts)
            setSwitch(p, s.switchOn != 0);
        s.state = 2;
    }
    // An interval has run out: switch every part the other way.
    if ((s.downTimer > s.tuning.downInterval || s.upTimer > s.tuning.upInterval) && s.state != 2) {
        s.switchOn = s.state == 0 ? 1 : 0;
        for (auto& p : s.parts)
            setSwitch(p, s.switchOn != 0);
        s.downTimer = 0.0f;
        s.upTimer = 0.0f;
    }
    // Moving while any part turns, else at the switch's end.
    const bool moving = std::ranges::any_of(s.parts, [](const Part& p) {
        return p.state == Part::State::TurningOn || p.state == Part::State::TurningOff;
    });
    s.state = moving ? 2 : s.switchOn;
    for (auto& p : s.parts)
        updatePart(p, m_tuning, dt);
}

void Mm1Bridges::update(float dt, const std::optional<Vec3>& player) {
    m_time = dt + m_time;
    for (auto& s : m_sets)
        updateSet(s, dt, player);
}

void Mm1Bridges::instancesIn(int room, std::vector<phys::Instance*>& out) const {
    if (room <= 0 || static_cast<std::size_t>(room) >= m_rooms.size())
        return;
    for (GizmoBody* b : m_rooms[static_cast<std::size_t>(room)])
        out.push_back(b);
}

std::vector<bangers::PlacedProp> Mm1Bridges::gateBases() const { return m_gateBases; }

} // namespace mm2::game::world
