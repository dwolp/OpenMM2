// Ambient traffic after MM2's aiVehicleAmbient and its goals (build 3393,
// MM2Recomp, documentation only); see Traffic.h and docs/ai.md.
// Also (MM2 names, docs/parity/mm2/ai.md): aiMap::RemoveAmbient (placeCar
// takes the pool's head), aiMap::FindAmbAppRoad (m_pathActive),
// aiPath::RemAmbPlayer, aiPath::RemoveAmbVehicle, aiPath::SubSectionDist,
// aiPath::SubSectionLength, aiPath::AddVehicle, aiPath::RemoveVehicle,
// aiPath::Reset, aiIntersection::AddVehicle, aiIntersection::RemoveVehicle,
// aiIntersection::AddToStopSignCntl, aiIntersection::RemoveFromStopSignCntl,
// aiIntersection::RemoveTotalFromStopSignCntl, aiIntersection::Reset,
// aiVehicleSpline::Reset, aiVehicleSpline::CurrentLane,
// aiVehicleSpline::CurrentRoadId, aiVehicleSpline::CurrentRdVert,
// aiVehicleSpline::TotLength, aiVehicleSpline::InAccident,
// aiVehicle::Init, aiVehicle::Reset, aiVehicle::Update, aiGoal::Update,
// aiGoalRandomDrive::aiGoalRandomDrive, aiGoalRandomDrive::Init,
// aiGoalRegainRail::aiGoalRegainRail, aiGoalRegainRail::Init,
// aiGoalAvoidPlayer::aiGoalAvoidPlayer, aiGoalAvoidPlayer::Init,
// aiGoalCollision::aiGoalCollision, aiGoalCollision::Init,
// aiVehicleAmbient::aiVehicleAmbient, aiRailSet::aiRailSet,
// aiRailSet::CalcRailPosOrient, aiRailSet::CalcXZPosOrient,
// aiObstacle::InAccident.
#include "ai/Traffic.h"

