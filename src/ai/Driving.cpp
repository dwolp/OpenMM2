// MM2's AI driving controller, aiVehiclePhysics (build 3393), on an
// ai::Course. See Driving.h and docs/ai.md ("Opponents and police") for
// what is ported and what is inferred.
// Also: aiVehiclePhysics::Position, aiVehiclePhysics::GetMatrix,
// aiVehiclePhysics::Speed, aiVehiclePhysics::FrontBumperDistance,
// aiVehiclePhysics::BackBumperDistance, aiVehiclePhysics::RSideDistance,
// aiVehiclePhysics::CurrentLane, aiVehiclePhysics::CurrentRoadId,
// aiVehiclePhysics::CurrentRdVert, aiVehiclePhysics::Type (trackedCar and the
// driver's accessors), aiStuck::Init, aiRouteNode::aiRouteNode,
// aiRouteNode::Reset (the route nodes), aiVehiclePlayer::Attach,
// aiVehiclePlayer::GetMatrix, aiVehiclePlayer::FrontBumperDistance,
// aiVehiclePlayer::BackBumperDistance, aiVehiclePlayer::LSideDistance,
// aiVehiclePlayer::RSideDistance, aiVehiclePlayer::CurrentLane,
// aiVehiclePlayer::CurrentRoadId, aiVehiclePlayer::CurrentRdVert,
// aiVehiclePlayer::Type (trackedCar).
#include "ai/Driving.h"
#include "city/Mm1Racer.h"

#include "ai/MapView.h"
#include "ai/PathGeometry.h"
#include "ai/Pedestrians.h"
#include "ai/Traffic.h"
#include "core/Libm.h"
#include "phys/AgeMath.h"
#include "phys/Bound.h"
#include "phys/World.h"
#include "phys/vehicle/CarSim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mm2::ai {
namespace {

// Right of a driving direction (Angel: forward -Z, right +X).
Vec3 rightOf(const Vec3& dir) {
    return {-dir.z, 0.0f, dir.x};
}

Vec3 flatUnit(const Vec3& v) {
    const Vec2 f{v.x, v.z};
    const float m = f.mag();
    return m > 1e-6f ? Vec3{f.x / m, 0.0f, f.y / m} : Vec3{0, 0, -1};
}

float xzDist(const Vec3& a, const Vec3& b) {
    return Vec2{a.x - b.x, a.z - b.z}.mag();
}

// Vector3::Angle: the angle between two vectors (0 when nearly parallel).
float vectorAngle(const Vec3& a, const Vec3& b) {
    const float m = a.mag2() * b.mag2();
    if (m <= 0.0f)
        return 0.0f;
    const float c = a.dot(b) * (1.0f / std::sqrt(m));
    if (c > 0.9999999f)
        return 0.0f;
    if (c < -1.0f)
        return 3.1415927f;
    return libm::acos(c);
}

// The steering MM2 computes from a heading error in
// aiVehiclePhysics::Forward and Mirror (x 1.33 x 1.428), before clamping.
float steeringGain(float angle) {
    return angle * 1.33f * 1.428f;
}

// Corners of a tracked car's box (front left, front right, back left, back
// right), as aiVehicle::PreAvoid / IsBlockingTarget build them from its
// matrix (GetMatrix) and its bumper and side distances.
std::array<Vec3, 4> corners(const TrackedCar& c) {
    const Vec3& f = c.forward;
    const Vec3 r = c.rightAxis();
    const Vec3 front = c.position + f * c.front(), back = c.position - f * c.back();
    const float left = c.leftDistance(), right = c.rightDistance();
    return {front - r * left, front + r * right, back - r * left, back + r * right};
}

// Vector3::InvMag-scaled copy (zero stays zero).
Vec3 unit3(const Vec3& v) {
    const float m2 = v.mag2();
    return m2 > 0.0f ? v * (1.0f / std::sqrt(m2)) : v;
}

// vehStuck states (vehStuck::Update): 1 watching after an impact, 2 stuck
// with the throttle pegged.
constexpr int kVehStuckIdle = 0;
constexpr int kVehStuckWatching = 1;
constexpr int kVehStuckPegged = 2;

} // namespace

Vec3 TrackedCar::rightAxis() const {
    if (right.mag2() > 0.0f)
        return right;
    return rightOf(flatUnit(forward));
}

TrackedCar trackedCar(const phys::CarSim& car, int id, bool player) {
    const Mat34& m = car.body.ics.matrix;
    TrackedCar t;
    t.id = id;
    t.position = m.m3;
    t.forward = -m.m2;
    t.right = m.m0;
    t.velocity = car.body.ics.linearVelocity;
    t.speed = car.speed();
    const Vec3 half = car.halfExtents();
    t.halfWidth = half.x;
    t.halfLength = half.z;
    if (player) {
        // aiVehiclePlayer: half of vehCarSim's Size (its InertiaBox).
        t.frontBumper = t.backBumper = car.params.inertiaBox.z * 0.5f;
        t.leftSide = t.rightSide = car.params.inertiaBox.x * 0.5f;
    } else if (const phys::Bound* b = car.body.collisionBound) {
        // aiVehiclePhysics::Init: the box of the car's bound.
        t.frontBumper = -b->boxMin.z;
        t.backBumper = b->boxMax.z;
        t.leftSide = -b->boxMin.x;
        t.rightSide = b->boxMax.x;
    }
    if (const phys::Bound* b = car.body.collisionBound) {
        t.boundMin = b->boxMin;
        t.boundMax = b->boxMax;
        t.hasBound = true;
    }
    t.body = &car.body;
    t.isPlayer = player;
    return t;
}

