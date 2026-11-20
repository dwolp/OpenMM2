#pragma once

// Runtime road network for ambient traffic and pedestrians, built from the
// city's AI map (city/<map>.bai, see docs/formats/bai.md and docs/ai.md).
//
// Each .bai path is a road between two intersections with lanes on both
// sides. MM2 (aiPath) calls the two sides by their travel direction:
// direction +1 rides the second side of the file ("right"), with increasing
// section index, and arrives at ends[0] (the intersection at center.back());
// direction -1 rides the first side ("left"), whose lane points are stored in
// their own travel order, and arrives at ends[1]. Lane 0 of a side is the
// leftmost lane in its direction of travel.
//
// Cities whose city/<map>.aimap sets "[Ambients Drive On The Left]" (London)
// get every two-way road reversed as MM2's aiPath::ReverseDirection does:
// direction +1 takes the other side's lane lines, reversed and in reverse lane
// order, and vice versa, so traffic keeps to the left and lane 0 is still the
// leftmost lane. One-way roads (no lanes on the first side) are unchanged.

#include "ai/PathGeometry.h"
#include "city/AiMap.h"
#include "city/Race.h"
#include "core/Math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mm2::ai {

// Arrival rule at a path end (the path end's "vehicleRule", MM2's
// aiPath intersection type): 0 stop sign (four-way stop), 1 traffic light,
// 3 no control.
enum class EntryRule : std::uint8_t { StopSign = 0, TrafficLight = 1, Uncontrolled = 3 };

// A polyline with cumulative arc lengths.
struct Polyline {
    std::vector<Vec3> points;
    std::vector<float> distances; // distances[0] = 0
    float length = 0.0f;

    void finalize();
    // Point and unit direction at arc length `s` (clamped to the ends).
    Vec3 pointAt(float s, Vec3* direction = nullptr) const;
    // Arc length of the point on the polyline closest to `p` in XZ, and the
    // XZ distance to it.
    float project(const Vec3& p, float* distance = nullptr) const;
};

// A lane in its direction of travel.
struct Lane {
    int id = 0;
    int path = 0;  // index into RoadNetwork::paths()
    int side = 0;  // 0 first side of the file (direction -1), 1 second side (direction +1)
    int dir = 1;   // MM2 direction: +1 or -1
    int index = 0; // MM2 lane number: 0 is the leftmost lane in the direction of travel
    int count = 1; // lanes on this side
    bool ambient = true; // side open to ambient traffic (side flag bit 0 clear)
    Polyline line;
    int fromIntersection = -1;
    int toIntersection = -1;
    EntryRule rule = EntryRule::Uncontrolled; // at toIntersection
    int lightSlot = -1;                       // traffic light slot at toIntersection, -1 = none
    float speedLimit = 15.0f;                 // m/s (the path's)
};

// Pedestrian walkway along one side of a path.
struct Sidewalk {
    int id = 0;
    int path = 0;
    int side = 0;
    Polyline centre; // walking line (the side's sidewalk polyline), at sidewalk height
    Polyline curb;   // road edge
    Polyline edge;   // building side
    float halfWidth = 1.5f;
    int startIntersection = -1; // at centre.points.front()
    int endIntersection = -1;   // at centre.points.back()
};

struct TrafficLightSite {
    int intersection = 0;
    int path = -1;        // the approach whose arrivals this light controls
    int lane = -1;        // first lane controlled by this light
    int arrivingLanes = 0; // lanes of that side (picks the single or dual model)
    Vec3 position;        // pole position (path end trafficLightPos)
    Vec3 axis;            // unit XZ direction from the pole towards trafficLightAxis
    std::string model;    // OpenMM2 Enhanced: an MM1 path end's own model (AiPathEnd), else empty
};

// How an intersection's lights take turns (aiTrafficLightSet::SetFourWay).
enum class LightCycle : std::uint8_t {
    Rotate,     // one approach after the other
    AllSources, // every approach has a light: an all-red walk phase follows each round
    FourWay,    // opposite approaches (light i and i + 2) together, then the walk phase
};

