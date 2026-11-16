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

} // namespace mm2::city