TrackedCar trackedAmbient(const AmbientCar& car, int id) {
    TrackedCar t;
    t.id = id;
    t.ambient = car.id;
    t.position = car.transform.m3;
    t.forward = -car.transform.m2;
    t.right = car.transform.m0;
    t.velocity = car.velocity;
    t.speed = car.speed;
    if (const VehicleData* d = car.data) {
        t.halfWidth = 0.5f * d->width();
        t.halfLength = 0.5f * d->length();
        // aiVehicleSpline::Init: the aiVehicleData box, centred on CG.
        t.frontBumper = d->size.z * 0.5f - d->cg.z;
        t.backBumper = d->cg.z + d->size.z * 0.5f;
        t.leftSide = d->size.x * 0.5f - d->cg.x;
        t.rightSide = d->cg.x + d->size.x * 0.5f;
    }
    return t;
}

bool nearestPlayer2(const Vec3& position, std::span<const TrackedCar> cars, float& distance2) {
    bool any = false;
    distance2 = 9999999.0f;
    for (const TrackedCar& c : cars) {
        if (!c.isPlayer)
            continue;
        any = true;
        const float dx = position.x - c.position.x;
        const float dy = position.y - c.position.y;
        const float dz = position.z - c.position.z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < distance2)
            distance2 = d;
    }
    return any;
}

float perFrame(float factor, float dt) {
    return libm::pow(factor, clampf(30.0f * dt, 0.01f, 2.0f));
}

float headingError(const Mat34& m, const Vec3& target) {
    const float dx = target.x - m.m3.x, dz = target.z - m.m3.z;
    return libm::atan2(dx * m.m0.x + dz * m.m0.z, -(dx * m.m2.x + dz * m.m2.z));
}

float forwardSpeed(const phys::CarSim& car) {
    return -car.body.ics.linearVelocity.dot(car.body.ics.matrix.m2);
}

bool goesOverSidewalks(std::string_view vehicle) {
    // The type names are compared case-sensitively (inlined strcmp).
    return vehicle != "vppanozgt";
}

void configureAiVehStuck(phys::CarSim& car, float timeThresh) {
    // aiVehiclePhysics::Init: TimeThresh 0.5, squared PosThresh 1.0 and
    // Rotation 0 (an AI car on its side is set upright rather than nudged).
    car.stuck.params.timeThresh = timeThresh;
    car.stuck.params.posThresh = 1.0f;
    car.stuck.posThreshSqr = 1.0f;
    car.stuck.params.rotation = 0.0f;
}

// --- aiStuck -----------------------------------------------------------------

void AiStuck::reset() {
    m_state = Idle;
    m_time = 0.0f;
}

bool AiStuck::pegged(const phys::CarSim& car) const {
    // aiStuck::Pegged: Throttle > 0.75 MaxThrottle, and while watching also
    // |Steering| < 0.5 (driving straight into something).
    if (!(car.engine.maxThrottle * 0.75f < car.engine.throttle))
        return false;
    return m_state == Stuck || std::abs(car.steering) < 0.5f;
}

void AiStuck::update(phys::CarSim& car, float dt) {
    // aiStuck::Update.
    const Vec3 pos = car.body.ics.matrix.m3;
    const float posSq = posThresh * posThresh;
    const float moveSq = moveThresh * moveThresh;
    if (m_lastPos.dist2(pos) > posSq)
        m_state = Idle;
    // MM2 starts watching when the car's own vehStuck does (after an impact).
    if (car.stuck.state == kVehStuckWatching && m_state == Idle) {
        m_time = 0.0f;
        m_lastPos = pos;
        m_state = Watching;
    }
    if (m_state == Watching) {
        m_time += dt;
        const float dx = m_lastPos.x - pos.x, dz = m_lastPos.z - pos.z;
        if (dx * dx + dz * dz < posSq && !(m_time < timeThresh) && pegged(car)) {
            m_state = Stuck;
            return;
        }
        if (!(m_lastPos.dist2(pos) > moveSq))
            return;
    } else if (m_state == Stuck) {
        if (!(m_lastPos.dist2(pos) > moveSq) && pegged(car)) {
            const float sign = car.steering > 0.0f ? 1.0f : (car.steering < 0.0f ? -1.0f : 0.0f);
            yawInPlace(car, -(sign * rotAmount * dt));
            return;
        }
    } else {
        return;
    }
    m_state = Idle;
}

// --- aiVehicle obstacle geometry -------------------------------------------

float blockingDistance(const TrackedCar& obstacle, const Vec3& from, const Vec3& to, float extra,
                       float width) {
    // A prop: aiBanger::IsBlockingTarget.
    if (obstacle.prop >= 0)
        return bangerBlockingDistance(obstacle.position, obstacle.propRadius, from, to, extra, width);
    // aiVehicle::IsBlockingTarget: the first corner (front left, front right,
    // back left, back right) ahead within the way plus `extra`, within
    // width / 2 + 1 m of the line and 0.7 rad of it (XZ).
    const Vec3 d = unit3(Vec3{to.x - from.x, 0.0f, to.z - from.z});
    const Vec3 r{-d.z, 0.0f, d.x};
    const float length = xzDist(from, to);
    const float halfWidth = width * 0.5f + 1.0f;
    for (const Vec3& c : corners(obstacle)) {
        const Vec3 rel = c - from;
        const float lateral = rel.x * r.x + rel.z * r.z;
        const float along = rel.x * d.x + rel.z * d.z;
        const float angle = libm::atan2(lateral, along);
        if (-halfWidth < lateral && lateral < halfWidth && along > 0.0f && along < length + extra &&
            angle > -0.7f && angle < 0.7f)
            return along;
    }
    return -1.0f;
}

