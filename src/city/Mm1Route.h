#pragma once

// OpenMM2 Enhanced: Midtown Madness 1's racer route planning, ported from
// Open1560's game.asm (GPL-3.0): aiGoalFollowWayPts's constructor (the race
// path's waypoint intersections and its start / finish links, 126727),
// PlanRoute (128585), LocateWayPtFromRoad (128769), LocateWayPtFromInt (129352),
// DetRdSegBetweenInts (129454) and aiPath::RoadDistance (72870). Paths are
// referred to by their index in Mm1AiMap::paths (the synthetic links are
// appended to the map, with that index as their id). See docs/mm1-audio-ai-plan.md.

#include "city/Mm1AiNav.h"

#include <span>
#include <vector>

namespace mm2::city {

// aiMap::AddIntersection(pos): a new intersection (id = its index) with no roads yet.
int mm1AddIntersection(Mm1AiMap& map, const Vec3& pos);

// aiPath::RoadDistance(pos, &vertex, &roadDist, row, &distToSide) (72870), the rails'
// own position along a road, on lane row `row`'s vertices (0: the innermost lane): the
// first vertex i from 2 with Z[i].(pos - lane[i]) > 0 and |lateral| < path.length + 5,
// lateral = -(X[i].(pos - c[i])) in 3D; roadDist = SubSectionLength(1, i) - dot. Nothing
// found: vertex 2, the others untouched. A path without lane rows (a synthetic link, whose
// lane vertices the original never fills in) uses its centre line (inferred).
Mm1RoadPos mm1RoadDistance(const Mm1AiPath& path, const Vec3& pos, int row = 0);

// aiGoalFollowWayPts::DetRdSegBetweenInts(a, b) (129454): the first road of a's outgoing
// list (sinkPaths: the roads that leave it) that is also in b's incoming list (sourcePaths).
// -1 when there is none.
int mm1DetRdSegBetweenInts(const Mm1AiMap& map, int a, int b);

// What the constructor builds from a race path file's rows (row 0 the grid place, the
// middle rows the waypoints, the last row the finish line).
struct Mm1RaceRoute {
    // WayPtIds[i]: the intersection id of row i; row 0 is left unset (-1), the middle rows
    // are the first type-2 component of the room under the row, the last row a new
    // intersection at the finish.
    std::vector<int> wayPtIds;
    int numWayPts = 0;
    int startLink = -1; // 0x2706: row 0 to row 1 (the first waypoint intersection)
    int lastLink = -1;  // 0x2708: the last waypoint intersection to the finish
    int nLastLink = -1; // the finish on to 1.25 times as far again
};

// The constructor's link building (126727): the start link (its sink the intersection
// MapComponent finds at row 1), each middle row's waypoint intersection, the finish link and
// the one beyond it (each [a, a + 0.2 d, 0.3, 0.7, 0.8, b] on the road's own centre line),
// wired to their intersections. Appends paths and intersections to `map`. Returns an empty
// route (numWayPts 0) when a middle row has no intersection in its room (the asm logs it and
// leaves the id unset; inferred here as a failure) or fewer than 3 rows are given.
// CreateRoadMap (the intersections' sorted direction lists) is not rebuilt for the synthetic
// intersections (inferred: nothing reads them for the links).
Mm1RaceRoute mm1BuildRaceRoute(Mm1AiMap& map, const Mm1AiNav& nav, std::span<const Vec3> rows);

// The racer's route state (aiGoalFollowWayPts's fields and its rail's CurLink / NextLink).
struct Mm1RouteState {
    int wayPtIdx = 1;
    int curLap = 1;
    int laps = 1;       // mmStatePack laps
    int compType = 0;   // CurMapCompType
    int compIdx = -1;   // CurMapCompIdx
    int cur = -1;       // rail->CurLink (a path index)
    int next = -1;      // rail->NextLink
    int vertex = 2;     // CurRdVertIdx
    float roadDist = 0.0f;   // rail->RoadDist
    float distToSide = 0.0f; // DistToSide
    int state = 0;      // State (5: on the finish link)
    bool finished = false; // IsFinished and IsStopped
};

// aiGoalFollowWayPts::LocateWayPtFromRoad(road, &cur, &next) (128769): from a road, where the
// next waypoint lies. Returns false when it is not reachable.
bool mm1LocateWayPtFromRoad(const Mm1AiMap& map, const Mm1RaceRoute& route, Mm1RouteState& st, int roadId,
                            const Vec3& carPos);

// aiGoalFollowWayPts::LocateWayPtFromInt(intersection, &next) (129352): the outgoing road that
// leads to the current waypoint.
bool mm1LocateWayPtFromInt(const Mm1AiMap& map, const Mm1RaceRoute& route, Mm1RouteState& st, int intersectionId);

// aiGoalFollowWayPts::PlanRoute() (128585): advances the waypoint index when the car is in
// the waypoint's intersection (the finish, after the last lap, switches to the finish link and
// stops the racer within 10 m of it), then locates the route from the car's component.
void mm1PlanRoute(const Mm1AiMap& map, const Mm1RaceRoute& route, Mm1RouteState& st, const Vec3& carPos);

} // namespace mm2::city
