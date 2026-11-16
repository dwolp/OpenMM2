#pragma once

// OpenMM2 Enhanced: Midtown Madness 1's rail geometry, ported from Open1560's
// game.asm (GPL-3.0) so the racers can follow MM1's own lines instead of MM2's
// route nodes. The helpers here are aiPath's small accessors that
// aiRailSet::CalcCopRailPosition (the target point of aiGoalFollowWayPts)
// builds on; the junction curve itself is mm1RailCurve (Mm1AiMap.h,
// CalcXZPosition, a cubic Hermite). See docs/mm1-audio-ai-plan.md.

#include "city/Mm1AiMap.h"

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

} // namespace mm2::city