void avoidPoints(const TrackedCar& obstacle, const Vec3& from, const Vec3& dir, float clearance, Vec3& left,
                 Vec3& right) {
    if (obstacle.prop >= 0) {
        // aiBanger::PreAvoid: from its origin P, across the (3D, unit) line
        // of sight n to it in XZ, at its radius (YRadius, at most 2) plus the
        // clearance: right P + (-n.z, 0, n.x) r, left P - (-n.z, 0, n.x) r
        // (`dir` is not used).
        const Vec3& p = obstacle.position;
        Vec3 n{p.x - from.x, p.y - from.y, p.z - from.z};
        const float m2 = (n.x * n.x + n.y * n.y) + n.z * n.z;
        const float inv = m2 == 0.0f ? 0.0f : 1.0f / std::sqrt(m2);
        const float nx = inv * n.x;
        const float nz = -(inv * n.z);
        const float r = std::min(obstacle.propRadius, 2.0f) + clearance;
        right = {nz * r + p.x, p.y + 0.0f, p.z + r * nx};
        left = {p.x - r * nz, p.y - 0.0f, p.z - r * nx};
        return;
    }
    // aiVehicle::PreAvoid: every corner pushed `clearance` both ways across
    // the (3D, unit) line of sight u to it, along (-u.z, u.y, u.x); the
    // leftmost and rightmost of these eight points seen along `dir`.
    std::array<Vec3, 8> points;
    const auto box = corners(obstacle);
    for (std::size_t i = 0; i < box.size(); ++i) {
        const Vec3 u = unit3(box[i] - from);
        const Vec3 across = Vec3{-u.z, u.y, u.x} * clearance;
        points[2 * i] = box[i] + across;
        points[2 * i + 1] = box[i] - across;
    }
    const Vec3 d = unit3(dir);
    const Vec3 r{-d.z, d.y, d.x};
    float lo = 99999.0f, hi = -99999.0f;
    left = right = obstacle.position;
    for (const Vec3& p : points) {
        const Vec3 rel = p - from;
        const float a = libm::atan2(rel.x * r.x + rel.z * r.z, rel.x * d.x + rel.z * d.z);
        if (a < lo) {
            lo = a;
            left = p;
        }
        if (a > hi) {
            hi = a;
            right = p;
        }
    }
}

// --- aiVehiclePhysics ----------------------------------------------------------

// The test of aiVehiclePhysics::DriveRoute: the car drives while its damage
// is at most the maximum (vehCarDamage CurrentDamage <= MaxDamage).
static bool aiWrecked(const phys::CarSim& car) {
    return car.damage.enabled && car.damage.maxDamage() < car.damage.currentDamage;
}

// aiVehiclePhysics::Init's type table: vppanoz 0, vpford 1, vpmustang99 2,
// vppanozgt 3, vpsemi 4, vpcaddie 5, vpbug 6, vppolice 7, vpbullet 8, vpbus
// 9, anything else -1 (an exact, case-sensitive compare).
static int vehicleTypeOf(std::string_view vehicle) {
    static constexpr std::string_view kTypes[] = {"vppanoz",  "vpford", "vpmustang99", "vppanozgt", "vpsemi",
                                                  "vpcaddie", "vpbug",  "vppolice",    "vpbullet",  "vpbus"};
    for (std::size_t i = 0; i < std::size(kTypes); ++i)
        if (vehicle == kTypes[i])
            return static_cast<int>(i);
    return -1;
}

PhysicsDriver::PhysicsDriver(phys::CarSim& car, int selfId, const MapView* map, int aiId,
                             std::string_view vehicle)
    : m_car(car), m_selfId(selfId), m_map(map), m_aiId(aiId), m_vehicleType(vehicleTypeOf(vehicle)) {
    // aiVehiclePhysics::Init: the bumper and side distances from the box of
    // the car's bound (model space).
    if (const phys::Bound* b = car.body.collisionBound) {
        m_frontBumper = -b->boxMin.z;
        m_backBumper = b->boxMax.z;
        m_leftSide = -b->boxMin.x;
        m_rightSide = b->boxMax.x;
    } else {
        const Vec3 half = car.halfExtents();
        m_frontBumper = m_backBumper = half.z;
        m_leftSide = m_rightSide = half.x;
    }
    reset();
}

void PhysicsDriver::reset() {
    // aiVehiclePhysics::Reset (which also resets the car through
    // vehCar::Reset; OpenMM2's callers place the car themselves). The
    // waypoint count goes back to 1 (MM2 keeps the list itself).
    m_lastState = State::Stop; // none: the next state runs its Init
    m_mm1RacerInited = false;
    m_state = State::Forward;
    m_wrecked = false;
    m_wreckTime = 0.0f;
    m_toDestination = false;
    m_wayPtIdx = 1;
    m_curLap = 1;
    m_curCompId = 1;
    if (m_wayPts.size() > 1)
        m_wayPts.resize(1);
    m_curCompType = kIntersectionComponent;
    m_bestRoute = -1;
    m_throttle = m_brake = m_steering = 0.0f;
    m_roads[0] = m_roads[1] = m_roads[2] = -1;
    m_nodes.fill(RouteNode{});
    for (auto& r : m_routes)
        r.fill(RouteNode{});
    m_stuck.reset();
    m_target = m_car.body.ics.matrix.m3;
    m_backupFrames = 0;
    m_backupTime = 0.0f;
}

void PhysicsDriver::apply() {
    m_car.setInputs(m_throttle, m_brake, m_steering, m_car.handBrake);
}

void PhysicsDriver::applyBrake(float brake, float dt) {
    // CalcSpeed / CalcRoadSpeed: off the throttle, brake, and damp the yaw.
    m_throttle = 0.0f;
    m_brake = clampf(brake, 0.0f, 1.0f);
    auto& ics = m_car.body.ics;
    ics.angularMomentum = ics.angularMomentum * perFrame(0.85f, dt);
}

