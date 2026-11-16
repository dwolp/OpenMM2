#pragma once

// OpenMM2 Enhanced: a Midtown Madness 1 racer's route following on MM1's own
// road network: aiGoalFollowWayPts::Init / Reset / Update (Open1560's
// aiGoalFollowWayPts.cpp and game.asm, GPL-3.0) without the collision
// avoidance (DetectCollision, DetectOpponentCollision and aiStuck are not
// ported yet), so a racer runs its rail through the map's roads and
// intersections to the next waypoint. The caller steers the car at update()'s
// target point and sets its throttle (MaxThrottle). See docs/mm1-audio-ai-plan.md.

#include "city/Mm1AiNav.h"
#include "city/Mm1Route.h"

#include <span>

namespace mm2::city {

// A race's copy of the city's road network (the racers' start and finish links are appended to
// it) and the room lookup MapComponent uses. Not copyable or movable (the nav refers to the map).
struct Mm1RaceWorld {
    Mm1AiMap map;
    Mm1AiNav nav;
    Mm1RaceWorld(const Mm1AiMap& base, std::function<int(const Vec3&)> room) : map(base), nav(map, std::move(room)) {}
    Mm1RaceWorld(const Mm1RaceWorld&) = delete;
    Mm1RaceWorld& operator=(const Mm1RaceWorld&) = delete;
};

class Mm1RacerAi {
public:
    // `map` and `nav` outlive the racer; the racer's links are appended to `map`.
    // `rows` are the race path file's rows (grid place, waypoints, finish line).
    Mm1RacerAi(Mm1AiMap& map, const Mm1AiNav& nav, std::span<const Vec3> rows, int laps, const Mm1RailParams& rail);

    bool valid() const { return m_route.numWayPts > 0; }

    // aiGoalFollowWayPts::Init + Reset: the rail starts on the start link (towards the road
    // between the first two waypoints) and the waypoint index is where that link's end lies.
    void init(const Vec3& pos, const Vec3& carBack, float speed);

    // One Update without the collision avoidance: the car's component, PlanRoute, then
    // SolveTargetPoint (CalcCopRailPosition at RoadDist + TargetPtOffset, DistToSide).
    // `carBack` is the car matrix's third row, `speed` Sim.Speed in m/s.
    // `shift` (m, positive right) moves the aim sideways from the car's own line (clamped to the road),
    // for going round a car in the way.
    Vec3 update(const Vec3& pos, const Vec3& carBack, float speed, float shift = 0.0f);

    // aiGoalFollowWayPts::CalcSpeed (131401): how hard the racer must brake for the next
    // sharp turn, as a fraction of the 23.76 m/s^2 the AI assumes (the caller brakes when it
    // is over 0.7, with Brakes = min(it, 1), and otherwise drives at MaxThrottle). The turn is
    // the one at the end of the current road when its angle is over 0.5 rad, else the one at
    // the end of the next road. A turn's speed is sqrt(23.76 R) (halved onto an alley) for
    // the rail's arc radius R (r0 = max(rightEdge - side * DistToSide [- TurnCenterPtOffset
    // for a right turn], 0.5), R = r0 / (1 - sin((3.14 - |a|) / 2))); the time to the arc's
    // start is its distance over the speed; the demand is (speed - corner) / (23.76 t) when
    // faster than the corner, else 0. Returns 0 with no turn ahead.
    float brakeDemand(float speed) const;

    const Mm1RouteState& state() const { return m_st; }
    float targetPtOffset() const { return m_tpo; }

private:
    Mm1AiMap& m_map;
    const Mm1AiNav& m_nav;
    Mm1RaceRoute m_route;
    Mm1RailParams m_rail;
    Mm1RouteState m_st;
    int m_lastType = -1;
    float m_tpo = 20.0f;

    void component(const Vec3& pos, const Vec3& carBack, float speed, int pathHint);
};

} // namespace mm2::city
