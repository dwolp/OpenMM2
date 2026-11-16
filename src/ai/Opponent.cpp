// Race opponents: MM2's aiRouteRacer (build 3393) driving an ai::PhysicsDriver
// (aiVehiclePhysics). See Opponent.h and docs/ai.md for what is ported and
// what is inferred.
// Also: aiRouteRacer::Disabled (update, below y -200).
#include "ai/Opponent.h"

#include "ai/MapView.h"
#include "ai/Traffic.h"
#include "core/Libm.h"
#include "phys/vehicle/CarSim.h"

#include <algorithm>
#include <cmath>

namespace mm2::ai {
namespace {

// OpenMM2: the race is over for a car that comes within 10 m of its
// destination on its final approach, when the game has not said so first
// (MM2 leaves finishing to the game's finish line).
constexpr float kFinishRadius = 10.0f;
// aiRouteRacer::DriveRoute: "Opponent %d has fallen through the world".
constexpr float kFallenThroughWorld = -200.0f;

float wrapAhead(const Course& c, float from, float to) {
    float d = to - from;
    if (c.loop() && c.length() > 0.0f) {
        d = std::fmod(d, c.length());
        if (d < 0.0f)
            d += c.length();
    }
    return d;
}

} // namespace

OpponentSettings OpponentSettings::fromData(std::span<const float> params, int laps) {
    // aiRaceData::aiRaceData's [Opponent] record and its defaults, passed to
    // RegisterRoute by aiRouteRacer::DriveRoute.
    auto at = [&](std::size_t i, float def) { return i < params.size() ? params[i] : def; };
    OpponentSettings s;
    s.route.maxThrottle = clampf(at(0, 1.0f), 0.0f, 1.0f);
    // params[1]: RegisterRoute's first flag; stored, never read.
    s.route.lookAhead = std::max(at(2, 50.0f), 1.0f);
    s.route.brakeThreshold = at(3, 0.7f);
    s.route.avoidTraffic = at(4, 1.0f) != 0.0f;
    s.route.avoidProps = at(5, 1.0f) != 0.0f;
    s.route.avoidPlayers = at(6, 1.0f) != 0.0f;
    s.route.avoidOpponents = at(7, 1.0f) != 0.0f;
    s.route.preferSidewalk = at(8, 0.0f) != 0.0f;
    s.route.cornerSpeedFactor = at(9, 1.0f);
    s.route.destinationSpeed = 0.0f;
    s.route.stopShort = 0.0f;
    s.laps = std::max(laps, 0);
    s.repairWhenWrecked = laps > 0;
    return s;
}

Opponent::Opponent(phys::CarSim& car, Course course, const OpponentSettings& settings, int selfId,
                   const MapView* map, RouteRegistration route, int racerIndex, std::string_view vehicle)
    : m_car(car), m_course(std::move(course)), m_settings(settings), m_selfId(selfId), m_map(map),
      m_route(std::move(route)), m_driver(car, selfId, map, racerIndex, vehicle) {
    m_prevCallback = car.onImpactCallback;
    car.onImpactCallback = [this](const phys::CarImpact& impact) { onImpact(impact); };
    configureAiVehStuck(car, 0.5f);
    // OpenMM2 Enhanced, a racer on MM1's physics: MM2's route search steers and
    // brakes round other racers; MM1's aiGoalFollowWayPts::DetectOpponentCollision
    // only shifts the aim sideways (AvoidOpponentCollision) and never brakes,
    // so it is left off until that is ported (reset() reuses m_settings).
    if (car.isMm1())
        m_settings.route.avoidOpponents = false;
    m_driver.params = m_settings.route;

    // The course's last leg: from its last waypoint to the end.
    const float start = m_course.startDistance();
    const float finish = m_course.finishDistance();
    m_lastLeg = m_course.loop() ? m_course.length() : std::max(0.0f, finish - start);
    for (const CourseLeg& leg : m_course.legs()) {
        const float toFinish = m_course.loop() ? wrapAhead(m_course, leg.end, finish) : finish - leg.end;
        if (toFinish >= 0.0f)
            m_lastLeg = std::min(m_lastLeg, toFinish);
    }
    reset();
}

Opponent::~Opponent() {
    m_car.onImpactCallback = m_prevCallback;
    m_car.params.carFrictionHandling = 1.0f;
}

std::unique_ptr<Opponent> Opponent::create(const MapView& map, phys::CarSim& car,
                                           std::span<const city::OpponentPoint> path, std::span<const float> params,
                                           int laps, int selfId, int racerIndex, std::string* error,
                                           const phys::GroundQuery* world, std::string_view vehicle,
                                           std::shared_ptr<city::Mm1RaceWorld> mm1World) {
    // The course (progress and recovery) through the same waypoints as the
    // driver.
    RouteRegistration route = routeFromPath(map, path, laps, car.isMm1());
    auto course = Course::fromOpponentPath(map.net(), path, laps > 0, error, route.wayPoints);
    if (!course)
        return nullptr;
    OpponentSettings s = OpponentSettings::fromData(params, laps);
    s.world = world;
    auto opp = std::make_unique<Opponent>(car, std::move(*course), s, selfId, &map, std::move(route), racerIndex,
                                          vehicle);
    if (car.isMm1() && mm1World && path.size() >= 3) {
        // aiGoalFollowWayPts on MM1's own roads; the rail's side extents are the car's half width
        // (the asm takes them from the body bound; inferred 1 m) and TurnCenterPtOffset is 0
        // (set by DetectCollision / AvoidCollision, not ported yet).
        std::vector<Vec3> rows;
        for (const city::OpponentPoint& p : path)
            rows.push_back(p.position);
        opp->m_mm1World = std::move(mm1World);
        opp->m_mm1Racer = std::make_unique<city::Mm1RacerAi>(opp->m_mm1World->map, opp->m_mm1World->nav, rows,
                                                             laps > 0 ? laps : 1, city::Mm1RailParams{1.0f, 1.0f, 0.0f});
        if (opp->m_mm1Racer->valid())
            opp->m_driver.setMm1Racer(opp->m_mm1Racer.get());
        else
            opp->m_mm1Racer.reset();
    }
    return opp;
}

RouteRegistration Opponent::routeFromPath(const MapView& map, std::span<const city::OpponentPoint> path,
                                          int laps, bool mm1) {
    // aiRouteRacer::Init: the rows between the first (the grid place) and
    // the last (the destination) are waypoints, each the first intersection
    // listed for the room its point lies in (lvlLevel::FindRoomId).
    RouteRegistration r;
    // aiRouteRacer::DriveRoute: the race's laps in circuits (game mode 3),
    // else 1; no heading wanted at the destination.
    r.laps = laps > 0 ? laps : 1;
    if (path.empty())
        return r;
    r.destination = path.back().position;
    // OpenMM2 Enhanced: MM2's route window starts on the leg after waypoint 0,
    // because MM2's grid stands in that waypoint's intersection room. MM1's
    // grids stand on the street before the first corner (circuit 0: the start
    // row is 150 m up the road from its first waypoint), so the window would
    // skip that leg and aim straight across the block at the road beyond it.
    // A start on a road (no intersection in its room) therefore begins with
    // the end of that road behind the car, the one farther from the next row.
    int startNode = -1; // the intersection the grid place leads out of
    if (mm1 && path.size() >= 3) {
        const Vec3& p0 = path[0].position;
        bool inIntersection = false;
        for (const RoomComponent& c : map.components(map.findRoom(p0, 0)))
            if (c.type == kIntersectionComponent) {
                if (!inIntersection)
                    startNode = c.id; // an MM1 grid in an intersection's room (below)
                inIntersection = true;
            }
        const RoadSpot spot = inIntersection ? RoadSpot{} : locateOnRoads(map.net(), p0);
        if (!inIntersection && spot.path >= 0 && spot.intersection < 0 && spot.distance < 30.0f) {
            const PathInfo& road = map.net().paths()[static_cast<std::size_t>(spot.path)];
            const Vec3& next = path[1].position;
            int behind = -1;
            float farthest = -1.0f;
            for (int end : road.intersection) {
                if (end < 0)
                    continue;
                const Vec3& c = map.net().intersections()[static_cast<std::size_t>(end)].centre;
                const float d = (c.x - next.x) * (c.x - next.x) + (c.z - next.z) * (c.z - next.z);
                if (d > farthest) {
                    farthest = d;
                    behind = end;
                }
            }
            startNode = behind;
        }
    }
    for (std::size_t i = 1; i + 1 < path.size(); ++i) {
        const Vec3& p = path[i].position;
        int node = -1;
        for (const RoomComponent& c : map.components(map.findRoom(p, 0))) {
            if (c.type == kIntersectionComponent) {
                node = c.id;
                break;
            }
        }
        if (node < 0) {
            // OpenMM2 (MM2 stops the game here: its rows all lie in an
            // intersection's room): a row on a road (MM1's lines, OpenMM2
            // Enhanced, are points along roads) takes the end of its road
            // nearer the next row, the way the line goes; the nearest
            // intersection could lie behind and turn the car back.
            const RoadSpot spot = locateOnRoads(map.net(), p);
            const Vec3& next = path[i + 1].position;
            if (spot.path >= 0 && spot.intersection < 0 && spot.distance < 30.0f) {
                const PathInfo& road = map.net().paths()[static_cast<std::size_t>(spot.path)];
                int best = -1;
                float bestD = 0.0f;
                for (int end : road.intersection) {
                    if (end < 0)
                        continue;
                    const Vec3& c = map.net().intersections()[static_cast<std::size_t>(end)].centre;
                    const float d = (c.x - next.x) * (c.x - next.x) + (c.z - next.z) * (c.z - next.z);
                    if (best < 0 || d < bestD) {
                        best = end;
                        bestD = d;
                    }
                }
                node = best;
            }
            if (node < 0)
                node = nearestIntersection(map.net(), p);
        }
        r.wayPoints.push_back(node);
    }
    // The start leg, unless the first waypoint already is where the grid leads out.
    if (startNode >= 0 && (r.wayPoints.empty() || r.wayPoints.front() != startNode))
        r.wayPoints.insert(r.wayPoints.begin(), startNode);
    return r;
}

void Opponent::reset() {
    // aiRouteRacer::Reset / aiVehiclePhysics::Reset.
    m_mode = m_held ? Mode::Held : Mode::Racing;
    m_finished = false;
    m_arrived = false;
    m_disabled = false;
    m_touchingPlayer = false;
    m_registered = false; // DriveRoute registers the route again
    m_driver.reset();
    m_driver.params = m_settings.route;
    if (m_car.trans.getCurrentGear() == -1)
        m_car.trans.setDrive();
    m_car.setInputs(0.0f, 0.0f, 0.0f, 0.0f);
    const Vec3 pos = m_car.body.ics.matrix.m3;
    float dist = 0.0f;
    m_s = m_course.locate(pos, m_course.startDistance(), 40.0f, &m_lateral, &dist);
    if (dist > 20.0f) // not near the start: anywhere along the line
        m_s = m_course.locate(pos, &m_lateral);
    float d = m_s - m_course.startDistance();
    if (m_course.loop()) {
        const float len = m_course.length();
        if (d > 0.5f * len)
            d -= len;
        else if (d < -0.5f * len)
            d += len;
    }
    m_progress = d;
    m_bestProgress = m_progress;
    m_noProgressTime = 0.0f;
}

void Opponent::finish() {
    m_finished = true;
}

void Opponent::setFinishLine(const Vec3& point, float headingDeg) {
    // aiMap::SetWaypoints: the point, and the z axis (0, 0, 1) turned by
    // RotateY(heading x -0.017453292).
    const float a = headingDeg * -0.017453292f;
    const float s = libm::sin(a);
    const float c = libm::cos(a);
    m_finishPoint = point;
    m_finishNormal = {s * 1.0f + c * 0.0f, 0.0f, c * 1.0f - s * 0.0f};
    m_hasFinishLine = true;
}

bool Opponent::crossedFinishLine() {
    // aiRouteRacer::Finished: within 30 m (XZ) of the line's point, on the
    // last lap, aiming at the last waypoint or past it. Between 20 and 30 m
    // it keeps which side of the line the car is on; within 20 m the car
    // has finished once its front bumper (+ 1 m) is across.
    const Vec3& p = m_car.body.ics.matrix.m3; // vehCarSim +0x90
    const float dx = p.x - m_finishPoint.x;
    const float dz = p.z - m_finishPoint.z;
    const float d2 = dz * dz + dx * dx;
    const PhysicsDriver& d = m_driver;
    if (!(d2 < 900.0f) || d.lap() != d.numLaps() || !(d.numWayPoints() - 1 <= d.wayPointIndex()))
        return false;
    auto sign = [](float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); };
    if (400.0f <= d2) {
        m_finishSide = dx * m_finishNormal.x + m_finishNormal.z * dz;
        return false;
    }
    const float before = sign(m_finishSide);
    const float along = dx * m_finishNormal.x + m_finishNormal.z * dz;
    const float beyond = along - (d.frontBumper() + 1.0f) * before;
    return sign(beyond) != before;
}