void PhysicsDriver::driveRoute(float dt, std::span<const TrackedCar> cars, const DriveContext& ctx) {
    // aiVehiclePhysics::DriveRoute.
    m_cars = cars;
    m_car.engine.maxThrottle = params.maxThrottle;
    auto& ics = m_car.body.ics;
    if (aiWrecked(m_car)) {
        // Wrecked: with Init's repair flag (circuits) the damage is cleared
        // 5 s after the car was wrecked; otherwise it coasts, no inputs, its
        // momentum down 5 % a frame. The driver's own throttle, brake and
        // steering values are left as they were.
        if (ctx.repairWhenWrecked && m_wrecked && m_wreckTime > 5.0f) {
            m_wrecked = false;
            m_car.damage.reset();
            return;
        }
        if (!m_wrecked) {
            m_wreckTime = 0.0f;
            m_wrecked = true;
        }
        m_wreckTime += dt;
        m_car.setInputs(0.0f, 0.0f, 0.0f, m_car.handBrake);
        ics.linearMomentum = ics.linearMomentum * perFrame(0.95f, dt);
        return;
    }
    switch (m_state) {
    case State::Forward:
        if (m_lastState != State::Forward) {
            initForward();
            m_lastState = State::Forward;
        }
        forward(dt, cars, ctx);
        break;
    case State::Backup:
        if (m_lastState != State::Backup) {
            initBackup();
            m_lastState = State::Backup;
        }
        backup(dt);
        break;
    case State::Shortcut:
        if (m_lastState != State::Shortcut) {
            initShortcut();
            m_lastState = State::Shortcut;
        }
        shortcut(dt, cars);
        break;
    case State::Stop:
        stop();
        break;
    }
}

void PhysicsDriver::initForward() {
    // aiVehiclePhysics::InitForward: the driver's steering and the car's
    // inputs off (not its handbrake), aiStuck's state cleared, vehStuck
    // reset, the target on the car, no routes; a car in reverse is put in
    // first gear (the gear is written without a shift).
    m_steering = 0.0f;
    m_car.setInputs(0.0f, 0.0f, 0.0f, m_car.handBrake);
    m_stuck.clearState();
    m_car.stuck.reset();
    m_target = m_car.body.ics.matrix.m3;
    m_numRoutes = 0;
    m_bestRoute = -1;
    m_routeBlocked.fill(0);
    m_routeOnSidewalk.fill(0);
    m_routeNodeCount.fill(0);
    if (m_car.trans.currentGear == phys::Transmission::kReverse)
        m_car.trans.currentGear = phys::Transmission::kFirst;
}

void PhysicsDriver::initShortcut() {
    // aiVehiclePhysics::InitShortcut: as InitForward, without touching the
    // inputs or the target.
    m_numRoutes = 0;
    m_bestRoute = -1;
    m_stuck.clearState();
    m_car.stuck.reset();
    m_routeBlocked.fill(0);
    m_routeOnSidewalk.fill(0);
    m_routeNodeCount.fill(0);
    if (m_car.trans.currentGear == phys::Transmission::kReverse)
        m_car.trans.currentGear = phys::Transmission::kFirst;
}

bool PhysicsDriver::handleStuck(float dt) {
    // Forward and Shortcut start with aiStuck::Update, then:
    auto& ics = m_car.body.ics;
    m_stuck.update(m_car, dt);
    if (m_car.stuck.state == kVehStuckPegged) {
        // the car's own vehStuck found it stuck: PlanRoute's waypoint
        // bookkeeping, then back up with both momenta cleared;
        if (m_map)
            planRoute();
        m_state = State::Backup;
        ics.linearMomentum = {};
        ics.angularMomentum = {};
        ++m_backups;
        return true;
    }
    if (m_stuck.state() == AiStuck::Stuck) {
        // aiStuck found it stuck with the throttle pegged: full throttle,
        // full lock, no brakes, written to the car alone; vehStuck cleared.
        m_car.setInputs(1.0f, 0.0f, 1.0f, m_car.handBrake);
        m_car.stuck.state = kVehStuckIdle;
        return true;
    }
    return false;
}

bool PhysicsDriver::undrivable() {
    // vehCar's drivable flag cleared (vehCar::SetDrivable(0, ...)): with the
    // front-left wheel on the ground the car is revved (throttle 1, no
    // brakes, steering 0); vehCar::PreUpdate then holds it.
    if (m_car.drivable)
        return false;
    if (m_car.wheels[0].hit)
        m_car.setInputs(1.0f, 0.0f, 0.0f, m_car.handBrake);
    return true;
}

void PhysicsDriver::forward(float dt, std::span<const TrackedCar> cars, const DriveContext& ctx) {
    // aiVehiclePhysics::Forward.
    auto& ics = m_car.body.ics;
    if (undrivable())
        return;
    if (handleStuck(dt))
        return;
    if (!m_map)
        return;
    // Where the car is: the room's components, the road from the last
    // waypoint to the next (or the destination's road) preferred.
    const int n = static_cast<int>(m_wayPts.size());
    int hint = -1;
    if (m_wayPtIdx < n) {
        const int prevWp = m_wayPtIdx == 0 ? m_wayPts[static_cast<std::size_t>(n - 1)]
                                           : m_wayPts[static_cast<std::size_t>(m_wayPtIdx - 1)];
        bool dir = false;
        hint = m_map->roadBetween(prevWp, m_wayPts[static_cast<std::size_t>(m_wayPtIdx)], &dir);
    } else if (m_destCompType == kRoadComponent || m_destCompType == kShortcutComponent) {
        hint = m_destCompId;
    }
    m_curRoom = m_map->mapComponent(ics.matrix.m3, m_curCompId, m_curCompType, m_curRoom, hint);

    // SolveRoadTargetPoint: plan, aim at the first point of the best route,
    // and set the speed for it.
    solveRoadTargetPoint(cars);
    calcSpeed(dt);

    const Vec3 pos = ics.matrix.m3;
    const Vec3 fwd = -ics.matrix.m2;
    Vec3 aim = m_target;
    // Past the destination (within 25 m, behind the car) while heading the
    // way wanted there: brake, steering away from the target (police chasing
    // a car: do not turn round on it).
    const bool atDestination = routeOrWorking(m_bestRoute)[1].dist == 9999.0f;
    const float dx = pos.x - m_dest.x, dz = pos.z - m_dest.z;
    const Vec3& h = m_destHeading;
    if (atDestination && dx * dx + dz * dz < 625.0f && fwd.x * h.x + fwd.z * h.z > 0.0f &&
        (m_dest.x - pos.x) * fwd.x + (m_dest.z - pos.z) * fwd.z < 0.0f) {
        m_brake = 1.0f;
        m_throttle = 0.0f;
        aim = pos - (m_target - pos);
    }
    // OpenMM2 Enhanced, a car on MM1's physics: aiGoalFollowWayPts::Update steers
    // by the plain heading error clamped to +-1 (no gain, no handbrake), and
    // while it is within 0.1 rad of the target and not touching the player its
    // angular momentum is cut to a tenth each update.
    const bool mm1 = m_car.isMm1();
    const float angle = headingError(ics.matrix, aim);
    const float raw = mm1 ? angle : steeringGain(angle);
    m_steering = clampf(raw, -1.0f, 1.0f);
    if (mm1 && !ctx.touchingPlayer && angle < 0.1f && angle > -0.1f)
        ics.angularMomentum = ics.angularMomentum * 0.1f;
    // Over 30 m/s and wanting more than full lock: the handbrake.
    const float handBrake = !mm1 && m_car.speed() > 30.0f && (raw < -1.0f || raw > 1.0f) ? 1.0f : 0.0f;
    m_car.setInputs(m_throttle, m_brake, m_steering, handBrake);
    // CarFrictionHandling 2 while in contact with the player, else 1.
    m_car.params.carFrictionHandling = ctx.touchingPlayer ? 2.0f : 1.0f;
    // OpenMM2 Enhanced, a car on MM1's physics: aiGoalFollowWayPts::Update
    // sets its Realism to 1 while it touches the player's car, else 0.
    if (m_car.isMm1()) {
        m_car.realism = ctx.touchingPlayer ? 1.0f : 0.0f;
        m_car.params.carFrictionHandling = 1.0f;
    }
}

