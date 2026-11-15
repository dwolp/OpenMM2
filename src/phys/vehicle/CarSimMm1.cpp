// OpenMM2 Enhanced: Midtown Madness 1's mmCarSim (Open1560's game.asm:
// mmCarSim::Init / ConfigureDrivetrain / Update, mmForce::Update,
// VehGyro::Update, asAero::Update), decoded with tools/x87decode.py. See
// docs/mm1physics.md. What is inferred is marked.

#include "phys/vehicle/CarSim.h"

#include "core/Libm.h"
#include "phys/InertialCS.h"
#include "phys/World.h"

#include <algorithm>
#include <cmath>

namespace mm2::phys {
namespace {

constexpr float kMm1Gravity = -19.8f;
// mmForce's YDownForce globals (Chicago): above Min height the car gets an
// extra downforce of mass x (Min .. Max, rising with the height).
constexpr float kYDownForceMinHeight = 30.0f;
constexpr float kYDownForceMaxHeight = 50.0f;
constexpr float kYDownForceMin = 60.0f;
constexpr float kYDownForceMax = 90.0f;

float damping(float v, float c, float v1, float v2) {
    // asAero::Update's damping along one axis: constant, linear and a
    // quadratic term whose |v| v is cut to a whole number (an __ftol).
    const float sign = v < 0.0f ? -1.0f : 1.0f;
    const float q = static_cast<float>(static_cast<int>(std::abs(v) * v));
    return ((-(sign * c)) - v * v1) - q * v2;
}

} // namespace

void CarSim::initMm1Parts(const VehicleGeometry& g) {
    const Mm1CarParams& m = *mm1Params;
    mm1State = {};
    mm1Dimensions = {m_boundBox.max.x - m_boundBox.min.x, 0.0f, m_boundBox.max.z - m_boundBox.min.z};
    damage.params.maxDamage = m.maxDamage != 0.0f ? m.maxDamage : damage.params.maxDamage;
    damage.params.medDamage = m.medDamage != 0.0f ? m.medDamage : damage.params.medDamage;

    // The wheels: MM1's files hold the left ones; the right ones copy them
    // (mmWheel::CopyVars; inferred that they share the lateral rubber pair).
    // The rear wheels carry flag 2, and the driven ones flag 4
    // (ConfigureDrivetrain).
    const int type = std::clamp(params.drivetrainType, 0, 2);
    for (int i = 0; i < 4; ++i) {
        const Mm1WheelParams& wp = i < 2 ? m.frontLeft : m.backLeft;
        const bool driven = type == 2 || (type == 0 ? i >= 2 : i < 2);
        wheels[static_cast<std::size_t>(i)].initMm1(wp, g.wheels[static_cast<std::size_t>(i)], params.mass,
                                                    kMm1Gravity, 4, (i >= 2 ? 2 : 0) | (driven ? 4 : 0));
    }
    engine.configureMm1(m.engine);
    trans.configureMm1(m.trans);

    drivetrains = {};
    for (Drivetrain& d : drivetrains)
        d.initMm1(params.mass);
    drivetrains[2].attach(&engine, &trans);
    Wheel& fl = wheels[0];
    Wheel& fr = wheels[1];
    Wheel& bl = wheels[2];
    Wheel& br = wheels[3];
    switch (type) {
    case 0: // rear-wheel drive: the engine drives the back wheels
        drivetrains[0].addWheel(&fl);
        drivetrains[1].addWheel(&fr);
        drivetrains[2].addWheel(&bl);
        drivetrains[2].addWheel(&br);
        m_drivetrainOrder = {0, 1, 2};
        m_numDrivetrains = 3;
        break;
    case 1: // front-wheel drive
        drivetrains[0].addWheel(&bl);
        drivetrains[1].addWheel(&br);
        drivetrains[2].addWheel(&fl);
        drivetrains[2].addWheel(&fr);
        m_drivetrainOrder = {0, 1, 2};
        m_numDrivetrains = 3;
        break;
    default: // all-wheel drive (no retail MM1 car has it; MM1 probes only the first wheel of it)
        for (Wheel* w : {&fl, &fr, &bl, &br})
            drivetrains[2].addWheel(w);
        m_drivetrainOrder = {2, 0, 0};
        m_numDrivetrains = 1;
        break;
    }
    for (Wheel& w : wheels)
        w.redistLoad = w.normalLoad;
    // mmTransmission/mmEngine::Init: nothing more to compute.
    for (std::size_t a = 0; a < 2; ++a)
        axles[a] = Axle{};
}

void CarSim::updateMm1(float dt, const WheelEnv& env) {
    const Mm1CarParams& m = *mm1Params;
    InertialCS& ics = body.ics;
    Mm1State& st = mm1State;

    // --- mmCarSim::Update ---
    // 1. Brake friction multipliers.
    float frontMul = 1.0f;
    float backMul = 1.0f;
    if (0.8f < brakes) {
        frontMul = m.brakeFrontFricMultiplier;
        backMul = m.brakeBackFricMultiplier;
    }
    if (0.8f < handBrake)
        backMul = m.brakeBackFricMultiplier * backMul;

    // 2. The drift and spin assist: a quick flick of the wheel (into
    // [SpinFromMin, SpinFromMax], then back through [SpinToMin, SpinToMax]
    // within SpinStartTime) puts the car in a spin for SpinEndTime.
    const float steer = steering;
    if (st.spinStartTimer > m.spinStartTime)
        st.spinStartTimer = 0.0f;
    const float a = std::abs(steer);
    const float sgn = steer >= 0.0f ? 1.0f : -1.0f;
    bool started = false;
    if (a >= m.spinFromMin && a <= m.spinFromMax) {
        if (sgn != st.spinSight || st.spinStartTimer == 0.0f) {
            st.spinStartTimer = dt;
            st.spinSight = -sgn;
            started = true;
        }
    }
    if (!started && a >= m.spinToMin && a <= m.spinToMax) {
        if (sgn == st.spinSight && st.spinStartTimer < m.spinStartTime && st.spinStartTimer != 0.0f) {
            st.spinState = 2;
            st.spinEndTimer = dt;
        }
    }
    if (st.spinEndTimer >= m.spinEndTime) {
        st.spinState = 0;
        st.spinEndTimer = 0.0f;
    }
    if (st.spinStartTimer != 0.0f)
        st.spinStartTimer = dt + st.spinStartTimer;
    if (st.spinEndTimer != 0.0f)
        st.spinEndTimer = dt + st.spinEndTimer;
    if (st.spinState == 2) {
        frontMul = m.frontSpinFricMultiplier * frontMul;
        backMul = m.backSpinFricMultiplier * backMul;
    } else if (!(a < m.driftThreshold)) {
        frontMul = m.frontDriftFricMultiplier * frontMul;
        backMul = m.backDriftFricMultiplier * backMul;
    }

    // 3. Changed multipliers go to the wheels; the rear wheels' steer
    // multiplier is 1.
    if (frontMul != st.frontFriction) {
        wheels[0].setFricMultiplier(frontMul);
        wheels[1].setFricMultiplier(frontMul);
    }
    if (backMul != st.backFriction) {
        wheels[2].setFricMultiplier(backMul);
        wheels[3].setFricMultiplier(backMul);
    }
    wheels[2].setSteerMultiplier(1.0f);
    wheels[3].setSteerMultiplier(1.0f);
    st.frontFriction = frontMul;
    st.backFriction = backMul;
    st.prevSteering = steer;
    // The speed: the magnitude of the frame velocity.
    const Vec3& fv = ics.frameVelocity;
    const float speed = std::sqrt(fv.x * fv.x + fv.y * fv.y + fv.z * fv.z);
    m_speed = speed;
    m_speedMph = speed * kMetersPerSecondToMph;

    // 4. An automatic car at rest with no throttle holds half the brake; a
    // manual one in neutral never has less than 0.05.
    st.holdingBrake = false;
    if (trans.isAutomatic && engine.throttle == 0.0f && speed < 0.5f && !(0.5f < brakes)) {
        brakes = 0.5f;
        st.holdingBrake = true;
    }
    if (!trans.isAutomatic && trans.getCurrentGear() == 0)
        brakes = std::max(brakes, 0.05f);

    // 5. The wheel inputs: the rear wheels steer against the front; the
    // handbrake replaces their brake.
    wheels[0].setInputsMm1(steer, wheels[0].brakeRatio * brakes);
    wheels[1].setInputsMm1(steer, wheels[1].brakeRatio * brakes);
    wheels[2].setInputsMm1(-steer, wheels[2].brakeRatio * brakes);
    wheels[3].setInputsMm1(-steer, wheels[3].brakeRatio * brakes);
    if (handBrake > 0.0f) {
        wheels[2].setInputsMm1(-steer, handBrake);
        wheels[3].setInputsMm1(-steer, handBrake);
    }
    wheels[1].copyVarsMm1(wheels[0]);
    wheels[3].copyVarsMm1(wheels[2]);

    // 6. The weight redistribution: the tyres' last forces move load between
    // the wheels (forward and sideways), within the RedistHeight and
    // RedistLongRatio.
    const float weight = -(ics.gravity.y * ics.mass);
    float gripLong = 0.0f, gripLat = 0.0f;
    for (const Wheel& w : wheels) {
        gripLong = gripLong + w.tireGripLong;
        gripLat = gripLat + w.tireGripLat;
    }
    const float dimX = mm1Dimensions.x != 0.0f ? mm1Dimensions.x : 1.0f;
    const float dimZ = mm1Dimensions.z != 0.0f ? mm1Dimensions.z : 1.0f;
    float rear = ((m.redistHeight / dimZ) * ((1.0f / weight) * gripLong)) + m.redistLongRatio;
    rear = rear <= 0.0f ? 0.0f : (1.0f <= rear ? 1.0f : rear);
    float left = ((m.redistHeight / dimX) * ((1.0f / weight) * gripLat)) - -0.5f;
    left = left <= 0.0f ? 0.0f : (1.0f <= left ? 1.0f : left);
    wheels[0].redistLoad = (left * (1.0f - rear)) * weight;
    wheels[1].redistLoad = ((1.0f - left) * (1.0f - rear)) * weight;
    wheels[2].redistLoad = (left * rear) * weight;
    wheels[3].redistLoad = ((1.0f - left) * rear) * weight;

    // --- The children ---
    const bool disabled = !drivable;
    engine.updateMm1(dt, trans, primary(), params.mass, damage.damage, false);
    trans.updateMm1(dt, engine, disabled);

    // asAero::Update: damping of the body's rotation (and motion) along its own axes.
    {
        const Mat34& im = ics.matrix;
        const Vec3& v = ics.linearVelocity;
        const Vec3& w = ics.angularVelocity;
        const Vec3 axes[3] = {im.m0, im.m1, im.m2};
        const Vec3 cd[2] = {m.aero.cDamp, m.aero.angCDamp};
        const Vec3 vd[2] = {m.aero.velDamp, m.aero.angVelDamp};
        const Vec3 v2[2] = {m.aero.vel2Damp, m.aero.angVel2Damp};
        const Vec3 inertia = ics.inertia;
        Vec3 linear{}, angular{};
        for (int k = 0; k < 3; ++k) {
            const Vec3& ax = axes[k];
            const float lv = (v.x * ax.x + v.y * ax.y) + v.z * ax.z;
            const float lw = (w.x * ax.x + w.y * ax.y) + w.z * ax.z;
            const float cl = k == 0 ? cd[0].x : (k == 1 ? cd[0].y : cd[0].z);
            const float vl = k == 0 ? vd[0].x : (k == 1 ? vd[0].y : vd[0].z);
            const float ql = k == 0 ? v2[0].x : (k == 1 ? v2[0].y : v2[0].z);
            const float ca = k == 0 ? cd[1].x : (k == 1 ? cd[1].y : cd[1].z);
            const float va = k == 0 ? vd[1].x : (k == 1 ? vd[1].y : vd[1].z);
            const float qa = k == 0 ? v2[1].x : (k == 1 ? v2[1].y : v2[1].z);
            const float fl = damping(lv, cl, vl, ql) * ics.mass;
            const float ta = damping(lw, ca, va, qa) * (k == 0 ? inertia.x : (k == 1 ? inertia.y : inertia.z));
            linear = {linear.x + ax.x * fl, linear.y + ax.y * fl, linear.z + ax.z * fl};
            angular = {angular.x + ax.x * ta, angular.y + ax.y * ta, angular.z + ax.z * ta};
        }
        if (linear.x != 0.0f || linear.y != 0.0f || linear.z != 0.0f)
            ics.applyForce(linear);
        if (angular.x != 0.0f || angular.y != 0.0f || angular.z != 0.0f)
            ics.applyTorque(angular);
    }

    // mmForce::Update: drag along the velocity and downforce along the car's
    // up axis (both with the speed squared), the yaw momentum bled by
    // (1 - dt) a sample, and in Chicago the YDownForce above 30 m.
    {
        const Mat34& im = ics.matrix;
        Vec3 f{};
        if (0.001f < speed) {
            const float inv = 1.0f / speed;
            const float drag = -(m.drag * (speed * speed));
            f = {drag * (inv * ics.linearVelocity.x), drag * (inv * ics.linearVelocity.y),
                 drag * (inv * ics.linearVelocity.z)};
        }
        const float down = (speed * speed) * m.downforce;
        f = {f.x - im.m1.x * down, f.y - im.m1.y * down, f.z - im.m1.z * down};
        ics.applyForce(f);
        ics.angularMomentum.y = (1.0f - dt) * ics.angularMomentum.y;
        if (mm1Chicago && ics.matrix.m3.y > kYDownForceMinHeight) {
            const float t = (ics.matrix.m3.y - kYDownForceMinHeight) / (kYDownForceMaxHeight - kYDownForceMinHeight);
            const float load = ((t * (kYDownForceMax - kYDownForceMin)) + kYDownForceMin) * ics.mass;
            ics.applyForce({0.0f, -load, 0.0f});
        }
    }

    // VehGyro::Update: a yaw torque with the steering and the speed squared
    // (cut to a whole number), and a self-righting torque for a tilted car.
    {
        const Mat34& im = ics.matrix;
        if (m.driftTorque != 0.0f) {
            const Vec3& v = ics.linearVelocity;
            const float s = (v.x * im.m2.x + v.y * im.m2.y) + v.z * im.m2.z;
            const float k = static_cast<float>(static_cast<int>(std::abs(s) * s));
            const float t = ((k * 1.0f) * steering) * m.driftTorque;
            ics.applyTorque({im.m1.x * t, im.m1.y * t, im.m1.z * t});
        }
        if (m.weebleTorque != 0.0f && im.m1.y < 0.99f) {
            Vec3 axis{-im.m1.z, 0.0f, im.m1.x}; // up x Y
            const float len2 = axis.x * axis.x + axis.z * axis.z;
            if (len2 > 0.0f) {
                const float inv = 1.0f / std::sqrt(len2);
                const float angle = libm::acos(im.m1.y) * m.weebleTorque;
                ics.applyTorque({axis.x * inv * angle, 0.0f, axis.z * inv * angle});
            }
        }
    }

    // The drivetrains with their wheels, in the order MM1 adds them as
    // children (the freetrains, then the engine-driven one).
    for (int i = 0; i < m_numDrivetrains; ++i)
        drivetrains[static_cast<std::size_t>(m_drivetrainOrder[static_cast<std::size_t>(i)])].updateMm1(
            env, params.mass, brakes, handBrake);
}

} // namespace mm2::phys
