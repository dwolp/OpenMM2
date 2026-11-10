#include "game/Interpolation.h"

#include "ai/World.h"
#include "game/TrafficBodies.h"
#include "game/bangers/BangerSet.h"
#include "phys/AgeMath.h"
#include "phys/World.h"

#include <algorithm>
#include <cmath>

namespace mm2::game {

namespace {

float determinant(const Mat34& m) { return m.m0.dot(m.m1.cross(m.m2)); }

// The rotation of a placement whose rows have lengths `scale`.
Mat34 unscaled(const Mat34& m, const Vec3& scale) {
    Mat34 r;
    r.m0 = m.m0 * (1.0f / scale.x);
    r.m1 = m.m1 * (1.0f / scale.y);
    r.m2 = m.m2 * (1.0f / scale.z);
    return r;
}

// vehWheel::Update spins a wheel's matrix about its own axle (its first
// row) by the accumulated turn (Matrix34::Rotate): undone and redone the
// same way.
Mat34 spun(Mat34 m, float turn) {
    const Vec3 axle = m.m0;
    phys::age::rotate(m, axle, turn);
    return m;
}

} // namespace

Mat34 blendTransform(const Mat34& from, const Mat34& to, float t) {
    if (!(t > 0.0f))
        return from;
    if (t >= 1.0f)
        return to;
    const Vec3 sa{from.m0.mag(), from.m1.mag(), from.m2.mag()};
    const Vec3 sb{to.m0.mag(), to.m1.mag(), to.m2.mag()};
    const Vec3 position = lerp(from.m3, to.m3, t);
    // A degenerate or mirrored basis has no rotation to slerp: the nearer
    // of the two, moved.
    const bool degenerate = !(sa.x > 0.0f && sa.y > 0.0f && sa.z > 0.0f && sb.x > 0.0f && sb.y > 0.0f &&
                              sb.z > 0.0f) ||
                            !(determinant(from) > 0.0f && determinant(to) > 0.0f);
    if (degenerate) {
        Mat34 r = t < 0.5f ? from : to;
        r.m3 = position;
        return r;
    }
    const Quat q = Quat::slerp(Quat::fromMatrix(unscaled(from, sa)), Quat::fromMatrix(unscaled(to, sb)), t);
    Mat34 r = q.toMatrix(position);
    r.m0 = r.m0 * lerp(sa.x, sb.x, t);
    r.m1 = r.m1 * lerp(sa.y, sb.y, t);
    r.m2 = r.m2 * lerp(sa.z, sb.z, t);
    return r;
}

float blendAngle(float from, float to, float t, float period) {
    if (!(t > 0.0f))
        return from;
    if (t >= 1.0f)
        return to;
    float d = std::fmod(to - from, period);
    if (d > period * 0.5f)
        d -= period;
    else if (d < -period * 0.5f)
        d += period;
    return from + d * t;
}

bool isJump(const Mat34& from, const Mat34& to, const JumpLimits& limits) {
    if (!(from.m3.dist2(to.m3) <= limits.distance * limits.distance))
        return true;
    // The turn from one to the other: cos = (trace(Ra Rb^T) - 1) / 2.
    float trace = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float la = from.row(i).mag(), lb = to.row(i).mag();
        if (!(la > 0.0f && lb > 0.0f))
            return true;
        trace += from.row(i).dot(to.row(i)) / (la * lb);
    }
    return !((trace - 1.0f) * 0.5f >= std::cos(limits.angle));
}

VehiclePose blendPose(const VehiclePose& from, const VehiclePose& to, float t) {
    if (t >= 1.0f)
        return to;
    VehiclePose out = to;
    const float u = std::max(t, 0.0f);
    out.body = blendTransform(from.body, to.body, u);
    for (std::size_t i = 0; i < out.wheelSpin.size(); ++i) {
        out.wheelSpin[i] = blendAngle(from.wheelSpin[i], to.wheelSpin[i], u, kTwoPi);
        out.wheelSteer[i] = lerp(from.wheelSteer[i], to.wheelSteer[i], u);
        out.wheelDrop[i] = lerp(from.wheelDrop[i], to.wheelDrop[i], u);
    }
    if (!from.hasWheelWorld || !to.hasWheelWorld)
        return out;
    // The wheels in the body's frame: steering, suspension and spin are
    // what move them there.
    const bool turns = from.hasWheelTurn && to.hasWheelTurn;
    const Mat34 fromInverse = from.body.fastInverse();
    const Mat34 toInverse = to.body.fastInverse();
    for (std::size_t i = 0; i < out.wheelWorld.size(); ++i) {
        if (!from.wheelValid[i] || !to.wheelValid[i])
            continue;
        if (!turns) {
            const Mat34 local =
                blendTransform(from.wheelWorld[i] * fromInverse, to.wheelWorld[i] * toInverse, u);
            out.wheelWorld[i] = local * out.body;
            continue;
        }
        const Mat34 a = spun(from.wheelWorld[i], -from.wheelTurn[i]) * fromInverse;
        const Mat34 b = spun(to.wheelWorld[i], -to.wheelTurn[i]) * toInverse;
        out.wheelTurn[i] = lerp(from.wheelTurn[i], to.wheelTurn[i], u);
        out.wheelWorld[i] = spun(blendTransform(a, b, u) * out.body, out.wheelTurn[i]);
    }
    return out;
}