void PhysicsDriver::initBackup() {
    // aiVehiclePhysics::InitBackup: back away from the first point of the
    // route planned last (no route: the working route's); into reverse.
    m_backupFrames = 0;
    m_backupTime = 0.0f;
    m_target = routeOrWorking(m_bestRoute)[1].pos;
    if (m_car.trans.currentGear != phys::Transmission::kReverse)
        m_car.trans.setReverse();
}

void PhysicsDriver::backup(float dt) {
    // aiVehiclePhysics::Backup: reverse with opposite lock until the car
    // points within 0.1 rad of the target (then it is turned onto it about
    // its own up axis), for at most 65 frames (66 / 30 s at OpenMM2's frame
    // rate). The inputs go to the car; the driver's values stay as they were.
    if (m_car.isMm1()) {
        backupMm1();
        return;
    }
    auto& ics = m_car.body.ics;
    const Vec3 d = m_target - ics.matrix.m3;
    const float angle = libm::atan2(d.dot(ics.matrix.m0), -d.dot(ics.matrix.m2));
    if (angle <= 0.1f && angle >= -0.1f) {
        m_car.steering = 0.0f;
        phys::age::rotate(ics.matrix, ics.matrix.m1, angle);
        finishedBackingUp();
        return;
    }
    m_car.steering = clampf(angle * -2.857143f, -1.0f, 1.0f);
    if (m_backupTime > 65.5f / 30.0f) {
        finishedBackingUp();
        return;
    }
    m_car.engine.throttle = 0.85f;
    m_car.brakes = 0.0f;
    m_backupTime += dt;
    ++m_backupFrames;
}

void PhysicsDriver::backupMm1() {
    // MM1's aiGoalBackup::Update (Open1560): reverse toward the road's centre
    // line across from the car, on the next road (else the current one):
    // aiPath::Index picks the first section ahead of the car (pos - centre
    // along z above 0) whose lateral offset is within the half width, else
    // the last but one. Steering -20/7 x the angle; while the angle is over
    // 0.1 rad and the car has been in gear under 5 s (3 s below 2 m/s), throttle
    // |angle| clamped to 0.1..0.85. Otherwise FinishedBackingUp: throttle off,
    // brakes on, and forward again once under 2 m/s.
    auto& ics = m_car.body.ics;
    const Mat34& m = ics.matrix;
    const city::AiPath* path = road(1) ? road(1) : road(0);
    const auto finished = [&] {
        m_car.engine.throttle = 0.0f;
        m_car.brakes = 1.0f;
        if (m_car.speed() < 2.0f) {
            m_state = road(0) ? State::Forward : State::Shortcut;
            m_car.stuck.reset(); // Stuck.Impacted = false
        }
    };
    if (!path || path->center.size() < 3) {
        finished();
        return;
    }
    const int n = static_cast<int>(path->center.size());
    int vi = n - 2;
    for (int i = 2; i < n; ++i) {
        const Vec3 d = m.m3 - path->center[static_cast<std::size_t>(i)];
        if (!(d.dot(path->zAxis[static_cast<std::size_t>(i)]) > 0.0f))
            continue;
        if (std::abs(d.dot(path->xAxis[static_cast<std::size_t>(i)])) < path->halfWidth) {
            vi = i;
            break;
        }
    }
    const Vec3& c = path->center[static_cast<std::size_t>(vi)];
    const Vec3& x = path->xAxis[static_cast<std::size_t>(vi)];
    const Vec3 offset = c + x * x.dot(m.m3 - c) - m.m3;
    const float angle = libm::atan2(offset.dot(m.m0), offset.dot(-m.m2));
    m_car.steering = clampf(angle * -(20.0f / 7.0f), -1.0f, 1.0f);
    if (angle < -0.1f || angle > 0.1f) {
        if (m_car.trans.timeInGear < (m_car.speed() > 2.0f ? 5.0f : 3.0f)) {
            m_car.engine.throttle = clampf(std::abs(angle), 0.1f, 0.85f);
            m_car.brakes = 0.0f;
            return;
        }
    }
    finished();
}

