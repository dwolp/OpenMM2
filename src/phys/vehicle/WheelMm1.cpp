// OpenMM2 Enhanced: Midtown Madness 1's mmWheel (Open1560's game.asm:
// mmWheel::Init, ComputeConstants, CopyVars, ComputeDwtdw and Update),
// decoded with tools/x87decode.py. See docs/mm1physics.md for the formulas
// and which parts are inferred. The structure is MM2's vehWheel's ancestor,
// so the contact and breakpoint code follows Wheel.cpp where the asm agrees.

#include "phys/vehicle/Wheel.h"

#include "core/Libm.h"
#include "phys/AgeMath.h"
#include "phys/InertialCS.h"
#include "phys/World.h"

#include <algorithm>
#include <cmath>

namespace mm2::phys {
namespace {

float sign1(float v) {
    return v >= 0.0f ? 1.0f : -1.0f;
}

// The displacement step of mmWheel::Update (one axis): `limit` is the signed
// displacement at which the tyre force reaches the friction limit, `step` the
// velocity x dt, `old` the displacement now. A displacement already past the
// limit stays; otherwise it moves by the step but not past the limit.
float stepDisplacement(float old, float step, float limit) {
    const float moved = step + old;
    if (step >= 0.0f) {
        if (limit <= old)
            return old;
        return limit < moved ? limit : moved;
    }
    if (limit <= moved)
        return moved;
    return limit < old ? limit : old;
}

} // namespace

void Wheel::initMm1(const Mm1WheelParams& p, const WheelGeometry& g, float m, float gravity, int numWheels,
                    int wheelFlags) {
    *this = Wheel{};
    mm1 = true;
    mm1Params = p;
    flags = wheelFlags;
    center = g.center;
    radius = g.radius;
    width = g.width;
    mass = m;
    hasCar = true;
    // mmWheel::Init: NormalLoad = -(ICS.Mass x gravity) / NumWheels.
    normalLoad = -(m * gravity) / static_cast<float>(numWheels);
    spring = p.spring;
    damping = p.damping;
    steeringRatio = p.steeringRatio;
    brakeRatio = p.brakeRatio;
    suspBlend = p.suspensionBlend;
    suspLimit = p.suspensionLimit;
    renderSuspLimit = p.renderableSuspensionLimit;
    stiffLong = p.rubberSpring;
    dampLong = p.rubberDamp;
    // mmWheel::Init: the lateral rubber falls back to the longitudinal one
    // when a file has none (inferred for the _opp files, which omit them).
    stiffLat = p.rubberSpringLat != 0.0f ? p.rubberSpringLat : p.rubberSpring;
    dampLat = p.rubberDampLat != 0.0f ? p.rubberDampLat : p.rubberDamp;
    // The MM2 readers of these (the skid marks, audio, the AI) look here.
    params.optimumSlipPercent = p.optimumSlipPercent;
    params.staticFric = p.staticFric;
    params.slidingFric = p.slidingFric;
    params.suspensionLimit = p.suspensionLimit;
    computeConstantsMm1();
    reset();
}

void Wheel::copyVarsMm1(const Wheel& o) {
    // mmWheel::CopyVars: Spring, Damping, SteeringRatio, BrakeRatio,
    // StaticFriction, SuspensionLimit, SuspensionBlend, the bump size, and
    // RubberSpring, RubberDamp, OptimumSlipPercent, StaticFric, SlidingFric
    // (not the lateral rubber pair, nor the multipliers), then
    // ComputeConstants.
    spring = o.spring;
    damping = o.damping;
    steeringRatio = o.steeringRatio;
    brakeRatio = o.brakeRatio;
    suspLimit = o.suspLimit;
    suspBlend = o.suspBlend;
    renderSuspLimit = o.renderSuspLimit;
    stiffLong = o.stiffLong;
    dampLong = o.dampLong;
    mm1Params.optimumSlipPercent = o.mm1Params.optimumSlipPercent;
    mm1Params.staticFric = o.mm1Params.staticFric;
    mm1Params.slidingFric = o.mm1Params.slidingFric;
    params.optimumSlipPercent = o.params.optimumSlipPercent;
    params.staticFric = o.params.staticFric;
    params.slidingFric = o.params.slidingFric;
    params.suspensionLimit = o.params.suspensionLimit;
    computeConstantsMm1();
}

void Wheel::computeConstantsMm1() {
    // mmWheel::ComputeConstants: the friction curve mu(s) = s (2 opt - s) U1
    // up to the slip U2 where it falls to SlidingFric x FricMultiplier.
    const float opt = mm1Params.optimumSlipPercent;
    const float s = mm1Params.staticFric * fricMultiplier;
    unkFric1 = s / (opt * opt);
    const float a = (s + s) / opt;
    const float disc = std::sqrt(a * a - ((mm1Params.slidingFric * fricMultiplier) * unkFric1) * 4.0f);
    const float b = unkFric1 * -2.0f;
    const float root1 = ((-a) - disc) / b;
    const float root2 = (disc - a) / b;
    unkFric2 = root2 <= root1 ? root1 : root2;
}

float Wheel::computeDwtdwMm1(float net, const WheelEnv& env) {
    steerAngle = -((steerMultiplier * steeringInput) * steeringRatio);
    const Mat34& frame = *env.frame;
    // The wheel's frame: the car's model matrix turned by the steering about
    // its own Y axis (Matrix34::Rotate).
    Mat34 a = frame;
    age::rotate(a, {0.0f, 1.0f, 0.0f}, steerAngle);
    const Vec3 mount = frame.transform(center);
    const Vec3& up = a.m1;
    const float reach = suspLimit + radius;
    const Vec3 top{up.x * reach + mount.x, up.y * reach + mount.y, up.z * reach + mount.z};
    const Vec3 bottom{mount.x - up.x * reach, mount.y - up.y * reach, mount.z - up.z * reach};

    hit = false;
    RayHit isect;
    if (env.ground && env.ground->wheelProbe(top, bottom, isect, env.self, &probeCache)) {
        intersection = isect;
        material = &env.ground->material(isect.material);
        // A water polygon counts as a miss (mmCullCity::IsPolyWater).
        hit = material->depth < 1.0f;
        const Vec3& n = isect.normal;
        if (hit && (n.x * n.x + n.y * n.y + n.z * n.z) == 0.0f)
            hit = false;
    }
    // The wheel's world matrix for the renderer: mounted, turned, displaced
    // by the (clamped) suspension travel and spun.
    matrix = a;
    matrix.m3 = mount;
    if (!hit) {
        // mmWheel::ComputeDwtdw without a contact: the breakpoint is the
        // sentinel the drivetrain reads as "no limit".
        return (net >= 0.0f ? 1.0f : -1.0f) * -1e10f;
    }

    const Vec3 n = isect.normal;
    position = isect.position;
    contactFrame.m3 = position;
    contactFrame.m1 = n;
    // The wheel's axes (A.m0 right, A.m2 back) and the contact's velocity: the
    // car's (the linear one for the driven wheels, the frame's otherwise)
    // plus the spin about the centre of mass at the contact, less the part
    // along the normal.
    InertialCS& ics = *env.ics;
    const Vec3 base = (flags & 4) ? ics.linearVelocity : ics.frameVelocity;
    const Vec3 r{position.x - ics.matrix.m3.x, position.y - ics.matrix.m3.y, position.z - ics.matrix.m3.z};
    const Vec3& w = ics.angularVelocity;
    const Vec3 v{(r.z * w.y - w.z * r.y) + base.x, (w.z * r.x - r.z * w.x) + base.y, (w.x * r.y - w.y * r.x) + base.z};
    const float vn = (n.x * v.x + n.y * v.y) + n.z * v.z;
    contactVelocity = {v.x - n.x * vn, v.y - n.y * vn, v.z - n.z * vn};
    normalVelocity = vn;
    const Vec3& vt = contactVelocity;
    latVelocity = (vt.x * a.m0.x + vt.y * a.m0.y) + vt.z * a.m0.z;
    fwdVelocity = -((vt.x * a.m2.x + vt.y * a.m2.y) + vt.z * a.m2.z);
    contactFrame.m0 = a.m0;
    contactFrame.m2 = a.m2;

    // The suspension: compression from where the probe meets the ground, its
    // rate limited below at -3 m/s, the load from the spring and damper on
    // top of the static weight share, floored at 0 and scaled by how level the
    // ground is.
    const float dist = isect.t * (2.0f * reach);
    const float prev = suspension;
    const float newSusp = (suspLimit + radius * 2.0f) - dist;
    float rate = (newSusp - prev) * env.invDt;
    if (rate <= -3.0f)
        rate = -3.0f;
    suspension = newSusp;
    suspensionVelocity = rate;
    float load = (spring * newSusp + damping * rate) + normalLoad;
    if (load < 0.0f)
        load = 0.0f;
    const float align = (n.z * up.z + n.y * up.y) + n.x * up.x;
    currentLoad = load * align;
    suspensionForce = currentLoad;
    const float base2 = (flags & 1) ? normalLoad : redistLoad;
    blendedLoad = (1.0f - suspBlend) * base2 + suspBlend * currentLoad;

    // Breakpoint: the wheel speed where the long slip reaches the optimum
    // (as MM2's vehWheel::ComputeDwtdw; the original's slope term is
    // discarded).
    const float opt = mm1Params.optimumSlipPercent;
    const float w0 = -(fwdVelocity / radius);
    const float lo = (1.0f - opt) * w0;
    const float hi = (opt + 1.0f) * w0;
    float c = longSlipPercent;
    if (c < -opt)
        c = -opt;
    else if (opt < c)
        c = opt;
    const float t = (1.0f - c) * w0;
    float inner, outer;
    if (lo <= hi) {
        inner = w0 < t ? w0 : t;
        outer = w0 <= t ? t : w0;
    } else {
        inner = t < w0 ? w0 : t;
        outer = t <= w0 ? t : w0;
    }
    const float rot = rotationSpeed;
    if (0.0f <= net) {
        if (fwdVelocity <= 0.0f) {
            if (outer <= rot)
                return outer;
            if (lo <= rot)
                return lo;
        } else {
            if (inner <= rot)
                return inner;
            if (hi <= rot)
                return hi;
        }
        return -1e11f;
    }
    if (fwdVelocity <= 0.0f) {
        if (rot <= inner)
            return inner;
        if (rot <= hi)
            return hi;
    } else {
        if (rot <= outer)
            return outer;
        if (rot <= lo)
            return lo;
    }
    return 1e11f;
}

void Wheel::updateMm1(const WheelEnv& env) {
    InertialCS& ics = *env.ics;
    const float dt = env.dt;
    steerAngle = -((steeringInput * steeringRatio) * steerMultiplier);
    if (!hit) {
        onGround = false;
        currentTireDispLat = 0.0f;
        currentTireDispLong = 0.0f;
        suspension = -suspLimit;
        tireGripLat = 0.0f;
        tireGripLong = 0.0f;
        tireResistance = 0.0f;
        slide = 0.0f;
        skidding = false;
    } else {
        onGround = true;
        // The surface's friction (agiPhysParameters), by the weather, then the
        // car's CarFrictionHandling (as MM2's wheel does).
        friction = material ? material->friction : 1.0f;
        friction = env.weatherFriction * friction;
        if (friction < 1.0f) {
            if (1.0f <= env.carFrictionHandling)
                friction = (1.0f / env.carFrictionHandling) * friction;
            else
                friction = (1.0f - env.carFrictionHandling) * (1.0f - friction) + friction;
        }
        const Mat34& a = matrix; // the wheel's frame, set by computeDwtdwMm1
        const Vec3& vt = contactVelocity;
        const float slipVel = rotationSpeed * radius + fwdVelocity;
        slipVelocity = slipVel;
        (void)vt;
        // The slip percentages: the lateral or longitudinal velocity over the
        // forward speed, limited to +-1.
        if (latVelocity == 0.0f)
            latSlipPercent = 0.0f;
        else if (std::abs(fwdVelocity) < std::abs(latVelocity))
            latSlipPercent = sign1(latVelocity);
        else
            latSlipPercent = latVelocity / std::abs(fwdVelocity);
        if (slipVel == 0.0f)
            longSlipPercent = 0.0f;
        else if (std::abs(fwdVelocity) < std::abs(slipVel))
            longSlipPercent = sign1(slipVel);
        else
            longSlipPercent = slipVel / std::abs(fwdVelocity);

        const float load = currentLoad > normalLoad ? currentLoad : normalLoad;
        const float opt = mm1Params.optimumSlipPercent;
        // The friction curve (mu x load), lateral: the sliding value past
        // UnkFriction2, else the parabola. The longitudinal one omits
        // FricMultiplier in the sliding branch and takes the car's
        // LongSlideMultiplier twice (as the asm does).
        const float sl = std::abs(latSlipPercent);
        const float muLat = sl >= unkFric2 ? fricMultiplier * mm1Params.slidingFric
                                           : sl * ((opt * 2.0f - sl) * unkFric1);
        const float sg = std::abs(longSlipPercent);
        float muLong = sg >= unkFric2 ? mm1Params.slidingFric : sg * ((opt * 2.0f - sg) * unkFric1);
        const float lsm = env.longSlideMultiplier;
        const float stepLat = dt * latVelocity;
        const float stepLong = dt * slipVel;
        const float limitLat = (((muLat * load) * friction) / stiffLat) * sign1(latVelocity);
        const float limitLong = ((((muLong * lsm) * load) * lsm) * friction) / stiffLong * sign1(slipVel);

        const float oldLat = currentTireDispLat;
        const float oldLong = currentTireDispLong;
        currentTireDispLat = stepDisplacement(oldLat, stepLat, limitLat);
        currentTireDispLong = stepDisplacement(oldLong, stepLong, limitLong);

        // The lateral damper's rate blends the real change (R = 1) with the
        // step the limit lets through (R = 0): mmCarSim's `*Realism`.
        float allowed = 0.0f;
        if (sign1(limitLat) == sign1(stepLat))
            allowed = sign1(limitLat) * std::min(std::abs(limitLat), std::abs(stepLat));
        const float R = env.realism;
        const float rateLat = (currentTireDispLat - oldLat) * R + (1.0f - R) * allowed;
        tireGripLat = -(currentTireDispLat * stiffLat) - (env.invDt * rateLat) * dampLat;
        tireGripLong = -(currentTireDispLong * stiffLong) - (env.invDt * (currentTireDispLong - oldLong)) * dampLong;
        tireResistance = tireGripLong * radius;
        // Sliding for the skid marks and the sound: how far the tyre is past
        // its optimum slip (inferred: MM1 reads the slips in mmCarSim's
        // ShouldSkid and the audio).
        slide = std::max(sl > opt ? 1.0f : sl / opt * 0.5f, sg > opt ? 1.0f : sg / opt * 0.5f);
        skidding = 0.5f < slide;

        // The force on the body: the load along the car's up axis, the tyre
        // grip along the wheel's lateral and (back) forward axes.
        const Mat34& im = ics.matrix;
        const Vec3 force{
            (currentLoad * im.m1.x + (tireGripLat * a.m0.x - tireGripLong * a.m2.x)),
            (currentLoad * im.m1.y + (tireGripLat * a.m0.y - tireGripLong * a.m2.y)),
            (currentLoad * im.m1.z + (tireGripLat * a.m0.z - tireGripLong * a.m2.z)),
        };
        ics.applyForce(force, position);
    }

    // The wheel's visual state (the PATCH in Open1560: the spin is added
    // once).
    rotation = dt * rotationSpeed + rotation;
    const float d = std::clamp(suspension, -renderSuspLimit, renderSuspLimit);
    matrix.m3 = {d * matrix.m1.x + matrix.m3.x, d * matrix.m1.y + matrix.m3.y, d * matrix.m1.z + matrix.m3.z};
    const Vec3 axle = matrix.m0;
    age::rotate(matrix, axle, rotation);
}

} // namespace mm2::phys