#include "core/Libm.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mm2::ai {
namespace {

constexpr float kNoDistance = 9999.0f;

Vec2 xz(const Vec3& v) {
    return {v.x, v.z};
}

float sign(float v) {
    return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

// aiRailSet::ComputeXZCurve: Hermite coefficients, summed in MM2's order.
void hermite(float p0, float p1, float m0, float m1, float out[4]) {
    out[0] = ((-2.0f * p1 + m0) + m1) + 2.0f * p0;
    out[1] = ((-m1 + -2.0f * m0) + 3.0f * p1) + -3.0f * p0;
    out[2] = m0;
    out[3] = p0;
}

float cubic(const float k[4], float t) {
    return ((t * k[0] + k[1]) * t + k[2]) * t + k[3];
}

float cubicSlope(const float k[4], float t) {
    return ((t * k[0]) * 3.0f + (k[1] + k[1])) * t + k[2];
}

// Hermite point on (p0, p1, m0, m1) at t, XZ only (aiRailSet::CalcXZPosition;
// t below 0 is clamped).
Vec3 hermitePoint(const Vec3& p0, const Vec3& p1, const Vec3& m0, const Vec3& m1, float t,
                  Vec3* direction = nullptr) {
    float kx[4], kz[4];
    hermite(p0.x, p1.x, m0.x, m1.x, kx);
    hermite(p0.z, p1.z, m0.z, m1.z, kz);
    t = std::max(t, 0.0f);
    if (direction)
        *direction = {cubicSlope(kx, t), 0.0f, cubicSlope(kz, t)};
    return {cubic(kx, t), lerp(p0.y, p1.y, clampf(t, 0.0f, 1.0f)), cubic(kz, t)};
}

float manhattanXZ(const Vec3& a, const Vec3& b) {
    return std::abs(a.z - b.z) + std::abs(a.x - b.x);
}

Mat34 frameFromRows(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& pos) {
    Mat34 m;
    m.m0 = a;
    m.m1 = b;
    m.m2 = c;
    m.m3 = pos;
    return m;
}

} // namespace

PlayerCar PlayerCar::at(const Vec3& pos, const Vec3& vel) {
    PlayerCar p;
    Vec3 f{vel.x, 0.0f, vel.z};
    const float yaw = f.mag2() > 0.01f ? libm::atan2(-f.x, -f.z) : 0.0f;
    // Mat34::rotationY(a) faces (-sin a, 0, -cos a): forward (-Z) along the
    // velocity (still, it faces -Z).
    p.transform = Mat34::rotationY(yaw);
    p.transform.m3 = pos;
    p.velocity = vel;
    return p;
}

// --- Construction ----------------------------------------------------------

Traffic::Traffic(const RoadNetwork& network, TrafficLights& lights, std::vector<VehicleData> types,
                 const TrafficSettings& settings, std::uint64_t seed)
    : m_net(network), m_lights(lights), m_types(std::move(types)), m_settings(settings), m_seed(seed),
      m_ownRng(seed), m_rng(&m_ownRng) {
    init();
}

Traffic::Traffic(const RoadNetwork& network, TrafficLights& lights, std::vector<VehicleData> types,
                 const TrafficSettings& settings, Random& random)
    : m_net(network), m_lights(lights), m_types(std::move(types)), m_settings(settings), m_rng(&random) {
    init();
}

void Traffic::init() {
    if (m_types.empty()) {
        VehicleData d;
        d.model = "va_sedans_s";
        m_types.push_back(d);
    }
    // aiMap::Init: AIMAP +0x3c = clamp(density, 0, 1) * 0.2; no vehicles at
    // all when the density is 0.
    const float density = clampf(m_settings.density, 0.0f, 1.0f);
    m_density = density * kAmbientDensityScale;
    // A rebuild (setDensity) starts from an empty free pool.
    m_pool.clear();
    const int pool = std::max(0, m_settings.poolSize);
    const int count = density == 0.0f ? 0 : pool;
    m_cars.resize(static_cast<std::size_t>(count));
    // Constructors of the vehicle array: aiRailSet (lane randomness), then
    // aiVehicleSpline (reaction ticks), for every car. MM2 builds the array
    // of the state's vehicle count before it looks at the density, so at
    // density 0 the constructors still draw (aiMap::Init sets the count to 0
    // after them).
    for (int i = 0; i < pool; ++i) {
        const float lane = m_rng->frand();
        const float react = m_rng->frand();
        if (i >= count)
            continue;
        Car& c = m_cars[static_cast<std::size_t>(i)];
        c.laneRandomness = libm::sin(lane * 6.2831f) * 0.5f;
        c.totReactTicks = 8 - static_cast<int>(react * -17.0f);
    }
    // Then per car: its type, aiVehicleAmbient::Init: aiVehicleSpline::Init
    // builds the aiVehicleInstance, whose ctor keeps an arbitrary number
    // (lvlInstance +0x18, the indicators' blink phase) and draws a paint job
    // (SetColor); Init then calls SetColor again, so the second draw is the
    // car's paint; then the aiGoalRandomDrive ctor.
    float exceedCounter = 0.0f;
    for (auto& c : m_cars) {
        c.type = pickType();
        // MM2's number is irand(int), a stateless LCG step, of the instance's
        // own address (no draw from the shared seed). OpenMM2 takes the same
        // step of the car's index scaled by the instance size, a stable
        // stand-in for its address (inferred).
        const auto index = static_cast<std::uint32_t>(&c - m_cars.data());
        c.blinkPhase = static_cast<int>(((index * 0x40u) * 214013u + 2531011u) >> 16 & 0x7FFFu);
        (void)m_rng->frand(); // aiVehicleInstance ctor's SetColor, overwritten below
        c.paint = m_rng->frand();
        c.exceedLimit = exceedCounter + exceedCounter;
        exceedCounter -= 1.0f;
        if (exceedCounter < 0.0f)
            exceedCounter = 4.0f;
        const float f = m_rng->frand(); // one draw for both
        c.accelFactor = f * 3.0f + 5.0f;
        c.separation = f * 2.5f + 0.5f;
        // aiVehicleSpline::Init: bumper and side distances from the box bound
        // (aiVehicleManager::AddVehicleDataEntry: centred at CG, Size extents).
        const VehicleData& d = m_types[static_cast<std::size_t>(c.type)];
        // aiVehicleSpline::Init keeps a vehicle named exactly "vabus" at
        // lane randomness -0.5 (no retail type has that name).
        if (d.model == "vabus")
            c.laneRandomness = -0.5f;
        c.backBumper = d.cg.z + d.size.z * 0.5f;
        c.frontBumper = d.size.z * 0.5f - d.cg.z;
        c.leftSide = d.size.x * 0.5f - d.cg.x;
        c.rightSide = d.cg.x + d.size.x * 0.5f;
    }
    // aiMap::Reset: every car into the free pool, the last one on top.
    for (int i = 0; i < count; ++i)
        m_pool.push_back(i);
    m_queues.resize(m_net.lanes().size());
    m_pathActive.assign(m_net.paths().size(), 0);
    m_stopWaiting.resize(m_net.intersections().size());
    m_nodeObstacles.resize(m_net.intersections().size());
    m_roadObstacles.resize(m_net.paths().size());
    if (const city::AiMap* src = m_net.source()) {
        for (std::size_t p = 0; p < m_roadObstacles.size() && p < src->paths.size(); ++p)
            for (auto& side : m_roadObstacles[p])
                side.resize(src->paths[p].center.size());
    }
    m_alwaysStop.assign(m_net.paths().size(), 0);
    m_stopAllowed.resize(m_net.intersections().size());
}

int Traffic::pickType() {
    // aiMap::Init: the first type whose cumulative probability exceeds frand.
    const float r = m_rng->frand();
    for (const auto& t : m_settings.types) {
        if (r < t.cumulative) {
            for (std::size_t i = 0; i < m_types.size(); ++i)
                if (str::iequals(m_types[i].model, t.model))
                    return static_cast<int>(i);
            break;
        }
    }
    return static_cast<int>(m_types.size()) - 1;
}

// --- Lane geometry (aiPath helpers) ----------------------------------------

const Lane* Traffic::laneOf(int path, int dir, int lane) const {
    const int id = m_net.lane(path, dir, lane);
    return id >= 0 ? &m_net.lanes()[static_cast<std::size_t>(id)] : nullptr;
}

float Traffic::laneLength(int path, int dir, int lane) const {
    const Lane* l = laneOf(path, dir, lane);
    return l ? l->line.length : 0.0f;
}

Vec3 Traffic::xAxisAt(int path, int dir, int i) const {
    const auto& x = m_net.paths()[static_cast<std::size_t>(path)].xAxis;
    if (x.empty())
        return {};
    const int n = static_cast<int>(x.size());
    const int k = dir == 1 ? i : n - 1 - i;
    return x[static_cast<std::size_t>(std::clamp(k, 0, n - 1))];
}

// Lane vertex i in travel order, moved sideways by the car's lane randomness.
Vec3 Traffic::lanePoint(const Car& c, int path, int dir, int lane, int i) const {
    const Lane* l = laneOf(path, dir, lane);
    if (!l || l->line.points.empty())
        return {};
    const auto& pts = l->line.points;
    const Vec3 p = pts[static_cast<std::size_t>(std::clamp(i, 0, static_cast<int>(pts.size()) - 1))];
    return p + xAxisAt(path, dir, i) * c.laneRandomness;
}

// aiPath::SubSectionDir: wAxis (the segment direction), unnormalised, scaled.
Vec3 Traffic::subSectionDir(int path, int dir, int i, float scale) const {
    const auto& w = m_net.source()->paths[static_cast<std::size_t>(path)].wAxis;
    const int n = static_cast<int>(w.size());
    if (n == 0)
        return {};
    if (dir == 1)
        return w[static_cast<std::size_t>(std::clamp(i, 0, n - 1))] * scale;
    return w[static_cast<std::size_t>(std::clamp(n - 1 - i, 0, n - 1))] * -scale;
}

// aiPath::IntersectionEntryVector: the travel direction at the road's end.
Vec3 Traffic::entryVector(int path, int dir, float scale) const {
    const auto& z = m_net.source()->paths[static_cast<std::size_t>(path)].zAxis;
    if (z.empty())
        return {};
    return dir == 1 ? z.back() * -scale : z.front() * scale;
}

// aiPath::IntersectionExitVector: the travel direction at the road's start.
Vec3 Traffic::exitVector(int path, int dir, float scale) const {
    const auto& z = m_net.source()->paths[static_cast<std::size_t>(path)].zAxis;
    if (z.empty())
        return {};
    return dir == 1 ? z.front() * -scale : z.back() * scale;
}

// aiPath::IntersectionEntryPt: the point `d` short of the lane's end. MM2
// measures the last segment on lane 0 whatever the lane.
Vec3 Traffic::entryPoint(int path, int dir, int lane, float d) const {
    const Lane* l = laneOf(path, dir, lane);
    const Lane* l0 = laneOf(path, dir, 0);
    if (!l || l->line.points.size() < 2)
        return l && !l->line.points.empty() ? l->line.points.back() : Vec3{};
    const auto& pts = l->line.points;
    const std::size_t n = pts.size();
    const auto& cum0 = (l0 ? l0 : l)->line.distances;
    const float len = cum0[n - 1] - cum0[n - 2];
    const float t = len > 0.0f ? (len - d) / len : 0.0f;
    return (pts[n - 1] - pts[n - 2]) * t + pts[n - 2];
}

// aiPath::SubSectionPt: the point `d` back from vertex i.
Vec3 Traffic::subSectionPoint(int path, int dir, int lane, int i, float d) const {
    const Lane* l = laneOf(path, dir, lane);
    if (!l || l->line.points.size() < 2)
        return {};
    const int n = static_cast<int>(l->line.points.size());
    i = std::clamp(i, 1, n - 1);
    const Vec3& p1 = l->line.points[static_cast<std::size_t>(i)];
    const Vec3& p0 = l->line.points[static_cast<std::size_t>(i - 1)];
    const auto& cum = l->line.distances;
    const float seg = cum[static_cast<std::size_t>(i)] - cum[static_cast<std::size_t>(i - 1)];
    const float t = seg > 0.0f ? 1.0f - d / seg : 0.0f;
    return (p1 - p0) * t + p0;
}

// aiPath::Index: the end vertex of the segment holding `dist`.
int Traffic::index(int path, int dir, int lane, float dist) const {
    const Lane* l = laneOf(path, dir, lane);
    if (!l || l->line.points.size() < 2)
        return 1;
    const auto& cum = l->line.distances;
    const int n = static_cast<int>(cum.size());
    dist = clampf(dist, 0.0f, cum.back());
    for (int i = 1; i < n; ++i)
        if (dist <= cum[static_cast<std::size_t>(i)] + 1e-5f)
            return i;
    return n - 1;
}

float Traffic::subSectionDist(int path, int dir, int lane, float dist) const {
    const Lane* l = laneOf(path, dir, lane);
    if (!l || l->line.points.size() < 2)
        return -1.0f;
    const int i = index(path, dir, lane, dist);
    return clampf(dist, 0.0f, l->line.length) - l->line.distances[static_cast<std::size_t>(i - 1)];
}

// Length of the turn from the car's lane end to its next lane: the
// Manhattan XZ distance between the two points (aiGoalRandomDrive).
float Traffic::turnLength(const Car& c) const {
    if (c.nextPath < 0)
        return 0.0f;
    const Lane* lane = laneOf(c.path, c.dir, c.lane);
    const int n = lane ? static_cast<int>(lane->line.points.size()) : 1;
    const Vec3 p0 =
        entryPoint(c.path, c.dir, c.lane, c.frontBumper) + xAxisAt(c.path, c.dir, n - 1) * c.laneRandomness;
    const Vec3 p1 = lanePoint(c, c.nextPath, c.nextDir, c.nextLane, 0);
    return manhattanXZ(p0, p1);
}

void Traffic::setCurve(Car& c, const Vec3& p0, const Vec3& p1, const Vec3& m0, const Vec3& m1) {
    hermite(p0.x, p1.x, m0.x, m1.x, c.curve[0]);
    hermite(p0.z, p1.z, m0.z, m1.z, c.curve[1]);
    c.turnY0 = p0.y;
    c.turnY1 = p1.y;
}

// aiRailSet::SolveXZCurve on the car's stored curve; the height runs
// linearly between the curve's end points.
Vec3 Traffic::curvePoint(const Car& c, float t, Vec3* direction) const {
    if (direction)
        *direction = {cubicSlope(c.curve[0], t), 0.0f, cubicSlope(c.curve[1], t)};
    return {cubic(c.curve[0], t), lerp(c.turnY0, c.turnY1, clampf(t, 0.0f, 1.0f)), cubic(c.curve[1], t)};
}

// aiRailSet::CalcRailPosition: the point of the car's rail `dist` along its
// lane, through the turn and onto the next road. The height is the road
// centre's at that distance (aiPath::CenterPosition), as AdjustAmbients reads it.
Vec3 Traffic::railPosition(const Car& c, float dist, Vec3* direction) const {
    const Lane* lane = laneOf(c.path, c.dir, c.lane);
    if (!lane || lane->line.points.size() < 2)
        return c.transform.m3;
    const auto& cum = lane->line.distances;
    const int S = static_cast<int>(cum.size());
    const float fb = c.frontBumper;
    const float L = lane->line.length;
    Vec3 out;
    if (dist < L - fb || c.nextPath < 0) {
        dist = std::min(dist, L - fb);
        const int i = index(c.path, c.dir, c.lane, dist);
        float segLen = cum[static_cast<std::size_t>(i)] - cum[static_cast<std::size_t>(i - 1)];
        Vec3 p0 = lanePoint(c, c.path, c.dir, c.lane, i - 1), p1, m0, m1;
        if (i > S - 2) {
            segLen -= fb;
            p1 = entryPoint(c.path, c.dir, c.lane, fb) + xAxisAt(c.path, c.dir, S - 2) * c.laneRandomness;
            m0 = subSectionDir(c.path, c.dir, i - 1, segLen);
            m1 = entryVector(c.path, c.dir, segLen);
        } else if (i == 1) {
            p1 = lanePoint(c, c.path, c.dir, c.lane, 1);
            m0 = exitVector(c.path, c.dir, segLen);
            m1 = subSectionDir(c.path, c.dir, 1, segLen);
        } else {
            p1 = lanePoint(c, c.path, c.dir, c.lane, i);
            m0 = subSectionDir(c.path, c.dir, i - 1, segLen);
            m1 = subSectionDir(c.path, c.dir, i, segLen);
        }
        const float t = segLen > 0.0f ? (dist - cum[static_cast<std::size_t>(i - 1)]) / segLen : 0.0f;
        out = hermitePoint(p0, p1, m0, m1, t, direction);
    } else {
        const float over = dist - (L - fb);
        const Vec3 p1 = lanePoint(c, c.nextPath, c.nextDir, c.nextLane, 0);
        const Vec3 p0 =
            entryPoint(c.path, c.dir, c.lane, fb) + xAxisAt(c.path, c.dir, S - 1) * c.laneRandomness;
        const float turnLen = manhattanXZ(p0, p1);
        if (over < turnLen) {
            // MM2 takes the exit vector with the current road's direction here.
            out = hermitePoint(p0, p1, entryVector(c.path, c.dir, turnLen),
                               exitVector(c.nextPath, c.dir, turnLen), over / turnLen, direction);
        } else {
            const Lane* next = laneOf(c.nextPath, c.nextDir, c.nextLane);
            if (!next || next->line.points.size() < 2) {
                if (direction)
                    *direction = exitVector(c.nextPath, c.nextDir, 1.0f);
                return p1;
            }
            const float d2 = over - turnLen;
            const int j = index(c.nextPath, c.nextDir, c.nextLane, d2);
            const auto& cn = next->line.distances;
            const float segLen = cn[static_cast<std::size_t>(j)] - cn[static_cast<std::size_t>(j - 1)];
            const Vec3 q0 = lanePoint(c, c.nextPath, c.nextDir, c.nextLane, j - 1);
            const Vec3 q1 = lanePoint(c, c.nextPath, c.nextDir, c.nextLane, j);
            const Vec3 m0 = j == 1 ? exitVector(c.nextPath, c.nextDir, segLen)
                                   : subSectionDir(c.nextPath, c.nextDir, j - 1, segLen);
            const Vec3 m1 = subSectionDir(c.nextPath, c.nextDir, j, segLen);
            out = hermitePoint(q0, q1, m0, m1,
                               segLen > 0.0f ? (d2 - cn[static_cast<std::size_t>(j - 1)]) / segLen : 0.0f,
                               direction);
        }
    }
    // aiPath::CenterPosition: the centre line's height at `dist`.
    const city::AiPath& src = m_net.source()->paths[static_cast<std::size_t>(c.path)];
    if (src.center.size() >= 2 && src.centerLengths.size() + 1 >= src.center.size()) {
        const float total = src.centerLengths.back();
        const float d = clampf(dist, 0.0f, total);
        for (std::size_t i = 1; i < src.center.size(); ++i) {
            const float a = i == 1 ? 0.0f : src.centerLengths[i - 2];
            const float b = src.centerLengths[i - 1];
            if (d <= b || i + 1 == src.center.size()) {
                const float t = b > a ? (d - a) / (b - a) : 0.0f;
                out.y = lerp(src.center[i - 1].y, src.center[i].y, clampf(t, 0.0f, 1.0f));
                break;
            }
        }
    }
    return out;
}

// --- Lane queues (aiPath ambient vehicle lists) -----------------------------

std::vector<int>& Traffic::queue(int path, int dir, int lane) {
    static std::vector<int> none;
    const int id = m_net.lane(path, dir, lane);
    if (id < 0) {
        none.clear();
        return none;
    }
    return m_queues[static_cast<std::size_t>(id)];
}

const std::vector<int>* Traffic::queueOf(int path, int dir, int lane) const {
    const int id = m_net.lane(path, dir, lane);
    return id >= 0 ? &m_queues[static_cast<std::size_t>(id)] : nullptr;
}

// aiPath::PushAmbVehicle: append at the tail (the rear).
void Traffic::pushVehicle(int car, int path, int dir, int lane) {
    auto& q = queue(path, dir, lane);
    if (std::ranges::find(q, car) == q.end())
        q.push_back(car);
}

// aiPath::PopAmbVehicle: remove the head (anything else is removed in place).
void Traffic::popVehicle(int car, int path, int dir, int lane) {
    removeVehicle(car, path, dir, lane);
}

// aiPath::AddAmbVehicle: sorted by distance, ties behind the cars already there.
void Traffic::addVehicle(int car, int path, int dir, int lane, float dist) {
    auto& q = queue(path, dir, lane);
    if (std::ranges::find(q, car) != q.end())
        return;
    auto it = q.begin();
    while (it != q.end() && !(m_cars[static_cast<std::size_t>(*it)].roadDist < dist))
        ++it;
    q.insert(it, car);
}

void Traffic::removeVehicle(int car, int path, int dir, int lane) {
    auto& q = queue(path, dir, lane);
    if (auto it = std::ranges::find(q, car); it != q.end())
        q.erase(it);
}

// The vehicle ahead of `car` in its list for `lane` of its road (vehicle
// +0x90[lane]).
int Traffic::ahead(int car, int lane) const {
    const Car& c = m_cars[static_cast<std::size_t>(car)];
    const auto* q = queueOf(c.path, c.dir, lane);
    if (!q)
        return -1;
    auto it = std::ranges::find(*q, car);
    if (it == q->end() || it == q->begin())
        return -1;
    return *(it - 1);
}

// aiVehicleSpline::ResetReactTicks: the car and everything behind it in its
// lane that is standing still waits its reaction time again.
void Traffic::resetReactTicks(int car) {
    const Car& c = m_cars[static_cast<std::size_t>(car)];
    const auto* q = queueOf(c.path, c.dir, c.lane);
    if (!q)
        return;
    auto it = std::ranges::find(*q, car);
    for (; it != q->end(); ++it) {
        Car& o = m_cars[static_cast<std::size_t>(*it)];
        if (o.speed < 0.01f)
            o.curReactTicks = 0;
    }
}

// --- Population (aiMap::AdjustAmbients, aiPath::ClearAmbients) --------------

void Traffic::activate(int path) {
    m_activePaths.insert(m_activePaths.begin(), path);
}

void Traffic::returnToPool(int car) {
    Car& c = m_cars[static_cast<std::size_t>(car)];
    // Out of every list it may be on (aiMap::AddAmbient).
    if (c.path >= 0) {
        const auto& info = m_net.paths()[static_cast<std::size_t>(c.path)];
        for (std::size_t l = 0; l < info.lanesOf(c.dir).size(); ++l)
            removeVehicle(car, c.path, c.dir, static_cast<int>(l));
        const int node = arrivalIntersection(m_net, c.path, c.dir);
        if (node >= 0) {
            std::erase(m_stopWaiting[static_cast<std::size_t>(node)], car);
            std::erase(m_stopAllowed[static_cast<std::size_t>(node)], car);
        }
    }
    // Out of the obstacle map: MM2 unlinks it from an intersection list and
    // leaves a road list entry behind (ClearAmbients wipes its own road's);
    // OpenMM2 unlinks both.
    if (c.mapType == kIntersectionComponent &&
        c.mapId >= 0 && static_cast<std::size_t>(c.mapId) < m_nodeObstacles.size()) {
        auto& list = m_nodeObstacles[static_cast<std::size_t>(c.mapId)];
        if (auto it = std::find(list.begin(), list.end(), car); it != list.end())
            list.erase(it);
    } else if (c.mapType == kRoadComponent) {
        if (auto* list = obstacleList(c.mapId, c.mapSide, c.mapVert))
            if (auto it = std::find(list->begin(), list->end(), car); it != list->end())
                list->erase(it);
    }
    c.mapType = c.mapId = c.mapVert = -1;
    c.active = false;
    c.physical = false;
    c.speed = 0.0f;
    m_pool.push_back(car);
}

void Traffic::clearPath(int path) {
    // ClearAmbients: the road's section obstacle lists emptied
    // (aiPath::ResetObstacles); then lanes of direction +1 first, then -1; a
    // car returns to the pool from the list of the lane it is drawn on.
    for (auto& side : m_roadObstacles[static_cast<std::size_t>(path)])
        for (auto& list : side)
            list.clear();
    for (int dir : {1, -1}) {
        const auto& lanes = m_net.paths()[static_cast<std::size_t>(path)].lanesOf(dir);
        for (std::size_t l = 0; l < lanes.size(); ++l) {
            auto cars = m_queues[static_cast<std::size_t>(lanes[l])];
            for (int car : cars) {
                removeVehicle(car, path, dir, static_cast<int>(l));
                if (m_cars[static_cast<std::size_t>(car)].drawLane == static_cast<int>(l))
                    returnToPool(car);
            }
        }
    }
    m_pathActive[static_cast<std::size_t>(path)] = 0;
    std::erase(m_activePaths, path);
}

// One car from the pool onto (path, dir, lane) at `dist`. Returns false when
// no next road can be chosen (the lane is given up); a spot too close to an
// opponent is skipped without using the car.
bool Traffic::placeCar(int slot, int path, int dir, int lane, float dist) {
    Car& c = m_cars[static_cast<std::size_t>(slot)];
    c.path = path;
    c.dir = dir;
    c.lane = c.drawLane = lane;
    c.roadDist = dist;
    if (!chooseNext(c))
        return false;
    const Vec3 at = railPosition(c, dist);
    for (const Vec3& o : m_opponents)
        if (o.dist2(at) < sq(kAmbientOpponentClearance))
            return true;
    m_pool.pop_back();
    ++c.spawns;
    // aiVehicleAmbient::Reset: aiVehicleSpline::Reset (speed, tyre
    // rotation, reaction ticks, the obstacle map state), then each goal's
    // Init (aiGoalRandomDrive::Init: acceleration and target speed 0, no
    // lane change or stop sign pending; aiGoalRegainRail::Init: one regain
    // attempt, its start point at the origin, base 0 and length 30;
    // aiGoalAvoidPlayer::Init: its heading and offsets 0).
    c.active = true;
    c.mapId = c.mapType = c.mapVert = -1;
    c.mapRoom = 0;
    c.mapSide = 0;
    c.physical = false;
    c.goal = AmbientGoal::RandomDrive;
    c.goalTicks = 0;
    c.speed = 0.0f;
    c.accel = 0.0f;
    c.target = 0.0f;
    c.tireRotation = 0.0f;
    c.laneChangeOk = false;
    c.atStopSign = false;
    c.enterInt = false;
    c.curReactTicks = c.totReactTicks;
    c.regainAttempts = 1;
    c.regainStart = {};
    c.regainBase = 0.0f;
    c.regainLength = kRegainDistance;
    c.heading = 0.0f;
    c.passOffset = 0.0f;
    c.centred = false;
    // (The indicators, aiVehicleInstance +0x1a, are left as they were: only
    // SolveVelocity, ChangeLanes and the end of a turn or lane change in
    // SolveRailType, aiGoalCollision::Reset and the end of a regain set them.)
    c.horn = false;
    c.fitted = false;
    c.transform = Mat34::identity();
    c.transform.m3 = at;
    addVehicle(slot, path, dir, lane, dist);
    return true;
}

void Traffic::adjustAmbients(int oldRoom, int newRoom, int slot) {
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    const city::AiMap* map = m_net.source();
    if (!map)
        return;
    static const std::vector<std::uint16_t> kNone;
    auto roomList = [&](int room) -> const std::vector<std::uint16_t>& {
        if (room < 0 || static_cast<std::size_t>(room) >= map->roomPathsNear.size())
            return kNone;
        return map->roomPathsNear[static_cast<std::size_t>(room)];
    };
    std::vector<int> added, removed;
    if (m_populateAll) {
        for (std::size_t p = 0; p < m_net.paths().size(); ++p)
            added.push_back(static_cast<int>(p));
    } else {
        const auto& from = roomList(oldRoom);
        const auto& to = roomList(newRoom);
        for (auto p : to)
            if (std::ranges::find(from, p) == from.end() && p < m_net.paths().size())
                added.push_back(p);
        for (auto p : from)
            if (std::ranges::find(to, p) == to.end() && p < m_net.paths().size())
                removed.push_back(p);
    }
    // aiPath::RemAmbPlayer: the player's bit off; a road no player keeps is
    // emptied (aiPath::ClearAmbients).
    for (int p : removed) {
        std::uint16_t& mask = m_pathActive[static_cast<std::size_t>(p)];
        if (mask == 0)
            continue;
        mask = static_cast<std::uint16_t>(mask & ~bit);
        if (mask == 0)
            clearPath(p);
    }

    // The total usable lane length of the new roads (centre length - 5 m per
    // lane, roads with an [Exceptions] entry aside) sets the spacing: about
    // density / 8 cars per metre spread evenly over all of them.
    auto open = [&](const PathInfo& info, int dir) { return (info.flagsOf(dir) & 1) == 0; };
    float total = 0.0f;
    for (int p : added) {
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(p)];
        if (info.hasException)
            continue;
        for (int dir : {1, -1})
            if (open(info, dir))
                total = (info.centreLength - 5.0f) * static_cast<float>(info.lanesOf(dir).size()) + total;
    }
    const int k = 1 - static_cast<int>(total * m_density * -0.125f);
    const float spacing = total / static_cast<float>(k);