VehiclePose placePose(const VehiclePose& pose, const Mat34& body) {
    VehiclePose out = pose;
    out.body = body;
    if (pose.hasWheelWorld) {
        const Mat34 move = pose.body.fastInverse() * body;
        for (std::size_t i = 0; i < out.wheelWorld.size(); ++i)
            out.wheelWorld[i] = pose.wheelWorld[i] * move;
    }
    return out;
}

// --- StepHistory ---------------------------------------------------------------------------

void StepHistory::beginStep() {
    ++m_step;
    // Entries the step before did not record belong to objects that have
    // gone or are no longer simulated.
    const std::uint64_t last = m_step - 1;
    std::erase_if(m_transforms, [last](const auto& e) { return e.second.step < last; });
    std::erase_if(m_poses, [last](const auto& e) { return e.second.step < last; });
}

void StepHistory::record(std::uint64_t key, const Mat34& transform, std::uint32_t generation, float turn) {
    TransformEntry& e = m_transforms[key];
    e.step = m_step;
    e.generation = generation;
    e.transform = transform;
    e.turn = turn;
}

void StepHistory::record(std::uint64_t key, const VehiclePose& pose, std::uint32_t generation) {
    PoseEntry& e = m_poses[key];
    e.step = m_step;
    e.generation = generation;
    e.pose = pose;
}

void StepHistory::clear() {
    m_transforms.clear();
    m_poses.clear();
}

void StepHistory::setAlpha(float alpha) { m_alpha = std::clamp(alpha, 0.0f, 1.0f); }

const StepHistory::TransformEntry* StepHistory::previous(std::uint64_t key, const Mat34& live,
                                                         std::uint32_t generation) const {
    const auto it = m_transforms.find(key);
    if (it == m_transforms.end() || it->second.step != m_step || it->second.generation != generation ||
        isJump(it->second.transform, live, m_limits))
        return nullptr;
    return &it->second;
}

const StepHistory::PoseEntry* StepHistory::previousPose(std::uint64_t key, const Mat34& live,
                                                        std::uint32_t generation) const {
    const auto it = m_poses.find(key);
    if (it == m_poses.end() || it->second.step != m_step || it->second.generation != generation ||
        isJump(it->second.pose.body, live, m_limits))
        return nullptr;
    return &it->second;
}

Mat34 StepHistory::transform(std::uint64_t key, const Mat34& live, std::uint32_t generation) const {
    const TransformEntry* e = previous(key, live, generation);
    return e ? blendTransform(e->transform, live, m_alpha) : live;
}

float StepHistory::turn(std::uint64_t key, const Mat34& live, float liveTurn, float period,
                        std::uint32_t generation) const {
    const TransformEntry* e = previous(key, live, generation);
    return e ? blendAngle(e->turn, liveTurn, m_alpha, period) : liveTurn;
}

VehiclePose StepHistory::pose(std::uint64_t key, const VehiclePose& live, std::uint32_t generation) const {
    const PoseEntry* e = previousPose(key, live.body, generation);
    return e ? blendPose(e->pose, live, m_alpha) : live;
}

bool StepHistory::blends(std::uint64_t key, const Mat34& live, std::uint32_t generation) const {
    return previous(key, live, generation) || previousPose(key, live, generation);
}

void recordAiStep(StepHistory& history, const ai::World& world) {
    history.beginStep();
    for (const ai::AmbientCar& c : world.cars())
        if (!c.physical)
            history.record(drawnKey(Drawn::RailCar, static_cast<std::uint64_t>(c.id)), c.transform,
                           static_cast<std::uint32_t>(c.spawns), c.tireRotation);
    for (const ai::Pedestrian& p : world.peds())
        history.record(drawnKey(Drawn::Pedestrian, static_cast<std::uint64_t>(p.id)), p.transform);
}

void recordTrafficBodies(StepHistory& history, const ai::World& ai, const TrafficBodies& bodies) {
    for (const ai::AmbientCar& c : ai.cars())
        if (const auto pose = trafficBodyPose(bodies, c.id))
            history.record(drawnKey(Drawn::TrafficCar, static_cast<std::uint64_t>(c.id)), *pose);
}

void recordTrafficBodies(StepHistory& history, const TrafficBodies& bodies) {
    for (const ai::AmbientCar& c : bodies.source().cars())
        if (const auto pose = trafficBodyPose(bodies, c.id))
            history.record(drawnKey(Drawn::TrafficCar, static_cast<std::uint64_t>(c.id)), *pose);
}

void recordProps(StepHistory& history, const bangers::BangerSet& props, const phys::World& world) {
    const auto& instances = props.instances();
    for (std::size_t i = 0; i < instances.size(); ++i)
        if (const phys::Body* body = props.body(i); body && world.contains(body))
            history.record(drawnKey(Drawn::Prop, i), instances[i].matrix);
}

std::optional<VehiclePose> trafficBodyPose(const TrafficBodies& bodies, int carId) {
    const Mat34* m = bodies.transformOf(carId);
    const auto wheels = m ? bodies.wheelsOf(carId) : std::nullopt;
    if (!wheels)
        return std::nullopt;
    VehiclePose pose;
    pose.body = *m;
    pose.wheelWorld = wheels->matrix;
    pose.wheelValid = wheels->valid;
    pose.hasWheelWorld = true;
    return pose;
}

} // namespace mm2::game
