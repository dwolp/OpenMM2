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
#include "city/Race.h"
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
    // StopLightPos[0..1]: the light's pole and a point it lines up with
    // (aiTrafficLightInstance::Init: X along pos[1] - pos[0], at pos[0]).
    std::array<Vec3, 2> stopLightPos{};
    // aiPath::HasBridge (set at load by mmBridgeSet::Init; not in the file):
    // MM1's ambient traffic and pedestrians never take the path. See
    // markMm1BridgePaths.
    bool hasBridge = false;
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
// edges as MM2's do. Entry rules from MM1's intersectionType (1 a light, 0
// a stop sign), each light's pole and axis points and model kept; divided
// and alley flags kept; MM1's fast roads (over 20 m/s, Lake Shore Drive)
// marked freeways (inferred); always blocked sides closed to ambient
// traffic. Intersection rooms are left 0 (loadCity
// sets them from the footprints, Mm1CellLocator).
AiMap mm1ToAiMap(const Mm1AiMap& mm1);

// mmBridgeSet::Init flags the paths its leaf's portal cell lists (asRenderWeb's,
// unported). Inferred stand-in: the paths whose centre line passes over a
// leaf's deck, given as each leaf's frame and its bound's box in that frame
// (1 m of slack around it). Returns how many paths it flagged. The
// conversion closes both sides of a flagged road to ambient traffic and
// pedestrians (MM2's road types 1 and 2).
struct Mm1LeafDeck {
    Mat34 frame;
    Aabb box;
};
int markMm1BridgePaths(Mm1AiMap& mm1, std::span<const Mm1LeafDeck> decks);

// An MM1 .aimap (roam.aimap or a race's; MM2's format) adapted to MM2's
// AI. Inferred, both:
//   * [Speed Limit] is in miles per hour (MM1's 30 where its roads' own
//     limits are 15 m/s and MM2's cities give 15): converted to m/s.
//   * An [Opponent]'s one number, MM1's OpponentRaceData::MaxThrottle
//     (0.30-0.95 across Chicago's races), is relative to MM1's racers, which
//     drive with Realism 0 (aiGoalFollowWayPts; unported); as MM2's
//     MaxThrottle the low ones crawl. MM1's range maps linearly onto MM2's
//     racers' (London's 0.86-1.0): 0.30 -> 0.85, 1.0 -> 1.0, keeping every
//     car's place in the order. Lines without one keep MM2's default, 1.0.
void adaptMm1AiMapConfig(AiMapConfig& config);

} // namespace mm2::city
