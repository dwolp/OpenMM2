// aiVehiclePhysics' route half (MM2 build 3393, MM2Recomp): the waypoints
// and the window of three roads round the car (RegisterRoute, PlanRoute,
// LocateWayPtFromRoad, SolveRoadTargetPoint, SolveShortcutTargetPoint), the
// route enumeration (CalcRoute, EnumRoutes, ContinueCheck, SaveTarget,
// SetTargetPtToDestination, DetermineBestRoute) and the obstacles
// (IsTargetBlocked, CalcObstacleAvoidPoints, EnumTargets).
#include "ai/Driving.h"
#include "ai/MapView.h"
#include "ai/PathGeometry.h"
#include "ai/Pedestrians.h"
#include "ai/Traffic.h"
#include "city/Mm1Racer.h"
#include "core/Libm.h"
#include "core/Log.h"
#include "phys/vehicle/CarSim.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace mm2::ai {

namespace {

float xzDistance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dz * dz + dx * dx);
}

// Vector3::Angle: the unsigned angle between two vectors (acos of the
// normalised dot product, 0 for nearly parallel ones).
float angleBetween(const Vec3& a, const Vec3& b) {
    const float m = std::sqrt(a.mag2() * b.mag2());
    if (m == 0.0f)
        return 0.0f;
    const float c = a.dot(b) / m;
    if (c > 0.9999999f)
        return 0.0f;
    if (c < -1.0f)
        return 3.1415927f;
    return libm::acos(c);
}

int wayPoint(const std::vector<int>& wp, int i) {
    return i >= 0 && static_cast<std::size_t>(i) < wp.size() ? wp[static_cast<std::size_t>(i)] : -1;
}

} // namespace

// --- the road window --------------------------------------------------------------

const city::AiPath* PhysicsDriver::road(int slot) const {
    // Slot 3 is the word after the window in MM2 (+0x278, the road the turns
    // were last set up for), which a few loops read when they run past the
    // window.
    if (!m_map || slot < 0 || slot > 3)
        return nullptr;
    return m_map->path(slot == 3 ? m_turnsRoad : m_roads[slot]);
}

const PathInfo* PhysicsDriver::roadInfo(int slot) const {
    if (!m_map || slot < 0 || slot > 3)
        return nullptr;
    return m_map->pathInfo(slot == 3 ? m_turnsRoad : m_roads[slot]);
}

int PhysicsDriver::windowIntersectionAhead(int slot) const {
    // The intersection road `slot` is driven into: its end-0 intersection
    // when driven with the vertex index, else its end-1 one.
    const PathInfo* info = roadInfo(slot);
    if (!info || slot > 2)
        return -1;
    return info->intersection[m_roadDir[slot] ? 0 : 1];
}

void PhysicsDriver::destMapComponent(const Vec3& pos, int& id, int& type) const {
    // aiVehiclePhysics::DestMapComponent: the destination room's intersection,
    // else a road or shortcut of the room the destination is on or next to
    // that ends at the last waypoint (any, without waypoints), else the room.
    const int room = m_map->findRoom(pos, 0);
    const auto& list = m_map->components(room);
    for (const RoomComponent& c : list) {
        if (c.type == kIntersectionComponent) {
            type = c.type;
            id = c.id;
            return;
        }
    }
    const int last = m_wayPts.empty() ? -1 : m_wayPts.back();
    for (const RoomComponent& c : list) {
        if (c.type != kShortcutComponent && c.type != kRoadComponent)
            continue;
        const city::AiPath* p = m_map->path(c.id);
        const PathInfo* info = m_map->pathInfo(c.id);
        if (!p || !info || pathIsPosOnRoad(*p, pos, 0.0f) >= 3)
            continue;
        if (m_wayPts.empty() || info->intersection[0] == last || info->intersection[1] == last) {
            type = c.type;
            id = c.id;
            return;
        }
    }
    type = kNoComponent;
    id = room;
}

void PhysicsDriver::registerRoute(const RouteRegistration& route) {
    // aiVehiclePhysics::RegisterRoute.
    m_numLaps = route.laps;
    m_wayPts = route.wayPoints;
    m_curLap = 1;
    m_turnsRoad = -1;
    const Vec3 pos = m_car.body.ics.matrix.m3;
    m_turnRefStart = pos;
    m_turnRef = pos;
    m_bestRoute = -1;
    m_target = pos;
    m_roads[0] = m_roads[1] = m_roads[2] = -1;
    m_turnAngle[0] = m_turnAngle[1] = 0.0f;
    m_nodes[0].road = 0;
    m_nodes[0].vert = 0;
    m_dest = route.destination;
    m_destHeading = route.destinationHeading;
    if (!m_map)
        return;
    destMapComponent(m_dest, m_destCompId, m_destCompType);
    m_curRoom = 0;
    m_curRoom = m_map->mapComponent(pos, m_curCompId, m_curCompType, 0);

    const int n = static_cast<int>(m_wayPts.size());
    // The window from the first waypoints (n >= 2): the roads between them,
    // the number of them in +0x968e.
    auto windowFromWayPoints = [&](bool setIndex) {
        if (n == 2) {
            m_roads[0] = m_map->roadBetween(m_wayPts[0], m_wayPts[1], &m_roadDir[0]);
            m_numWayPtRoads = 1;
        } else if (n == 3) {
            m_roads[0] = m_map->roadBetween(m_wayPts[0], m_wayPts[1], &m_roadDir[0]);
            m_roads[1] = m_map->roadBetween(m_wayPts[1], m_wayPts[2], &m_roadDir[1]);
            m_numWayPtRoads = 2;
        } else if (n > 3) {
            m_roads[0] = m_map->roadBetween(m_wayPts[0], m_wayPts[1], &m_roadDir[0]);
            m_roads[1] = m_map->roadBetween(m_wayPts[1], m_wayPts[2], &m_roadDir[1]);
            m_roads[2] = m_map->roadBetween(m_wayPts[2], m_wayPts[3], &m_roadDir[2]);
            m_numWayPtRoads = 3;
        } else {
            return;
        }
        if (setIndex)
            m_wayPtIdx = 1;
    };
    // The car's place across its road (+0x9738): from the centre line at a
    // vertex, positive to the right.
    auto offsetAt = [&](const city::AiPath& p, int k, bool forward) {
        const Vec3& c = p.center[static_cast<std::size_t>(k)];
        const Vec3& x = p.xAxis[static_cast<std::size_t>(k)];
        if (forward)
            return (c.x - pos.x) * x.x + (c.z - pos.z) * x.z;
        return -x.z * (c.z - pos.z) + -x.x * (c.x - pos.x);
    };

    switch (m_curCompType) {
    case kNoComponent: {
        // Off the roads: the window from the waypoints, Shortcut (unless
        // backing up).
        m_wayPtIdx = 0;
        m_numWayPtRoads = 0;
        windowFromWayPoints(false);
        if (m_state != State::Backup)
            m_state = State::Shortcut;
        const city::AiPath* p = road(0);
        if (p) {
            const int last = static_cast<int>(p->center.size()) - 1;
            m_roadOffset = m_roadDir[0] ? offsetAt(*p, 0, true) : offsetAt(*p, last, false);
        } else {
            m_roadOffset = 0.0f;
        }
        return;
    }
    case kRoadComponent:
    case kShortcutComponent: {
        m_wayPtIdx = 0;
        m_roadDir[0] = true;
        m_roads[0] = m_curCompId;
        m_numWayPtRoads = 0;
        windowFromWayPoints(true);
        m_state = State::Forward;
        const city::AiPath* p = road(0);
        if (!p)
            return;
        const int last = static_cast<int>(p->center.size()) - 1;
        const int rv = pathRoadVertice(*p, pos, m_roadDir[0] ? 1 : 0);
        const int v = rv < 0 ? 0 : std::min(rv, last);
        m_roadOffset = m_roadDir[0] ? offsetAt(*p, v, true) : offsetAt(*p, last - v, false);
        return;
    }
    case kIntersectionComponent: {
        m_numWayPtRoads = 0;
        m_wayPtIdx = 0;
        const Intersection* node = m_map->intersection(m_curCompId);
        m_roads[0] = node && !node->paths.empty() ? node->paths.front() : -1;
        const PathInfo* info = roadInfo(0);
        m_roadDir[0] = info && info->intersection[0] == m_curCompId;
        windowFromWayPoints(true);
        m_state = State::Forward;
        const city::AiPath* p = road(0);
        if (!p)
            return;
        const int last = static_cast<int>(p->center.size()) - 1;
        m_roadOffset = m_roadDir[0] ? offsetAt(*p, 0, true) : offsetAt(*p, last, false);
        return;
    }
    default:
        return;
    }
}