void PhysicsDriver::finishedBackingUp() {
    // aiVehiclePhysics::FinishedBackingUp: forward again (Shortcut without a
    // road), vehStuck reset, both momenta quartered, throttle off and brakes
    // on (the car's inputs).
    m_state = road(0) ? State::Forward : State::Shortcut;
    m_car.stuck.reset();
    auto& ics = m_car.body.ics;
    ics.linearMomentum = ics.linearMomentum * 0.25f;
    ics.angularMomentum = ics.angularMomentum * 0.25f;
    m_car.engine.throttle = 0.0f;
    m_car.brakes = 1.0f;
}

void PhysicsDriver::shortcut(float dt, std::span<const TrackedCar> cars) {
    // aiVehiclePhysics::Shortcut / SolveShortcutTargetPoint: straight for the
    // next waypoint intersection, or the destination less the stop distance;
    // 1 m up; steering gain 1.33, at most 0.75.
    m_cars = cars;
    auto& ics = m_car.body.ics;
    if (undrivable())
        return;
    if (handleStuck(dt))
        return;
    if (m_map) {
        m_curRoom = m_map->mapComponent(ics.matrix.m3, m_curCompId, m_curCompType, m_curRoom);
        solveShortcutTargetPoint();
    }
    calcSpeed(dt);
    m_steering = clampf(headingError(ics.matrix, m_target) * 1.33f, -0.75f, 0.75f);
    apply();
}

void PhysicsDriver::stop() {
    // aiVehiclePhysics::Stop: brakes on, steering for the destination.
    m_target = m_dest;
    m_steering = clampf(headingError(m_car.body.ics.matrix, m_target) * 1.33f, -1.0f, 1.0f);
    m_throttle = 0.0f;
    m_brake = 1.0f;
    m_routeNodeCount[0] = 0;
    m_numRoutes = 0;
    apply();
}

void PhysicsDriver::mirror(float dt, const TrackedCar& target) {
    // aiVehiclePhysics::Mirror: hold 3 m/s under `target`'s speed and take
    // its heading (a police car blocking a suspect).
    auto& ics = m_car.body.ics;
    m_target = target.position;
    const float want = target.currentSpeed() - 3.0f;
    const float speed = m_car.speed();
    if (speed <= want) {
        m_throttle = 0.5f;
        m_brake = 0.0f;
    } else {
        const float brake = (speed - want) / (kAiGripFactor * 19.8f);
        if (brake <= 0.3f) {
            m_throttle = 0.0f;
            m_brake = 0.0f;
        } else {
            applyBrake(brake, dt);
        }
    }
    const Vec3& f = target.forward;
    const float angle = libm::atan2(f.x * ics.matrix.m0.x + f.z * ics.matrix.m0.z,
                                   -(f.x * ics.matrix.m2.x + f.z * ics.matrix.m2.z));
    m_steering = clampf(steeringGain(angle), -1.0f, 1.0f);
    apply();
}

// --- speed ---------------------------------------------------------------------

void PhysicsDriver::calcSpeed(float dt) {
    // aiVehiclePhysics::CalcSpeed: the bend at the first route point, from
    // the angle between the way to it and the way on from it. Its corner
    // speed takes the radius of a curve 10 m from the bend:
    // v = sqrt(tan((3.14 - a) / 2) * 10 * 1.2 * 19.8) * cornerSpeedFactor.
    // (With no best route MM2 reads the count of route -1, which is the
    // last route's sidewalk flag, and the working route's nodes.)
    static const bool shiftMode = std::getenv("OPENMM2_MM1_TRAFFIC_SHIFT") != nullptr;
    if (m_mm1Racer && m_mm1Racer->valid() && !shiftMode && m_mm1Blocked && m_mm1BlockSpeed < 3.0f &&
        std::fabs(m_mm1BlockSide) < 1.6f) {
        // A safety net, not caution: a standing car dead ahead (a queue at a red light) and no more room to
        // stop than a hard stop (7.5 m/s^2) needs: full brake. MM2's avoidance steers round it before this.
        const float v = m_car.speed();
        if (v > 3.0f && v * v / (2.0f * 7.5f) + 4.0f > m_mm1BlockAhead * 0.9f) {
            applyBrake(1.0f, dt);
            return;
        }
    }
    if (m_mm1Racer && m_mm1Racer->valid() && shiftMode && m_mm1Blocked && m_mm1BlockAhead < 140.0f) {
        // A slower car still in the lane ahead: brake to its speed with 6 m to spare, assuming 7.5 m/s^2
        // (a car stops no harder than that; MM1's 23.76 m/s^2 would brake too late).
        // Still in the line (or no way round): down to its speed; passing it: at most 8 m/s over it, so
        // a standing car is passed at about 18 mph.
        const float v = m_car.speed();
        const float vt = m_mm1BlockSpeed + (m_mm1InWay || m_mm1MustStop ? 0.0f : 8.0f);
        if (v > vt + 1.0f) {
            const float t = std::max((m_mm1BlockAhead - 6.0f) / v, 0.05f);
            const float demand = (v - vt) / (t * 7.5f);
            if (demand > 0.7f) {
                applyBrake(demand, dt);
                return;
            }
        }
    }
    if (m_mm1Racer && m_mm1Racer->valid() && m_dbgAvoiding) {
        // While MM2's avoidance steers, a corner on the rail still needs MM1's braking.
        const float demand = m_mm1Racer->brakeDemand(m_car.speed());
        if (demand > 0.7f) {
            applyBrake(demand, dt);
            return;
        }
    }
    if (m_mm1Racer && m_mm1Racer->valid() && !m_dbgAvoiding) {
        // OpenMM2 Enhanced: aiGoalFollowWayPts::CalcSpeed on MM1's rail (the arc's real radius and
        // distance), with MM2's physics (while MM2's avoidance steers round a car, its own route-based braking runs): over 0.7 of the AI's deceleration, brake; else MaxThrottle.
        const float demand = m_mm1Racer->brakeDemand(m_car.speed());
        if (demand > 0.7f) {
            applyBrake(demand, dt);
            return;
        }
        calcRoadSpeed(dt);
        return;
    }
    const int count =
        m_bestRoute < 0 ? m_routeOnSidewalk[kMaxRoutes - 1]
                        : m_routeNodeCount[static_cast<std::size_t>(m_bestRoute)];
    if (count > 2) {
        const auto& n = routeOrWorking(m_bestRoute);
        const float a = vectorAngle(n[1].pos - n[0].pos, n[2].pos - n[1].pos);
        if (a > kSharpTurn) {
            const float speed = m_car.speed();
            const double t = libm::tan((3.14 - std::abs(static_cast<double>(a))) * 0.5);
            const float v = static_cast<float>(std::sqrt(t * 10.0 * kAiGripFactor * 19.8) *
                                               static_cast<double>(params.cornerSpeedFactor));
            float brake = 0.0f;
            if (v < speed)
                brake = (speed - v) / ((n[1].dist / speed) * kAiGripFactor * 19.8f);
            if (params.brakeThreshold < brake) {
                applyBrake(brake, dt);
                return;
            }
        }
    }
    calcRoadSpeed(dt);
}

