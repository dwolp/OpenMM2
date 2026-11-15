// MM2 (docs/parity/ai-vehicles.md, docs/parity/mm2/ai.md): aiMap::ReadBinary,
// aiIntersection::NumSinks, aiIntersection::SetFourWay, aiIntersection::Road.
#include "ai/RoadNetwork.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mm2::ai {

void Polyline::finalize() {
    distances.assign(points.size(), 0.0f);
    for (std::size_t i = 1; i < points.size(); ++i)
        distances[i] = distances[i - 1] + points[i].dist(points[i - 1]);
    length = distances.empty() ? 0.0f : distances.back();
}

Vec3 Polyline::pointAt(float s, Vec3* direction) const {
    if (points.empty())
        return {};
    if (points.size() == 1) {
        if (direction)
            *direction = Vec3::zAxis() * -1.0f;
        return points[0];
    }
    s = clampf(s, 0.0f, length);
    // First segment whose end lies at or beyond s.
    auto it = std::lower_bound(distances.begin() + 1, distances.end(), s);
    std::size_t i = static_cast<std::size_t>(std::distance(distances.begin(), it));
    if (i >= points.size())
        i = points.size() - 1;
    const Vec3& a = points[i - 1];
    const Vec3& b = points[i];
    const float seg = distances[i] - distances[i - 1];
    const float t = seg > 1e-6f ? (s - distances[i - 1]) / seg : 0.0f;
    if (direction) {
        const Vec3 d = b - a;
        *direction = d.mag2() > 1e-12f ? d.normalized() : Vec3{0, 0, -1};
    }
    return lerp(a, b, t);
}