void Opponent::onImpact(const phys::CarImpact& impact) {
    if (m_prevCallback)
        m_prevCallback(impact);
    if (impact.otherBody && impact.otherBody == m_playerBody)
        m_touchingPlayer = true;
}

int Opponent::lapsDone() const {
    if (!m_course.loop() || m_course.length() <= 0.0f)
        return 0;
    // Reaching the finish (within kFinishRadius) completes the last lap.
    if (m_settings.laps > 0 && m_course.raceDistance(m_settings.laps) - m_progress <= kFinishRadius)
        return m_settings.laps;
    // Laps end at the finish line, which can lie a few metres behind or
    // ahead of the car's grid place.
    const float lineOffset = m_course.raceDistance(1) - m_course.length();
    return std::max(0, static_cast<int>(std::floor((m_progress - lineOffset) / m_course.length())));
}

float Opponent::remainingDistance() const {
    if (m_course.loop() && m_settings.laps <= 0)
        return 1e9f;
    return m_course.raceDistance(m_settings.laps) - m_progress;
}

void Opponent::trackProgress(float dt) {
    const Vec3 pos = m_car.body.ics.matrix.m3;
    // Searched ahead only while the car drives forward (10 m back too while
    // it backs up or stands), so that where the route turns back on itself
    // (a shortcut road leaving an intersection beside the road that came
    // in) the car is not placed on the way it came.
    const float ahead = 30.0f + m_car.speed() * dt * 2.0f;
    const bool forward = m_driver.state() != PhysicsDriver::State::Backup && m_car.speed() > 1.0f;
    const float behind = forward ? 0.0f : 10.0f;
    const float s = m_course.locate(pos, m_s + 0.5f * (ahead - behind), 0.5f * (ahead + behind), &m_lateral);
    float delta = s - m_s;
    if (m_course.loop()) {
        const float len = m_course.length();
        if (delta > 0.5f * len)
            delta -= len;
        else if (delta < -0.5f * len)
            delta += len;
    }
    m_progress += delta;
    m_s = s;
}