    float carry = 0.0f, leftover = 0.0f;
    for (int p : added) {
        // aiPath::AddAmbPlayer: the player's bit on; a road another player
        // already keeps is populated already.
        std::uint16_t& mask = m_pathActive[static_cast<std::size_t>(p)];
        const std::uint16_t before = mask;
        mask = static_cast<std::uint16_t>(mask | bit);
        if (before != 0)
            continue;
        activate(p);
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(p)];
        for (int dir : {-1, 1}) {
            if (!open(info, dir))
                continue;
            const int lanes = static_cast<int>(info.lanesOf(dir).size());
            if (info.hasException) {
                // aiMap::NumCars on the first lane of direction +1, then each
                // lane gets that many cars at (i + 1/2) spacings, jittered.
                const int n = static_cast<int>(laneLength(p, 1, 0) * info.density / kAmbientCarSpacing);
                for (int l = 0; l < lanes; ++l) {
                    const float space = laneLength(p, dir, l) / static_cast<float>(n + 1);
                    for (int i = 0; i < n && !m_pool.empty(); ++i) {
                        const float jitter = libm::sin(m_rng->frand() * 6.28f);
                        const float dist = (static_cast<float>(i + 1) * space - space * 0.5f) +
                                           (space - 10.0f) * jitter * 0.5f;
                        if (!placeCar(m_pool.back(), p, dir, l, dist))
                            break;
                    }
                }
                continue;
            }
            const float usable = info.centreLength - 5.0f;
            for (int l = 0; l < lanes; ++l) {
                carry = usable + carry;
                float s = 0.0f;
                while (carry > spacing + 0.001f) {
                    carry = carry - spacing;
                    s = (spacing - leftover) + s;
                    leftover = 0.0f;
                    if (m_pool.empty())
                        break;
                    const Car& next = m_cars[static_cast<std::size_t>(m_pool.back())];
                    const float dist = clampf(s, 0.0f, std::max(0.0f, usable - next.frontBumper));
                    if (!placeCar(m_pool.back(), p, dir, l, dist))
                        break;
                }
                leftover = carry;
            }
        }
    }
}

void Traffic::populateAll() {
    m_populateAll = true;
}

int Traffic::poolFree() const {
    return static_cast<int>(m_pool.size());
}

void Traffic::reset() {
    // aiMap::Reset: ResetRandomSeed first. A shared stream is the owner's
    // to seed (ai::World::reset); the traffic's own goes back to its seed.
    if (m_rng == &m_ownRng)
        m_rng->seed(static_cast<std::uint32_t>(m_seed));
    // aiPath::Reset: the lane lists, the section obstacle lists
    // (ResetObstacles), the populated flag and list link, the always
    // stop / go flags.
    for (auto& q : m_queues)
        q.clear();
    for (auto& path : m_roadObstacles)
        for (auto& side : path)
            for (auto& list : side)
                list.clear();
    std::ranges::fill(m_pathActive, 0);
    std::ranges::fill(m_alwaysStop, 0);
    m_activePaths.clear();
    // aiIntersection::Reset: the stop sign lists and the vehicle list.
    for (auto& list : m_stopWaiting)
        list.clear();
    for (auto& list : m_stopAllowed)
        list.clear();
    for (auto& list : m_nodeObstacles)
        list.clear();
    // Every car back into the pool (aiMap::AddAmbient, in index order: the
    // last one is taken first) with its rail reset (aiRailSet::Reset: no
    // road, distances 0, the reaction distance 25 m).
    m_pool.clear();
    for (std::size_t i = 0; i < m_cars.size(); ++i) {
        Car& c = m_cars[i];
        c.active = false;
        c.physical = false;
        c.mapType = c.mapId = c.mapVert = -1;
        c.speed = 0.0f;
        c.accel = 0.0f;
        c.target = 0.0f;
        c.horn = false;
        c.path = c.nextPath = -1;
        c.lane = c.drawLane = c.nextLane = 0;
        c.rail = Rail::Lane;
        c.section = 0;
        c.roadDist = c.laneChangeDist = c.segDist = c.segLen = 0.0f;
        c.enterInt = false;
        c.reactDist = kIntersectionReactDist;
        m_pool.push_back(static_cast<int>(i));
    }
    // The next step populates the roads round the players' rooms
    // (aiMap::Reset's AdjustAmbients from room 0).
    m_started = false;
    m_slotRooms.fill(0);
    m_slotPresent.fill(false);
    m_avoidEvents.clear();
    publish();
}

// --- aiGoalRandomDrive ------------------------------------------------------

bool Traffic::chooseNext(Car& c) {
    RailLink next;
    if (chooseNextLaneLink(m_net, {c.path, c.dir, c.drawLane}, *m_rng, next)) {
        c.nextPath = next.path;
        c.nextDir = next.dir;
        c.nextLane = next.lane;
        return true;
    }
    c.nextPath = -1;
    return false;
}

// aiGoalRandomDrive::SpeedLimit: the road's limit plus the car's excess;
// freeways add 5 m/s per lane counted from the rightmost (MM2; MM1's
// SolveVelocity has only the road's limit and the excess).
float Traffic::speedLimit(const Car& c) const {
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    if (info.freeway() && !(m_net.source() && m_net.source()->mm1)) {
        const int lanes = static_cast<int>(info.lanesOf(c.dir).size());
        return static_cast<float>((lanes - c.lane) - 1) * 5.0f + c.exceedLimit + info.speedLimit;
    }
    return info.speedLimit + c.exceedLimit;
}

// aiVehicleSpline::DistanceToIntersection: from the front bumper to the
// lane's end.
float Traffic::distanceToIntersection(const Car& c) const {
    const float laneLen = laneLength(c.path, c.dir, c.lane);
    switch (c.rail) {
    case Rail::Lane:
        return laneLen - (c.roadDist + c.frontBumper);
    case Rail::LaneChange:
        return ((laneLen - c.frontBumper) + (c.segLen - c.segDist)) - c.laneChangeDist;
    default:
        return kNoDistance;
    }
}

// aiVehicleSpline::DistanceToVehicle: centre to centre along the rails.
float Traffic::distanceToVehicle(const Car& a, const Car& b) const {
    if (a.path != b.path)
        return (a.segLen - a.segDist) + b.roadDist;
    auto euclid = [&] { return a.transform.m3.dist(b.transform.m3); };
    if (b.rail == Rail::Regain)
        return euclid();
    switch (a.rail) {
    case Rail::Lane:
        if (b.rail == Rail::LaneChange) {
            const float bl = laneLength(b.path, b.dir, b.drawLane);
            const float q = bl - a.frontBumper;
            if (a.drawLane == b.drawLane || bl <= 0.0f)
                return (q * 0.25f + b.segDist) - a.roadDist;
            return ((q * 0.25f + b.segDist) * laneLength(a.path, a.dir, a.drawLane)) / bl - a.roadDist;
        }
        return b.roadDist - a.roadDist;
    case Rail::Turn:
        if (b.rail == Rail::Lane)
            return b.roadDist - a.roadDist;
        if (b.rail == Rail::Turn)
            return m_net.paths()[static_cast<std::size_t>(a.path)].intersection[0] ==
                           m_net.paths()[static_cast<std::size_t>(b.path)].intersection[0]
                       ? b.segDist - a.segDist
                       : kNoDistance;
        return kNoDistance;
    case Rail::LaneChange:
        if (b.rail == Rail::LaneChange) {
            const Lane* la = laneOf(a.path, a.dir, a.drawLane);
            const Lane* lb = laneOf(b.path, b.dir, b.drawLane);
            if (!la || !lb)
                return kNoDistance;
            const int j = index(a.path, a.dir, b.drawLane, b.roadDist);
            return (la->line.distances[static_cast<std::size_t>(j)] - a.roadDist) -
                   (lb->line.distances[static_cast<std::size_t>(j)] - b.roadDist);
        }
        return b.roadDist - (a.laneChangeDist - (a.segLen - a.segDist));
    default:
        return euclid();
    }
}

// aiGoalRandomDrive::Reset: puts the car on the curve for its lane distance.
void Traffic::resetRandomDrive(Car& c) {
    c.atStopSign = false;
    c.enterInt = false;
    const Lane* lane = laneOf(c.path, c.dir, c.lane);
    if (!lane || lane->line.points.size() < 2)
        return;
    float laneEnd = lane->line.length - c.frontBumper;
    c.reactDist = kIntersectionReactDist;
    c.laneChangeOk = c.roadDist <= laneEnd * 0.25f;
    float turnLen = 0.0f;
    if (c.nextPath >= 0) {
        turnLen = turnLength(c);
        if (turnLen + laneEnd < c.roadDist) {
            // Beyond the turn: onto the next road.
            const int idx = static_cast<int>(&c - m_cars.data());
            c.roadDist -= turnLen + laneEnd;
            popVehicle(idx, c.path, c.dir, c.drawLane);
            pushVehicle(idx, c.nextPath, c.nextDir, c.nextLane);
            c.path = c.nextPath;
            c.dir = c.nextDir;
            c.lane = c.drawLane = c.nextLane;
            chooseNext(c);
            lane = laneOf(c.path, c.dir, c.lane);
            if (!lane || lane->line.points.size() < 2)
                return;
            laneEnd = lane->line.length - c.frontBumper;
            turnLen = c.nextPath >= 0 ? turnLength(c) : 0.0f;
        }
    }
    const int S = static_cast<int>(lane->line.points.size());
    const auto& cum = lane->line.distances;
    if (laneEnd <= c.roadDist && c.nextPath >= 0) {
        // In the turn.
        c.segDist = c.roadDist - laneEnd;
        c.segLen = std::max(turnLen, 1e-3f);
        const Vec3 p0 = entryPoint(c.path, c.dir, c.lane, c.frontBumper) +
                        xAxisAt(c.path, c.dir, S - 1) * c.laneRandomness;
        const Vec3 p1 = lanePoint(c, c.nextPath, c.nextDir, c.nextLane, 0);
        setCurve(c, p0, p1, entryVector(c.path, c.dir, c.segLen),
                 exitVector(c.nextPath, c.nextDir, c.segLen));
        c.rail = Rail::Turn;
        c.section = S;
        const int node = arrivalIntersection(m_net, c.path, c.dir);
        c.transform = Mat34::identity();
        c.transform.m3 = node >= 0 ? m_net.intersections()[static_cast<std::size_t>(node)].centre : p0;
    } else {
        c.roadDist = std::min(c.roadDist, laneEnd);
        const int i = index(c.path, c.dir, c.lane, c.roadDist);
        c.section = i;
        c.segDist = subSectionDist(c.path, c.dir, c.lane, c.roadDist);
        float segLen = cum[static_cast<std::size_t>(i)] - cum[static_cast<std::size_t>(i - 1)];
        Vec3 p0 = lanePoint(c, c.path, c.dir, c.lane, i - 1), p1, m0, m1;
        if (i < S - 1) {
            p1 = lanePoint(c, c.path, c.dir, c.lane, i);
            m0 = i == 1 ? exitVector(c.path, c.dir, segLen) : subSectionDir(c.path, c.dir, i - 1, segLen);
            m1 = subSectionDir(c.path, c.dir, i, segLen);
        } else {
            segLen -= c.frontBumper;
            p1 = subSectionPoint(c.path, c.dir, c.lane, i, c.frontBumper) +
                 xAxisAt(c.path, c.dir, i) * c.laneRandomness;
            m0 = subSectionDir(c.path, c.dir, i - 1, segLen);
            m1 = entryVector(c.path, c.dir, segLen);
        }
        c.segLen = std::max(segLen, 1e-3f);
        setCurve(c, p0, p1, m0, m1);
        c.rail = Rail::Lane;
        // The vertices of the drawn lane, interpolated.
        const Lane* drawn = laneOf(c.path, c.dir, c.drawLane);
        const Lane* src = drawn ? drawn : lane;
        const float t = clampf(c.segDist / c.segLen, 0.0f, 1.0f);
        c.transform = Mat34::identity();
        c.transform.m3 = lerp(src->line.points[static_cast<std::size_t>(i - 1)],
                              src->line.points[static_cast<std::size_t>(std::min(i, S - 1))], t);
    }
    c.curReactTicks = c.totReactTicks;
    // Reset ends by posing the car on its curve (the pose solver Update
    // calls), whatever its speed.
    solvePose(c);
}

bool Traffic::stopSignOkayToGo(int node, int car) {
    // aiIntersection::StopSignOkayToGo: when nobody has permission, the
    // first car to have stopped gets it, together with the first waiting
    // car from the same road.
    auto& allowed = m_stopAllowed[static_cast<std::size_t>(node)];
    auto& waiting = m_stopWaiting[static_cast<std::size_t>(node)];
    if (allowed.empty() && !waiting.empty()) {
        const int first = waiting.front();
        waiting.erase(waiting.begin());
        allowed.push_back(first);
        // (Only an ambient car, type 0, takes another of its road along; an
        // external vehicle such as a cable car, type 5, goes alone.)
        if (!isExternal(first)) {
            const int road = m_cars[static_cast<std::size_t>(first)].path;
            for (auto it = waiting.begin(); it != waiting.end(); ++it) {
                if (!isExternal(*it) && m_cars[static_cast<std::size_t>(*it)].path == road) {
                    allowed.push_back(*it);
                    waiting.erase(it);
                    break;
                }
            }
        }
    }
    return std::ranges::find(allowed, car) != allowed.end();
}

