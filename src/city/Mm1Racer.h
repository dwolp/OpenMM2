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
    Vec3 update(const Vec3& pos, const Vec3& carBack, float speed);

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