float Polyline::project(const Vec3& p, float* distance) const {
    float best = std::numeric_limits<float>::max(), bestS = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Vec2 a{points[i - 1].x, points[i - 1].z}, b{points[i].x, points[i].z}, q{p.x, p.z};
        const Vec2 ab = b - a;
        const float len2 = ab.mag2();
        const float t = len2 > 1e-9f ? clampf((q - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
        const float d = (a + ab * t - q).mag();
        if (d < best) {
            best = d;
            bestS = distances[i - 1] + t * (distances[i] - distances[i - 1]);
        }
    }
    if (distance)
        *distance = best;
    return bestS;
}

RoadNetwork RoadNetwork::build(const city::AiMap& map, const NetworkOptions& options) {
    RoadNetwork net;
    net.m_source = &map;
    net.m_driveOnLeft = options.driveOnLeft;

    net.m_intersections.resize(map.intersections.size());
    for (std::size_t i = 0; i < map.intersections.size(); ++i) {
        auto& n = net.m_intersections[i];
        n.id = static_cast<int>(i);
        n.room = map.intersections[i].room;
        n.centre = map.intersections[i].center;
        for (auto p : map.intersections[i].paths)
            n.paths.push_back(static_cast<int>(p));
    }
    auto validIntersection = [&](std::uint32_t id) {
        return id < net.m_intersections.size() ? static_cast<int>(id) : -1;
    };

    net.m_paths.resize(map.paths.size());
    for (std::size_t p = 0; p < map.paths.size(); ++p) {
        const city::AiPath& src = map.paths[p];
        PathInfo& info = net.m_paths[p];
        info.id = static_cast<int>(p);
        info.flags = src.flags;
        info.shortcut = map.isShortcut(p);
        info.halfWidth = src.halfWidth;
        info.xAxis = src.xAxis;
        info.sharpTurns = initRoadTurns(src);
        info.centreLength = src.centerLengths.empty() ? 0.0f : src.centerLengths.back();
        // Speed limits (aiMap::Init): an [Exceptions] entry sets the road's
        // limit to its own value, zero included; otherwise freeways get the
        // city's [Speed Limit] + 12.5 and other roads the limit itself.
        info.speedLimit = options.defaultSpeedLimit + (info.freeway() ? 12.5f : 0.0f);
        // OpenMM2 Enhanced: an MM1 road keeps its own limit from the .bai
        // (15 m/s, 28 on its freeways). MM1's aiMap::Init reads the race
        // file's [Speed Limit] and [Exceptions] but nothing in the code
        // Open1560 holds uses them (inferred).
        if (map.mm1)
            info.speedLimit = src.speedLimit;
        for (const auto& e : options.exceptions) {
            if (!map.mm1 && e.road == static_cast<int>(p)) {
                info.hasException = true;
                info.density = e.density;
                info.speedLimit = e.speedLimit;
                break;
            }
        }
        for (int end = 0; end < 2; ++end) {
            const auto& e = src.ends[static_cast<std::size_t>(end)];
            info.intersection[end] = validIntersection(e.intersection);
            info.rule[end] = static_cast<EntryRule>(e.vehicleRule == 1 ? 1 : e.vehicleRule == 0 ? 0 : 3);
            info.endFlags[end] = e.unknown2;
            // The file stores the path's index in each end's intersection list.
            if (info.intersection[end] >= 0) {
                const auto& node = net.m_intersections[static_cast<std::size_t>(info.intersection[end])];
                const auto& list = node.paths;
                int idx = e.roadIndex < list.size() && list[e.roadIndex] == static_cast<int>(p)
                              ? static_cast<int>(e.roadIndex)
                              : -1;
                for (std::size_t k = 0; idx < 0 && k < list.size(); ++k)
                    if (list[k] == static_cast<int>(p))
                        idx = static_cast<int>(k);
                info.roadIndex[end] = idx;
            }
        }
        if (!src.center.empty()) {
            info.centreStart = src.center.front();
            info.centreEnd = src.center.back();
            for (const auto& c : src.center)
                info.bounds.expand(c);
        }

        // aiPath::ReverseDirection, applied to two-way roads when driving on
        // the left: each side rides the other side's lane lines, reversed in
        // point order and in lane order. The lane counts stay with their
        // sides (equal on every reversed retail road).
        const bool reverse = options.driveOnLeft && src.left.numLanes != 0;
        for (int sideIdx = 0; sideIdx < 2; ++sideIdx) {
            const city::AiRoadSide& side = sideIdx == 0 ? src.left : src.right;
            const city::AiRoadSide& other = sideIdx == 0 ? src.right : src.left;
            info.sideFlags[static_cast<std::size_t>(sideIdx)] = side.roadType;
            // Direction +1 (second side) arrives at ends[0]; -1 at ends[1].
            const int arriveEnd = sideIdx == 1 ? 0 : 1;
            // (A shortcut road's lane row is all zeros in the files; ambient
            // cars never drive one, so it gets no lane.)
            for (int l = 0; l < side.numLanes && !info.shortcut; ++l) {
                std::vector<Vec3> points;
                if (reverse) {
                    const int k = other.numLanes - 1 - l;
                    if (k < 0 || static_cast<std::size_t>(k) >= other.polylines.size())
                        break;
                    const auto& line = other.polylines[static_cast<std::size_t>(k)];
                    points.assign(line.rbegin(), line.rend());
                } else {
                    if (static_cast<std::size_t>(l) >= side.polylines.size())
                        break;
                    points = side.polylines[static_cast<std::size_t>(l)];
                }
                Lane lane;
                lane.id = static_cast<int>(net.m_lanes.size());
                lane.path = static_cast<int>(p);
                lane.side = sideIdx;
                lane.dir = sideIdx == 1 ? 1 : -1;
                lane.index = l;
                lane.count = side.numLanes;
                lane.ambient = (side.roadType & 1) == 0;
                lane.line.points = std::move(points);
                lane.line.finalize();
                lane.toIntersection = info.intersection[arriveEnd];
                lane.fromIntersection = info.intersection[1 - arriveEnd];
                lane.rule = info.rule[arriveEnd];
                lane.speedLimit = info.speedLimit;
                info.lanes.push_back(lane.id);
                info.sideLanes[static_cast<std::size_t>(sideIdx)].push_back(lane.id);
                net.m_lanes.push_back(std::move(lane));
            }

            // The sidewalk line follows the lanes (MM2's vertex rows: lanes,
            // then the sidewalk; aiPath::SidewalkVertice), then come the tram
            // and train lines, the curb and the outer edge. ReverseDirection
            // leaves the sidewalk in place.
            const std::size_t base = static_cast<std::size_t>(side.numLanes + side.numTrams + side.numTrains);
            if (side.numSidewalks > 0 && base + 2 < side.polylines.size()) {
                Sidewalk walk;
                walk.id = static_cast<int>(net.m_sidewalks.size());
                walk.path = static_cast<int>(p);
                walk.side = sideIdx;
                walk.centre.points = side.polylines[static_cast<std::size_t>(side.numLanes)];
                walk.curb.points = side.polylines[base + 1];
                walk.edge.points = side.polylines[base + 2];
                // Pedestrians stand on the sidewalk surface: the outer edge
                // height (the centre polyline sits halfway up the curb).
                for (std::size_t i = 0; i < walk.centre.points.size() && i < walk.edge.points.size(); ++i)
                    walk.centre.points[i].y = walk.edge.points[i].y;
                walk.centre.finalize();
                walk.curb.finalize();
                walk.edge.finalize();
                if (!walk.curb.points.empty() && !walk.edge.points.empty()) {
                    const Vec3 a = walk.curb.points.front(), b = walk.edge.points.front();
                    walk.halfWidth = 0.5f * Vec2{a.x - b.x, a.z - b.z}.mag();
                }
                // Points run with increasing section index: front at ends[1].
                walk.startIntersection = info.intersection[1];
                walk.endIntersection = info.intersection[0];
                if (walk.centre.length > 1.0f && walk.halfWidth > 0.25f) {
                    info.sidewalks.push_back(walk.id);
                    net.m_sidewalks.push_back(std::move(walk));
                }
            }
        }
    }

    // Connectivity and traffic light sites.
    for (const Lane& lane : net.m_lanes) {
        if (lane.toIntersection >= 0)
            net.m_intersections[static_cast<std::size_t>(lane.toIntersection)].incoming.push_back(lane.id);
        if (lane.fromIntersection >= 0)
            net.m_intersections[static_cast<std::size_t>(lane.fromIntersection)].outgoing.push_back(lane.id);
    }
    for (const Sidewalk& w : net.m_sidewalks) {
        for (int n : {w.startIntersection, w.endIntersection})
            if (n >= 0)
                net.m_intersections[static_cast<std::size_t>(n)].sidewalks.push_back(w.id);
    }
    for (auto& node : net.m_intersections) {
        int sources = 0, sinks = 0;
        for (int pathId : node.paths) {
            if (pathId < 0 || static_cast<std::size_t>(pathId) >= map.paths.size())
                continue;
            const auto& src = map.paths[static_cast<std::size_t>(pathId)];
            PathInfo& info = net.m_paths[static_cast<std::size_t>(pathId)];
            // aiIntersection::NumSources / NumSinks as MM2 codes them: a
            // source is a path whose ends[0] is here or whose first side has
            // lanes; a "sink" a departing side whose flag bit 0 is set.
            const bool atEnd0 = info.intersection[0] == node.id;
            if (atEnd0 || src.left.numLanes != 0)
                ++sources;
            if (info.intersection[1] == node.id) {
                if (info.sideFlags[1] & 1)
                    ++sinks;
            } else if (src.left.numLanes != 0 && (info.sideFlags[0] & 1)) {
                ++sinks;
            }
            // One light per path end here with a traffic light, in the
            // intersection's path order (aiTrafficLightSet ctor; the light
            // indices are handed out the same way by SetFourWay). Both take
            // end 1 when the path's end-1 intersection is this one, else end
            // 0, so a loop road (both ends here) gets its end-1 light only.
            const int end = info.intersection[1] == node.id ? 1 : (atEnd0 ? 0 : -1);
            if (end < 0 || src.ends[static_cast<std::size_t>(end)].vehicleRule != 1)
                continue;
            const auto& e = src.ends[static_cast<std::size_t>(end)];
            TrafficLightSite site;
            site.intersection = node.id;
            site.path = pathId;
            site.position = e.trafficLightPos;
            Vec3 d = e.trafficLightAxis - e.trafficLightPos;
            d.y = 0.0f;
            site.axis = d.mag2() > 1e-12f ? d.normalized() : Vec3{1, 0, 0};
            site.model = e.trafficLightModel;
            // Lanes arriving at this end: direction +1 at ends[0], -1 at ends[1].
            const int side = end == 0 ? 1 : 0;
            site.arrivingLanes = side == 1 ? src.right.numLanes : src.left.numLanes;
            const int slot = static_cast<int>(net.m_lights.size());
            for (int laneId : info.sideLanes[static_cast<std::size_t>(side)]) {
                Lane& lane = net.m_lanes[static_cast<std::size_t>(laneId)];
                lane.lightSlot = slot;
                if (site.lane < 0)
                    site.lane = laneId;
            }
            node.lights.push_back(slot);
            net.m_lights.push_back(site);
        }
        // aiTrafficLightSet::SetFourWay. With the sink count as MM2 codes it
        // no retail intersection qualifies as a four-way one.
        if (!node.lights.empty()) {
            if (sinks == 4 && sources == 4 && node.paths.size() == 4)
                node.cycle = LightCycle::FourWay;
            else if (static_cast<int>(node.lights.size()) == sources)
                node.cycle = LightCycle::AllSources;
        }
        if (node.cycle == LightCycle::FourWay) {
            // The approaches' end flags get 3, so ambient traffic goes
            // straight on there (aiMap::ChooseStraightLinkAt4Way).
            for (int pathId : node.paths) {
                PathInfo& info = net.m_paths[static_cast<std::size_t>(pathId)];
                for (int end = 0; end < 2; ++end)
                    if (info.intersection[end] == node.id)
                        info.endFlags[end] |= 3;
            }
        }
    }
    return net;
}

int RoadNetwork::lane(int path, int dir, int index) const {
    if (path < 0 || static_cast<std::size_t>(path) >= m_paths.size())
        return -1;
    const auto& lanes = m_paths[static_cast<std::size_t>(path)].lanesOf(dir);
    if (index < 0 || static_cast<std::size_t>(index) >= lanes.size())
        return -1;
    return lanes[static_cast<std::size_t>(index)];
}

std::vector<int> RoadNetwork::exits(int intersection, int fromPath) const {
    std::vector<int> out;
    if (intersection < 0 || static_cast<std::size_t>(intersection) >= m_intersections.size())
        return out;
    for (int laneId : m_intersections[static_cast<std::size_t>(intersection)].outgoing)
        if (m_lanes[static_cast<std::size_t>(laneId)].path != fromPath)
            out.push_back(laneId);
    return out;
}

std::vector<int> RoadNetwork::sidewalksAt(int intersection, int fromSidewalk) const {
    std::vector<int> out;
    if (intersection < 0 || static_cast<std::size_t>(intersection) >= m_intersections.size())
        return out;
    for (int w : m_intersections[static_cast<std::size_t>(intersection)].sidewalks)
        if (w != fromSidewalk)
            out.push_back(w);
    return out;
}

} // namespace mm2::ai