void Traffic::removeFromStopSign(int node, int car) {
    std::erase(m_stopAllowed[static_cast<std::size_t>(node)], car);
}

// aiGoalRandomDrive::OkayToEnterIntersection.
bool Traffic::okayToEnter(int idx, float dist) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    const int end = c.dir == 1 ? 0 : 1;
    // A road told to always stop (aiPath::AllwaysStop, set round the racers):
    // never enter.
    if (m_alwaysStop[static_cast<std::size_t>(c.path)])
        return false;
    switch (info.rule[end]) {
    case EntryRule::Uncontrolled:
        resetReactTicks(idx);
        return true;
    case EntryRule::TrafficLight: {
        const Lane* lane = laneOf(c.path, c.dir, c.lane);
        return !lane || lane->lightSlot < 0 || m_lights.state(lane->lightSlot) == LightState::Green;
    }
    case EntryRule::StopSign:
        break;
    }
    if (c.speed >= 0.5f || dist >= 1.5f)
        return false;
    const int node = info.intersection[end];
    if (node < 0)
        return true;
    if (!c.atStopSign) {
        c.atStopSign = true;
        m_stopWaiting[static_cast<std::size_t>(node)].push_back(idx);
    }
    return stopSignOkayToGo(node, idx);
}

// aiGoalRandomDrive::UpcomingAccident: a car out of its normal driving
// (hit, regaining its lane, avoiding the player, parked) in the intersection
// ahead or on the next road. MM2 finds them through its obstacle map; here a
// car counts where its rail registers it (inferred equivalent).
bool Traffic::inAccident(int o) const {
    // aiVehicleSpline::InAccident (any goal but driving its rail), or the
    // external vehicle's own answer.
    if (o < 0)
        return false;
    if (isExternal(o)) {
        const auto k = static_cast<std::size_t>(o) - m_cars.size();
        return k < m_externals.size() && m_externals[k]->inAccident();
    }
    return m_cars[static_cast<std::size_t>(o)].goal != AmbientGoal::RandomDrive;
}

int Traffic::addExternal(const ExternalVehicle* vehicle) {
    m_externals.push_back(vehicle);
    return static_cast<int>(m_cars.size() + m_externals.size() - 1);
}

void Traffic::listOnRoad(int entry, int path, int side, int bucket) {
    if (auto* list = obstacleList(path, side, bucket))
        list->insert(list->begin(), entry);
}

void Traffic::unlistFromRoad(int entry, int path, int side, int bucket) {
    if (auto* list = obstacleList(path, side, bucket))
        if (auto it = std::ranges::find(*list, entry); it != list->end())
            list->erase(it);
}

void Traffic::listAtIntersection(int entry, int node) {
    if (node >= 0 && static_cast<std::size_t>(node) < m_nodeObstacles.size())
        m_nodeObstacles[static_cast<std::size_t>(node)].insert(m_nodeObstacles[static_cast<std::size_t>(node)].begin(),
                                                               entry);
}

void Traffic::unlistFromIntersection(int entry, int node) {
    if (node < 0 || static_cast<std::size_t>(node) >= m_nodeObstacles.size())
        return;
    auto& list = m_nodeObstacles[static_cast<std::size_t>(node)];
    if (auto it = std::ranges::find(list, entry); it != list.end())
        list.erase(it);
}

void Traffic::joinStopSign(int node, int entry) {
    if (node >= 0 && static_cast<std::size_t>(node) < m_stopWaiting.size())
        m_stopWaiting[static_cast<std::size_t>(node)].push_back(entry);
}

bool Traffic::upcomingAccident(const Car& c) const {
    auto inAccident = [&](int o) { return this->inAccident(o); };
    for (int o : intersectionVehicles(arrivalIntersection(m_net, c.path, c.dir)))
        if (inAccident(o))
            return true;
    if (c.nextPath >= 0 && static_cast<std::size_t>(c.nextPath) < m_roadObstacles.size()) {
        const auto& lists = m_roadObstacles[static_cast<std::size_t>(c.nextPath)];
        for (std::size_t v = 0; v < lists[1].size(); ++v) {
            for (int o : lists[1][v])
                if (inAccident(o))
                    return true;
            for (int o : lists[0][v])
                if (inAccident(o))
                    return true;
        }
    }
    return false;
}

// aiPath::RoadCapacity: the car, and every car ahead of it on its road bound
// for the same next road, must fit before the last car of the next lane.
bool Traffic::roadCapacity(int idx) const {
    const Car& c = m_cars[static_cast<std::size_t>(idx)];
    const auto* q = queueOf(c.nextPath, c.nextDir, c.nextLane);
    if (!q || q->empty())
        return true;
    const Car& tail = m_cars[static_cast<std::size_t>(q->back())];
    auto totLength = [](const Car& o) { return (o.separation + o.frontBumper) + o.backBumper; };
    float acc = tail.backBumper;
    int o = ahead(idx, c.lane);
    while (o >= 0) {
        const Car& oc = m_cars[static_cast<std::size_t>(o)];
        if (oc.path != c.path || !(c.roadDist < oc.roadDist))
            break;
        if (oc.nextPath == c.nextPath)
            acc += totLength(oc);
        o = ahead(o, oc.lane);
    }
    return totLength(c) + acc < tail.roadDist;
}

// aiGoalRandomDrive::AnyVehiclesComingThisWay: a car committed to the same
// intersection from another road whose way (its lane end to its next lane's
// start) crosses this car's line. As MM2 codes it, the other roads' lanes and
// vertices are read for this car's own directions.
bool Traffic::anyVehiclesComingThisWay(const Car& c) const {
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    if (info.freeway() || (c.nextPath >= 0 && m_net.paths()[static_cast<std::size_t>(c.nextPath)].freeway()))
        return false;
    const int node = arrivalIntersection(m_net, c.path, c.dir);
    if (node < 0)
        return false;
    const Vec3& pos = c.transform.m3;
    const Vec3& right = c.transform.m0;
    for (int other : m_net.intersections()[static_cast<std::size_t>(node)].paths) {
        if (other == c.path)
            continue;
        const auto& lanes = m_net.paths()[static_cast<std::size_t>(other)].lanesOf(c.dir);
        for (std::size_t l = 0; l < lanes.size(); ++l) {
            const auto& q = m_queues[static_cast<std::size_t>(lanes[l])];
            if (q.empty())
                continue;
            const Car& h = m_cars[static_cast<std::size_t>(q.front())];
            if (!h.enterInt || h.nextPath < 0)
                continue;
            const Lane* from = laneOf(h.path, c.dir, h.drawLane);
            const Lane* to = laneOf(h.nextPath, c.nextDir, h.nextLane);
            if (!from || !to || from->line.points.empty() || to->line.points.empty())
                continue;
            const float a = (from->line.points.back() - pos).dot(right);
            const float b = (to->line.points.front() - pos).dot(right);
            if (sign(a) != sign(b))
                return true;
        }
    }
    return false;
}

// aiGoalRandomDrive::AvoidCollision: car following.
void Traffic::avoidCollision(Car& c, const Car& lead, float d) {
    const float u = clampf(lead.speed - 2.5f, 0.0f, 999.0f);
    const float leadAccel = lead.accel;
    const float gap = lead.backBumper + c.frontBumper;
    if (d < gap) {
        if (lead.rail != Rail::LaneChange) {
            c.speed = 0.0f;
            c.accel = 0.0f;
            c.target = 0.0f;
        } else {
            c.accel = c.speed * c.speed * -0.16666667f;
            c.target = 0.0f;
        }
        return;
    }
    const float limit = speedLimit(c);
    if (gap + c.separation <= d) {
        if (u <= c.speed) {
            const float room = c.separation + lead.backBumper + c.frontBumper;
            const bool sameKind = (c.rail == Rail::LaneChange && lead.rail == Rail::LaneChange) ||
                                  (c.rail == Rail::Turn && lead.rail == Rail::Turn);
            const float span = sameKind ? (d - room) - 2.0f : d - room;
            // (MM2 divides by zero when the gap is exactly met; kept finite.)
            c.accel = (u * u - c.speed * c.speed) / (span != 0.0f ? span + span : 1e-6f);
        } else {
            c.accel = c.accelFactor;
        }
        c.target = limit < u ? limit : u;
        return;
    }
    if (u <= c.speed) {
        c.speed = c.speed * 0.75f;
        if (leadAccel <= 0.0f) {
            c.accel = 0.0f;
            c.target = 0.0f;
            return;
        }
        c.accel = c.accelFactor < leadAccel ? c.accelFactor : leadAccel;
    } else {
        c.accel = c.accelFactor;
    }
    c.target = u <= limit ? u : limit;
}

// aiGoalRandomDrive::SolveVelocity.
void Traffic::solveVelocity(int idx, float dt) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    float leadDist = 99999.0f;
    const float dist = distanceToIntersection(c);
    const float v = c.speed;
    const bool reacted = c.totReactTicks < c.curReactTicks;
    const float limit = speedLimit(c);
    auto cruise = [&] {
        c.target = limit + 0.0001f;
        c.accel = c.accelFactor;
    };

    if (c.reactDist <= dist || c.rail == Rail::Turn) {
        const int lead = ahead(idx, c.lane);
        if (lead >= 0) {
            leadDist = distanceToVehicle(c, m_cars[static_cast<std::size_t>(lead)]);
            if (20.0f <= leadDist) {
                if (c.target < limit || c.accel < c.accelFactor)
                    cruise();
            } else if (reacted) {
                avoidCollision(c, m_cars[static_cast<std::size_t>(lead)], leadDist);
            }
        } else if (c.rail == Rail::Turn) {
            // The last car of the lane being turned into, or a car of this
            // road turning into the same lane ahead of this one.
            int best = -1;
            const auto* tail = queueOf(c.nextPath, c.nextDir, c.nextLane);
            if (tail && !tail->empty() && tail->back() != idx) {
                best = tail->back();
                leadDist = distanceToVehicle(c, m_cars[static_cast<std::size_t>(best)]);
            }
            const auto& lanes = m_net.paths()[static_cast<std::size_t>(c.path)].lanesOf(c.dir);
            for (int laneId : lanes) {
                const auto& q = m_queues[static_cast<std::size_t>(laneId)];
                if (q.empty() || q.front() == idx)
                    continue;
                const Car& o = m_cars[static_cast<std::size_t>(q.front())];
                if (o.rail != Rail::Turn || o.nextPath != c.nextPath || o.nextLane != c.nextLane)
                    continue;
                const float d = distanceToVehicle(c, o);
                if (d < leadDist) {
                    leadDist = d;
                    best = q.front();
                }
            }
            if (0.0f < leadDist && leadDist < 20.0f) {
                if (reacted && best >= 0)
                    avoidCollision(c, m_cars[static_cast<std::size_t>(best)], leadDist);
            } else if (!(limit <= c.target && c.accelFactor <= c.accel)) {
                cruise();
            }
        } else if (c.target < limit) {
            cruise();
        }
    } else {
        // Within reach of the intersection.
        if (!c.enterInt && okayToEnter(idx, dist)) {
            bool committed = false;
            if (!upcomingAccident(c) && c.nextPath >= 0 && roadCapacity(idx)) {
                committed = true;
                if (!anyVehiclesComingThisWay(c)) {
                    c.enterInt = true;
                    if (c.speed < 0.01f && dist < 0.5f)
                        c.curReactTicks = c.totReactTicks - 3;
                }
            }
            if (!committed)
                chooseNext(c);
        }
        if (!c.enterInt) {
            // The car ahead, or the one ahead of it when that one is regaining
            // its lane. As coded, the distance is measured to the car found but
            // AvoidCollision is given the car directly ahead.
            const int direct = ahead(idx, c.lane);
            int lead = direct;
            if (lead >= 0 && m_cars[static_cast<std::size_t>(lead)].goal == AmbientGoal::RegainRail)
                lead = ahead(lead, c.lane);
            const Car* lc = lead >= 0 ? &m_cars[static_cast<std::size_t>(lead)] : nullptr;
            if (lc && lc->rail != Rail::Turn && lc->path == c.path) {
                leadDist = distanceToVehicle(c, *lc);
                if (20.0f <= leadDist) {
                    if (c.target < limit || c.accel < c.accelFactor)
                        cruise();
                } else if (reacted) {
                    avoidCollision(c, m_cars[static_cast<std::size_t>(direct)], leadDist);
                }
            } else if (!reacted || 2.0f < v || dist <= 1.0f) {
                // Stop at the line.
                if (c.target == 0.0f) {
                    if (c.speed < 0.2f && dist < 0.5f)
                        c.speed = 0.0f;
                } else {
                    const float room = (dist - 0.25f) + (dist - 0.25f);
                    c.accel = -((v * v) / (room != 0.0f ? room : 1e-6f));
                    c.target = 0.0f;
                }
            } else {
                // Stopped short of the line: creep up to it.
                c.speed = 2.0f;
                c.reactDist = kRestartReactDist;
                cruise();
            }
        } else {
            const int lead = ahead(idx, c.lane);
            if (lead < 0) {
                if (c.target < limit && reacted)
                    cruise();
            } else {
                const Car& lc = m_cars[static_cast<std::size_t>(lead)];
                leadDist = distanceToVehicle(c, lc);
                if (20.0f <= leadDist) {
                    if (reacted) {
                        if (limit <= c.target) {
                            if (0.0f < c.target && c.accel == 0.0f)
                                c.accel = c.accelFactor;
                        } else {
                            cruise();
                        }
                    }
                } else if (lc.target != c.target && reacted) {
                    avoidCollision(c, lc, leadDist);
                }
            }
        }
        // Indicators (aiRailSet::SolveTurnType).
        if (c.nextPath >= 0) {
            const TurnType turn = solveTurnType(m_net, {c.path, c.dir, c.drawLane}, c.nextPath, c.nextDir);
            if (turn == TurnType::Right)
                c.signal = TurnSignal::Right;
            else if (turn == TurnType::Left)
                c.signal = TurnSignal::Left;
        }
    }

    // Integration.
    c.speed = dt * c.accel + c.speed;
    if ((c.speed < c.target + 0.05f && c.accel < 0.0f) || (c.target < c.speed && 0.0f < c.accel)) {
        c.speed = c.target;
        c.accel = 0.0f;
    }
    const int lead = ahead(idx, c.lane);
    if (lead >= 0 && m_cars[static_cast<std::size_t>(lead)].speed < 0.01f && c.speed < 0.01f &&
        leadDist < 20.0f)
        c.curReactTicks = 0;
    c.segDist = dt * c.speed + c.segDist;
    c.roadDist = dt * c.speed + c.roadDist;
}

