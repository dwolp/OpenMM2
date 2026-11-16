#pragma once

// Race opponent driving a physics car (phys::CarSim with its vehicle's own
// tune: MM2 builds AI cars with vehCar::Init(<car>) like the player's) along
// its .opp route: MM2's aiRouteRacer, an aiVehiclePhysics
// (ai::PhysicsDriver) given its route once (aiRouteRacer::DriveRoute ->
// RegisterRoute):
//   * aiRouteRacer::Init reads the .opp rows: the first is the grid place and
//     heading, the last the destination, and each row between names the
//     intersection of the PSDL room it lies in (the room's first
//     intersection component): those are the waypoints. Circuits drive them
//     `laps` times, other races once;
//   * on its first frame after Reset the racer registers the route and holds
//     the ambient traffic at its first waypoint (aiIntersection::StopSources);
//   * the [Opponent] line of the race's .aimap tunes the driver
//     (aiRaceData; see OpponentSettings::fromData);
//   * held at the start it revs in neutral with the brakes on (undrivable);
//   * a car that falls through the world is disabled (no more driving), a wrecked
//     car is repaired after 5 s in circuits only.
// MM2 has no rubber-banding: nothing scales the opponents by race position.
//
// OpenMM2 also lays the .opp route out as an ai::Course along the roads, to
// measure the racer's progress (race positions, laps) and to put a car that
// has made no progress for a long time back on its route (not in MM2).
//
// Per frame, before the physics step:
//   opponent.setHeld(!session.racersReleased() || !session.opponentActive(i));
//   opponent.update(dt, cars);
// A racer that crosses the finish line drives on to its destination (MM2's
// game only reads aiRouteRacer::Finished, here finished(): the line the race
// gave aiMap::SetWaypoints, see setFinishLine); finish() is for tools and
// tests.
//
// First ported from Open1560 (MM1's aiVehicleOpponent). Open1560 - An Open
// Source Re-Implementation of Midtown Madness 1 Beta, Copyright (C) 2020
// Brick. GPL-3.0-or-later; OpenMM2 port under the same licence.

#include "ai/Course.h"
#include "ai/Driving.h"
#include "city/Mm1Racer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace mm2::phys {
class CarSim;
class GroundQuery;
struct Impact;
} // namespace mm2::phys

namespace mm2::ai {

class MapView;

struct OpponentSettings {
    // aiRouteRacer::DriveRoute's RegisterRoute settings, from the race's
    // .aimap [Opponent] line (see fromData).
    RouteParams route;
    // Circuit laps (0 = point-to-point race).
    int laps = 0;
    // aiVehiclePhysics::Init's repair flag (game mode 3, circuits): a wrecked car
    // is repaired after 5 s; elsewhere it stays wrecked.
    bool repairWhenWrecked = false;
    // OpenMM2: top speed for scripted cars (m/s, 0 = none); not in MM2.
    float speedLimit = 0.0f;
    // OpenMM2 recovery (not in MM2): no progress for this long puts the car
    // back on its line; 0 disables it.
    float resetAfterSeconds = 10.0f;
    // Used instead of CarSim::reset for teleports (e.g. a SimVehicle with a
    // trailer); receives the model matrix.
    std::function<void(const Mat34&)> resetCar;
    // Static level geometry (the phys::World) for the OpenMM2 recovery to
    // avoid walls. Optional.
    const phys::GroundQuery* world = nullptr;