float PhysicsDriver::checkDistance(int turn) const {
    // aiVehiclePhysics::CheckDistance: within the turn's setback plus the
    // look-ahead (squared).
    const float d = m_turnSetback[turn] + params.lookAhead;
    return d * d;
}

void PhysicsDriver::calcRoadSpeed(float dt) {
    // aiVehiclePhysics::CalcRoadSpeed.
    m_brake = 0.0f;
    m_throttle = m_car.engine.maxThrottle;
    const Vec3 pos = m_car.body.ics.matrix.m3;
    const float speed = m_car.speed();
    const float dx = pos.x - m_dest.x, dz = pos.z - m_dest.z;
    const float d2 = dx * dx + dz * dz;
    const int n = static_cast<int>(m_wayPts.size());
    if (d2 < 5000.0f && m_wayPtIdx >= n && m_curLap >= m_numLaps) {
        // Arriving: brake to the destination speed at the destination (less
        // the stop distance); stop dead within 2.5 m of it.
        const float d = std::sqrt(d2) - params.stopShort;
        float brake = 0.0f;
        if (params.destinationSpeed < speed)
            brake = (speed - params.destinationSpeed) / ((d / speed) * kAiGripFactor * 19.8f);
        if (d * 0.014 < brake)
            applyBrake(brake, dt);
        if (d < 2.5f && params.destinationSpeed <= 0.0f) {
            m_throttle = 0.0f;
            m_brake = 1.0f;
        }
        return;
    }
    if (!m_map)
        return;
    const auto& best = routeOrWorking(m_bestRoute);
    const RouteNode& n0 = best[0];
    // The sharp turns of the window's roads, from the slot the route starts
    // in (MM2 reads it from node 0's turn code, which CalcRoute zeroes).
    // OpenMM2 Enhanced: a car on MM1's physics brakes only for junction turns,
    // as MM1's aiGoalFollowWayPts::CalcSpeed (no bends within a road).
    for (int w = m_car.isMm1() ? 3 : n0.turnCode; w < 3; ++w) {
        const city::AiPath* p = road(w);
        const PathInfo* info = roadInfo(w);
        if (!p || !info)
            continue;
        const bool rd = w <= 2 ? m_roadDir[w] : false;
        const auto& turns = info->sharpTurns;
        for (int t = 0; t < static_cast<int>(turns.size()); ++t) {
            const int vi = sharpTurnVertIndex(*p, turns, t, true);
            const SharpTurn& st = sharpTurn(turns, t, rd);
            const float v = std::sqrt(st.radius * kAiGripFactor * 19.8f) * params.cornerSpeedFactor;
            const float ix = n0.pos.x - st.point.x, iz = n0.pos.z - st.point.z;
            const int verts = static_cast<int>(p->center.size());
            float dist;
            if (rd) {
                const Vec3& z = p->zAxis[static_cast<std::size_t>(std::clamp(vi, 0, verts - 1))];
                dist = (ix * z.x + iz * z.z) - st.setback;
            } else {
                int k = 1;
                if (vi + 2 < verts) {
                    const Vec3 seg =
                        p->center[static_cast<std::size_t>(vi + 1)] - p->center[static_cast<std::size_t>(vi)];
                    const Vec3& x = p->xAxis[static_cast<std::size_t>(vi)];
                    const Vec3& z = p->zAxis[static_cast<std::size_t>(vi)];
                    const float a = libm::atan2(-x.z * seg.z + -x.x * seg.x, -z.z * seg.z + -z.x * seg.x);
                    if (sharpTurn(turns, t, false).angle != a)
                        k = 2;
                }
                const Vec3& z = p->zAxis[static_cast<std::size_t>(std::clamp(vi + k, 0, verts - 1))];
                dist = (-z.z * iz + -z.x * ix) - st.setback;
            }
            float brake = 0.0f;
            if (v < speed)
                brake = (speed - v) / ((dist / speed) * kAiGripFactor * 19.8f);
            if (params.brakeThreshold < brake) {
                applyBrake(brake, dt);
                goto junctions;
            }
        }
    }
junctions:
    // The window's junction turns: the first while the car is within its
    // setback plus the look-ahead of its corner, else the second.
    {
        const city::AiPath* r0 = road(0);
        const float a0 = m_turnAngle[0];
        const float cx0 = pos.x - m_turnCorner[0].x, cz0 = pos.z - m_turnCorner[0].z;
        if (r0 && (a0 < -0.7f || 0.7f < a0) && cz0 * cz0 + cx0 * cx0 < checkDistance(0)) {
            float v = std::sqrt(kAiGripFactor * m_turnRadius[0] * 19.8f) * params.cornerSpeedFactor;
            const PathInfo* r1 = roadInfo(1);
            const bool halved = r1 && (r1->flags & 2);
            if (halved)
                v *= 0.5f;
            float brake = 0.0f;
            if (v < speed) {
                const int last = static_cast<int>(r0->center.size()) - 1;
                const float ix = n0.pos.x - m_turnCorner[0].x, iz = n0.pos.z - m_turnCorner[0].z;
                float dist;
                if (!m_roadDir[0]) {
                    const Vec3& z = r0->zAxis.front();
                    dist = (-z.x * ix + -z.z * iz) - m_turnSetback[0];
                } else {
                    const Vec3& z = r0->zAxis[static_cast<std::size_t>(last)];
                    dist = (ix * z.x + iz * z.z) - m_turnSetback[0];
                }
                brake = (speed - v) / ((dist / speed) * kAiGripFactor * 19.8f);
            }
            if (params.brakeThreshold < brake && m_brake < brake) {
                applyBrake(brake, dt);
                // MM1 (CalcSpeed): braking for a turn onto a flagged road also
                // cuts the linear momentum by 3 % an update.
                if (halved && m_car.isMm1())
                    m_car.body.ics.linearMomentum = m_car.body.ics.linearMomentum * perFrame(0.97f, dt);
            }
            return;
        }
        const float a1 = m_turnAngle[1];
        const float cx1 = pos.x - m_turnCorner[1].x, cz1 = pos.z - m_turnCorner[1].z;
        if (road(1) && (a1 < -0.7f || 0.7f < a1) && cz1 * cz1 + cx1 * cx1 < checkDistance(1)) {
            float v = std::sqrt(kAiGripFactor * m_turnRadius[1] * 19.8f) * params.cornerSpeedFactor;
            if (const PathInfo* r2 = roadInfo(2); r2 && (r2->flags & 2))
                v *= 0.5f;
            float brake = 0.0f;
            if (v < speed) {
                const float ex = n0.pos.x - m_turnCorner[1].x, ez = n0.pos.z - m_turnCorner[1].z;
                const float dist = std::sqrt(ez * ez + ex * ex) - m_turnSetback[1];
                brake = (speed - v) / ((dist / speed) * kAiGripFactor * 19.8f);
            }
            if (params.brakeThreshold < brake && m_brake < brake)
                applyBrake(brake, dt);
        }
    }
}