// aiGoalRandomDrive::SolveRailType: the next segment, the turn through the
// intersection, and the end of a turn or lane change. Returns false when the
// car left the populated roads.
bool Traffic::solveRailType(int idx) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    if (c.segDist <= c.segLen)
        return true;
    const Lane* lane = laneOf(c.path, c.dir, c.lane);
    if (!lane || lane->line.points.size() < 2) {
        returnToPool(idx);
        return false;
    }
    int S = static_cast<int>(lane->line.points.size());
    auto beginTurn = [&](float backFromEnd) {
        const Vec3 p0 =
            entryPoint(c.path, c.dir, c.lane, backFromEnd) + xAxisAt(c.path, c.dir, S - 1) * c.laneRandomness;
        const Vec3 p1 = lanePoint(c, c.nextPath, c.nextDir, c.nextLane, 0);
        c.segLen = std::max(manhattanXZ(p0, p1), 1e-3f);
        setCurve(c, p0, p1, entryVector(c.path, c.dir, c.segLen),
                 exitVector(c.nextPath, c.nextDir, c.segLen));
        c.rail = Rail::Turn;
    };
    switch (c.rail) {
    case Rail::Lane: {
        c.section += 1;
        c.segDist -= c.segLen;
        const auto& cum = lane->line.distances;
        if (c.nextPath < 0 || c.section <= S - 1) {
            const int i = std::min(c.section, S - 1);
            float segLen = cum[static_cast<std::size_t>(i)] - cum[static_cast<std::size_t>(i - 1)];
            if (c.section > S - 2) {
                segLen -= c.frontBumper;
                if (segLen <= 0.0f && c.nextPath >= 0) {
                    beginTurn(c.frontBumper + segLen);
                    break;
                }
                const Vec3 p0 = lanePoint(c, c.path, c.dir, c.lane, i - 1);
                const Vec3 p1 = entryPoint(c.path, c.dir, c.lane, c.frontBumper) +
                                xAxisAt(c.path, c.dir, S - 2) * c.laneRandomness;
                c.segLen = std::max(segLen, 1e-3f);
                setCurve(c, p0, p1, subSectionDir(c.path, c.dir, i - 1, c.segLen),
                         entryVector(c.path, c.dir, c.segLen));
            } else {
                c.segLen = std::max(segLen, 1e-3f);
                setCurve(c, lanePoint(c, c.path, c.dir, c.lane, i - 1),
                         lanePoint(c, c.path, c.dir, c.lane, i), subSectionDir(c.path, c.dir, i - 1, c.segLen),
                         subSectionDir(c.path, c.dir, i, c.segLen));
            }
        } else {
            beginTurn(c.frontBumper);
        }
        break;
    }
    case Rail::Turn: {
        // Through the intersection.
        c.atStopSign = false;
        c.enterInt = false;
        c.laneChangeOk = true;
        c.reactDist = kIntersectionReactDist;
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
        const int end = c.dir == 1 ? 0 : 1;
        if (info.rule[end] == EntryRule::StopSign && info.intersection[end] >= 0)
            removeFromStopSign(info.intersection[end], idx);
        popVehicle(idx, c.path, c.dir, c.drawLane);
        if (c.nextPath < 0 || !m_pathActive[static_cast<std::size_t>(c.nextPath)]) {
            // The next road is not populated: one-way roads give the car
            // back (MM2 tests the first side's flag whatever the direction),
            // others turn it round onto the opposite side of its road.
            if (info.sideFlags[0] & 1) {
                returnToPool(idx);
                return false;
            }
            c.dir = -c.dir;
            const int count = static_cast<int>(info.lanesOf(c.dir).size());
            if (count == 0) {
                returnToPool(idx);
                return false;
            }
            if (count <= c.drawLane)
                c.lane = c.drawLane = count - 1;
            pushVehicle(idx, c.path, c.dir, c.drawLane);
        } else {
            pushVehicle(idx, c.nextPath, c.nextDir, c.nextLane);
            c.lane = c.drawLane = c.nextLane;
            c.dir = c.nextDir;
            c.path = c.nextPath;
        }
        lane = laneOf(c.path, c.dir, c.lane);
        if (!lane || lane->line.points.size() < 2) {
            returnToPool(idx);
            return false;
        }
        S = static_cast<int>(lane->line.points.size());
        chooseNext(c);
        c.section = 1;
        c.roadDist = c.segDist - c.segLen;
        c.segDist = c.roadDist;
        const auto& cum = lane->line.distances;
        float segLen = cum[1] - cum[0];
        Vec3 p1, m1;
        if (S == 2) {
            segLen -= c.frontBumper;
            p1 = subSectionPoint(c.path, c.dir, c.lane, 1, c.frontBumper) +
                 xAxisAt(c.path, c.dir, 1) * c.laneRandomness;
            m1 = entryVector(c.path, c.dir, segLen);
        } else {
            p1 = lanePoint(c, c.path, c.dir, c.lane, 1);
            m1 = subSectionDir(c.path, c.dir, 1, segLen);
        }
        c.segLen = std::max(segLen, 1e-3f);
        setCurve(c, lanePoint(c, c.path, c.dir, c.lane, 0), p1, exitVector(c.path, c.dir, c.segLen), m1);
        c.rail = Rail::Lane;
        c.signal = TurnSignal::None;
        break;
    }
    case Rail::LaneChange: {
        c.roadDist = (c.segDist - c.segLen) + c.laneChangeDist;
        const int i = index(c.path, c.dir, c.lane, c.roadDist);
        c.section = i;
        c.segDist = subSectionDist(c.path, c.dir, c.lane, c.roadDist);
        const auto& cum = lane->line.distances;
        float segLen = cum[static_cast<std::size_t>(i)] - cum[static_cast<std::size_t>(i - 1)];
        const Vec3 p0 = lanePoint(c, c.path, c.dir, c.lane, i - 1);
        if (i < S - 1) {
            c.segLen = std::max(segLen, 1e-3f);
            setCurve(c, p0, lanePoint(c, c.path, c.dir, c.lane, i),
                     subSectionDir(c.path, c.dir, i - 1, c.segLen), subSectionDir(c.path, c.dir, i, c.segLen));
        } else {
            segLen -= c.frontBumper;
            c.segLen = std::max(segLen, 1e-3f);
            setCurve(c, p0,
                     subSectionPoint(c.path, c.dir, c.lane, i, c.frontBumper) +
                         xAxisAt(c.path, c.dir, i) * c.laneRandomness,
                     subSectionDir(c.path, c.dir, i - 1, c.segLen), entryVector(c.path, c.dir, c.segLen));
        }
        removeVehicle(idx, c.path, c.dir, c.drawLane);
        c.drawLane = c.lane;
        chooseNext(c);
        c.rail = Rail::Lane;
        c.signal = TurnSignal::None;
        break;
    }
    case Rail::Regain:
        break;
    }
    return true;
}

// aiGoalRandomDrive::SolveLane: on roads over 60 m, move to the neighbouring
// lane with fewer cars beyond the first quarter.
void Traffic::solveLane(Car& c) {
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    if (!(60.0f < info.centreLength))
        return;
    const float q = laneLength(c.path, c.dir, c.drawLane) * 0.25f;
    auto count = [&](int lane) {
        int n = 0;
        if (const auto* list = queueOf(c.path, c.dir, lane))
            for (int o : *list) {
                if (!(q < m_cars[static_cast<std::size_t>(o)].roadDist))
                    break;
                ++n;
            }
        return n;
    };
    int n = count(c.drawLane);
    if (0 < c.drawLane) {
        const int m = count(c.drawLane - 1);
        if (m < n) {
            c.lane = c.drawLane - 1;
            n = m;
        }
    }
    const int lanes = static_cast<int>(info.lanesOf(c.dir).size());
    if (c.drawLane < lanes - 1 && count(c.drawLane + 1) < n)
        c.lane = c.drawLane + 1;
}

// aiGoalRandomDrive::ChangeLanes: a curve from the car's position onto the
// new lane, ending a quarter of the lane plus 30 m along it.
void Traffic::changeLanes(int idx) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    if (c.lane == c.drawLane)
        return;
    const Lane* oldLane = laneOf(c.path, c.dir, c.drawLane);
    const Lane* newLane = laneOf(c.path, c.dir, c.lane);
    if (!oldLane || !newLane || oldLane->line.points.size() < 2) {
        c.lane = c.drawLane;
        return;
    }
    const int oldL = c.drawLane, newL = c.lane;
    const auto& co = oldLane->line.distances;
    const auto& cn = newLane->line.distances;
    const int S = static_cast<int>(co.size());
    // The car's distance carried over to the new lane, for its list.
    int i = index(c.path, c.dir, oldL, c.roadDist);
    const float remaining = co[static_cast<std::size_t>(i)] - c.roadDist;
    addVehicle(idx, c.path, c.dir, newL, cn[static_cast<std::size_t>(i)] - remaining);
    // Start: the car's place on its current curve.
    const float laneEnd = oldLane->line.length - c.frontBumper;
    Vec3 startDir;
    const Vec3 start = curvePoint(c, c.segDist / c.segLen, &startDir);
    // End: a quarter of the lane plus 30 m, carried over to the new lane.
    const float target = laneEnd * 0.25f + 30.0f;
    i = index(c.path, c.dir, oldL, target);
    const float back = co[static_cast<std::size_t>(i)] - target;
    c.laneChangeDist = cn[static_cast<std::size_t>(i)] - back;
    const int k = index(c.path, c.dir, newL, c.laneChangeDist);
    float segLen;
    Vec3 q0, q1, m0, m1;
    if (k == S - 1) {
        // MM2 adds the back bumper to this segment's length.
        segLen = (cn[static_cast<std::size_t>(k)] - cn[static_cast<std::size_t>(k - 1)]) + c.backBumper;
        q0 = newLane->line.points[static_cast<std::size_t>(k - 1)];
        q1 = subSectionPoint(c.path, c.dir, newL, k, c.frontBumper);
        m0 = subSectionDir(c.path, c.dir, k - 1, segLen);
        m1 = entryVector(c.path, c.dir, segLen);
    } else {
        segLen = cn[static_cast<std::size_t>(k)] - cn[static_cast<std::size_t>(k - 1)];
        q0 = newLane->line.points[static_cast<std::size_t>(k - 1)];
        q1 = newLane->line.points[static_cast<std::size_t>(k)];
        m0 = subSectionDir(c.path, c.dir, k - 1, segLen);
        m1 = subSectionDir(c.path, c.dir, k, segLen);
    }
    q0 += xAxisAt(c.path, c.dir, k - 1) * c.laneRandomness;
    q1 += xAxisAt(c.path, c.dir, k) * c.laneRandomness;
    Vec3 endDir;
    const float tEnd = segLen > 0.0f ? subSectionDist(c.path, c.dir, newL, c.laneChangeDist) / segLen : 0.0f;
    const Vec3 end = hermitePoint(q0, q1, m0, m1, tEnd, &endDir);
    c.segDist = 0.0f;
    c.segLen = std::max(c.laneChangeDist - c.roadDist, 1e-3f);
    auto scaled = [&](Vec3 d) {
        const float m2 = d.mag2();
        return m2 == 0.0f ? Vec3{} : d * (c.segLen / std::sqrt(m2));
    };
    Vec3 endPoint = end;
    endPoint.y = lerp(q0.y, q1.y, clampf(tEnd, 0.0f, 1.0f));
    setCurve(c, start, endPoint, scaled(startDir), scaled(endDir));
    c.signal = oldL < newL ? TurnSignal::Right : TurnSignal::Left;
    c.rail = Rail::LaneChange;
}

// The car's matrix (the solver aiGoalRandomDrive calls after moving it).
bool Traffic::anyPlayerWithin(const Vec3& p, float radius2) const {
    for (const PlayerCar& player : m_players)
        if (player.valid && Vec2{player.transform.m3.x - p.x, player.transform.m3.z - p.z}.mag2() < radius2)
            return true;
    return false;
}

void Traffic::solvePose(Car& c) {
    const float t = c.segLen > 0.0f ? c.segDist / c.segLen : 0.0f;
    Vec3 F;
    Vec3 P = curvePoint(c, t, &F);
    const float f2 = F.mag2();
    F = f2 == 0.0f ? Vec3{0, 0, -1} : F * (1.0f / std::sqrt(f2));
    const Vec3 R{-F.z, 0.0f, F.x};
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    const city::AiPath& src = m_net.source()->paths[static_cast<std::size_t>(c.path)];
    const bool flat = info.flags & 0x8;
    const bool nextFlat =
        c.nextPath >= 0 && (m_net.paths()[static_cast<std::size_t>(c.nextPath)].flags & 0x8);
    // Flat roads (path flag 0x8): upright, at the road's first centre height.
    if ((c.nextPath < 0 && flat) || (c.goal == AmbientGoal::RandomDrive && flat && nextFlat)) {
        const float y = src.center.empty() ? P.y : src.center[0].y;
        c.transform = frameFromRows(R, Vec3::yAxis(), -F, {P.x, y, P.z});
        c.fitted = false;
        return;
    }
    // Any player within 100 m (the pose solver walks aiMap's player list).
    const bool playerNear = anyPlayerWithin(P, 10000.0f);
    const int S = static_cast<int>(src.center.size());
    if (!playerNear && c.goal != AmbientGoal::RegainRail && c.rail == Rail::Lane && c.section < S - 1 &&
        !src.xAxis.empty()) {
        // The road section's own frame; the height along the drawn lane.
        const int k = c.dir == 1 ? c.section : S - c.section;
        const auto u = static_cast<std::size_t>(std::clamp(k, 0, S - 1));
        const Vec3 a = c.dir == 1 ? -src.xAxis[u] : src.xAxis[u];
        const Vec3 c2 = c.dir == 1 ? src.zAxis[u] : -src.zAxis[u];
        float h = P.y;
        const Lane* drawn = laneOf(c.path, c.dir, c.drawLane);
        if (drawn && c.section < static_cast<int>(drawn->line.points.size())) {
            const Vec3& v0 = drawn->line.points[static_cast<std::size_t>(c.section - 1)];
            const Vec3& v1 = drawn->line.points[static_cast<std::size_t>(c.section)];
            h = (v1.y - v0.y) * t + v0.y;
        }
        c.transform = frameFromRows(a, src.yAxis[u], c2, {P.x, h, P.z});
        c.fitted = false;
        return;
    }
    // Three corners fitted to the ground: front left, front right, back left.
    const float y = c.transform.m3.y;
    Vec3 fl = P - R * c.leftSide + F * c.frontBumper;
    Vec3 fr = P + R * c.rightSide + F * c.frontBumper;
    Vec3 bl = P - R * c.leftSide - F * c.backBumper;
    for (Vec3* corner : {&fl, &fr, &bl}) {
        corner->y = m_probe ? y : P.y;
        if (!m_probe)
            continue;
        for (int k = 1; k <= 3; ++k) {
            Vec3 hit;
            const float r = 2.5f * static_cast<float>(k);
            if (m_probe({corner->x, corner->y + r, corner->z}, {corner->x, corner->y - r, corner->z}, hit)) {
                corner->y = hit.y;
                break;
            }
        }
    }
    Vec3 a = fr - fl;
    Vec3 b2 = bl - fl;
    const float u = c.frontBumper / (c.backBumper + c.frontBumper);
    const float v = c.leftSide / (c.rightSide + c.leftSide);
    const Vec3 pos = fl + a * v + b2 * u;
    a = a.normalized();
    b2 = b2.normalized();
    const Vec3 up{b2.y * a.z - b2.z * a.y, a.x * b2.z - b2.x * a.z, b2.x * a.y - a.x * b2.y};
    c.transform = frameFromRows(a, up, b2, pos);
    c.fitted = true;
}