    // The numbers after the car and path file of an [Opponent] line
    // (aiRaceData::aiRaceData, "%s %s %f %d %f %f %d %d %d %d %d %f"):
    //   MaxThrottle, (unused flag), look-ahead distance, brake threshold,
    //   avoid traffic, avoid props, avoid players, avoid opponents,
    //   prefer the sidewalk, corner speed factor
    // e.g. "0.86 0 50.0 0.7 1 1 1 1 0 1.0". Missing numbers keep aiRaceData's
    // defaults (1.0, 0, 50.0, 0.7, 1, 1, 1, 1, 0, 1.0).
    static OpponentSettings fromData(std::span<const float> params, int laps);
};

class Opponent {
public:
    enum class Mode : std::uint8_t {
        Held,      // waiting for the start (or inactive in this lesson)
        Racing,    // aiVehiclePhysics::Forward (or Shortcut)
        BackingUp, // aiVehiclePhysics::Backup
        Wrecked,   // damage over the maximum
        Stopped,   // disabled (fell through the world), or braked to a stop
    };

    // `selfId` is the id this car has in the TrackedCar lists, `racerIndex`
    // its aiRouteRacer index (from 0), `vehicle` the car's base name
    // (aiVehiclePhysics::Init's vehicle type). Without a map the car has no
    // route to drive.
    Opponent(phys::CarSim& car, Course course, const OpponentSettings& settings, int selfId = -1,
             const MapView* map = nullptr, RouteRegistration route = {}, int racerIndex = 0,
             std::string_view vehicle = {});
    ~Opponent();
    Opponent(const Opponent&) = delete;
    Opponent& operator=(const Opponent&) = delete;

    // From the session's OpponentSetup: .opp rows and aimap numbers.
    // `world` is OpponentSettings::world; `vehicle` the car's name
    // (aiVehiclePhysics::Init's type, see goesOverSidewalks).
    static std::unique_ptr<Opponent> create(const MapView& map, phys::CarSim& car,
                                            std::span<const city::OpponentPoint> path, std::span<const float> params,
                                            int laps, int selfId, int racerIndex,
                                            std::string* error = nullptr,
                                            const phys::GroundQuery* world = nullptr,
                                            std::string_view vehicle = {},
                                            std::shared_ptr<city::Mm1RaceWorld> mm1World = nullptr);
    // aiRouteRacer::Init and DriveRoute's RegisterRoute arguments for an .opp
    // route: the waypoints (the first intersection component of each middle
    // row's room; OpenMM2 takes the nearest intersection when the room has
    // none, where MM2 stops with "Point %d is not in a cross road"), the
    // destination (the last row) and the laps (`laps` in circuits, else 1).
    // OpenMM2 Enhanced: MM1's grids stand on the street before the first
    // corner (or in the room of the intersection behind it), where MM2's stand
    // in the first waypoint's own room, so the intersection the grid leads out
    // of is put before the first waypoint (the window then starts on that leg).
    static RouteRegistration routeFromPath(const MapView& map, std::span<const city::OpponentPoint> path,
                                           int laps, bool mm1 = false);

    // Call after placing the car on its grid spot (aiRouteRacer::Reset).
    void reset();
    void setHeld(bool held) { m_held = held; }
    void setSpeedLimit(float metresPerSecond) { m_settings.speedLimit = metresPerSecond; }
    // See OpponentSettings::resetCar (e.g. SimVehicle::reset, for trailers).
    void setResetCar(std::function<void(const Mat34&)> reset) { m_settings.resetCar = std::move(reset); }
    // One AI frame: reads the car, writes its inputs. `cars` may include
    // this car (skipped by id).
    void update(float dt, std::span<const TrackedCar> cars);
    // The race is over for this car: it drives on to its destination and
    // stops there.
    void finish();
    // aiMap::SetWaypoints: the finish line aiRouteRacer::Finished tests, at
    // the race's last checkpoint (mmSingleRace::InitGameObjects) or its
    // first (mmSingleCircuit), `headingDeg` its stored heading: the line's
    // normal is the z axis turned by -heading (Vector3::RotateY). Without
    // one (tools, tests) OpenMM2's own end-of-course test decides.
    void setFinishLine(const Vec3& point, float headingDeg);
    // Collision report for this car (installed on CarSim::onImpactCallback
    // by the constructor, chaining any callback already set).
    void onImpact(const phys::CarImpact& impact);

