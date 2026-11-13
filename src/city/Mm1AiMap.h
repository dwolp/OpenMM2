#pragma once

// Midtown Madness 1's AI road network (city/<city>/<city>.bai) and its
// conversion to MM2's (city::AiMap), so MM2's traffic, pedestrians and
// police can drive an MM1 city. MM2 never reads this; OpenMM2 reads it for
// the Enhanced fork. See docs/formats/mm1city.md.
//
// Layout after Open1560's aiMap / aiPath / aiIntersection::ReadBinary
// (game.asm, GPL-3.0) and the class layouts in mmai/aiPath.h and
// aiIntersection.h. Verified: Chicago's file parses to its last byte.
//
// MM1's paths run one way and come in pairs (OncomingPath); MM2's carry both
// directions as a path's left and right sides. Each MM1 lane, sidewalk and
// boundary row runs from the path's source intersection to its sink, its
// first and last points at those intersections' centres.

#include "city/AiMap.h"
#include "core/Math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm2::city {

struct Mm1AiPath {
    int id = 0;
    int vertexCount = 0;
    int numLanes = 0, numSidewalks = 0;
    int stopLightIndex = 0;
    int intersectionType = 0;
    bool alwaysBlocked = false, pedBlocked = false, divided = false, flat = false, bridge = false, alley = false;
    float length = 0, speedLimit = 0;
    std::string stopLight; // the stop light's model: tplttrafc, tplttrafcdual, tpsstop (a stop sign)
    int oncoming = -1;     // the path the other way
    int edgeIndex = 0, pathIndex = 0;
    int sink = 0, source = 0; // intersections at the last and first vertices
    // (numLanes + numSidewalks) rows of vertexCount points, lanes first
    // (the innermost lane first), then the sidewalk.
    std::vector<std::vector<Vec3>> rows;
    std::vector<Vec3> center, xAxis, normal, zAxis;
    std::vector<Vec3> edge, curb; // the right side's outer edge and curb (Boundaries' two rows)
    std::vector<Vec3> leftCurb;   // LBoundaries: the far side's curb
    std::array<float, 5> laneWidths{}; // each row's offset from the centre line
};

struct Mm1AiIntersection {
    int id = 0;
    Vec3 position;
    std::vector<int> sinkPaths, sourcePaths;
    // Every path at the intersection, sorted by the road's direction from it.
    std::vector<int> paths;
    std::vector<float> directions;
};

struct Mm1AiMap {
    std::vector<Mm1AiPath> paths;
    std::vector<Mm1AiIntersection> intersections;
    // Two path lists per cell (MM1 path ids), as MM2's two room lists.
    std::vector<std::vector<int>> cellPathsNear, cellPathsIn;
};

std::optional<Mm1AiMap> parseMm1Bai(std::span<const std::byte> data, std::string* error = nullptr);

// MM2's road network from MM1's: a path per oncoming pair (the lower id
// first; its lanes are MM2's right side, its partner's reversed the left),
// the rows' collapsed end points dropped so lanes end at the intersections'
// edges as MM2's do. Entry rules from the stop light model (tpsstop a stop
// sign, else a light); divided and alley flags kept; MM1's fast roads (over
// 20 m/s, Lake Shore Drive) marked freeways (inferred); always blocked
// sides closed to ambient traffic. Intersection rooms are left 0.
AiMap mm1ToAiMap(const Mm1AiMap& mm1);

} // namespace mm2::city