// aiGoalRandomDrive::Update.
void Traffic::updateRandomDrive(int idx, float dt) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    if (c.goalTicks == 0)
        resetRandomDrive(c);
    solveVelocity(idx, dt);
    // (MM2 does not look at the rail type here: the first quarter of the
    // lane is always passed on a lane rail.)
    if (m_settings.laneChanges && c.laneChangeOk &&
        laneLength(c.path, c.dir, c.drawLane) * 0.25f < c.roadDist) {
        solveLane(c);
        changeLanes(idx);
        c.laneChangeOk = false;
    }
    if (c.speed <= 0.0f) {
        // A stopped car is only refitted once a player comes within 100 m
        // (for each such player in the list, as coded).
        if (!c.fitted) {
            for (const PlayerCar& player : m_players) {
                if (player.valid && Vec2{player.transform.m3.x - c.transform.m3.x,
                                         player.transform.m3.z - c.transform.m3.z}
                                            .mag2() < 10000.0f) {
                    c.fitted = true;
                    solvePose(c);
                }
            }
        }
    } else if (solveRailType(idx)) {
        solvePose(c);
        // The first player of the list within 25 m, in its path and not
        // behind the car ahead: the car avoids that player.
        for (std::size_t k = 0; k < m_players.size(); ++k) {
            const PlayerCar& player = m_players[k];
            const Vec2 d{c.transform.m3.x - player.transform.m3.x, c.transform.m3.z - player.transform.m3.z};
            if (player.valid && d.mag2() < sq(kPlayerZoneDistance) && detectPlayerCollision(c, player) &&
                !ambientBlockingPlayer(idx, player)) {
                c.avoidSlot = m_playerSlots[k];
                c.goal = AmbientGoal::AvoidPlayer;
                c.goalTicks = 0;
                return;
            }
        }
    } else {
        return; // back in the pool
    }
    ++c.curReactTicks;
    ++c.goalTicks;
}

// --- Player reactions ---------------------------------------------------------

// aiVehicleSpline::DetectPlayerCollision: the player's centre within the
// car's width along the next three 10 m pieces of its rail.
bool Traffic::detectPlayerCollision(const Car& c, const PlayerCar& p) const {
    Vec3 s = c.transform.m3;
    for (int i = 1; i <= 3; ++i) {
        const Vec3 q = railPosition(c, c.roadDist + static_cast<float>(i) * 10.0f);
        Vec2 n = xz(q - s);
        const float m2 = n.mag2();
        n = m2 == 0.0f ? Vec2{} : n * (1.0f / std::sqrt(m2));
        const Vec2 pp = xz(p.transform.m3 - s);
        const float along = pp.y * n.y + pp.x * n.x;
        const float lat = n.x * pp.y + -n.y * pp.x;
        if (along < 11.0f && c.frontBumper < along && -c.leftSide < lat && lat < c.rightSide)
            return true;
        s = q;
    }
    return false;
}

// aiVehicleSpline::DetectPlayerZoneCollision: the player within the next
// 25 m of rail, at least 3 m either side.
bool Traffic::detectPlayerZoneCollision(const Car& c, const PlayerCar& p) const {
    const Vec3& C = c.transform.m3;
    const Vec3 Z = c.transform.m2, X = c.transform.m0;
    const Vec3 q = railPosition(c, 25.0f + c.roadDist);
    const float zoneLen = Z.dot(C - q);
    const float railLat = (-X).dot(C - q);
    const float lo = std::min(railLat, -3.0f), hi = std::max(railLat, 3.0f);
    const float pz = Z.dot(C - p.transform.m3);
    const float px = (-X).dot(C - p.transform.m3);
    return pz < zoneLen && c.frontBumper < pz && lo < px && px < hi;
}

// aiVehicleSpline::IsThePlayerInFrontOfMe.
bool Traffic::playerInFront(const Car& c, const PlayerCar& p) const {
    const Vec3 d = c.transform.m3 - p.transform.m3;
    return c.transform.m2.dot(d) > c.frontBumper && d.mag2() < 625.0f;
}

// aiVehicleSpline::IsAmbientBlockingPlayer: the car ahead in the lane is
// nearer than the player.
bool Traffic::ambientBlockingPlayer(int idx, const PlayerCar& p) const {
    const Car& c = m_cars[static_cast<std::size_t>(idx)];
    const int lead = ahead(idx, c.lane);
    if (lead < 0)
        return false;
    const Vec3& C = c.transform.m3;
    const Vec3& Z = c.transform.m2;
    return Z.dot(C - m_cars[static_cast<std::size_t>(lead)].transform.m3) < Z.dot(C - p.transform.m3);
}

// Fits an off-rail car to the ground (the function aiGoalAvoidPlayer calls):
// three corners probed 5 m up and down from the current matrix.
void Traffic::fitOffRail(Car& c) {
    if (!m_probe)
        return;
    Mat34& m = c.transform;
    const Vec3 R = m.m0, F = -m.m2;
    Vec3 corners[3] = {m.m3 - R * c.leftSide + F * c.frontBumper, m.m3 + R * c.rightSide + F * c.frontBumper,
                       m.m3 - R * c.leftSide - F * c.backBumper};
    for (Vec3& p : corners) {
        Vec3 hit;
        if (m_probe(p + Vec3{0, 5, 0}, p - Vec3{0, 5, 0}, hit))
            p.y = hit.y;
        else
            p.y = m.m3.y;
    }
    Vec3 a = corners[1] - corners[0];
    Vec3 b = corners[2] - corners[0];
    const float u = c.frontBumper / (c.backBumper + c.frontBumper);
    const float v = c.leftSide / (c.rightSide + c.leftSide);
    m.m3 = corners[0] + a * v + b * u;
    m.m0 = a.normalized();
    m.m2 = b.normalized();
    m.m1 = m.m2.cross(m.m0);
}

// aiGoalAvoidPlayer: off its rail, swerving round the player.
void Traffic::updateAvoidPlayer(int idx, float dt) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    // aiMap::Player(+0xe6): the player it avoids.
    const auto slot = static_cast<std::size_t>(std::clamp(c.avoidSlot, 0, kMaxTrafficPlayers - 1));
    const PlayerCar& p = m_slotCars[slot];
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    const float cap = info.speedLimit + c.exceedLimit;
    const float R = p.radius;
    auto brakeFor = [&] {
        const float d = c.transform.m3.dist(p.transform.m3);
        const float room = d - ((R + c.separation) + c.frontBumper);
        c.accel = -((c.speed * c.speed) / (room != 0.0f ? room + room : 1e-6f));
    };
    if (c.goalTicks == 0) {
        // Reset: a chance to honk (the horn audio decides), the speed law,
        // the pass offset and whether the player is dead ahead.
        c.horn = true;
        m_avoidEvents.push_back(static_cast<int>(&c - m_cars.data()));
        if (cap <= c.speed)
            c.accel = c.accelFactor;
        else
            brakeFor();
        c.passOffset = (R + c.rightSide) + 2.5f;
        c.heading = libm::atan2(c.transform.m2.x, c.transform.m2.z);
        const float lat = c.transform.m0.dot(p.transform.m3 - c.transform.m3);
        c.centred = -0.4f <= lat && lat <= 0.4f;
    }
    ++c.goalTicks;
    // aiGoalAvoidPlayer::AvoidPlayer.
    {
        const Vec3 d = p.transform.m3 - c.transform.m3;
        const float lat = c.transform.m0.dot(d);
        const float fwd = (-c.transform.m2).dot(d);
        const float f = m_rng->frand();
        const float off = c.passOffset;
        float beta;
        if ((info.flags & 0x1) && c.drawLane == 0) {
            if (f < 0.5f) {
                c.speed -= 0.5f;
                beta = 0.4f;
            } else {
                beta = libm::atan2(lat + off, fwd);
            }
        } else if (c.centred) {
            c.accel -= 10.0f;
            beta = lat < 0.0f ? libm::atan2(lat + off, fwd) : libm::atan2(lat - off, fwd);
        } else if (std::abs(lat) <= 0.4f) {
            beta = 0.4f;
        } else if (lat > 0.4f) {
            beta = (p.horn || f < 0.7f) ? libm::atan2(lat - off, fwd) : libm::atan2(lat + off, fwd);
        } else {
            beta = (p.horn || f < 0.7f) ? libm::atan2(lat + off, fwd) : libm::atan2(lat - off, fwd);
        }
        float ratio = cap > 0.0f ? c.speed / cap : 0.0f;
        if (c.centred && c.speed > 5.0f)
            ratio = 1.0f;
        beta = clampf(beta, -0.02f, 0.02f);
        // Per update, as MM2 (not scaled by the time step).
        c.heading = beta * 1.5f * ratio + c.heading;
        const Vec3 pos = c.transform.m3;
        c.transform = Mat34::rotationY(c.heading);
        c.transform.m3 = pos;
        c.speed = dt * c.accel + c.speed;
        if (c.speed < 0.25f)
            c.speed = 0.0f;
        if (cap < c.speed)
            c.speed = cap;
        const float step = dt * c.speed;
        c.roadDist += step;
        c.transform.m3 -= c.transform.m2 * step;
        fitOffRail(c);
    }
    if (c.speed < cap)
        brakeFor();
    // The rail distance from where the car now is: its room
    // (lvlLevel::FindRoomId from its last) and the room's first road or
    // intersection (aiMap::MapComponentType). In an intersection, the
    // drawn lane's whole length plus how far the car is past the road's end
    // along its axis (XZ); on a road, aiMap::DetermineRoadPosInfo on that
    // road (with the car's side; only the distance is kept, as coded); in
    // neither, the same on the car's own road.
    {
        int id = c.path;
        int type = kNoComponent;
        if (m_map) {
            c.mapRoom = m_map->findRoom(c.transform.m3, c.mapRoom);
            type = m_map->mapComponentType(c.mapRoom, id);
        }
        const Vec3& pos = c.transform.m3;
        if (type == kIntersectionComponent) {
            const city::AiPath& road = m_net.source()->paths[static_cast<std::size_t>(c.path)];
            const std::size_t n = road.center.size();
            float past = 0.0f;
            if (n > 0 && road.zAxis.size() >= n) {
                if (c.dir == 1) {
                    const Vec3& e = road.center[n - 1];
                    const Vec3& z = road.zAxis[n - 1];
                    past = -z.x * (pos.x - e.x) + -z.z * (pos.z - e.z);
                } else {
                    const Vec3& e = road.center[0];
                    const Vec3& z = road.zAxis[0];
                    past = (pos.x - e.x) * z.x + (pos.z - e.z) * z.z;
                }
            }
            c.roadDist = laneLength(c.path, c.dir, c.drawLane) + past;
        } else {
            const int road = type == kRoadComponent ? id : c.path;
            int vert = 0, lane = 0;
            float lateral = 0.0f;
            determineRoadPosInfo(pos, c.path, road, c.dir, vert, c.roadDist, lane, lateral);
        }
    }
    if (!detectPlayerZoneCollision(c, p) && !playerInFront(c, p)) {
        c.goal = AmbientGoal::RegainRail;
        c.goalTicks = 0;
    }
    // dgPhysManager::DeclareMover(instance, 2, 0x0a): off its rail it
    // collides with the city and the instances round it.
    c.moverFlags = 0x0a;
}

// aiGoalRegainRail::Reset: a curve of up to 30 m from where the car is back
// onto its lane. MM2 first maps the car onto the road or intersection under
// it (aiMap::MapComponent); here it keeps its road and lane (inferred).
bool Traffic::roadPosInfo(int path, const Mat34& m, int& vert, float& dist, int& lane, int& dir) const {
    // aiPath::DetermineRoadPosInfo.
    const city::AiMap* map = m_net.source();
    if (!map || path < 0 || static_cast<std::size_t>(path) >= map->paths.size())
        return false;
    const city::AiPath& p = map->paths[static_cast<std::size_t>(path)];
    const int n = static_cast<int>(p.center.size());
    if (p.xAxis.size() < p.center.size() || p.zAxis.size() < p.center.size())
        return false;
    for (int i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const float dx = m.m3.x - p.center[k].x, dz = m.m3.z - p.center[k].z;
        const float along = dx * p.zAxis[k].x + dz * p.zAxis[k].z;
        const float lat = dz * p.xAxis[k].z + dx * p.xAxis[k].x;
        if (!(0.0f <= along && -p.halfWidth < lat && lat < p.halfWidth))
            continue;
        vert = i;
        dir = m.m2.x * p.zAxis[k].x + m.m2.z * p.zAxis[k].z < 0.0f ? -1 : 1;
        const city::AiRoadSide& side = dir == 1 ? p.right : p.left;
        lane = 0;
        for (int l = 0; l < side.numLanes && 2 * l + 1 < 10; ++l) {
            const float lo = side.params[static_cast<std::size_t>(2 * l)];
            const float hi = side.params[static_cast<std::size_t>(2 * l + 1)];
            if (lo < lat && lat < hi) {
                lane = l;
                break;
            }
        }
        // The distance along the lane: to the vertex, less (with the vertex
        // order) or plus (against it) how far before it the car is.
        const Lane* l = laneOf(path, dir, lane);
        if (!l || l->line.distances.size() != static_cast<std::size_t>(n))
            return true;
        const auto& cum = l->line.distances;
        dist = dir == 1 ? cum[k] - along : cum[static_cast<std::size_t>(n - 1 - i)] + along;
        return true;
    }
    return false;
}

