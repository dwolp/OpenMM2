#pragma once

// OpenMM2 Enhanced: Midtown Madness 1's rail geometry, ported from Open1560's
// game.asm (GPL-3.0) so the racers can follow MM1's own lines instead of MM2's
// route nodes. The helpers here are aiPath's small accessors that
// aiRailSet::CalcCopRailPosition (the target point of aiGoalFollowWayPts)
// builds on; the junction curve itself is mm1RailCurve (Mm1AiMap.h,
// CalcXZPosition, a cubic Hermite). See docs/mm1-audio-ai-plan.md.

#include "city/Mm1AiMap.h"

#include <span>

namespace mm2::city {

// aiPath::CenterLength(from, to): the centre line's length between two vertices
// (the difference of the cumulative CenterOffsets).
float mm1CenterLength(const Mm1AiPath& path, int from, int to);

// aiPath::CenterIndex(dist): `dist` (clamped to 0..the length between vertices
// 1 and n-2) as a vertex: the first index from 2 whose offset from vertex 1,
// plus 9.98e-6, reaches it; n-2 when none does.
int mm1CenterIndex(const Mm1AiPath& path, float dist);

// aiPath::SubSectionDir(out, vertex, _, scale): the vertex's unit direction
// times `scale` (the rails' curve tangent).
Vec3 mm1SubSectionDir(const Mm1AiPath& path, int vertex, float scale);

// aiPath::IntersectionEntryVector(out, scale): the way the road enters its sink
// intersection: -ZAxis[n-2] times `scale`.
Vec3 mm1IntersectionEntryVector(const Mm1AiPath& path, float scale);

// aiPath::IntersectionExitVector(out, scale): the way the road leaves its
// source intersection: -ZAxis[2] times `scale`.
Vec3 mm1IntersectionExitVector(const Mm1AiPath& path, float scale);

// aiRailSet::SolveTurnType(cur, next) (110125): the turn from `cur`'s end onto
// `next`, from the angle a = atan2(-(X.v), -(Z.v)) of next's first segment
// v = next.center[2] - next.center[1] in cur's last frame (X = xAxis, Z =
// zAxis at vertex n-2): 0 when a > 0.5, 1 when a < -0.5, else 3 (straight).
// Verified on Chicago's first corner (path 316 east onto 320 north, a left
// turn: a = -pi/2): 1 is a left turn, 0 a right turn, 3 straight on; MM1's
// xAxis points to the left of the direction of travel, as MM2's does.
int mm1SolveTurnType(const Mm1AiPath& cur, const Mm1AiPath& next);

// The turn angle a = atan2(-(X.v), -(Z.v)) of `next`'s first segment in `cur`'s last frame (the
// angle SolveTurnType, CalcRailLength and the AI's corner tests all use; > 0 a right turn, < 0
// a left turn). 0 when either path is too short.
float mm1TurnAngle(const Mm1AiPath& cur, const Mm1AiPath& next);

// aiRailSet::CalcTurnIntersection(out, dir, cur, next) (107302): where the
// turn's two lines meet. dir > 0: `cur`'s curb point at its last-but-one
// vertex moved along its xAxis by `turnCenterPtOffset` (rail +0x40), heading
// -ZAxis, meets the line through next's curb[1] along ZAxis[1]; dir <= 0: the
// left curbs, next.leftCurb[1] moved by -cur.ZAxis[n-2] * offset, along
// -next.ZAxis[1]. out.y is the second line's start point's y. The two lines'
// intersection is P2 + D2 * cross(D1, P2 - P1) / cross(D2, D1) in XZ.
Vec3 mm1CalcTurnIntersection(const Mm1AiPath& cur, const Mm1AiPath& next, int dir, float turnCenterPtOffset);

// The rail's own numbers CalcRailLength and CalcTurnIntersection read:
// aiRailSet LSideDist (+0x8) and RSideDist (+0xC) (the car's half extents, set
// from its body bound) and TurnCenterPtOffset (+0x40).
struct Mm1RailParams {
    float lSide = 0.0f;
    float rSide = 0.0f;
    float turnCenterPtOffset = 0.0f;
};

// aiPath's right edge as the rails read it: LaneLengths[2 * numLanes - 1].
float mm1RightEdge(const Mm1AiPath& path);

// aiRailSet::CalcRailLength (106800): the rail's length over `cur` and onto
// `next`: A = CenterLength(1, n-2); with no next road A; next road within 0.7
// rad of straight: A + dist(next.center[1], cur.center[n-2]); a sharper turn:
// the corner P (CalcTurnIntersection with dir = the turn's sign) gives
// d1 = -Z.(P - cur.center[n-2]) and d2 = next.Z[1].(P - next.center[1]), and the
// length is d1 + A + d2 (the arc's setback q enters as -q + 2q - q and
// cancels, as it does in the asm), with r0 = 0 if (rightEdge - rSide) <= 0 else
// min(rightEdge - rSide, 2 * nextRightEdge - (lSide + rSide)), the arc radius
// r0 / (1 - sin t), t = (3.14 - |a|) / 2, and q = radius * cos t.
float mm1CalcRailLength(const Mm1AiPath& cur, const Mm1AiPath* next, const Mm1RailParams& rail);

// aiRailSet::CalcRailState(dist) (106187): 1 on a road, 2 in the junction or
// turn. With no next road 1. Straight junction (|a| < 0.7): 1 short of A, 2
// while dist - A is below the gap, else 1. A turn: 1 short of the arc's start
// d1 + A - q, 2 within 2q of it, 1 beyond (the asm's closing re-test
// (q + dist3) < q cannot hold there; kept for fidelity).
int mm1CalcRailState(const Mm1AiPath& cur, const Mm1AiPath* next, const Mm1RailParams& rail, float dist);

// aiRailSet::CalcCopRailPosition(out, _, dist, offset) (102568): the rail's
// position `dist` metres along the current road, through the junction and onto
// the next road, `offset` metres to the right of the centre line (positive:
// right; the asm negates it and adds xAxis, which points left). state is the
// asm's return value: 1 on a road, 2 in a junction (and 2 on the first 10 m of a
// road that has a next road).
//  - No next road: a Hermite along the current road (CalcXZPosition, y = 0);
//    past its end, a linear extension of the last segment.
//  - A next road within 0.7 rad of straight: the same Hermite on the current
//    road, then across the junction from the entry point (cur.center[n-2] -
//    X.offset) to the exit point (next.center[1] - X.o', o' the offset held between
//    the car's own lower bound and the next road's width less its side extent and
//    3 m) and onto the next road; a fixed point 10 m past its end when the
//    distance runs beyond it.
//  - A sharper turn (|a| >= 0.7): s = the turn's sign, r0 = 0 if rpre <= 0 else
//    min(rpre, 2 * nextRightEdge - (lSide + rSide)) with rpre = rightEdge(cur) -
//    s * offset (less turnCenterPtOffset when s = +1); theta = (3.14 - |a|) / 2,
//    Rarc = r0 / (1 - sin theta), q = Rarc cos theta; P = CalcTurnIntersection;
//    d1 = -Z.(P - cur.center[n-2]); the arc starts at X = d1 + A - q. Before it the
//    road's Hermite (state 2 while d1 + dist < q); then, for 2q of distance, the
//    arc phi = into / (2q) * |a|: P + q Z - (Rarc - r0) s X + s Rarc cos(phi) X -
//    Rarc sin(phi) Z (X, Z = cur's VertXDir / VertZDir at n-2), state 2; then the
//    next road from its vertex 1 at d'' = into - q - d2 (d2 = Z_next[1].(P -
//    next.center[1])), its offset taken from the car's own position (the second
//    argument): -(X_next[1].(carPos - next.center[1])) clamped to +-(rightEdge(next) -
//    r0), and its last segment's tangents the stored directions.
struct Mm1RailPosition {
    Vec3 pos;
    int state = 0;
};
Mm1RailPosition mm1CalcCopRailPosition(const Mm1AiPath& cur, const Mm1AiPath* next, const Mm1RailParams& rail,
                                       const Vec3& carPos, float dist, float offset);

// aiPath::AddPathVerts(Vector3*, count) (70254): the road MM1 makes from a
// polyline at load (a racer's start and finish links). One lane and one
// sidewalk row (no lane vertices), speed limit 15 m/s (aiPath's constructor),
// LaneWidths[0..1] = 7, LaneLengths[0..1] = (0, 10): a 10 m half width. Normals
// up; zAxis[i] = normalize(c[i-1] - c[i]) for i = 2..n-2 with z[1] = z[2];
// xAxis[i] = z[i] x normal for i = 1..n-2 with x[n-1] = x[n-2]; subSectionDirs[i]
// = normalize(c[i+1] - c[i-1]) for i = 2..n-2; both curbs and the outer edge are
// c[i] - 10 x[i] (leftCurb c[i] + 10 x[i]) inside, c at the two ends; the cumulative
// centre lengths. Entries the asm leaves unset are zero here.
Mm1AiPath mm1PathFromVerts(std::span<const Vec3> verts, int id);

// aiGoalFollowWayPts's constructor (126727): a racer's START LINK, built from
// rows 0 and 1 of its path file: [p0, p0 + 0.2 d, p0 + 0.3 d, p0 + 0.7 d,
// p0 + 0.8 d, p1], d = p1 - p0, as aiPath 0x2706; p1 is the first waypoint
// intersection's centre.
Mm1AiPath mm1StartLink(const Vec3& p0, const Vec3& p1);

// aiPath::CenterDistance(pos, &vertex, &roadDist, &distToSide) (73042): the car's
// place along a road. From vertex 2 on, the first vertex i whose z-axis dot
// (pos - c[i]) is not negative (the car is before it); the lateral offset
// -(x.(pos - c)) in xz (at vertex n-2 for the last vertex); accepted when
// |lateral| < path.length + 5. Then vertex = i, roadDist = CenterLength(1, i) - dot
// and distToSide = lateral. `vertex` is 2 when nothing is found (the asm sets
// it first); the other outputs are only written on success.
struct Mm1RoadPos {
    bool found = false;
    int vertex = 2;
    float roadDist = 0.0f;
    float distToSide = 0.0f;
};
Mm1RoadPos mm1CenterDistance(const Mm1AiPath& path, const Vec3& pos);

// aiMap::DetermineOppRoadPosInfo(pos, carBack, rail, &vertex, ..., id) (64235):
// CenterDistance on path `id`; if Z[vertex].carBack is not positive (the car
// points against the road; carBack is the car matrix's third row) it asks the
// oncoming path instead. Returns the path id used (the oncoming path's own id
// when it did; a path without one is its own oncoming path).
int mm1DetermineOppRoadPosInfo(const Mm1AiMap& map, const Vec3& pos, const Vec3& carBack, int pathId,
                               Mm1RoadPos* out);

// aiMap::PredictIntersectionPath(intersection, matrix) (64411): of the
// intersection's paths that have sidewalks (in its PathIds order), the one whose
// first segment (center[1] -> center[2]) points most along the car's direction of
// travel, -matrix.m2 (+m2 in reverse); strictly greater scores win, starting
// from -1e6. -1 when there is none.
int mm1PredictIntersectionPath(const Mm1AiMap& map, int intersectionId, const Vec3& carBack, bool reverse);

// The sharp-turn part of aiMap::DetermineOppMapComponent's intersection branch
// (asm loc_4378A9): where a car in the intersection is along the rail's turn arc and
// how far ahead its target point should be. a = the turn angle as in
// CalcCopRailPosition (|a| >= 0.7, else valid = false: that is the straight case).
// s = sign(a); rpre = rightEdge(cur) - s * distToSide - rSide; r0 = 0.5 if rpre <= 0.5
// else min(rpre, 2 * rightEdge(next) - rSide) (note: unlike CalcCopRailPosition's
// floor of 0 and its lSide + rSide); theta = (3.14 - |a|) / 2, R = r0 / (1 - sin theta),
// q = R cos theta, P = CalcTurnIntersection(s), d1 = -Z.(P - cur.center[n-2]). The
// circle's centre S = P + q Z - (R - r0) s X; W = carPos - S; phi = atan2(-Z.W, s X.W),
// rho = |W| (in the plane of the dot products, with y). roadDist = (d1 + A - q) +
// 2 phi q / (s a); targetPtOffset = clamp(rho - R + 0.488 v^2 / (23.76 R), 7, 20).
// The constants 0.488 and 23.76 are the asm's; their meaning (a lateral-g style speed
// term) is inferred.
struct Mm1ArcProgress {
    bool valid = false;
    float roadDist = 0.0f;
    float targetPtOffset = 20.0f;
};
Mm1ArcProgress mm1IntersectionArcProgress(const Mm1AiPath& cur, const Mm1AiPath& next, const Mm1RailParams& rail,
                                          const Vec3& carPos, float distToSide, float speed);

// The "on no component" branch of aiMap::DetermineOppMapComponent (asm loc_4388E0):
// the car is projected onto the rail's current path with CenterDistance's search (the
// first vertex from 2 the car is not past) but with no width test; distToSide =
// -(x.d) in xz, clamped to +-max(0, rightEdge(paths[pathId]) - rSide) (0 when pathId
// is negative); roadDist = CenterLength(1, vertex) - dot. Nothing found: vertex 2,
// the other outputs untouched (found = false). The target offset is 20 there.
Mm1RoadPos mm1ProjectOffRoad(const Mm1AiMap& map, const Mm1AiPath& cur, const Vec3& pos, int pathId, float rSide);

} // namespace mm2::city