    Mode mode() const { return m_mode; }
    // aiRouteRacer::Finished as the game keeps it: the racer crossed the
    // race's finish line (setFinishLine); without a line, arrived().
    bool finished() const { return m_finished; }
    // OpenMM2: the racer has driven its route: within 10 m of the end of its
    // course on its last leg, or of its destination past the last waypoint
    // of the last lap. MM2's racers stop at their destination (RegisterRoute
    // with destination speed 0), which some .opp routes put short of the
    // race's finish line, so a racer can arrive without ever finishing.
    bool arrived() const { return m_arrived; }
    bool disabled() const { return m_disabled; }
    int lapsDone() const;
    // Metres driven along the course since the start (laps included).
    float progress() const { return m_progress; }
    float courseDistance() const { return m_s; }
    float side() const { return m_lateral; }
    Vec3 targetPoint() const { return m_driver.target(); }
    const Course& course() const { return m_course; }
    const OpponentSettings& settings() const { return m_settings; }
    const PhysicsDriver& driver() const { return m_driver; }
    const RouteRegistration& route() const { return m_route; }
    // OpenMM2 (tools, tests): drive `route` instead, registered on the next
    // update as after Reset.
    void setRoute(RouteRegistration route) {
        m_route = std::move(route);
        m_registered = false;
    }
    // This car as an obstacle for the other drivers (aiObstacle::CurrentRoadIdx
    // asks its driver; IsTargetBlocked compares the racers' indices).
    void describe(TrackedCar& car) const {
        car.racer = &m_driver;
        car.racerIndex = m_driver.aiId();
    }
    int resets() const { return m_resets; }
    // aiRouteRacer::Update's mover declaration this frame: within 200 m of a
    // player (3, 0x1b), else (2, 0x13) in a race game (2, 0x1b otherwise).
    MoverDeclaration mover() const { return m_mover; }
    int backups() const { return m_driver.backups(); }

private:
    void trackProgress(float dt);
    float remainingDistance() const;
    // aiRouteRacer::Finished.
    bool crossedFinishLine();

    phys::CarSim& m_car;
    Course m_course;
    OpponentSettings m_settings;
    int m_selfId = -1;
    const MapView* m_map = nullptr;
    RouteRegistration m_route;
    PhysicsDriver m_driver;
    // OpenMM2 Enhanced: an MM1 racer's native route following (null: MM2's route driver).
    std::shared_ptr<city::Mm1RaceWorld> m_mm1World;
    std::unique_ptr<city::Mm1RacerAi> m_mm1Racer;
    bool m_registered = false; // aiRouteRacer 0x9784 == 0x9782: the route is registered
    MoverDeclaration m_mover;
    std::function<void(const phys::CarImpact&)> m_prevCallback;

    Mode m_mode = Mode::Held;
    bool m_held = false;
    bool m_wasHeld = false; // undrivable last frame (released: into first gear)
    bool m_finished = false;
    bool m_arrived = false;
    bool m_disabled = false;
    bool m_hasFinishLine = false;
    Vec3 m_finishPoint;            // aiMap +0x1a0
    Vec3 m_finishNormal{0, 0, 1};  // aiMap +0x1ac
    float m_finishSide = 0.0f;     // aiRouteRacer +0x9790 (never reset)
    bool m_touchingPlayer = false;
    const phys::Body* m_playerBody = nullptr;
    float m_s = 0.0f;        // arc length on the course
    float m_lateral = 0.0f;  // current offset from the line (+ right)
    float m_progress = 0.0f; // unwrapped, from the start
    float m_lastLeg = 0.0f;  // course distance from the last waypoint to the destination
    float m_bestProgress = 0.0f;
    float m_noProgressTime = 0.0f;
    int m_resets = 0;
    int m_resetStreak = 0;
    float m_lastResetProgress = -1e9f;
};

} // namespace mm2::ai