struct Intersection {
    int id = 0;
    int room = 0;
    Vec3 centre;
    std::vector<int> paths;    // in the file's order (MM2 sorts them by angle when baking)
    std::vector<int> incoming; // lanes arriving here
    std::vector<int> outgoing; // lanes leaving here
    std::vector<int> lights;   // indices into RoadNetwork::lights(), in path list order
    LightCycle cycle = LightCycle::Rotate;
    std::vector<int> sidewalks;
};

struct PathInfo {
    int id = 0;
    std::uint16_t flags = 0; // 0x1 divided, 0x2 alley, 0x4 freeway
    bool shortcut = false;   // a shortcut road of <city>_sup.bai (racers and police only)
    float halfWidth = 0.0f;
    float speedLimit = 15.0f;
    bool hasException = false; // [Exceptions] entry of the race's AI map
    float density = 0.0f;      // the exception's density
    float centreLength = 0.0f;
    std::vector<int> lanes;         // all lanes of both sides
    std::array<std::vector<int>, 2> sideLanes; // by side (0 first, 1 second), in MM2 lane order
    std::array<std::uint16_t, 2> sideFlags{};  // per side; bit 0: no ambient traffic
    std::vector<int> sidewalks;     // up to two
    int intersection[2] = {-1, -1}; // at center.back() and center.front() (the .bai ends[0], ends[1])
    int roadIndex[2] = {-1, -1};    // this path's index in each end's intersection path list
    EntryRule rule[2] = {EntryRule::Uncontrolled, EntryRule::Uncontrolled};
    std::uint16_t endFlags[2] = {0, 0}; // 3 at four-way lights: traffic goes straight on
    Vec3 centreStart, centreEnd;
    Aabb bounds;
    std::vector<Vec3> xAxis; // per section, to the left of direction +1
    // aiPath's sharp-turn records (InitRoadTurns at load). aiPath::CalcRoadTurns
    // rewrites their circles for whichever AI car asks, so they are shared,
    // mutable state as in MM2.
    mutable std::vector<SharpTurn> sharpTurns;

    bool freeway() const { return (flags & 0x4) != 0; }
    // Lanes of direction `dir` (+1 uses the second side).
    const std::vector<int>& lanesOf(int dir) const { return sideLanes[dir == 1 ? 1 : 0]; }
    std::uint16_t flagsOf(int dir) const { return sideFlags[dir == 1 ? 1 : 0]; }
};

struct NetworkOptions {
    bool driveOnLeft = false;
    float defaultSpeedLimit = 15.0f; // [Speed Limit] of the city's AI map
    std::vector<city::AiRoadException> exceptions;
};

class RoadNetwork {
public:
    static RoadNetwork build(const city::AiMap& map, const NetworkOptions& options);

    const std::vector<PathInfo>& paths() const { return m_paths; }
    const std::vector<Lane>& lanes() const { return m_lanes; }
    const std::vector<Sidewalk>& sidewalks() const { return m_sidewalks; }
    const std::vector<Intersection>& intersections() const { return m_intersections; }
    const std::vector<TrafficLightSite>& lights() const { return m_lights; }
    const city::AiMap* source() const { return m_source; }
    bool driveOnLeft() const { return m_driveOnLeft; }

    // Lane `index` of `path` in direction `dir`, or -1.
    int lane(int path, int dir, int index) const;
    // Lanes leaving `intersection` other than back along `fromPath` (no U-turns).
    std::vector<int> exits(int intersection, int fromPath) const;
    // Sidewalks meeting `intersection` other than `fromSidewalk`.
    std::vector<int> sidewalksAt(int intersection, int fromSidewalk) const;

private:
    const city::AiMap* m_source = nullptr;
    bool m_driveOnLeft = false;
    std::vector<PathInfo> m_paths;
    std::vector<Lane> m_lanes;
    std::vector<Sidewalk> m_sidewalks;
    std::vector<Intersection> m_intersections;
    std::vector<TrafficLightSite> m_lights;
};

} // namespace mm2::ai