void Traffic::pathRoadDistance(int path, const Vec3& pos, int lane, int side, int& vert, float& dist,
                               float& lateral) const {
    vert = 1;
    const city::AiMap* map = m_net.source();
    if (!map || path < 0 || static_cast<std::size_t>(path) >= map->paths.size())
        return;
    const city::AiPath& p = map->paths[static_cast<std::size_t>(path)];
    const city::AiRoadSide& s = side == 1 ? p.right : p.left;
    const int lanes = static_cast<int>(s.numLanes);
    if (lanes < lane)
        lane = lanes;
    // The row: a lane in its own order (as aiPath keeps it), or the side's
    // sidewalk row, whose lengths are the file's (3D, within 1e-4 m).
    std::vector<Vec3> sidewalkRow;
    std::vector<float> sidewalkCum;
    const std::vector<Vec3>* row = nullptr;
    const std::vector<float>* cum = nullptr;
    if (lane < lanes) {
        const Lane* l = laneOf(path, side, lane);
        if (!l)
            return;
        row = &l->line.points;
        cum = &l->line.distances;
    } else {
        if (static_cast<std::size_t>(lanes) >= s.polylines.size())
            return;
        sidewalkRow = s.polylines[static_cast<std::size_t>(lanes)];
        sidewalkCum.assign(sidewalkRow.size(), 0.0f);
        for (std::size_t i = 1; i < sidewalkRow.size(); ++i)
            sidewalkCum[i] = sidewalkRow[i].dist(sidewalkRow[i - 1]) + sidewalkCum[i - 1];
        row = &sidewalkRow;
        cum = &sidewalkCum;
    }
    const int n = static_cast<int>(p.center.size());
    if (static_cast<int>(row->size()) < n || static_cast<int>(cum->size()) < n ||
        static_cast<int>(p.zAxis.size()) < n || static_cast<int>(p.xAxis.size()) < n)
        return;
    for (int i = 1; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const Vec3& z = p.zAxis[k];
        const Vec3 d = pos - (*row)[k];
        const float along = (d.x * z.x + d.y * z.y) + d.z * z.z;
        if (!(0.0f < along))
            continue;
        const Vec3& x = p.xAxis[k];
        const Vec3 e = pos - p.center[k];
        const float off = -((e.x * x.x + e.y * x.y) + e.z * x.z);
        lateral = off;
        if (std::abs(off) < p.halfWidth + 5.0f) {
            vert = i;
            dist = (*cum)[k] - along;
            return;
        }
    }
}

void Traffic::determineRoadPosInfo(const Vec3& pos, int railPath, int path, int side, int& vert, float& dist,
                                   int& lane, float& lateral) const {
    const city::AiMap* map = m_net.source();
    if (!map || path < 0 || static_cast<std::size_t>(path) >= map->paths.size() || railPath < 0 ||
        static_cast<std::size_t>(railPath) >= map->paths.size())
        return;
    pathRoadDistance(path, pos, 0, side, vert, dist, lateral);
    const city::AiRoadSide& counts = side == 1 ? map->paths[static_cast<std::size_t>(path)].right
                                               : map->paths[static_cast<std::size_t>(path)].left;
    const city::AiRoadSide& bounds = side == 1 ? map->paths[static_cast<std::size_t>(railPath)].right
                                               : map->paths[static_cast<std::size_t>(railPath)].left;
    lane = static_cast<int>(counts.numLanes) - 1;
    const int rows = static_cast<int>(counts.numLanes) + static_cast<int>(counts.numSidewalks);
    for (int l = 0; l < rows && 2 * l + 1 < static_cast<int>(bounds.params.size()); ++l) {
        if (bounds.params[static_cast<std::size_t>(2 * l)] < lateral &&
            lateral < bounds.params[static_cast<std::size_t>(2 * l + 1)]) {
            lane = l;
            break;
        }
    }
    pathRoadDistance(path, pos, lane, side, vert, dist, lateral);
}

int Traffic::predictIntersectionPath(int node, const Mat34& m, bool freeway) const {
    // aiMap::PredictAmbIntersectionPath: of the roads leaving `node` on an
    // open side, the one whose first section points most along the car's
    // heading (that direction unnormalised for a road leaving from its last
    // vertex, as coded); the freeway version takes freeways only, both
    // directions normalised. The first listed road when none qualifies.
    const city::AiMap* map = m_net.source();
    if (!map || node < 0 || static_cast<std::size_t>(node) >= m_net.intersections().size())
        return -1;
    const auto& list = m_net.intersections()[static_cast<std::size_t>(node)].paths;
    const Vec3 h = (-m.m2).normalized();
    float best = -999999.0f;
    int choice = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const int id = list[i];
        if (id < 0 || static_cast<std::size_t>(id) >= map->paths.size())
            continue;
        const city::AiPath& p = map->paths[static_cast<std::size_t>(id)];
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(id)];
        const std::size_t n = p.center.size();
        if (n < 2)
            continue;
        Vec3 d;
        bool scored = false;
        const bool fromEnd1 = info.intersection[1] == node;
        if (fromEnd1 && (info.sideFlags[1] & 1) == 0 && (!freeway || info.freeway())) {
            d = (p.center[1] - p.center[0]).normalized();
            scored = true;
        } else if ((freeway ? info.intersection[0] == node : !fromEnd1) && (info.sideFlags[0] & 1) == 0 &&
                   (!freeway || info.freeway())) {
            d = p.center[n - 2] - p.center[n - 1];
            if (freeway)
                d = d.normalized();
            scored = true;
        }
        if (!scored)
            continue;
        const float score = h.x * d.x + (d.y * h.y + d.z * h.z);
        if (best < score) {
            best = score;
            choice = static_cast<int>(i);
        }
    }
    return list.empty() ? -1 : list[static_cast<std::size_t>(choice)];
}

void Traffic::resetRegainRail(int idx) {
    // aiGoalRegainRail::Reset.
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    const Vec3 pos = c.transform.m3;
    if (pos.dist2(c.regainStart) <= 1.0f) {
        ++c.regainAttempts;
    } else {
        c.regainAttempts = 1;
        c.regainStart = pos;
    }
    auto park = [&] { c.goal = AmbientGoal::Parked; };
    // Where the car is now (aiMap::MapComponent, its road preferred). Without
    // a map (tools, tests) it is taken to be on its road.
    int compId = c.path, compType = kRoadComponent;
    if (m_map)
        c.mapRoom = m_map->mapComponent(pos, compId, compType, c.mapRoom, c.path);
    float dist = c.regainBase;
    float regainLength = kRegainDistance;
    if (compType == kRoadComponent) {
        // On a road (its own or another): off the old lane lists, onto the
        // lane under the car, the next road chosen afresh; parked on a
        // freeway against the way it was going or on a side closed to
        // ambient cars.
        const PathInfo& old = m_net.paths()[static_cast<std::size_t>(c.path)];
        for (std::size_t l = 0; l < old.lanesOf(c.dir).size(); ++l)
            removeVehicle(idx, c.path, c.dir, static_cast<int>(l));
        roadPosInfo(compId, c.transform, c.section, dist, c.drawLane, c.dir);
        c.path = compId;
        regainLength = kRegainDistance;
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
        if ((info.freeway() && c.dir != c.nextDir) || (info.flagsOf(c.dir) & 1)) {
            park();
            return;
        }
        chooseNext(c);
        c.lane = c.drawLane;
        addVehicle(idx, c.path, c.dir, c.drawLane, regainLength + dist);
    } else if (compType == kIntersectionComponent) {
        // In an intersection: the road out of it that best matches the car's
        // heading becomes the next road, and the car rejoins its rail as far
        // through the turn as it is past the start of that road's lanes.
        const PathInfo& cur = m_net.paths()[static_cast<std::size_t>(c.path)];
        const bool nextFreeway =
            c.nextPath >= 0 && m_net.paths()[static_cast<std::size_t>(c.nextPath)].freeway();
        const bool freeway = cur.freeway() || (cur.lanesOf(c.dir).size() == 1 && nextFreeway);
        const int next = predictIntersectionPath(compId, c.transform, freeway);
        const int firstDir =
            next >= 0 && m_net.paths()[static_cast<std::size_t>(next)].intersection[1] == compId ? 1 : -1;
        const Lane* first = next >= 0 ? laneOf(next, firstDir, 0) : nullptr;
        if (next < 0 || !first || first->line.points.empty()) {
            park();
            return;
        }
        c.nextPath = next;
        const PathInfo& nextInfo = m_net.paths()[static_cast<std::size_t>(next)];
        c.nextDir = nextInfo.intersection[1] == compId ? 1 : -1;
        // (OpenMM2: the next lane kept within the new road's lanes.)
        const int nextLanes = static_cast<int>(nextInfo.lanesOf(c.nextDir).size());
        c.nextLane = std::clamp(c.nextLane, 0, std::max(nextLanes - 1, 0));
        const Vec3 start = first->line.points.front();
        const Lane* lane = laneOf(c.path, c.dir, c.lane);
        const int n = lane ? static_cast<int>(lane->line.points.size()) : 1;
        const Vec3 entry = entryPoint(c.path, c.dir, c.lane, c.frontBumper) +
                           xAxisAt(c.path, c.dir, n - 1) * c.laneRandomness;
        const float turn = std::abs(entry.x - start.x) + std::abs(entry.z - start.z);
        const city::AiPath& np = m_net.source()->paths[static_cast<std::size_t>(next)];
        const Vec3 back = c.nextDir == 1 ? np.zAxis.front() : -np.zAxis.back();
        const Vec3 rel = pos - start;
        const float before = back.x * rel.x + back.y * rel.y + back.z * rel.z;
        const float curLen = laneLength(c.path, c.dir, c.drawLane);
        dist = ((curLen - c.frontBumper) + turn) - before;
        const float total = curLen + laneLength(next, c.nextDir, c.nextLane) + turn;
        regainLength = dist + kRegainDistance <= total ? kRegainDistance : total - dist;
        if ((cur.freeway() && c.dir != c.nextDir) || (nextInfo.flagsOf(c.nextDir) & 1)) {
            park();
            return;
        }
    } else {
        // On no road or intersection (or a shortcut road, which MM2 stops
        // the game on): parked.
        park();
        return;
    }
    // The curve from the car, along its heading, to its rail point the
    // regain length on, arriving along the rail.
    c.regainLength = regainLength;
    c.regainBase = dist;
    Vec3 railDir;
    const Vec3 end = railPosition(c, dist + regainLength, &railDir);
    railDir.y = 0.0f;
    const Vec3 endDir =
        railDir.mag2() > 0.0f ? railDir.normalized() * regainLength : -c.transform.m2 * regainLength;
    const Vec3 fwd = -c.transform.m2;
    setCurve(c, pos, end, fwd * regainLength, endDir);
    c.turnY1 = end.y;
    c.segDist = 0.0f;
    c.segLen = regainLength;
    c.rail = Rail::Regain;
    c.enterInt = false;
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    c.target = info.speedLimit + c.exceedLimit;
    c.roadDist = c.regainBase;
}

// aiGoalRegainRail::Update.
void Traffic::updateRegainRail(int idx, float dt) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    if (c.goalTicks == 0) {
        resetRegainRail(idx);
        if (c.goal != AmbientGoal::RegainRail)
            return;
    }
    ++c.goalTicks;
    // Any player of the list in its path: it avoids that one.
    for (std::size_t k = 0; k < m_players.size(); ++k) {
        if (m_players[k].valid && detectPlayerCollision(c, m_players[k])) {
            c.avoidSlot = m_playerSlots[k];
            c.goal = AmbientGoal::AvoidPlayer;
            c.goalTicks = 0;
            return;
        }
    }
    if (c.speed > c.target)
        c.speed = c.target;
    else if (c.speed < c.target)
        c.speed = dt * c.accelFactor + c.speed;
    c.segDist = c.speed * dt + c.segDist;
    c.roadDist = c.segDist + c.regainBase;
    solvePose(c);
    if (c.segDist > c.regainLength) {
        c.goal = AmbientGoal::RandomDrive;
        c.goalTicks = 0;
        c.signal = TurnSignal::None; // hazards off
    }
    c.moverFlags = 0x0a; // DeclareMover(instance, 2, 0x0a), as aiGoalAvoidPlayer
}

// --- Physics hand-over ---------------------------------------------------------

void Traffic::impact(int carId, const Vec3& impulse) {
    if (carId < 0 || static_cast<std::size_t>(carId) >= m_cars.size())
        return;
    Car& c = m_cars[static_cast<std::size_t>(carId)];
    if (!c.active || c.physical)
        return;
    // aiVehicleAmbient::Impact(1).
    c.speed = 0.0f;
    c.target = m_net.paths()[static_cast<std::size_t>(c.path)].speedLimit + c.exceedLimit + 0.0001f;
    c.goal = AmbientGoal::Collision;
    c.goalTicks = 0;
    c.physical = true;
    publish();
    if (m_onImpact)
        for (auto& a : m_public)
            if (a.id == carId)
                m_onImpact(a, impulse);
}

void Traffic::detach(int carId, const Mat34& transform, bool upright) {
    if (carId < 0 || static_cast<std::size_t>(carId) >= m_cars.size())
        return;
    Car& c = m_cars[static_cast<std::size_t>(carId)];
    if (!c.active)
        return;
    c.physical = false;
    c.transform = transform;
    // aiVehicleActive::Detach: upright on the ground and never wrecked ->
    // aiVehicleAmbient::Impact(0); otherwise a wreck for good.
    if (upright && !c.wreck) {
        c.speed = 0.0f;
        c.target = m_net.paths()[static_cast<std::size_t>(c.path)].speedLimit + c.exceedLimit + 0.0001f;
        c.goal = c.regainAttempts == 3 ? AmbientGoal::Parked : AmbientGoal::RegainRail;
        c.goalTicks = 0;
    } else {
        c.wreck = true;
    }
}

void Traffic::setPhysicalTransform(int carId, const Mat34& transform) {
    if (carId >= 0 && static_cast<std::size_t>(carId) < m_cars.size())
        m_cars[static_cast<std::size_t>(carId)].transform = transform;
}

void Traffic::release(int carId) {
    if (carId >= 0 && static_cast<std::size_t>(carId) < m_cars.size() &&
        m_cars[static_cast<std::size_t>(carId)].active)
        returnToPool(carId);
}

bool Traffic::accidentAt(int intersection, int path, int dir) const {
    auto inAccident = [&](int o) { return this->inAccident(o); };
    for (int o : intersectionVehicles(intersection))
        if (inAccident(o))
            return true;
    if (path < 0 || static_cast<std::size_t>(path) >= m_roadObstacles.size())
        return false;
    const auto& lists = m_roadObstacles[static_cast<std::size_t>(path)];
    const int n = static_cast<int>(lists[1].size());
    const int k = dir == 1 ? 1 : n - 1;
    for (int o : roadVehicles(path, 1, k))
        if (inAccident(o))
            return true;
    for (int o : roadVehicles(path, -1, k))
        if (inAccident(o))
            return true;
    return false;
}

void Traffic::stopSources(int intersection, bool stop) {
    if (intersection < 0 || static_cast<std::size_t>(intersection) >= m_net.intersections().size())
        return;
    for (int p : m_net.intersections()[static_cast<std::size_t>(intersection)].paths) {
        if (p < 0 || static_cast<std::size_t>(p) >= m_net.paths().size())
            continue;
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(p)];
        // The road's control at its end here: end 0's when its end-0
        // intersection is this one, else end 1's (a loop road: end 0).
        const EntryRule rule = info.intersection[0] == intersection ? info.rule[0] : info.rule[1];
        if (rule == EntryRule::StopSign || rule == EntryRule::TrafficLight)
            m_alwaysStop[static_cast<std::size_t>(p)] = stop ? 1 : 0;
    }
}