void yawInPlace(phys::CarSim& car, float angle) {
    phys::age::rotate(car.body.ics.matrix, {0.0f, 1.0f, 0.0f}, angle);
}

bool blocked(const phys::GroundQuery* world, const Vec3& a, const Vec3& b, float height) {
    if (!world)
        return false;
    const Vec3 up{0.0f, height, 0.0f};
    phys::RayHit hit;
    return world->probe(a + up, b + up, hit);
}

void placeOnCourse(phys::CarSim& car, const Course& course, float s, float side, std::span<const TrackedCar> others,
                   int selfId, const std::function<void(const Mat34&)>& resetCar, const phys::GroundQuery* world) {
    Vec3 dir;
    const Vec3 p = course.pointAt(s, &dir);
    const Vec3 f = flatUnit(dir);
    const Vec3 r = rightOf(f);
    float left, right;
    course.edges(s, left, right);
    const float lo = -left + 1.5f, hi = right - 1.5f;
    // The car's own side of the road first (there may be a median).
    const float own = side >= 0.0f ? 1.0f : -1.0f;
    const float sideEdge = own > 0.0f ? hi : lo;
    float best = clampf(side, lo, hi), bestScore = -1e9f;
    for (float cand : {side, 0.5f * sideEdge, 0.75f * sideEdge, 0.25f * sideEdge, 0.0f, -0.5f * sideEdge}) {
        cand = clampf(cand, lo, hi);
        const Vec3 at = p + r * cand;
        float clear = 1e9f;
        for (const TrackedCar& c : others)
            if (c.id != selfId && std::abs(c.position.y - at.y) < 4.0f)
                clear = std::min(clear, Vec2{c.position.x - at.x, c.position.z - at.z}.mag());
        // A car-sized cross of probes must not touch any wall.
        const bool walls = blocked(world, at - r * 1.3f, at + r * 1.3f, 0.8f) ||
                           blocked(world, at - f * 2.8f, at + f * 2.8f, 0.8f);
        const float score = std::min(clear, 7.0f) - (walls ? 100.0f : 0.0f);
        if (score > bestScore + 1e-3f) {
            bestScore = score;
            best = cand;
        }
        if (score >= 7.0f)
            break;
    }
    // MM2's placement of a racer: the reset position (mmGame::
    // CollideAIOpponents: the wheels' probe from 2 m above the point to 10 m
    // below; on a hit, the hit raised by 0.9 m) and rotation, then
    // vehCarSim::Reset: the body's centre at that position plus
    // CenterOfGravity, the identity turned about Y; the model origin one
    // R * CenterOfGravity on (vehCarSim::SetWorldMatrix).
    const float rotation = phys::resetRotationOf(Mat34::rotationY(-libm::atan2(f.x, -f.z)));
    Vec3 at = p + r * best;
    if (world) {
        phys::RayHit hit;
        if (world->wheelProbe({at.x, at.y + 2.0f, at.z}, {at.x, at.y - 10.0f, at.z}, hit, &car.body, nullptr))
            at = {hit.position.x, hit.position.y + 0.9f, hit.position.z};
    }
    const Vec3& cg = car.centerOfGravity;
    Mat34 m = Mat34::identity();
    m.m3 = {cg.x + at.x, cg.y + at.y, cg.z + at.z};
    phys::age::rotate(m, {0.0f, 1.0f, 0.0f}, rotation);
    m.m3 = {((m.m1.x * cg.y + m.m2.x * cg.z) + cg.x * m.m0.x) + m.m3.x,
            ((m.m1.y * cg.y + m.m0.y * cg.x) + m.m2.y * cg.z) + m.m3.y,
            ((m.m1.z * cg.y + m.m0.z * cg.x) + m.m2.z * cg.z) + m.m3.z};
    const float current = car.damage.currentDamage, damage = car.damage.damage;
    if (resetCar)
        resetCar(m);
    else
        car.reset(m);
    car.damage.currentDamage = current;
    car.damage.damage = damage;
}

} // namespace mm2::ai