void PhysicsDriver::resumeRoute(int wayPtIdx, int lap) {
    // OpenMM2 recovery (not in MM2): after the car was put back further along
    // its route and the route registered again, carry on from waypoint
    // `wayPtIdx` of lap `lap`. On a road PlanRoute finds the window from it
    // (LocateWayPtFromRoad, which also steps past a waypoint the car was put
    // beyond); elsewhere the window is the waypoints' roads round it.
    const int n = static_cast<int>(m_wayPts.size());
    m_curLap = std::max(lap, 1);
    m_wayPtIdx = std::clamp(wayPtIdx, 0, n);
    if (!m_map || m_wayPtIdx < 1 || m_wayPtIdx >= n)
        return;
    if (m_curCompType == kRoadComponent || m_curCompType == kShortcutComponent)
        return;
    const int i = m_wayPtIdx;
    m_roads[0] = m_map->roadBetween(wayPoint(m_wayPts, i - 1), wayPoint(m_wayPts, i), &m_roadDir[0]);
    m_roads[1] = m_map->roadBetween(wayPoint(m_wayPts, i), wayPoint(m_wayPts, i + 1), &m_roadDir[1]);
    m_roads[2] = m_map->roadBetween(wayPoint(m_wayPts, i + 1), wayPoint(m_wayPts, i + 2), &m_roadDir[2]);
}

int PhysicsDriver::currentRoadIdx(const int roads[3], const bool dirs[3], int* vert) const {
    // aiVehiclePhysics::CurrentRoadIdx: only the car's own current road
    // counts, with the vertex in the car's own direction.
    (void)dirs;
    const city::AiPath* p = road(0);
    if (!p)
        return -1;
    const int n = static_cast<int>(p->center.size());
    const int k = pathIndex(*p, m_car.body.ics.matrix.m3);
    if (vert)
        *vert = m_roadDir[0] ? k : n - k - 1;
    for (int i = 0; i < 3; ++i)
        if (roads[i] == m_roads[0])
            return i;
    return -1;
}

int PhysicsDriver::planRoute() {
    // aiVehiclePhysics::PlanRoute: the next waypoint reached, and the window
    // re-anchored on the road or intersection the car is in. Returns 0.
    const int n = static_cast<int>(m_wayPts.size());
    if (m_wayPtIdx < n && m_curCompType == kIntersectionComponent && m_curCompId == m_wayPts[m_wayPtIdx]) {
        if (m_wayPtIdx > 0) {
            bool dir = false;
            const int between = m_map->roadBetween(wayPoint(m_wayPts, m_wayPtIdx - 1),
                                                   wayPoint(m_wayPts, m_wayPtIdx), &dir);
            if (between != m_roads[0])
                locateWayPtFromRoad(between);
        }
        ++m_wayPtIdx;
        if (m_wayPtIdx == n && (m_curLap < m_numLaps || m_numLaps == -1)) {
            m_wayPtIdx = 1;
            ++m_curLap;
        }
    }
    if (m_curCompType == kRoadComponent || m_curCompType == kShortcutComponent) {
        locateWayPtFromRoad(m_curCompId);
    } else if (m_curCompType == kIntersectionComponent && m_numWayPtRoads < 3 &&
               m_destCompType == kRoadComponent) {
        // In an intersection with a free slot: the destination road, when the
        // intersection has it.
        if (const Intersection* node = m_map->intersection(m_curCompId)) {
            for (int p : node->paths) {
                if (p != m_destCompId)
                    continue;
                const auto slot = static_cast<std::size_t>(m_numWayPtRoads);
                m_roads[slot] = p;
                const PathInfo* info = m_map->pathInfo(p);
                m_roadDir[slot] = info && info->intersection[1] == m_curCompId;
                break;
            }
        }
    }
    return 0;
}

int PhysicsDriver::locateWayPtFromRoad(int roadId) {
    // aiVehiclePhysics::LocateWayPtFromRoad: the car is on `roadId`.
    const city::AiPath* p = m_map->path(roadId);
    const PathInfo* info = m_map->pathInfo(roadId);
    if (!p || !info)
        return 0;
    const int n = static_cast<int>(m_wayPts.size());
    const Vec3 pos = m_car.body.ics.matrix.m3;
    const int verts = static_cast<int>(p->center.size());
    if (m_wayPtIdx >= n) {
        // Past the last waypoint: this road alone, in the way the car faces.
        m_roads[0] = roadId;
        m_roads[1] = m_roads[2] = -1;
        const int rv = pathRoadVertice(*p, pos, 1);
        const int v = rv < 0 ? 0 : std::min(rv, verts - 1);
        const Vec3& z = p->zAxis[static_cast<std::size_t>(v)];
        const Vec3& m2 = m_car.body.ics.matrix.m2;
        m_roadDir[0] = z.x * m2.x + z.z * m2.z >= 0.0f;
        return 1;
    }
    // Already past the current waypoint (the road ends at the next one).
    const int next = wayPoint(m_wayPts, m_wayPtIdx + 1);
    if (info->intersection[0] == next || info->intersection[1] == next ||
        (m_wayPtIdx == n - 1 && m_destCompId == m_curCompId))
        ++m_wayPtIdx;
    const int cur = wayPoint(m_wayPts, m_wayPtIdx);
    if (info->intersection[0] == cur) {
        m_roads[0] = roadId;
        m_roadDir[0] = true;
        m_numWayPtRoads = 0;
    } else if (info->intersection[1] == cur) {
        m_roads[0] = roadId;
        m_roadDir[0] = false;
        m_numWayPtRoads = 0;
    } else {
        // The waypoint is one road further on, past either end.
        for (int end = 0; end < 2; ++end) {
            const Intersection* node = m_map->intersection(info->intersection[end]);
            if (!node)
                continue;
            for (int q : node->paths) {
                const PathInfo* qi = m_map->pathInfo(q);
                if (!qi)
                    continue;
                bool found = false;
                bool qdir = false;
                if (qi->intersection[0] == cur) {
                    found = true;
                    qdir = true;
                } else if (qi->intersection[1] == cur) {
                    found = true;
                    qdir = false;
                }
                if (!found)
                    continue;
                m_roadDir[0] = end == 0;
                m_roads[0] = roadId;
                m_roads[1] = q;
                m_roadDir[1] = qdir;
                if (n <= m_wayPtIdx + 1)
                    m_roads[2] = -1;
                else
                    m_roads[2] = m_map->roadBetween(wayPoint(m_wayPts, m_wayPtIdx),
                                                    wayPoint(m_wayPts, m_wayPtIdx + 1), &m_roadDir[2]);
                return 1;
            }
        }
        // "I'm Lost!!"
        return 0;
    }
    // The next roads from the waypoint list (round to the start of the list
    // on a lap still to drive).
    const int i = m_wayPtIdx;
    const bool finished = m_curLap >= m_numLaps && m_numLaps != -1;
    int x = -1, y = -1, z = -1;
    if (i == n - 1) {
        x = wayPoint(m_wayPts, i);
        if (finished) {
            m_numWayPtRoads = 1;
        } else {
            y = wayPoint(m_wayPts, 1);
            z = wayPoint(m_wayPts, 2);
            m_numWayPtRoads = 3;
        }
    } else if (i == n - 2) {
        x = wayPoint(m_wayPts, i);
        y = wayPoint(m_wayPts, i + 1);
        if (finished) {
            m_numWayPtRoads = 2;
        } else {
            z = wayPoint(m_wayPts, 1);
            m_numWayPtRoads = 3;
        }
    } else {
        x = wayPoint(m_wayPts, i);
        y = wayPoint(m_wayPts, i + 1);
        z = wayPoint(m_wayPts, i + 2);
        m_numWayPtRoads = 3;
    }
    m_roads[1] = m_map->roadBetween(x, y, &m_roadDir[1]);
    m_roads[2] = m_map->roadBetween(y, z, &m_roadDir[2]);
    // The destination's road after the last waypoint's.
    if ((m_destCompType == kRoadComponent || m_destCompType == kShortcutComponent) && m_numWayPtRoads >= 1 &&
        m_numWayPtRoads <= 2) {
        const int lastNode = wayPoint(m_wayPts, n - 1);
        if (const Intersection* node = m_map->intersection(lastNode)) {
            for (int q : node->paths) {
                if (q != m_destCompId)
                    continue;
                const auto slot = static_cast<std::size_t>(m_numWayPtRoads);
                m_roads[slot] = q;
                const PathInfo* qi = m_map->pathInfo(q);
                m_roadDir[slot] = qi && qi->intersection[1] == lastNode;
                break;
            }
        }
    }
    return 1;
}