bool Traffic::alwaysStop(int path) const {
    return path >= 0 && static_cast<std::size_t>(path) < m_alwaysStop.size() &&
           m_alwaysStop[static_cast<std::size_t>(path)] != 0;
}

// --- Update ------------------------------------------------------------------

void Traffic::step(float dt, const Vec3& pos, const Vec3& vel, int playerRoom) {
    step(dt, PlayerCar::at(pos, vel), playerRoom);
}

bool Traffic::populate(int playerRoom) {
    TrafficPlayer local;
    local.room = playerRoom;
    return populate(std::span(&local, 1));
}

bool Traffic::populate(std::span<const TrafficPlayer> players) {
    if (m_started)
        return false;
    m_started = true;
    for (const TrafficPlayer& p : players) {
        if (p.slot < 0 || p.slot >= kMaxTrafficPlayers || m_slotPresent[static_cast<std::size_t>(p.slot)])
            continue;
        m_slotPresent[static_cast<std::size_t>(p.slot)] = true;
        m_slotRooms[static_cast<std::size_t>(p.slot)] = p.room;
        adjustAmbients(0, p.room, p.slot);
    }
    m_populateAll = false;
    return true;
}

void Traffic::step(float dt, const PlayerCar& player, int playerRoom) {
    TrafficPlayer local;
    local.car = player;
    local.room = playerRoom;
    step(dt, std::span(&local, 1));
}

void Traffic::step(float dt, std::span<const TrafficPlayer> players) {
    m_players.clear();
    m_playerSlots.clear();
    std::array<bool, kMaxTrafficPlayers> listed{};
    for (const TrafficPlayer& p : players) {
        if (p.slot < 0 || p.slot >= kMaxTrafficPlayers || listed[static_cast<std::size_t>(p.slot)])
            continue;
        listed[static_cast<std::size_t>(p.slot)] = true;
        m_players.push_back(p.car);
        m_playerSlots.push_back(p.slot);
        m_slotCars[static_cast<std::size_t>(p.slot)] = p.car;
    }
    // Population: aiMap::Reset, then whenever a player enters a new room
    // (aiMap::Update). A player gone from the list gives up its roads from
    // the room they were populated for (aiMap::RemovePlayer asks the room
    // of its car's last place; inferred equivalent), one new to it starts
    // from room 0 (aiMap::AddPlayer).
    if (!populate(players)) {
        for (int slot = 0; slot < kMaxTrafficPlayers; ++slot) {
            const auto s = static_cast<std::size_t>(slot);
            if (m_slotPresent[s] && !listed[s]) {
                adjustAmbients(m_slotRooms[s], 0, slot);
                m_slotRooms[s] = 0;
                m_slotPresent[s] = false;
            }
        }
        for (const TrafficPlayer& p : players) {
            if (p.slot < 0 || p.slot >= kMaxTrafficPlayers)
                continue;
            const auto s = static_cast<std::size_t>(p.slot);
            if (!m_slotPresent[s]) {
                m_slotPresent[s] = true;
                m_slotRooms[s] = 0;
            }
            if (p.room != 0 && p.room != m_slotRooms[s]) {
                adjustAmbients(m_slotRooms[s], p.room, p.slot);
                m_slotRooms[s] = p.room;
            }
        }
    }
    // Lights that turned green restart their queues (aiTrafficLightSet::
    // Update -> aiPath::ResetVehicleReactTicks).
    for (const GreenEvent& g : m_lights.newGreens()) {
        if (g.path < 0)
            continue;
        const PathInfo& info = m_net.paths()[static_cast<std::size_t>(g.path)];
        const int dir = info.intersection[1] != g.intersection ? 1 : -1;
        for (int laneId : info.lanesOf(dir)) {
            const auto& q = m_queues[static_cast<std::size_t>(laneId)];
            if (!q.empty())
                resetReactTicks(q.front());
        }
    }
    for (auto& c : m_cars) {
        c.horn = false;
        c.moverFlags = 0; // declared afresh by this step's updates
    }
    // aiPath::UpdateAmbients for each populated road (most recently
    // populated first), direction -1 then +1, each lane list from its front:
    // a car is updated in the list of its logical lane, once a frame (the
    // vehicles' update parity). As coded, the walk takes the car behind
    // before updating a car and stops after as many cars as the list holds
    // at each step, so when a car leaves the list (onto its next road, or
    // into another lane) the last car of the list waits for the next frame.
    std::vector<std::uint8_t> seen(m_cars.size(), 0);
    const std::vector<int> paths = m_activePaths;
    for (int p : paths) {
        for (int dir : {-1, 1}) {
            const auto& lanes = m_net.paths()[static_cast<std::size_t>(p)].lanesOf(dir);
            for (std::size_t l = 0; l < lanes.size(); ++l) {
                const auto& q = m_queues[static_cast<std::size_t>(lanes[l])];
                int car = q.empty() ? -1 : q.front();
                for (std::size_t i = 0; car >= 0 && i < q.size(); ++i) {
                    int behind = -1;
                    if (auto it = std::ranges::find(q, car); it != q.end() && it + 1 != q.end())
                        behind = *(it + 1);
                    if (m_cars[static_cast<std::size_t>(car)].lane == static_cast<int>(l) &&
                        !seen[static_cast<std::size_t>(car)]) {
                        seen[static_cast<std::size_t>(car)] = 1;
                        updateCar(car, dt);
                    }
                    car = behind;
                }
            }
        }
    }
    publish();
}

// aiVehicleAmbient::Update: the running goal (its Reset first), then
// aiVehicleSpline::Update.
void Traffic::updateCar(int idx, float dt) {
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    if (!c.active)
        return;
    switch (c.goal) {
    case AmbientGoal::RandomDrive:
        updateRandomDrive(idx, dt);
        break;
    case AmbientGoal::Collision:
        if (c.goalTicks == 0)
            c.signal = TurnSignal::Hazard; // aiGoalCollision::Reset
        ++c.goalTicks;
        // aiGoalCollision::Update: a wreck (flag 2) is declared (2, 0x08), so
        // whatever drives into it collides with it.
        if (c.wreck)
            c.moverFlags = 0x08;
        break;
    case AmbientGoal::RegainRail:
        updateRegainRail(idx, dt);
        break;
    case AmbientGoal::AvoidPlayer:
        updateAvoidPlayer(idx, dt);
        break;
    case AmbientGoal::Parked:
        break;
    }
    if (!c.active)
        return;
    // aiVehicleSpline::Update: the tyres turn with the distance driven.
    c.tireRotation += dt * c.speed;
    if (c.tireRotation > kTireRotationWrap)
        c.tireRotation -= kTireRotationWrap;
    updateObstacleMap(idx);
}

std::vector<int>* Traffic::obstacleList(int path, int side, int bucket) {
    if (path < 0 || static_cast<std::size_t>(path) >= m_roadObstacles.size())
        return nullptr;
    auto& lists = m_roadObstacles[static_cast<std::size_t>(path)][side == 1 ? 1 : 0];
    if (bucket < 0 || static_cast<std::size_t>(bucket) >= lists.size())
        return nullptr;
    return &lists[static_cast<std::size_t>(bucket)];
}

std::span<const int> Traffic::roadVehicles(int path, int side, int bucket) const {
    if (path < 0 || static_cast<std::size_t>(path) >= m_roadObstacles.size())
        return {};
    const auto& lists = m_roadObstacles[static_cast<std::size_t>(path)][side == 1 ? 1 : 0];
    if (bucket < 0 || static_cast<std::size_t>(bucket) >= lists.size())
        return {};
    return lists[static_cast<std::size_t>(bucket)];
}

std::span<const int> Traffic::intersectionVehicles(int node) const {
    if (node < 0 || static_cast<std::size_t>(node) >= m_nodeObstacles.size())
        return {};
    return m_nodeObstacles[static_cast<std::size_t>(node)];
}

void Traffic::updateObstacleMap(int idx) {
    // aiVehicleSpline::UpdateObstacleMap: the car's component by its AI
    // matrix (aiMap::CoreMapComponent, its rail's road preferred); on a road,
    // the section ahead of it on the side of its rail there; re-listed when
    // that changes. Moves from one intersection straight into another are
    // not followed, and leaving every component takes the car off the road
    // list of its rail's side (which misses if it was listed on the other),
    // as coded.
    Car& c = m_cars[static_cast<std::size_t>(idx)];
    if (!m_map || !c.active)
        return;
    int id = c.mapId;
    int type = kNoComponent;
    c.mapRoom = m_map->coreMapComponent(c.transform.m3, id, type, c.mapRoom, c.path);
    auto unlink = [&](int path, int side, int bucket) {
        if (auto* list = obstacleList(path, side, bucket))
            if (auto it = std::find(list->begin(), list->end(), idx); it != list->end())
                list->erase(it);
    };
    auto unlinkNode = [&](int node) {
        if (node < 0 || static_cast<std::size_t>(node) >= m_nodeObstacles.size())
            return;
        auto& list = m_nodeObstacles[static_cast<std::size_t>(node)];
        if (auto it = std::find(list.begin(), list.end(), idx); it != list.end())
            list.erase(it);
    };
    if (type == kRoadComponent) {
        const city::AiPath* p = m_map->path(id);
        if (!p)
            return;
        const int side = id == c.path ? c.dir : c.nextDir;
        const int n = static_cast<int>(p->center.size());
        const int v = std::clamp(pathRoadVertice(*p, c.transform.m3, side), 1, n - 1);
        if (c.mapType == kRoadComponent && c.mapId == id && c.mapVert == v && c.mapSide == side)
            return;
        if (c.mapType == kIntersectionComponent)
            unlinkNode(c.mapId);
        else if (c.mapType == kRoadComponent)
            unlink(c.mapId, c.mapSide, c.mapVert);
        c.mapType = kRoadComponent;
        c.mapId = id;
        c.mapVert = v;
        c.mapSide = side;
        if (auto* list = obstacleList(id, side, v))
            list->insert(list->begin(), idx);
    } else if (type == kIntersectionComponent) {
        if (c.mapType == kIntersectionComponent)
            return;
        if (c.mapType == kRoadComponent)
            unlink(c.mapId, c.mapSide, c.mapVert);
        c.mapType = kIntersectionComponent;
        c.mapId = id;
        c.mapVert = -1;
        c.mapSide = 0;
        if (id >= 0 && static_cast<std::size_t>(id) < m_nodeObstacles.size()) {
            auto& list = m_nodeObstacles[static_cast<std::size_t>(id)];
            list.insert(list.begin(), idx);
        }
    } else {
        if (c.mapType == kNoComponent)
            return;
        if (c.mapType == kIntersectionComponent)
            unlinkNode(c.mapId);
        else if (c.mapType == kRoadComponent)
            unlink(c.mapId, c.dir, c.mapVert);
        c.mapType = kNoComponent;
        c.mapVert = -1;
    }
}

int Traffic::currentRoadIdx(int car, const int roads[3], const bool dirs[3], int* vert) const {
    if (isExternal(car)) {
        const auto k = static_cast<std::size_t>(car) - m_cars.size();
        if (k >= m_externals.size()) {
            *vert = 0;
            return -1;
        }
        return m_externals[k]->currentRoadIdx(roads, dirs, vert);
    }
    // aiVehicleSpline::CurrentRoadIdx.
    const Car& c = m_cars[static_cast<std::size_t>(car)];
    const city::AiMap* src = m_net.source();
    if (!src || c.path < 0 || static_cast<std::size_t>(c.path) >= src->paths.size()) {
        *vert = 0;
        return 0;
    }
    const int n = static_cast<int>(src->paths[static_cast<std::size_t>(c.path)].center.size());
    const int v = c.section;
    for (int i = 0; i < 3; ++i) {
        if (roads[i] != c.path)
            continue;
        if (v != n) {
            const bool same = dirs[i] == (c.dir == 1);
            *vert = same ? v : n - v;
            return i;
        }
        // On the turn through its arrival intersection.
        if (!dirs[i]) {
            *vert = 0;
            return i;
        }
        if (i < 2 && roads[i + 1] >= 0) {
            *vert = 0;
            return i + 1;
        }
        *vert = v - 1;
        return i;
    }
    const PathInfo& info = m_net.paths()[static_cast<std::size_t>(c.path)];
    const int carArrival = info.intersection[c.dir == 1 ? 0 : 1];
    for (int i = 0; i < 3; ++i) {
        if (roads[i] < 0 || static_cast<std::size_t>(roads[i]) >= m_net.paths().size())
            continue;
        const PathInfo& w = m_net.paths()[static_cast<std::size_t>(roads[i])];
        if (carArrival == w.intersection[dirs[i] ? 0 : 1]) {
            *vert = 0;
            return i + 1;
        }
    }
    *vert = 0;
    return 0;
}

void Traffic::publish() {
    m_public.clear();
    for (std::size_t i = 0; i < m_cars.size(); ++i) {
        const Car& c = m_cars[i];
        if (!c.active)
            continue;
        AmbientCar a;
        a.id = static_cast<int>(i);
        a.data = &m_types[static_cast<std::size_t>(c.type)];
        a.model = a.data->model;
        a.paint = c.paint;
        a.transform = c.transform;
        a.speed = c.speed;
        a.velocity = -c.transform.m2 * c.speed;
        a.tireRotation = c.tireRotation;
        // aiVehicleInstance::DrawGlow: the tail lights while decelerating or
        // standing.
        a.braking = c.accel < 0.0f || c.speed == 0.0f;
        a.signal = c.signal;
        a.blinkPhase = c.blinkPhase;
        a.horn = c.horn;
        a.goal = c.goal;
        a.physical = c.physical;
        a.wreck = c.wreck;
        a.moverFlags = c.moverFlags;
        a.spawns = c.spawns;
        m_public.push_back(std::move(a));
    }
}

std::size_t Traffic::activeCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(m_cars, [](const Car& c) { return c.active; }));
}

Traffic::DebugCar Traffic::debug(int carId) const {
    DebugCar d;
    if (carId < 0 || static_cast<std::size_t>(carId) >= m_cars.size())
        return d;
    const Car& c = m_cars[static_cast<std::size_t>(carId)];
    if (!c.active)
        return d;
    d.lane = m_net.lane(c.path, c.dir, c.lane);
    d.nextLane = c.nextPath >= 0 ? m_net.lane(c.nextPath, c.nextDir, c.nextLane) : -1;
    d.s = c.roadDist;
    d.turning = c.rail == Rail::Turn;
    d.changingLane = c.rail == Rail::LaneChange;
    d.entered = c.enterInt;
    d.accel = c.accel;
    d.targetVelocity = c.target;
    d.reactTicks = c.curReactTicks;
    d.totReactTicks = c.totReactTicks;
    d.goal = c.goal;
    d.lead = ahead(carId, c.lane);
    if (d.lead >= 0)
        d.leadDistance = distanceToVehicle(c, m_cars[static_cast<std::size_t>(d.lead)]);
    return d;
}

} // namespace mm2::ai