void Opponent::update(float dt, std::span<const TrackedCar> cars) {
    for (const TrackedCar& c : cars)
        if (c.isPlayer)
            m_playerBody = c.body;
    // aiRouteRacer::Update declares the racer every frame, held, driving or
    // disabled (the test is made after driving; the positions barely move).
    float near2 = 0.0f;
    if (nearestPlayer2(m_car.body.ics.matrix.m3, cars, near2) && near2 < 40000.0f)
        m_mover = {3, 0x1b};
    else
        m_mover = {2, kRaceGameMovers ? 0x13u : 0x1bu};
    trackProgress(dt);
    // dgPhysManager::CollideInstances marks what the player's car hits
    // (lvlInstance flag 0x8000, cleared each frame).
    const bool touching = m_touchingPlayer || m_car.body.hitByPlayer;
    m_touchingPlayer = false;

    if (!m_registered && !m_disabled && m_map) {
        // aiRouteRacer::DriveRoute, the first frame after Reset (held at the
        // start or not): RegisterRoute, then the ambient traffic is held at
        // the first waypoint (aiIntersection::StopSources).
        m_driver.registerRoute(m_route);
        if (Traffic* traffic = m_map->traffic(); traffic && !m_route.wayPoints.empty())
            traffic->stopSources(m_route.wayPoints.front(), true);
    }
    m_registered = true;

    if (m_held && !m_finished) {
        // mmGameSingle::DisableRacers makes the car undrivable
        // (vehCar::SetDrivable(0, 1)): aiVehiclePhysics::Forward only revs it
        // (with its front-left wheel on the ground: throttle 1, no brakes,
        // steering 0) and vehCar::PreUpdate holds it with the brakes on and
        // the gearbox in neutral. DisableRacers also turns the car's damage
        // off (impacts are not recorded) until EnableRacers.
        m_mode = Mode::Held;
        if (!m_wasHeld) {
            m_car.setDrivable(false, 1);
            m_car.damage.enabled = false;
        }
        if (m_car.wheels[0].hit)
            m_car.setInputs(1.0f, 0.0f, 0.0f, m_car.handBrake);
        m_car.preUpdate();
        m_wasHeld = true;
        m_noProgressTime = 0.0f;
        return;
    }
    if (m_wasHeld) {
        // mmGameSingle::EnableRacers: vehCar::SetDrivable(1, 1) puts the
        // gearbox in first (vehTransmission::SetForward), and the opponents'
        // damage is on again.
        m_wasHeld = false;
        m_car.setDrivable(true, 1);
        m_car.damage.enabled = true;
    }

    // Fallen through the world (aiRouteRacer::DriveRoute): disabled. From then
    // on aiRouteRacer::Update only calls Disabled, which sets the Stop state
    // without driving: the car keeps its last inputs.
    if (m_disabled) {
        m_mode = Mode::Stopped;
        return;
    }
    const Vec3 pos = m_car.body.ics.matrix.m3;

    // OpenMM2 recovery (not in MM2): no progress for a long time, or fallen
    // out of the city, puts the car back on its line.
    // (Not once the car has arrived: MM2's racer simply stands at its
    // destination, finished or not.)
    if (!m_finished && !m_arrived && !m_disabled && !m_driver.wrecked()) {
        if (m_progress > m_bestProgress + 5.0f) {
            m_bestProgress = m_progress;
            m_noProgressTime = 0.0f;
        } else {
            m_noProgressTime += dt;
        }
        const float lineY = m_course.pointAt(m_s).y;
        const bool fell = pos.y < lineY - 15.0f;
        if (fell || (m_settings.resetAfterSeconds > 0.0f && m_noProgressTime > m_settings.resetAfterSeconds)) {
            // Stuck (or fallen) at the same place again: put it further along
            // each time.
            m_resetStreak = std::abs(m_progress - m_lastResetProgress) < 15.0f ? m_resetStreak + 1 : 0;
            m_lastResetProgress = m_progress;
            float ahead = std::min(15.0f * static_cast<float>(m_resetStreak), 60.0f);
            if (!m_course.loop())
                ahead = std::max(0.0f, std::min(ahead, m_course.finishDistance() - m_s - 5.0f));
            placeOnCourse(m_car, m_course, m_s + ahead, m_lateral, cars, m_selfId, m_settings.resetCar,
                          m_settings.world);
            m_s = m_course.wrap(m_s + ahead);
            m_progress += ahead;
            // The route registered again where the car now is, from the
            // waypoint and lap it had reached.
            const int wayPt = m_driver.wayPointIndex();
            const int lap = m_driver.lap();
            m_driver.reset();
            if (m_map) {
                m_driver.registerRoute(m_route);
                m_driver.resumeRoute(wayPt, lap);
            }
            m_noProgressTime = 0.0f;
            m_bestProgress = m_progress;
            ++m_resets;
            return;
        }
    }

    const float remaining = remainingDistance();
    DriveContext ctx;
    ctx.repairWhenWrecked = m_settings.repairWhenWrecked;
    ctx.touchingPlayer = touching;

    // OpenMM2: the race is over for a car that comes within kFinishRadius of
    // the end of its course on its last leg, or of its destination once its
    // driver has passed the last waypoint of the last lap (as
    // aiRouteRacer::Finished asks, the game's finish line aside).
    const PhysicsDriver& d = m_driver;
    const bool lastLeg = m_map && d.wayPointIndex() >= d.numWayPoints() && d.lap() >= d.numLaps();
    const Vec3 toDest = m_route.destination - m_car.body.ics.matrix.m3;
    const bool atDestination = toDest.x * toDest.x + toDest.z * toDest.z <= kFinishRadius * kFinishRadius;
    const bool courseEnd = remaining <= m_lastLeg + 0.5f && remaining <= kFinishRadius;
    if (courseEnd || (lastLeg && atDestination))
        m_arrived = true;
    if (m_hasFinishLine) {
        // The game asks aiRouteRacer::Finished every frame (mmSingleRace /
        // mmSingleCircuit::UpdateOpponentStatus) and keeps the answer.
        if (crossedFinishLine())
            finish();
    } else if (!m_finished && m_arrived) {
        finish();
    }
    // The race being over changes nothing in MM2 (aiRouteRacer::Finished is
    // only asked by the game): the car drives on to its destination, where
    // CalcRoadSpeed holds it with the brakes.
    m_driver.driveRoute(dt, cars, ctx);
    // aiRouteRacer::DriveRoute: below y = -200 the car has fallen through the
    // world and is disabled from the next frame.
    if (m_car.body.ics.matrix.m3.y < kFallenThroughWorld)
        m_disabled = true;

    if (m_settings.speedLimit > 0.0f && m_driver.state() == PhysicsDriver::State::Forward) {
        // OpenMM2 hook for scripted cars (tests): hold a top speed.
        const float speed = m_car.speed();
        if (speed > m_settings.speedLimit - 1.0f) {
            const float throttle =
                std::min(m_car.engine.throttle, speed > m_settings.speedLimit ? 0.0f : 0.3f);
            const float brakes =
                speed > m_settings.speedLimit + 2.0f ? std::max(m_car.brakes, 0.3f) : m_car.brakes;
            m_car.setInputs(throttle, brakes, m_car.steering, m_car.handBrake);
        }
    }

    if (m_driver.wrecked())
        m_mode = Mode::Wrecked;
    else if (m_driver.state() == PhysicsDriver::State::Backup)
        m_mode = Mode::BackingUp;
    else if (m_driver.state() == PhysicsDriver::State::Stop)
        m_mode = Mode::Stopped;
    else
        m_mode = Mode::Racing;
}

} // namespace mm2::ai