void PhysicsDriver::solveRoadTargetPoint(std::span<const TrackedCar> cars) {
    // aiVehiclePhysics::SolveRoadTargetPoint.
    m_toDestination = m_wayPtIdx >= static_cast<int>(m_wayPts.size());
    planRoute();
    if (m_turnsRoad != m_roads[0]) {
        m_turnsRoad = m_roads[0];
        const Vec3 pos = m_car.body.ics.matrix.m3;
        m_turnRefStart = pos;
        m_turnRef = pos;
        initRoadTurns();
        calcRoadTurns();
    }
    calcRoute(cars);
    m_target = routeOrWorking(m_bestRoute)[1].pos;
    // OpenMM2 Enhanced: the MM2 route search's own avoidance takes over while its best route has an avoid
    // point (2) or no way round (3) among its first nodes (another car, a player or a racer, in the way);
    // otherwise the racer follows MM1's rail.
    // MM2's own route-search avoidance handles traffic too by default (it ran racers round traffic and
    // each other faster than the rail shift below, in the logs); OPENMM2_MM1_TRAFFIC_SHIFT selects the shift.
    static const bool shiftMode = std::getenv("OPENMM2_MM1_TRAFFIC_SHIFT") != nullptr;
    bool avoiding = false;
    if (m_bestRoute >= 0) {
        const auto& rn = routeOrWorking(m_bestRoute);
        const int cnt = m_routeNodeCount[static_cast<std::size_t>(m_bestRoute)];
        // Within about 3 s of travel (and at least 60 m) along the route.
        const float reach = std::max(60.0f, 3.0f * m_car.speed());
        for (int i = 1; i < cnt; ++i) {
            const RouteNode& nd = rn[static_cast<std::size_t>(i)]; // dist: path length from the car
            if (i > 1 && nd.dist > reach)
                break;
            if (nd.kind >= 2 && (nd.blockKind != 1 || !shiftMode)) // (with OPENMM2_MM1_TRAFFIC_SHIFT traffic is gone round on the rail, below)
                avoiding = true;
        }
    }
    m_dbgAvoiding = avoiding;
    if (static const char* dn = std::getenv("OPENMM2_DEBUG_NODES"); dn && m_aiId == std::atoi(dn) && m_bestRoute >= 0) {
        // Development aid: the racer's route nodes (the points the MM2 driver plans through), 3 times a second.
        static int cnt = 0;
        if (++cnt % 10 == 1) {
            const auto& rn = routeOrWorking(m_bestRoute);
            const int nc = m_routeNodeCount[static_cast<std::size_t>(m_bestRoute)];
            const Vec3 cp = m_car.body.ics.matrix.m3;
            std::string line = std::format("nodes: car {:.1f} {:.1f} mph {:.0f} target {:.1f} {:.1f} avoid {} n {}", cp.x, cp.z,
                                           m_car.speed() * 2.237f, m_target.x, m_target.z, avoiding ? 1 : 0, nc);
            for (int i = 0; i < nc; ++i)
                line += std::format(" | {:.1f} {:.1f} k{} t{}", rn[static_cast<std::size_t>(i)].pos.x,
                                    rn[static_cast<std::size_t>(i)].pos.z, rn[static_cast<std::size_t>(i)].kind,
                                    rn[static_cast<std::size_t>(i)].turnCode);
            log::info("{}", line);
        }
    }
    m_dbgNearAmbient = -1.0f;
    {
        const Mat34& dm = m_car.body.ics.matrix;
        for (const TrackedCar& o : cars) {
            if (!o.isAmbient() || o.id == m_selfId)
                continue;
            const Vec3 d = o.position - dm.m3;
            const float ahead = -d.dot(dm.m2), side = d.dot(dm.m0);
            if (ahead > 0.0f && ahead < 60.0f && std::fabs(side) < 6.0f && (m_dbgNearAmbient < 0.0f || ahead < m_dbgNearAmbient))
                m_dbgNearAmbient = ahead;
        }
    }
    // OpenMM2 Enhanced: the nearest ambient car in the racer's own lane ahead, for calcSpeed's brake (see
    // RouteParams::brakeForTraffic). Its width plus ours and 0.5 m is the lane; the reach is a hard stop's distance.
    m_laneAhead = -1.0f;
    if (params.brakeForTraffic) {
        const Mat34& dm = m_car.body.ics.matrix;
        const float v = m_car.speed();
        const float reach = std::clamp(v * v / 10.0f + 20.0f, 25.0f, 120.0f);
        for (const TrackedCar& o : cars) {
            if (!o.isAmbient() || o.id == m_selfId)
                continue;
            const Vec3 d = o.position - dm.m3;
            const float ahead = -d.dot(dm.m2), side = d.dot(dm.m0);
            if (ahead <= 0.0f || ahead > reach || std::fabs(side) > m_rightSide + o.halfWidth + 0.5f)
                continue;
            if (m_laneAhead < 0.0f || ahead < m_laneAhead) {
                m_laneAhead = ahead;
                m_laneSpeed = std::max(o.currentSpeed(), 0.0f);
            }
        }
    }
    // OpenMM2 Enhanced: an ambient car in the racer's way is gone round on the rail: the aim moves
    // sideways early (its look-ahead is at least 25 m, 1.6 s at speed) to a clearance of both half
    // widths and 1.2 m, to the side with the car's own offset, or towards the road's middle when
    // the car is dead ahead; the racer brakes only when it is close and still in the lane.
    m_mm1Blocked = false;
    m_mm1MustStop = false;
    float shiftTarget = 0.0f;
    if (m_mm1Racer && m_mm1Racer->valid()) {
        const Mat34& dm = m_car.body.ics.matrix;
        const float v = m_car.speed();
        // Far enough to stop for a stationary car: the braking distance at about 7 m/s^2 plus 15 m.
        const float reach = std::clamp(std::max(1.6f * v, v * v / 14.0f + 15.0f), 25.0f, 140.0f);
        struct Obs {
            float ahead, side, clear, speed;
        };
        std::vector<Obs> obs;
        float nearest = 1e9f;
        for (const TrackedCar& o : cars) {
            if (!o.isAmbient() || o.id == m_selfId)
                continue;
            const Vec3 d = o.position - dm.m3;
            const float ahead = -d.dot(dm.m2), side = d.dot(dm.m0);
            if (ahead <= 0.0f || ahead > reach || std::fabs(side) > 9.0f)
                continue;
            obs.push_back({ahead, side, m_rightSide + o.halfWidth + 1.2f, std::max(o.currentSpeed(), 0.0f)});
            if (std::fabs(side) <= obs.back().clear + 1.0f)
                nearest = std::min(nearest, ahead);
        }
        if (nearest < 1e9f) {
            // The cars within 25 m beyond the nearest in the way: where is a line clear of all of them
            // (and on the road)? The smallest shift that is.
            const float road = m_mm1Racer->sideLimit();
            const float myD = m_mm1Racer->state().distToSide;
            std::vector<float> cand{0.0f};
            for (const Obs& o : obs)
                if (o.ahead <= nearest + 25.0f) {
                    cand.push_back(o.side - o.clear);
                    cand.push_back(o.side + o.clear);
                }
            float bestShift = 1e9f;
            for (float c : cand) {
                if (std::fabs(myD + c) > road)
                    continue;
                bool free = true;
                for (const Obs& o : obs)
                    if (o.ahead <= nearest + 25.0f && std::fabs(o.side - c) < o.clear - 0.05f)
                        free = false;
                if (free && std::fabs(c) < std::fabs(bestShift))
                    bestShift = c;
            }
            for (const Obs& o : obs)
                if (o.ahead == nearest) {
                    m_mm1Blocked = true;
                    m_mm1BlockAhead = o.ahead;
                    m_mm1BlockSpeed = o.speed;
                    m_mm1BlockSide = o.side;
                    m_mm1InWay = std::fabs(o.side - m_mm1Shift) < o.clear * 0.9f;
                }
            if (bestShift < 1e8f) {
                shiftTarget = shiftMode ? std::clamp(bestShift, -6.0f, 6.0f) : 0.0f;
            } else {
                m_mm1MustStop = true; // no way round: stop behind it
            }
        }
    }
    m_mm1Shift += (shiftTarget - m_mm1Shift) * 0.15f;
    if (m_mm1Racer && m_mm1Racer->valid()) {
        // aiGoalFollowWayPts::Update (without its collision avoidance): MM1's own rail.
        const Mat34& m = m_car.body.ics.matrix;
        if (!m_mm1RacerInited) {
            m_mm1Racer->init(m.m3, m.m2, m_car.speed());
            m_mm1RacerInited = true;
        }
        const Vec3 t = m_mm1Racer->update(m.m3, m.m2, m_car.speed(), m_mm1Shift);
        if (!avoiding)
            m_target = {t.x, m.m3.y, t.z};
        if (static const char* dbg = std::getenv("OPENMM2_DEBUG_MM1RACER"); dbg && m_aiId == std::atoi(dbg)) {
            static int n_ = 0;
            if (++n_ % 10 == 1) {
                const auto& st = m_mm1Racer->state();
                log::info("mm1racer {}: car ({:.1f},{:.1f}) {:.0f} mph comp {}/{} cur {} next {} wp {} roadDist {:.1f} "
                          "side {:.1f} tpo {:.1f} target ({:.1f},{:.1f})",
                          m_aiId, m.m3.x, m.m3.z, m_car.speed() * 2.237f, st.compType, st.compIdx, st.cur, st.next,
                          st.wayPtIdx, st.roadDist, st.distToSide, m_mm1Racer->targetPtOffset(), t.x, t.z);
            }
        }
    } else if (m_car.isMm1() && m_bestRoute >= 0)
        m_target = mm1RailTarget(m_target);
}

