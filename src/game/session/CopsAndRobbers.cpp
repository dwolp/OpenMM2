#include "game/session/CopsAndRobbers.h"

#include "city/Race.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mm2::game::session {
namespace {

// mmMultiCR::FondleCarMass: gold weight option -> extra kg and the
// carrier's throttle cap (mmGame::UpdateSteeringBrakes, multiplayer, above
// first gear).
constexpr std::array<float, 3> kGoldMassKg{0.0f, 100.0f, 200.0f};
constexpr std::array<float, 3> kGoldThrottle{1.0f, 0.9f, 0.81f};

// mmMultiCR::UpdateTimeWarning: minutes left and their thresholds.
constexpr std::array<int, 5> kWarningMinutes{20, 15, 10, 5, 1};

std::uint32_t nextRandom(std::uint32_t& rng) {
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fffu;
}

} // namespace

std::optional<CrLocations> loadCrLocations(const vfs::Vfs& vfs, const std::string& raceDir) {
    return loadCrLocationsIn(vfs, "race/" + raceDir + "/");
}

std::optional<CrLocations> loadCrLocationsIn(const vfs::Vfs& vfs, const std::string& folder) {
    auto bytes = vfs.readAll(folder + "multicopwaypoints.csv");
    if (!bytes)
        bytes = vfs.readAll(folder + "copswaypoints.csv"); // MM1's name
    if (!bytes)
        return std::nullopt;
    auto pts = city::parseWaypoints(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    // mmMultiCR::LoadCSV needs at least three places.
    if (!pts || pts->size() < 3)
        return std::nullopt;
    CrLocations loc;
    for (const auto& p : *pts)
        loc.points.push_back(p.position);
    return loc;
}

CopsAndRobbers::CopsAndRobbers(const CrSettings& settings, const CrLocations& locations)
    : m_settings(settings), m_locations(locations), m_rng(settings.seed ? settings.seed : 1) {
    m_timeLeft = settings.timeLimitSeconds;
    m_lastWarning = settings.timeLimitSeconds;
    newSet();
}

void CopsAndRobbers::addCar(int id, CrTeam team) {
    m_cars.emplace_back(id, team);
    m_scores.emplace_back(id, 0);
}

CrTeam CopsAndRobbers::teamOf(int id) const {
    for (const auto& [cid, team] : m_cars)
        if (cid == id)
            return team;
    return CrTeam::Robber;
}

Vec3 CopsAndRobbers::deliveryTarget(CrTeam team) const { return teamZero(team) ? m_set.bank : m_set.hideout; }

int CopsAndRobbers::score(CrTeam team) const {
    // mmMultiCR::UpdateHUD: team totals are the members' scores.
    int total = 0;
    for (const auto& [id, s] : m_scores)
        if (teamZero(teamOf(id)) == teamZero(team))
            total += s;
    return total;
}

int CopsAndRobbers::playerScore(int id) const {
    for (const auto& [cid, s] : m_scores)
        if (cid == id)
            return s;
    return 0;
}

float CopsAndRobbers::carrierExtraMassKg() const {
    return kGoldMassKg[static_cast<std::size_t>(std::clamp(m_settings.goldMass, 0, 2))];
}

float CopsAndRobbers::carrierThrottleCap() const {
    return kGoldThrottle[static_cast<std::size_t>(std::clamp(m_settings.goldMass, 0, 2))];
}

Vec3 CopsAndRobbers::randomPoint() {
    // GetRandomPoints' picker (an unnamed helper): a coin flip between an AI
    // intersection and a row of the pool other than the last.
    const auto& pool = m_locations.points;
    const bool coin = (nextRandom(m_rng) & 1u) != 0;
    if ((pool.size() < 2 || coin) && m_settings.randomIntersection)
        if (auto p = m_settings.randomIntersection())
            return *p;
    if (pool.empty())
        return {};
    if (pool.size() == 1)
        return pool.front();
    return pool[nextRandom(m_rng) % (pool.size() - 1)];
}

void CopsAndRobbers::newSet() {
    // mmMultiCR::GetNewSet / GetRandomPoints: gold, then a different bank,
    // then a hideout different from both.
    m_set.gold = randomPoint();
    m_set.bank = randomPoint();
    for (int i = 0; i < 64 && m_set.bank == m_set.gold; ++i)
        m_set.bank = randomPoint();
    m_set.hideout = randomPoint();
    for (int i = 0; i < 64 && (m_set.hideout == m_set.bank || m_set.hideout == m_set.gold); ++i)
        m_set.hideout = randomPoint();
    m_goldPos = m_set.gold;
    m_carrier = -1;
    m_goldActive = true;
    m_events.push_back({EventType::NewSet});
}

void CopsAndRobbers::drop(int carId, const Vec3& at, bool toSpawn, bool knocked) {
    // mmMultiCR::DropGold: where the carrier is, unless that is in deep
    // water or the drop is forced (the water, a fall); then back to the
    // set's place. Either way on the ground under it (FindGround).
    const bool stays = !toSpawn && (!m_settings.canDropAt || m_settings.canDropAt(at));
    const Vec3 p = stays ? at : m_set.gold;
    m_goldPos = m_settings.findGround ? m_settings.findGround(p) : p;
    m_carrier = -1;
    m_goldActive = true;
    m_events.push_back({EventType::GoldDropped, carId, knocked ? 1 : 0});
}

std::vector<CopsAndRobbers::Message> CopsAndRobbers::playerLeft(int id, bool host) {
    std::vector<Message> out;
    if (!host || m_carrier != id || m_over)
        return out;
    drop(id, m_goldPos, false);
    out.push_back({Message::Type::GoldDropped, id, m_goldPos});
    return out;
}

void CopsAndRobbers::take(int carId) {
    // StealGold / OppStealGold, and Score(25) for the taker.
    m_carrier = carId;
    m_goldActive = false;
    score(carId, kPickupPoints);
    m_events.push_back({EventType::GoldTaken, carId, kPickupPoints});
}

void CopsAndRobbers::deliver(int carId) {
    // UpdateBank / UpdateHideout (and the others on message 600):
    // Score(100), nobody carries the gold.
    score(carId, kDeliveryPoints);
    m_carrier = -1;
    m_goldActive = false; // until the new set
    m_events.push_back({EventType::GoldDelivered, carId, kDeliveryPoints});
}

void CopsAndRobbers::tickLimits(float dt) {
    // UpdateLimit (the time and point limits) and UpdateTimeWarning. A
    // machine the host tells (limitsFromHost) only counts down and warns.
    if (m_settings.timeLimitSeconds > 0.0f) {
        m_timeLeft = std::max(0.0f, m_timeLeft - dt);
        if (m_timeLeft < 0.1f && !m_settings.limitsFromHost) {
            m_over = true;
            m_events.push_back({EventType::TimeUp});
            return;
        }
    }
    if (!m_settings.limitsFromHost)
        checkLimits();
    if (m_over)
        return;
    if (m_settings.timeLimitSeconds > 0.0f) {
        for (int minutes : kWarningMinutes) {
            const float mark = static_cast<float>(minutes) * 60.0f;
            if (m_timeLeft < mark && mark < m_lastWarning) {
                m_lastWarning = m_timeLeft;
                m_events.push_back({EventType::TimeWarning, -1, minutes});
            }
        }
    }
}

void CopsAndRobbers::limitReached(EventType type, int car, int value) {
    if (m_over || (type != EventType::TimeUp && type != EventType::PointLimit))
        return;
    m_over = true;
    m_events.push_back({type, car, value});
}

std::vector<CopsAndRobbers::Message> CopsAndRobbers::updateNetwork(float dt, int self, bool host,
                                                                    const std::vector<Car>& cars,
                                                                    const std::vector<Impact>& impacts) {
    std::vector<Message> out;
    if (m_over)
        return out;
    const Car* me = nullptr;
    for (const auto& c : cars)
        if (c.id == self)
            me = &c;
    auto locked = [&](int id) {
        for (const auto& [cid, t] : m_lockout)
            if (cid == id && t > 0.0f)
                return true;
        return false;
    };
    auto lock = [&](int id, float seconds) {
        for (auto& [cid, t] : m_lockout)
            if (cid == id) {
                t = std::max(t, seconds);
                return;
            }
        m_lockout.emplace_back(id, seconds);
    };
    for (auto& [cid, t] : m_lockout)
        t -= dt;
    if (!me)
        return out;

    // ImpactCallback (the physics step before this frame): a damaging hit
    // from another player's car whose total reaches 250 knocks the local
    // carrier's gold loose (DropGold, SendGoldDrop, "You dropped the
    // gold!") and locks it out for 2 s (state 7).
    if (m_carrier == self) {
        for (const auto& im : impacts) {
            const int other = im.a == self ? im.b : (im.b == self ? im.a : -1);
            if (other < 0 || other == self || im.impulse < kStealImpulse)
                continue;
            lock(self, kDropLockout);
            drop(self, me->position, false, true);
            out.push_back({Message::Type::GoldDropped, self, m_goldPos});
            break;
        }
    }
    // UpdateGame state 4: a wrecked car sits out 5 s (state 6) and drops
    // the gold where it is; the water or a fall sends it back to its place.
    if (me->wrecked && !locked(self)) {
        lock(self, kWreckPenalty);
        if (m_carrier == self) {
            drop(self, me->position, false);
            out.push_back({Message::Type::GoldDropped, self, m_goldPos});
        }
    }
    if (me->inWater && m_carrier == self) {
        drop(self, me->position, true);
        out.push_back({Message::Type::GoldDropped, self, m_goldPos});
    }

    tickLimits(dt);
    if (m_over)
        return out;

    // UpdateGold: the carried gold rides 2 m above its carrier; the local
    // car takes free gold within 5 m in the gold's room (a host at once,
    // telling the others, and only with another player in the game; a
    // client asks the host and the gold waits).
    if (m_carrier >= 0) {
        for (const auto& c : cars)
            if (c.id == m_carrier)
                m_goldPos = c.position + Vec3{0.0f, 2.0f, 0.0f};
    } else if (m_goldActive && !locked(self) && !me->wrecked &&
               me->position.dist2(m_goldPos) < kGoldRadius * kGoldRadius &&
               (!m_settings.sameRoom || m_settings.sameRoom(m_goldPos, me->position))) {
        if (host) {
            if (cars.size() > 1) {
                take(self);
                out.push_back({Message::Type::GoldTaken, self, m_goldPos});
            }
        } else {
            m_goldActive = false;
            out.push_back({Message::Type::PickupRequest, self, m_goldPos});
        }
    }
    // UpdateBank / UpdateHideout: the local carrier within 12 m of its base,
    // whose room it can reach.
    const Vec3 base = deliveryTarget(teamOf(self));
    if (m_carrier == self && me->position.dist2(base) < kBaseRadius * kBaseRadius &&
        (!m_settings.baseReachable || m_settings.baseReachable(base, me->position))) {
        deliver(self);
        out.push_back({Message::Type::GoldDelivered, self, m_goldPos});
        if (host) {
            newSet(); // GetNewSet, SendChangeSet
            out.push_back({Message::Type::NewSet, -1, m_goldPos, m_set});
        }
    }
    return out;
}

std::vector<CopsAndRobbers::Message> CopsAndRobbers::receive(const Message& m, int from, bool host) {
    std::vector<Message> out;
    switch (m.type) {
    case Message::Type::PickupRequest:
        // 0x25e at the host: granted while nobody carries the gold.
        if (host && m_carrier < 0 && !m_over) {
            take(from);
            out.push_back({Message::Type::GoldTaken, from, m_goldPos});
        }
        break;
    case Message::Type::GoldTaken: take(m.car); break; // 0x25a
    case Message::Type::GoldDropped:
        // 0x259: the gold at the place the carrier sent.
        m_goldPos = m.position;
        m_carrier = -1;
        m_goldActive = true;
        m_events.push_back({EventType::GoldDropped, m.car});
        break;
    case Message::Type::GoldDelivered:
        // 600: the deliverer scores; the host draws the next set.
        deliver(m.car >= 0 ? m.car : from);
        if (host) {
            newSet();
            out.push_back({Message::Type::NewSet, -1, m_goldPos, m_set});
        }
        break;
    case Message::Type::NewSet:
        // 0x261: the host's places.
        m_set = m.set;
        m_goldPos = m_set.gold;
        m_carrier = -1;
        m_goldActive = true;
        m_events.push_back({EventType::NewSet});
        break;
    case Message::Type::Limit:
        limitReached(m.pointLimit ? EventType::PointLimit : EventType::TimeUp, m.car, m.value);
        break;
    }
    return out;
}

// --- Under the host's authority (OpenMM2) ------------------------------------------

bool CopsAndRobbers::locked(int id) const {
    for (const auto& [cid, t] : m_lockout)
        if (cid == id && t > 0.0f)
            return true;
    return false;
}

void CopsAndRobbers::lockOut(int id, float seconds) {
    for (auto& [cid, t] : m_lockout)
        if (cid == id) {
            t = std::max(t, seconds);
            return;
        }
    m_lockout.emplace_back(id, seconds);
}

void CopsAndRobbers::tickLockouts(float dt) {
    for (auto& [cid, t] : m_lockout)
        t -= dt;
}

bool CopsAndRobbers::canTake(const Car& c) const {
    // mmMultiCR::UpdateGold: free gold within 5 m in the car's room, the car
    // not sitting out a wreck (state 6) or a lost gold (state 7).
    return m_carrier < 0 && m_goldActive && !locked(c.id) && !c.wrecked &&
           c.position.dist2(m_goldPos) < kGoldRadius * kGoldRadius &&
           (!m_settings.sameRoom || m_settings.sameRoom(m_goldPos, c.position));
}

std::vector<CopsAndRobbers::Message> CopsAndRobbers::updateHost(float dt, const std::vector<Car>& cars,
                                                                 const std::vector<Impact>& impacts) {
    std::vector<Message> out;
    if (m_over)
        return out;
    tickLockouts(dt);
    // Each car's ImpactCallback (the physics step before this frame),
    // UpdateGame state 4's wreck and the water handlers, as its own machine
    // runs them: a damaging hit of 250 or more from another player's car
    // knocks the carrier's gold loose where it is and locks it out for 2 s;
    // a wrecked car sits out 5 s, dropping the gold where it is; the water
    // or a fall sends the gold back to its place.
    bool dropped = false;
    for (const auto& c : cars) {
        if (m_carrier == c.id) {
            // The carrier's own impacts (its vehCarDamage::ApplyImpact).
            for (const auto& im : impacts) {
                if (im.a != c.id || im.b < 0 || im.b == c.id || im.impulse < kStealImpulse)
                    continue;
                lockOut(c.id, kDropLockout);
                drop(c.id, c.position, false, true);
                out.push_back({Message::Type::GoldDropped, c.id, m_goldPos, {}, true});
                dropped = true;
                break;
            }
        }
        if (c.wrecked && !locked(c.id)) {
            lockOut(c.id, kWreckPenalty);
            if (m_carrier == c.id) {
                drop(c.id, c.position, false);
                out.push_back({Message::Type::GoldDropped, c.id, m_goldPos});
                dropped = true;
            }
        }
        if (c.inWater && m_carrier == c.id) {
            drop(c.id, c.position, true);
            out.push_back({Message::Type::GoldDropped, c.id, m_goldPos});
            dropped = true;
        }
    }
    // UpdateLimit and UpdateTimeWarning (the host's alone), before the
    // frame's pickups and deliveries.
    tickLimits(dt);
    if (m_over) {
        for (auto it = m_events.rbegin(); it != m_events.rend(); ++it)
            if (it->type == EventType::TimeUp || it->type == EventType::PointLimit) {
                Message m{Message::Type::Limit, it->car};
                m.pointLimit = it->type == EventType::PointLimit;
                m.value = it->value;
                out.push_back(m);
                break;
            }
        return out;
    }
    // UpdateGold, then UpdateBank / UpdateHideout, for each car: the gold
    // rides 2 m above its carrier; a car at free gold takes it (the host
    // grants it while nobody carries it, and only with another player in
    // the game); a carrier within 12 m of its base, whose room it can
    // reach, delivers it and the host draws the next places. Nobody takes
    // gold in the frame it was dropped or delivered (the others learn of
    // it by message in MM2).
    for (const auto& c : cars) {
        if (m_carrier == c.id) {
            m_goldPos = c.position + Vec3{0.0f, 2.0f, 0.0f};
        } else if (!dropped && cars.size() > 1 && canTake(c)) {
            take(c.id);
            out.push_back({Message::Type::GoldTaken, c.id, m_goldPos});
        }
        const Vec3 base = deliveryTarget(teamOf(c.id));
        if (m_carrier == c.id && c.position.dist2(base) < kBaseRadius * kBaseRadius &&
            (!m_settings.baseReachable || m_settings.baseReachable(base, c.position))) {
            deliver(c.id);
            out.push_back({Message::Type::GoldDelivered, c.id, m_goldPos});
            newSet();
            out.push_back({Message::Type::NewSet, -1, m_goldPos, m_set});
            dropped = true;
        }
    }
    return out;
}

bool CopsAndRobbers::updatePredicted(float dt, const Car& me, const std::vector<Car>& cars, int players) {
    if (m_over)
        return false;
    tickLockouts(dt);
    if (me.wrecked && !locked(me.id))
        lockOut(me.id, kWreckPenalty);
    tickLimits(dt);
    // UpdateGold: the carried gold rides 2 m above its carrier.
    for (const auto& c : cars)
        if (c.id == m_carrier)
            m_goldPos = c.position + Vec3{0.0f, 2.0f, 0.0f};
    if (m_over || m_pending || players < 2 || !canTake(me))
        return false;
    // Another car at the gold too, or on its way there: the host decides
    // between them (its own car first), so this machine waits for its word
    // as MM2's did. (The nearest point of the car's way in the next
    // kContestSeconds.)
    const float reach = kGoldRadius + kContestMargin;
    for (const auto& c : cars) {
        if (c.id == me.id)
            continue;
        const Vec3 way = c.velocity * kContestSeconds;
        const float len2 = way.dot(way);
        const float along =
            len2 > 0.0f ? std::clamp((m_goldPos - c.position).dot(way) / len2, 0.0f, 1.0f) : 0.0f;
        if ((c.position + way * along).dist2(m_goldPos) < reach * reach)
            return false;
    }
    // UpdateGold on a machine that is not the host: MM2 asks the host
    // (0x25e) and shows the gold taken when the host's 0x25a arrives;
    // OpenMM2 shows it at once and the host's decision confirms it.
    take(me.id);
    m_pending = true;
    return true;
}

void CopsAndRobbers::applyHost(const Message& m, int self) {
    // A pickup this machine predicted, which the host's word shows was not
    // its car's.
    auto undo = [&] {
        if (!m_pending)
            return;
        m_pending = false;
        if (m_carrier == self) {
            m_carrier = -1;
            m_goldActive = true;
            m_goldPos = m_set.gold;
        }
        for (auto& [cid, sc] : m_scores)
            if (cid == self)
                sc = std::max(0, sc - kPickupPoints);
        m_events.push_back({EventType::PickupUndone, self});
    };
    switch (m.type) {
    case Message::Type::PickupRequest: break; // a host's word never asks
    case Message::Type::GoldTaken:
        if (m.car == self && m_pending && m_carrier == self) {
            m_pending = false; // confirmed: shown already
            return;
        }
        undo();
        take(m.car);
        return;
    case Message::Type::GoldDropped:
        if (m.car != self)
            undo();
        else
            m_pending = false;
        m_goldPos = m.position;
        m_carrier = -1;
        m_goldActive = true;
        if (m.car == self && m.knocked)
            lockOut(self, kDropLockout);
        m_events.push_back({EventType::GoldDropped, m.car, m.knocked ? 1 : 0});
        return;
    case Message::Type::GoldDelivered:
        if (m.car != self)
            undo();
        m_pending = false;
        deliver(m.car);
        return;
    case Message::Type::NewSet:
        undo();
        receive(m, 0, false);
        return;
    case Message::Type::Limit:
        undo();
        receive(m, 0, false);
        return;
    }
}

CopsAndRobbers::State CopsAndRobbers::state() const {
    State s;
    s.carrier = m_carrier;
    s.goldActive = m_goldActive;
    s.goldPosition = m_goldPos;
    s.set = m_set;
    for (const auto& [id, sc] : m_scores)
        s.scores.emplace_back(id, sc);
    s.over = m_over;
    return s;
}

void CopsAndRobbers::adopt(const State& s, int self, bool confirmed) {
    if (m_pending) {
        if (s.carrier == self) {
            m_pending = false;
        } else if (confirmed) {
            // The host has run this car well past the pickup and has not
            // granted it.
            m_pending = false;
            m_events.push_back({EventType::PickupUndone, self});
        } else {
            return; // still on its way: the prediction stands
        }
    }
    m_carrier = s.carrier;
    m_goldActive = s.goldActive;
    if (m_carrier < 0)
        m_goldPos = s.goldPosition;
    m_set = s.set;
    for (const auto& [id, sc] : s.scores)
        for (auto& [cid, mine] : m_scores)
            if (cid == id)
                mine = sc;
}

void CopsAndRobbers::score(int id, int points) {
    // mmMultiCR::Score.
    for (auto& [cid, s] : m_scores)
        if (cid == id)
            s += points;
    m_scored = true;
}

void CopsAndRobbers::checkLimits() {
    // mmMultiCR::UpdateLimit (point limit): a player's score in
    // Free-For-All, a team's otherwise.
    if (m_settings.pointLimit <= 0 || !m_scored)
        return;
    if (m_settings.mode == CopsAndRobbersMode::FreeForAll) {
        for (const auto& [id, s] : m_scores) {
            if (s >= m_settings.pointLimit) {
                m_over = true;
                m_events.push_back({EventType::PointLimit, id, s});
                return;
            }
        }
        return;
    }
    for (CrTeam t : {CrTeam::Cop, CrTeam::Robber}) {
        if (score(t) >= m_settings.pointLimit) {
            m_over = true;
            m_events.push_back({EventType::PointLimit, -1, score(t)});
            return;
        }
    }
}

void CopsAndRobbers::update(float dt, const std::vector<Car>& cars, const std::vector<Impact>& impacts) {
    if (m_over)
        return;
    auto find = [&](int id) -> const Car* {
        for (const auto& c : cars)
            if (c.id == id)
                return &c;
        return nullptr;
    };
    auto locked = [&](int id) {
        for (const auto& [cid, t] : m_lockout)
            if (cid == id && t > 0.0f)
                return true;
        return false;
    };
    auto lock = [&](int id, float seconds) {
        for (auto& [cid, t] : m_lockout)
            if (cid == id) {
                t = std::max(t, seconds);
                return;
            }
        m_lockout.emplace_back(id, seconds);
    };
    for (auto& [cid, t] : m_lockout)
        t -= dt;

    // mmMultiCR::ImpactCallback runs during the physics step, before the
    // frame's UpdateGame: a hard enough hit from another car knocks the
    // gold loose; the carrier cannot take it back for 2 s (state 7).
    bool droppedByImpact = false;
    if (const Car* c = m_carrier >= 0 ? find(m_carrier) : nullptr) {
        for (const auto& im : impacts) {
            const int other = im.a == m_carrier ? im.b : (im.b == m_carrier ? im.a : -1);
            if (other < 0 || im.impulse < kStealImpulse || !find(other))
                continue;
            lock(c->id, kDropLockout);
            drop(c->id, c->position, false, true);
            droppedByImpact = true;
            break;
        }
    }

    // UpdateGame state 4: wrecked cars sit out 5 s (state 6); a wrecked
    // carrier drops the gold where it is.
    for (const auto& c : cars)
        if (c.wrecked)
            lock(c.id, kWreckPenalty);

    // Then UpdateLimit (the time running out, the point limit, checked
    // before this frame's deliveries) and UpdateTimeWarning ("N minutes
    // remaining" as each mark is passed).
    if (m_settings.timeLimitSeconds > 0.0f) {
        m_timeLeft = std::max(0.0f, m_timeLeft - dt);
        if (m_timeLeft < 0.1f) {
            m_over = true;
            m_events.push_back({EventType::TimeUp});
            return;
        }
    }
    checkLimits();
    if (m_over)
        return;
    if (m_settings.timeLimitSeconds > 0.0f) {
        for (int minutes : kWarningMinutes) {
            const float mark = static_cast<float>(minutes) * 60.0f;
            if (m_timeLeft < mark && mark < m_lastWarning) {
                m_lastWarning = m_timeLeft;
                m_events.push_back({EventType::TimeWarning, -1, minutes});
            }
        }
    }

    // UpdateGold, then UpdateBank / UpdateHideout.
    if (m_carrier < 0) {
        // mmMultiCR::UpdateGold: within 5 m of the gold picks it up. Each
        // machine tests only its own car and the others learn of a drop from
        // the carrier's SendGoldDrop message, so nobody takes the gold in the
        // frame it was knocked loose (inferred from that message round trip;
        // this all-in-one simulation has no latency of its own).
        for (const auto& c : cars) {
            if (droppedByImpact)
                break;
            if (c.wrecked || locked(c.id))
                continue;
            if (c.position.dist2(m_goldPos) < kGoldRadius * kGoldRadius) {
                m_carrier = c.id;
                score(c.id, kPickupPoints);
                m_events.push_back({EventType::GoldTaken, c.id, kPickupPoints});
                break;
            }
        }
    } else if (const Car* c = find(m_carrier); !c) {
        drop(m_carrier, m_goldPos, true);
    } else if (c->inWater) {
        drop(c->id, c->position, true);
    } else if (c->wrecked) {
        drop(c->id, c->position, false);
    } else {
        // UpdateGold: the carried gold rides 2 m above the carrier.
        m_goldPos = c->position + Vec3{0.0f, 2.0f, 0.0f};
        // mmMultiCR::UpdateBank / UpdateHideout: delivered within 12 m.
        if (c->position.dist2(deliveryTarget(teamOf(c->id))) < kBaseRadius * kBaseRadius) {
            score(c->id, kDeliveryPoints);
            m_events.push_back({EventType::GoldDelivered, c->id, kDeliveryPoints});
            newSet();
        }
    }
}

std::vector<CopsAndRobbers::Event> CopsAndRobbers::takeEvents() {
    std::vector<Event> out;
    out.swap(m_events);
    return out;
}

} // namespace mm2::game::session