Vec3 PhysicsDriver::mm1RailTarget(const Vec3& fallback) {
    // OpenMM2 Enhanced, a car on MM1's physics: MM1's SolveTargetPoint aims at
    // a point on the rail `TargetPtOffset` ahead of the car's position along it
    // (aiRailSet::CalcCopRailPosition(RoadDist + TargetPtOffset)), not at the
    // end of the road as MM2's route does. TargetPtOffset (aiMap::
    // DetermineOppMapComponent) is 20 m with no next road, else a speed term
    // clamped to 7..20 m; the formula's speed term is not decoded, so
    // 0.5 s of travel clamped to 7..20 m is used (inferred). The point is
    // found by walking the planned route's nodes from the car's projection
    // onto its first segment, so a corner is taken along the route's turn.
    const auto& n = routeOrWorking(m_bestRoute);
    const int count = m_routeNodeCount[static_cast<std::size_t>(m_bestRoute)];
    if (count < 2)
        return fallback;
    const Vec3 pos = m_car.body.ics.matrix.m3;
    if (static const char* dbgId = std::getenv("OPENMM2_DEBUG_ROUTE"); dbgId && m_aiId == std::atoi(dbgId)) {
        static int n_ = 0;
        if (++n_ % 60 == 1) {
            const city::AiPath* r0 = road(0);
            std::string s = std::format("route: car ({:.1f},{:.1f}) dir0 {} road0 {} ", pos.x, pos.z, m_roadDir[0],
                                        r0 && !r0->center.empty()
                                            ? std::format("[{:.0f},{:.0f}]->[{:.0f},{:.0f}] hw {:.1f}",
                                                          r0->center.front().x, r0->center.front().z,
                                                          r0->center.back().x, r0->center.back().z, r0->halfWidth)
                                            : "none");
            s += std::format(" || comp type {} id {} wayIdx {} ways", m_curCompType, m_curCompId, m_wayPtIdx);
            for (std::size_t k = 0; k < std::min<std::size_t>(5, m_wayPts.size()); ++k)
                s += std::format(" {}", m_wayPts[k]);
            for (int w = 0; w < 3; ++w)
                if (const city::AiPath* rw = road(w); rw && !rw->center.empty())
                    s += std::format(" || road{} [{:.0f},{:.0f}]->[{:.0f},{:.0f}] dir {}", w, rw->center.front().x,
                                     rw->center.front().z, rw->center.back().x, rw->center.back().z, m_roadDir[w]);
            for (int i = 0; i < count; ++i) {
                const auto& nd = n[static_cast<std::size_t>(i)];
                s += std::format(" | ({:.1f},{:.1f}) rd{} v{} k{} s{}", nd.pos.x, nd.pos.z, nd.road, nd.vert, nd.kind,
                                 nd.surface);
            }
            log::info("{}", s);
        }
    }
    // TargetPtOffset, aiMap::DetermineOppMapComponent's road branch (decoded
    // from Open1560's game.asm 62779-63860):
    //  - the next road within 0.65 rad of this one's last segment: 20 m;
    //  - a sharper turn ahead, the car before the turn's arc start: the
    //    distance x to that arc start (the corner less the setback) while
    //    x < 30 m, at least 7 m; 20 m while it is 30 m or more;
    //  - inside the arc: MM1 solves the arc geometry for a value clamped to
    //    7..20 m (the formula is not decoded; 7 m is used, inferred).
    float left = 20.0f;
    {
        const city::AiPath* r0 = road(0);
        const float a0 = m_turnAngle[0];
        if (r0 && !r0->zAxis.empty() && (a0 <= -0.65f || 0.65f <= a0)) {
            const float ix = pos.x - m_turnCorner[0].x, iz = pos.z - m_turnCorner[0].z;
            float x;
            if (!m_roadDir[0]) {
                const Vec3& z = r0->zAxis.front();
                x = (-z.x * ix + -z.z * iz) - m_turnSetback[0];
            } else {
                const Vec3& z = r0->zAxis.back();
                x = (ix * z.x + iz * z.z) - m_turnSetback[0];
            }
            if (x <= 0.0f)
                left = 7.0f;
            else if (x < 30.0f)
                left = std::max(x, 7.0f);
        }
    }
    Vec3 from = n[0].pos;
    Vec3 to = n[1].pos;
    // The rail: the road's first lane line on the side of its direction of
    // travel (lane lines run in their side's travel order). The car's nearest
    // point on it, then `left` metres along; MM2's route node only gives the
    // corner at the road's end, which a straight line from the car cuts across
    // the whole road (and across a grid that spans both halves of it).
    const city::AiPath* rd = road(0);
    const city::AiRoadSide* side = rd ? (m_roadDir[0] ? &rd->right : &rd->left) : nullptr;
    bool onLane = side && side->numLanes > 0 && !side->polylines.empty() && side->polylines[0].size() >= 2;
    if (onLane) {
        // A car at or past the lane's end (inside the junction) is not on it.
        const Vec3& end = side->polylines[0].back();
        const Vec3& prev = side->polylines[0][side->polylines[0].size() - 2];
        Vec3 ab = end - prev;
        ab.y = 0.0f;
        onLane = (pos - end).dot(ab) < 0.0f;
    }
    if (onLane) {
        const auto& lane = side->polylines[0];
        std::size_t bestSeg = 0;
        Vec3 bestPoint = lane[0];
        float bestD2 = 1e30f;
        for (std::size_t s = 0; s + 1 < lane.size(); ++s) {
            Vec3 ab = lane[s + 1] - lane[s];
            ab.y = 0.0f;
            const float l2 = ab.dot(ab);
            const float t = l2 > 1e-6f ? std::clamp((pos - lane[s]).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
            const Vec3 q = lane[s] + (lane[s + 1] - lane[s]) * t;
            const float dx = q.x - pos.x, dz = q.z - pos.z;
            if (dx * dx + dz * dz < bestD2) {
                bestD2 = dx * dx + dz * dz;
                bestSeg = s;
                bestPoint = q;
            }
        }
        // The racer's own place across the road (MM1 passes the car's DistToSide
        // to CalcCopRailPosition, so each racer keeps its own line instead of
        // all taking one): its offset from the lane line to the right of travel,
        // taken from its grid place once and kept, held in the driving half
        // (never left of the lane line, not past the road's right edge).
        const auto rightOf = [&](std::size_t s) {
            Vec3 t = lane[s + 1] - lane[s];
            const float l = std::sqrt(t.x * t.x + t.z * t.z);
            return l > 1e-5f ? Vec3{-t.z / l, 0.0f, t.x / l} : Vec3{0.0f, 0.0f, 0.0f};
        };
        // No lane change on the straight (MM1's racers hold their grid line until
        // the lead-in to the turn, where the junction target pulls them across):
        // the offset stays as the grid gave it, on either half of the road.
        if (m_mm1LaneOffset > 1e8f)
            m_mm1LaneOffset = (pos - bestPoint).dot(rightOf(bestSeg));
        // Only on the opening street (the grid's own line); after the first
        // turn the racers take the first or second lane on their side of the
        // road, 0 to 5 m from the lane line (as the user saw in MM1; the lane
        // spacing is 5 m in Chicago's roads).
        const bool grid = m_curLap <= 1 && m_wayPtIdx <= 1;
        const float o = grid ? std::clamp(m_mm1LaneOffset, -std::max(0.0f, rd->halfWidth - 0.5f),
                                          std::max(0.0f, rd->halfWidth - 5.0f))
                             : std::clamp(m_mm1LaneOffset, 0.0f, std::min(5.0f, std::max(0.0f, rd->halfWidth - 5.0f)));
        Vec3 cur = bestPoint;
        for (std::size_t s = bestSeg; s + 1 < lane.size(); ++s) {
            Vec3 d = lane[s + 1] - cur;
            d.y = 0.0f;
            const float rem = std::sqrt(d.dot(d));
            if (left <= rem && rem > 1e-4f)
                return cur + (lane[s + 1] - cur) * (left / rem) + rightOf(s) * o;
            left -= rem;
            cur = lane[s + 1];
        }
        from = cur + rightOf(lane.size() - 2) * o; // the lane's end: on along the route's own nodes
    } else {
        // No lane lines: the car's projection onto n[0] -> n[1], never behind n[0].
        Vec3 seg = to - from;
        seg.y = 0.0f;
        const float len2 = seg.dot(seg);
        if (len2 > 1e-4f) {
            const float t = std::clamp((pos - from).dot(seg) / len2, 0.0f, 1.0f);
            from = from + (to - from) * t;
        }
    }
    for (int i = 1; i < count; ++i) {
        to = n[static_cast<std::size_t>(i)].pos;
        Vec3 d = to - from;
        d.y = 0.0f;
        const float rem = std::sqrt(d.dot(d));
        if (left <= rem && rem > 1e-4f)
            return from + (to - from) * (left / rem);
        left -= rem;
        from = to;
    }
    return from; // the route's last node
}

void PhysicsDriver::solveShortcutTargetPoint() {
    // aiVehiclePhysics::SolveShortcutTargetPoint: straight for the next
    // waypoint intersection; on reaching it the window is rebuilt from the
    // waypoints after it and the car drives on (Forward). Past the last
    // waypoint: the destination, less the stop distance.
    const int n = static_cast<int>(m_wayPts.size());
    const Vec3 pos = m_car.body.ics.matrix.m3;
    if (m_wayPtIdx < n) {
        if (const Intersection* node = m_map->intersection(m_wayPts[static_cast<std::size_t>(m_wayPtIdx)]))
            m_target = node->centre;
        if (m_curCompType == kIntersectionComponent && m_curCompId == wayPoint(m_wayPts, m_wayPtIdx)) {
            const int i = m_wayPtIdx;
            for (int w = 0; w < 3; ++w)
                m_roads[w] = m_map->roadBetween(wayPoint(m_wayPts, i + w), wayPoint(m_wayPts, i + w + 1),
                                                &m_roadDir[w]);
            ++m_wayPtIdx;
            initRoadTurns();
            m_state = State::Forward;
            if (m_wayPtIdx == n && (m_curLap < m_numLaps || m_numLaps == -1)) {
                m_wayPtIdx = 0;
                ++m_curLap;
            }
        }
    } else if (params.stopShort == 0.0f) {
        m_target = m_dest;
    } else {
        const float dz = m_dest.z - pos.z, dx = m_dest.x - pos.x;
        const float d = std::sqrt(dz * dz + dx * dx);
        if (params.stopShort <= d) {
            const float t = (d - params.stopShort) / d;
            m_target = pos + (m_dest - pos) * t;
        } else {
            m_target = pos;
        }
    }
    m_bestRoute = 0;
    m_routeNodeCount[0] = 0;
    m_routes[0][0].road = 0;
    m_routes[0][0].turnCode = 0;
    m_target.y += 1.0f;
}

// --- the route enumeration ---------------------------------------------------------

const TrackedCar* PhysicsDriver::trackedById(int id) const {
    for (const TrackedCar& c : m_cars)
        if (c.id == id)
            return &c;
    for (const auto& [key, c] : m_propObstacles)
        if (c.id == id)
            return &c;
    return nullptr;
}

// Ids of the props as obstacles: beyond every car's.
constexpr int kPropObstacleIds = 1000000;

const TrackedCar* PhysicsDriver::propObstacle(int index, int component, bool onRoad) {
    const Pedestrians* props = m_map ? m_map->props() : nullptr;
    if (!props || index < 0 || static_cast<std::size_t>(index) >= props->obstacles().size())
        return nullptr;
    const auto key = std::make_tuple(index, component, onRoad);
    if (auto it = m_propObstacles.find(key); it != m_propObstacles.end())
        return &it->second;
    // aiBanger: the prop's ground origin (Position), its YRadius (Radius
    // caps it at 2) and centre (the instance's GetPosition); still, as the
    // pedestrians' lists keep it where it was placed (inferred: MM2 reads
    // the instance's current matrix).
    const PedObstacle& o = props->obstacles()[static_cast<std::size_t>(index)];
    TrackedCar t;
    t.id = kPropObstacleIds + static_cast<int>(m_propObstacles.size());
    t.position = o.origin;
    t.speed = 0.0f; // aiBanger::Speed
    t.prop = index;
    t.propComponent = component;
    t.propOnRoad = onRoad;
    t.propRadius = o.yRadius;
    t.propCentre = o.position;
    return &m_propObstacles.emplace(key, t).first->second;
}

const TrackedCar* PhysicsDriver::ambientObstacle(int index) const {
    // An entry of the traffic's obstacle lists (a Traffic car index) as this
    // frame's car.
    for (const TrackedCar& c : m_cars)
        if (c.ambient == index)
            return &c;
    return nullptr;
}

void PhysicsDriver::calcRoute(std::span<const TrackedCar> cars) {
    // aiVehiclePhysics::CalcRoute: node 0 at the car, then every route from
    // it (EnumRoutes), the straightest chosen (DetermineBestRoute).
    m_cars = cars;
    const Vec3 pos = m_car.body.ics.matrix.m3;
    RouteNode& n0 = m_nodes[0];
    n0.pos = {pos.x, pos.y + 1.0f, pos.z};
    n0.turnSum = 0.0f;
    n0.dist = 0.0f;
    n0.turnCode = 0;
    int slot = 0;
    int v = 0;
    if (const city::AiPath* p0 = road(0)) {
        const int n = static_cast<int>(p0->center.size());
        v = pathRoadVertice(*p0, pos, m_roadDir[0] ? 1 : 0);
        if (v == n) {
            if (const city::AiPath* p1 = road(1)) {
                slot = 1;
                const int n1 = static_cast<int>(p1->center.size());
                v = pathRoadVertice(*p1, pos, m_roadDir[1] ? 1 : 0);
                if (v == n1) {
                    if (const city::AiPath* p2 = road(2)) {
                        slot = 2;
                        v = pathRoadVertice(*p2, pos, m_roadDir[2] ? 1 : 0);
                    } else {
                        v = n1 - 1;
                    }
                }
            } else {
                v = n - 1;
            }
        }
    }
    n0.vert = v;
    n0.road = slot;
    m_numRoutes = 0;
    m_routeBlocked.fill(0);
    m_routeOnSidewalk.fill(0);
    if (m_wayPtIdx < static_cast<int>(m_wayPts.size()) && inSharpTurn(0) == 0) {
        n0.kind = 0;
        m_turnRefStart = pos;
    } else {
        n0.kind = 1;
    }
    m_turnRef = m_turnRefStart;
    calcRoadTurns();
    enumRoutes(1);
    determineBestRoute();
}

void PhysicsDriver::determineBestRoute() {
    // aiVehiclePhysics::DetermineBestRoute: the least turning; first among
    // the routes over the sidewalk when they are preferred, then among the
    // routes with a way round every obstacle, then among all.
    m_bestRoute = -1;
    auto pass = [&](auto&& eligible) {
        float best = 99999.0f;
        for (int r = 0; r < m_numRoutes; ++r) {
            if (!eligible(r))
                continue;
            const int count = m_routeNodeCount[static_cast<std::size_t>(r)];
            const auto ur = static_cast<std::size_t>(r);
            const float cost = m_routes[ur][static_cast<std::size_t>(count - 1)].turnSum;
            if (cost < best) {
                best = cost;
                m_bestRoute = r;
            }
        }
    };
    if (params.preferSidewalk)
        pass([&](int r) { return m_routeOnSidewalk[static_cast<std::size_t>(r)] != 0; });
    if (m_bestRoute == -1)
        pass([&](int r) { return m_routeBlocked[static_cast<std::size_t>(r)] == 0; });
    if (m_bestRoute == -1)
        pass([](int) { return true; });
}

void PhysicsDriver::nodeTurnAndDistance(int i) {
    // The turning and the path length to node i (SaveTarget,
    // SetTargetPtToDestination, SaveTurnTarget and the target functions all
    // end this way, the length first).
    RouteNode& n = m_nodes[static_cast<std::size_t>(i)];
    const RouteNode& p = m_nodes[static_cast<std::size_t>(i - 1)];
    if (i == 1) {
        const auto& m = m_car.body.ics.matrix;
        n.turnSum = angleBetween(-m.m2, n.pos - m.m3);
    } else {
        const RouteNode& pp = m_nodes[static_cast<std::size_t>(i - 2)];
        n.turnSum = angleBetween(p.pos - pp.pos, n.pos - p.pos) + p.turnSum;
    }
}

void PhysicsDriver::enumRoutes(int i) {
    // aiVehiclePhysics::EnumRoutes: node i from node i - 1, branching round
    // an obstacle in the way.
    if (i > kMaxNodes - 1)
        return;
    const auto ui = static_cast<std::size_t>(i);
    Vec3 start = m_nodes[ui - 1].pos;
    const int k = m_nodes[ui - 1].road;
    const city::AiPath* p = road(k);
    const PathInfo* info = roadInfo(k);
    const bool dir = k >= 0 && k <= 2 ? m_roadDir[k] : false;
    if (!m_toDestination) {
        if (p && info)
            ai::calcRoadTurns(*p, info->sharpTurns, m_turnRef, dir);
        int t = inSharpTurn(i - 1);
        if (t != 0) {
            m_nodes[ui - 1].turnCode = t - 1;
            calcSharpTurnTarget(i, i - 1);
        } else {
            m_turnRef = m_nodes[ui - 1].pos;
            calcRoadTarget(i, start);
            t = inSharpTurn(i);
            if (t != 0) {
                m_nodes[static_cast<std::size_t>(i)].turnCode = t - 1;
                calcSharpTurnTarget(i, i);
            }
        }
    } else if (!p) {
        setTargetPtToDestination(i);
    } else {
        if (info)
            ai::calcRoadTurns(*p, info->sharpTurns, m_turnRef, dir);
        int t = inSharpTurn(i - 1);
        if (t != 0) {
            m_nodes[ui - 1].turnCode = t - 1;
            if (calcSharpTurnTarget(i, i - 1) != 0) {
                RouteNode& n = m_nodes[static_cast<std::size_t>(i)];
                n.turnCode += 1;
                m_turnRef = n.pos;
                if (const city::AiPath* q = road(n.road); q && pathIsPosOnRoad(*q, n.pos, 2.0f) > 1)
                    calcDestinationTarget(i, start);
            }
        } else {
            m_turnRef = m_nodes[ui - 1].pos;
            calcDestinationTarget(i, start);
            t = inSharpTurn(i);
            if (t != 0) {
                m_nodes[static_cast<std::size_t>(i)].turnCode = t - 1;
                calcSharpTurnTarget(i, i);
            }
        }
    }
    if (i > kMaxNodes - 1)
        return;
    const auto u = static_cast<std::size_t>(i);
    m_nodes[u].surface = 1;
    int kind = 0;
    const TrackedCar* obs =
        isTargetBlocked(m_nodes[u - 1].pos, m_nodes[u].pos, m_nodes[u - 1].vert, m_nodes[u - 1].road,
                        m_nodes[u].vert + 3, m_nodes[u].road, 2.0f * (m_backBumper + m_frontBumper), &kind);
    if (!obs) {
        continueCheck(i);
        return;
    }
    const Vec3 at = obs->position;
    if (kind == 2 && (m_wayPtIdx <= 2 || m_nodes[u - 1].kind == 1)) {
        continueCheck(i);
        return;
    }
    const float dx = m_nodes[u - 1].pos.x - at.x, dz = m_nodes[u - 1].pos.z - at.z;
    if (dz * dz + dx * dx >= params.lookAhead * params.lookAhead) {
        continueCheck(i);
        return;
    }
    Vec3 pts[10];
    int obstacles[10] = {};
    int surfaces[10] = {};
    int kinds[10] = {};
    const int m = calcObstacleAvoidPoints(*obs, i, m_vehicleType != 3, pts, obstacles, surfaces, kinds);
    for (int j = 0; j < m; ++j) {
        const TrackedCar* o = trackedById(obstacles[j]);
        saveTarget(i, pts[j], o ? *o : *obs, surfaces[j], kind, kinds[j]);
    }
}

void PhysicsDriver::saveTarget(int i, const Vec3& pt, const TrackedCar& obstacle, int surface, int blockKind,
                               int kind) {
    // aiVehiclePhysics::SaveTarget: node i at an avoid point, on the road the
    // obstacle is on.
    int vert = 0;
    int slot = obstacleRoadIdx(obstacle, &vert);
    const city::AiPath* p = road(slot);
    if (!p) {
        --slot;
        p = road(slot);
        vert = p ? static_cast<int>(p->center.size()) - 1 : 0;
    }
    if (p)
        vert = std::clamp(vert, 0, static_cast<int>(p->center.size()) - 1);
    RouteNode& n = m_nodes[static_cast<std::size_t>(i)];
    const RouteNode& prev = m_nodes[static_cast<std::size_t>(i - 1)];
    n.kind = kind;
    n.blockKind = blockKind;
    n.obstacle = obstacle.id;
    n.surface = surface;
    n.pos = pt;
    n.pos.y += 1.0f;
    n.dist = xzDistance(prev.pos, n.pos) + prev.dist;
    nodeTurnAndDistance(i);
    n.vert = vert;
    n.road = slot;
    continueCheck(i);
}

void PhysicsDriver::continueCheck(int i) {
    // aiVehiclePhysics::ContinueCheck: plan on from node i while the route
    // is shorter than the look-ahead (on to the destination once its road is
    // reached), else keep it as a candidate.
    const RouteNode& n = m_nodes[static_cast<std::size_t>(i)];
    const city::AiPath* p = road(n.road);
    if (n.dist < params.lookAhead && p && m_numRoutes < 10) {
        const int verts = static_cast<int>(p->center.size());
        if (n.road == m_numWayPtRoads || (n.vert >= verts - 1 && n.road + 1 <= 2 && !road(n.road + 1))) {
            m_toDestination = true;
            enumRoutes(i + 1);
            return;
        }
    }
    if (n.dist < params.lookAhead && road(2) && m_numRoutes < 10) {
        const int verts2 = static_cast<int>(road(2)->center.size());
        if (n.road != 2 || n.vert != verts2) {
            enumRoutes(i + 1);
            return;
        }
    }
    // Commit (no bound check in MM2; the routes hold 25).
    if (m_numRoutes >= kMaxRoutes)
        return;
    const auto r = static_cast<std::size_t>(m_numRoutes);
    for (int k = 0; k <= i; ++k) {
        const RouteNode& node = m_nodes[static_cast<std::size_t>(k)];
        m_routes[r][static_cast<std::size_t>(k)] = node;
        if (node.surface == 2)
            m_routeOnSidewalk[r] = 1;
        if (node.kind == 3)
            m_routeBlocked[r] = 1;
    }
    m_routeNodeCount[r] = i + 1;
    ++m_numRoutes;
}

void PhysicsDriver::setTargetPtToDestination(int i) {
    // aiVehiclePhysics::SetTargetPtToDestination: the destination (less the
    // stop distance), marked by a path length of 9999.
    RouteNode& n = m_nodes[static_cast<std::size_t>(i)];
    const RouteNode& p = m_nodes[static_cast<std::size_t>(i - 1)];
    if (params.stopShort == 0.0f) {
        n.pos = m_dest;
    } else {
        const float d = xzDistance(p.pos, m_dest);
        if (d < params.stopShort) {
            n.pos = p.pos;
        } else {
            const float t = (d - params.stopShort) / d;
            n.pos = p.pos + (m_dest - p.pos) * t;
        }
    }
    n.pos.y += 1.0f;
    n.kind = p.kind;
    n.vert = p.vert;
    n.road = p.road;
    n.turnCode = p.turnCode;
    n.dist = 9999.0f;
    nodeTurnAndDistance(i);
}

// --- obstacles ------------------------------------------------------------------------

int PhysicsDriver::obstacleRoadIdx(const TrackedCar& o, int* vert) const {
    // aiObstacle::CurrentRoadIdx of each kind of car, against this car's
    // window.
    const bool dirs[3] = {m_roadDir[0], m_roadDir[1], m_roadDir[2]};
    if (o.prop >= 0) {
        // aiBanger::CurrentRoadIdx: on a road, the first window slot with
        // that road (the vertex ahead of the prop's centre there,
        // aiPath::RoadVertice by the slot's direction); in an intersection,
        // the slot after a road (of the first two) arriving there, vertex 1,
        // else slot 0 when the first road leaves from it; else none.
        if (o.propOnRoad) {
            for (int i = 0; i < 3; ++i) {
                if (m_roads[i] < 0 || m_roads[i] != o.propComponent)
                    continue;
                const city::AiPath* p = m_map ? m_map->path(o.propComponent) : nullptr;
                *vert = p ? pathRoadVertice(*p, o.propCentre, dirs[i] ? 1 : -1) : 0;
                return i;
            }
            return -1;
        }
        for (int i = 0; i < 2; ++i) {
            const PathInfo* info = roadInfo(i);
            if (!info)
                continue;
            if (info->intersection[dirs[i] ? 0 : 1] == o.propComponent) {
                *vert = 1;
                return i + 1;
            }
        }
        const PathInfo* first = roadInfo(0);
        if (first && first->intersection[dirs[0] ? 1 : 0] == o.propComponent) {
            *vert = 1;
            return 0;
        }
        return -1;
    }
    if (o.racer)
        return o.racer->currentRoadIdx(m_roads, dirs, vert);
    if (o.ambient >= 0 && m_map && m_map->traffic())
        return m_map->traffic()->currentRoadIdx(o.ambient, m_roads, dirs, vert);
    if (o.isPlayer) {
        // aiVehiclePlayer::CurrentRoadIdx: the road it was last found on and
        // its vertex there (counted from vertex 0 whatever the direction).
        if (o.playerRoad < 0)
            return -1;
        for (int i = 0; i < 3; ++i) {
            if (m_roads[i] != o.playerRoad)
                continue;
            const city::AiPath* p = m_map->path(o.playerRoad);
            const int last = p ? static_cast<int>(p->center.size()) - 1 : 0;
            *vert = o.playerVert;
            if (o.playerVert != last)
                return i;
            if (road(i + 1)) {
                *vert = 0;
                return i + 1;
            }
            *vert = last;
            return i;
        }
        return -1;
    }
    return -1;
}

const TrackedCar* PhysicsDriver::isTargetBlocked(const Vec3& from, const Vec3& to, int startVert,
                                                  int startSlot, int endVert, int endSlot, float extra,
                                                  int* kind) {
    // aiVehiclePhysics::IsTargetBlocked: the nearest obstacle in the way,
    // from the obstacle lists of the window's roads step by step (step 0 the
    // intersection a road is entered from, step v its v-th section in the
    // direction of travel) until the step with the end point or the first
    // step with a hit; then the players and, past the second waypoint, the
    // other racers. Police are never obstacles.
    const float width = m_leftSide + m_rightSide;
    float best = 99999.0f;
    const TrackedCar* result = nullptr;
    auto test = [&](const TrackedCar& o, int k) {
        const float d = blockingDistance(o, from, to, extra, width);
        if (d > -1.0f && d < best) {
            best = d;
            result = &o;
            *kind = k;
        }
    };
    const Traffic* traffic = params.avoidTraffic && m_map ? m_map->traffic() : nullptr;
    const Pedestrians* props = params.avoidProps && m_map ? m_map->props() : nullptr;
    // A prop of a list counts only when nothing short of a car can break it
    // (aiBanger::BreakThreshold over 250 000), except on the side -1 lists,
    // which MM2 tests without the threshold (as coded).
    auto testProps = [&](std::span<const int> list, int component, bool onRoad, bool threshold) {
        for (int i : list) {
            if (threshold && !(250000.0f < props->obstacles()[static_cast<std::size_t>(i)].impulseLimit2))
                continue;
            if (const TrackedCar* o = propObstacle(i, component, onRoad))
                test(*o, 5);
        }
    };
    if (const city::AiPath* pe = road(endSlot); pe && endVert > static_cast<int>(pe->center.size())) {
        ++endSlot;
        endVert = 1;
    }
    if (traffic || props) {
        int v = startVert;
        bool done = false;
        for (int r = startSlot; r <= endSlot && !done; ++r) {
            const city::AiPath* p = road(r);
            const PathInfo* info = roadInfo(r);
            if (!p || !info)
                continue; // (v is not reset for the next road)
            const int n = static_cast<int>(p->center.size());
            const bool fwd = r <= 2 ? m_roadDir[r] : false;
            const int roadId = r == 3 ? m_turnsRoad : m_roads[r];
            for (; v < n; ++v) {
                if (v == 0) {
                    // The intersection the road is entered from: its cars,
                    // then its props (none in play: aiMap::Reset empties
                    // that list).
                    const int node = info->intersection[fwd ? 1 : 0];
                    if (traffic)
                        for (int id : traffic->intersectionVehicles(node))
                            if (const TrackedCar* o = ambientObstacle(id))
                                test(*o, 1);
                    if (props)
                        testProps(props->nodeObstacles(node), node, false, true);
                } else {
                    // The section's side-1 cars, side-1 props, side -1 cars
                    // and side -1 props, in MM2's order.
                    if (traffic)
                        for (int id : traffic->roadVehicles(roadId, 1, fwd ? v : n - v))
                            if (const TrackedCar* o = ambientObstacle(id))
                                test(*o, 1);
                    if (props)
                        testProps(props->sectionObstacles(roadId, fwd ? v : n - v, 1), roadId, true, true);
                    if (traffic)
                        for (int id : traffic->roadVehicles(roadId, -1, fwd ? n - v : v))
                            if (const TrackedCar* o = ambientObstacle(id))
                                test(*o, 1);
                    if (props)
                        testProps(props->sectionObstacles(roadId, fwd ? v : n - v, -1), roadId, true, false);
                }
                if ((v == endVert && r == endSlot) || result) {
                    done = true;
                    break;
                }
            }
            v = 0;
        }
    }
    if (params.avoidPlayers) {
        for (const TrackedCar& o : m_cars) {
            if (!o.isPlayer || o.id == m_selfId)
                continue;
            int vert = 0;
            if (obstacleRoadIdx(o, &vert) > -1)
                test(o, 0);
        }
    }
    if (params.avoidOpponents && m_wayPtIdx > 2) {
        for (const TrackedCar& o : m_cars) {
            if (!o.racer || o.racerIndex == m_aiId)
                continue;
            int vert = 0;
            if (obstacleRoadIdx(o, &vert) > -1)
                test(o, 2);
        }
    }
    return result;
}

int PhysicsDriver::calcObstacleAvoidPoints(const TrackedCar& obstacle, int i, bool allowSidewalk, Vec3* pts,
                                           int* obs, int* surface, int* kind) {
    // aiVehiclePhysics::CalcObstacleAvoidPoints: the obstacle's corners
    // pushed out left and right (aiVehicle::PreAvoid), each searched for a
    // way past (EnumTargets); none: the point stays, marked as having no
    // way round.
    const auto ui = static_cast<std::size_t>(i);
    const RouteNode& prev = m_nodes[ui - 1];
    if (prev.kind == 1) {
        // (MM2 leaves the surface unset here.)
        pts[0] = m_nodes[ui].pos;
        kind[0] = 1;
        obs[0] = obstacle.id;
        surface[0] = 1;
        return 1;
    }
    const auto& m = m_car.body.ics.matrix;
    Vec3 from, dir;
    if (i == 1) {
        from = m_nodes[0].pos;
        dir = -m.m2;
    } else {
        from = prev.pos;
        dir = prev.pos - m_nodes[ui - 2].pos;
    }
    Vec3 left, right;
    avoidPoints(obstacle, from, dir, m_rightSide + 2.0f, left, right);
    int count = 0;
    // Is a point ahead (within 1.57 rad) in the car's frame (node 1) or the
    // road's frame at the previous node?
    auto ahead = [&](const Vec3& pt, bool mirrored) {
        const Vec3 d = pt - prev.pos;
        float lat, lon;
        if (i == 1) {
            lat = d.x * m.m0.x + d.z * m.m0.z;
            lon = d.x * -m.m2.x + d.z * -m.m2.z;
        } else {
            const city::AiPath* p = road(prev.road);
            if (!p)
                return false;
            const int n = static_cast<int>(p->center.size());
            const bool fwd = prev.road <= 2 ? m_roadDir[prev.road] : false;
            // The right-hand point reads the vertex from the other end (as
            // coded).
            int k = prev.vert;
            bool useFwd = fwd;
            if (mirrored)
                k = fwd ? n - 1 - prev.vert : prev.vert;
            else
                k = fwd ? prev.vert : n - 1 - prev.vert;
            k = std::clamp(k, 0, n - 1);
            const Vec3& x = p->xAxis[static_cast<std::size_t>(k)];
            const Vec3& z = p->zAxis[static_cast<std::size_t>(k)];
            if (useFwd) {
                lat = -(d.x * x.x + d.z * x.z);
                lon = -(d.x * z.x + d.z * z.z);
            } else {
                lat = d.x * x.x + d.z * x.z;
                lon = d.x * z.x + d.z * z.z;
            }
        }
        const float a = libm::atan2(lat, lon);
        return a < 1.57f && a > -1.57f;
    };
    if (ahead(left, false))
        enumTargets(left, obstacle, i, prev.road, prev.vert, -1, allowSidewalk, 0, pts, obs, surface, kind,
                    &count);
    if (ahead(right, true))
        enumTargets(right, obstacle, i, prev.road, prev.vert, 1, allowSidewalk, 0, pts, obs, surface, kind,
                    &count);
    if (count == 0) {
        pts[0] = m_nodes[ui].pos;
        obs[0] = obstacle.id;
        kind[0] = 3;
        surface[0] = 1;
        return 1;
    }
    return count;
}

void PhysicsDriver::enumTargets(const Vec3& pt, const TrackedCar& obstacle, int i, int roadSlot, int roadVert,
                                int side, bool allowSidewalk, int depth, Vec3* pts, int* obs, int* surface,
                                int* kind, int* count) {
    // aiVehiclePhysics::EnumTargets: `pt` passes `obstacle` on `side` if it
    // is on the road (or the sidewalk, when allowed) and nothing else is in
    // the way; a further car in the way is passed on the same side, ten
    // deep.
    if (++depth == 10 || *count >= 10)
        return;
    int obsVert = 0;
    const int obsSlot = obstacleRoadIdx(obstacle, &obsVert);
    const city::AiPath* p = road(obsSlot);
    if (obsSlot < 0 || !p)
        return;
    const int surf = pathIsPosOnRoad(*p, pt, m_rightSide);
    if (!(surf == 1 || (surf == 2 && allowSidewalk)))
        return;
    const auto ui = static_cast<std::size_t>(i);
    const RouteNode& prev = m_nodes[ui - 1];
    int blockKind = 0;
    const TrackedCar* blk = isTargetBlocked(prev.pos, pt, roadVert, roadSlot, obsVert + 2, obsSlot,
                                            2.0f * (m_backBumper + m_frontBumper), &blockKind);
    auto append = [&](const Vec3& at, const TrackedCar& o) {
        if (*count >= 10)
            return;
        const auto k = static_cast<std::size_t>(*count);
        obs[k] = o.id;
        pts[k] = at;
        surface[k] = surf;
        kind[k] = 2;
        ++*count;
    };
    if (!blk || blockKind == 2) {
        append(pt, obstacle);
        return;
    }
    const auto& m = m_car.body.ics.matrix;
    Vec3 from, dir;
    if (i == 1) {
        from = m_nodes[0].pos;
        dir = -m.m2;
    } else {
        from = prev.pos;
        dir = prev.pos - m_nodes[ui - 2].pos;
    }
    Vec3 left, right;
    avoidPoints(*blk, from, dir, m_rightSide + 2.0f, left, right);
    // Ahead within 1.57 rad in the road's frame at the planning vertex.
    auto ahead = [&](const Vec3& q) {
        const city::AiPath* rp = road(roadSlot);
        if (!rp)
            return false;
        const int n = static_cast<int>(rp->center.size());
        const bool fwd = roadSlot <= 2 ? m_roadDir[roadSlot] : false;
        const int k = std::clamp(fwd ? roadVert : n - 1 - roadVert, 0, n - 1);
        const Vec3& x = rp->xAxis[static_cast<std::size_t>(k)];
        const Vec3& z = rp->zAxis[static_cast<std::size_t>(k)];
        const Vec3 d = q - prev.pos;
        const float lat = fwd ? -(d.x * x.x + d.z * x.z) : d.x * x.x + d.z * x.z;
        const float lon = fwd ? -(d.x * z.x + d.z * z.z) : d.x * z.x + d.z * z.z;
        const float a = libm::atan2(lat, lon);
        return a < 1.57f && a > -1.57f;
    };
    // The gap before a further ambient car more than 15 m on, nearer the
    // car than the first, within the look-ahead and ahead along the road.
    const Vec3 pos = m.m3;
    const Vec3 a = obstacle.position, b = blk->position;
    const float abx = a.x - b.x, abz = a.z - b.z;
    const float bcx = b.x - pos.x, bcz = b.z - pos.z;
    const float acx = a.x - pos.x, acz = a.z - pos.z;
    if (abz * abz + abx * abx > 225.0f && bcz * bcz + bcx * bcx < acz * acz + acx * acx) {
        const Vec3 gap = side == -1 ? right : left;
        const float px = prev.pos.x - b.x, pz = prev.pos.z - b.z;
        if (ahead(gap) && pz * pz + px * px < params.lookAhead * params.lookAhead && blockKind == 1)
            append(gap, *blk);
    }
    const Vec3 next = side == -1 ? left : right;
    if (ahead(next))
        enumTargets(next, *blk, i, roadSlot, roadVert, side, allowSidewalk, depth, pts, obs, surface, kind,
                    count);
}

} // namespace mm2::ai
