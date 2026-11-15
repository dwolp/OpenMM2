// OpenMM2 Enhanced: Midtown Madness 1's mmDrivetrain::Update (Open1560's
// game.asm 159596-160085). MM2's vehDrivetrain descends from it; the
// differences (brake force from the pedal and the first wheel's radius, a
// fixed damping of 300, no limited-slip differential, the nearer of two
// breakpoints) are listed in docs/mm1physics.md.

#include "phys/vehicle/Drivetrain.h"
#include "phys/vehicle/Engine.h"
#include "phys/vehicle/Transmission.h"

#include <cmath>

namespace mm2::phys {

void Drivetrain::updateMm1(const WheelEnv& env, float carMass, float brakes, float handBrake) {
    const float dt = env.dt;
    const int n = m_numWheels;
    Wheel& w0 = *m_wheels[0];

    // The brake: the handbrake for a wheel group flagged 2 (the rear), else
    // the pedal times the wheel's BrakeRatio (plus 0.002); the static
    // coefficient while the first wheel is stopped.
    const float input = (w0.flags & 2) ? handBrake : brakes * w0.brakeRatio + 0.002f;
    const float coef = w0.rotationSpeed == 0.0f ? brakeStaticCoef : brakeDynamicCoef;
    const float scaled = coef * input;
    float brake = 0.0f;
    if (n > 0) {
        const float per = scaled / w0.radius;
        for (int i = 0; i < n; ++i)
            brake = brake + per;
    }

    // The engine's torque through the gear, less the tyres' last torques.
    float net = 0.0f;
    float ratio = 0.0f;
    if (m_engine)
        net = m_engine->torque;
    if (m_trans) {
        ratio = m_trans->currentRatio();
        net = net * ratio;
    }
    for (int i = 0; i < n; ++i)
        net = net - m_wheels[static_cast<std::size_t>(i)]->tireResistance;

    // The brake opposes the rotation; at rest it holds against the net torque.
    float eff;
    bool holds = false;
    const float w = w0.rotationSpeed;
    if (w == 0.0f) {
        if (net < 0.0f) {
            const float x = net + brake;
            eff = 0.0f < x ? 0.0f : x;
        } else {
            const float x = net - brake;
            eff = 0.0f <= x ? x : 0.0f;
        }
    } else {
        const float s = w < 0.0f ? -1.0f : 1.0f;
        const float b = s * brake;
        holds = std::abs(net) <= std::abs(b);
        eff = net + b;
    }

    // The inertia: the car's mass, a little more through a gear.
    float inertia;
    if (m_engine)
        inertia = (0.02f - ((ratio * ratio) * -0.0002f)) * carMass;
    else
        inertia = carMass * 0.01f;

    float torque = -eff;
    std::array<float, 2> limit{};
    const int probes = n == 2 ? 2 : 1; // the original probes the first wheel, and the second of a pair
    for (int i = 0; i < probes; ++i)
        limit[static_cast<std::size_t>(i)] = m_wheels[static_cast<std::size_t>(i)]->computeDwtdwMm1(eff, env);
    // The wheels' slopes (always 0) add to MM1's fixed damping.
    constexpr float kDamp = 300.0f;

    float wNew = w;
    float wv = w;
    for (int iter = 1;; ++iter) {
        wNew = (torque / (dt * kDamp + inertia)) * dt + wv;
        float m;
        int ci = 0;
        if (n == 2) {
            if (eff < 0.0f)
                m = limit[0] < limit[1] ? limit[0] : limit[1];
            else
                m = limit[0] <= limit[1] ? limit[1] : limit[0];
            ci = m == limit[0] ? 0 : 1;
        } else {
            m = limit[0];
        }
        bool crossed;
        if (eff < 0.0f)
            crossed = m < wNew;
        else if (eff <= 0.0f)
            crossed = false;
        else
            crossed = wNew < m;
        if (!crossed)
            break;
        // Past a breakpoint: stop there and go on with the torque that is left.
        torque = torque - (m - wv) * kDamp;
        wv = m;
        const float s = eff < 0.0f ? -1.0f : 1.0f;
        limit[static_cast<std::size_t>(ci)] = s * -99999997952.0f;
        if (iter > 20)
            break;
    }
    // The brake stops the wheels but never reverses them.
    if (holds && ((wNew < 0.0f && 0.0f < w) || (w < 0.0f && 0.0f < wNew)))
        wNew = 0.0f;
    // In gear the wheels cannot outrun the engine's MaxRPM.
    if (m_engine && m_trans) {
        const float s = wNew < 0.0f ? -1.0f : 1.0f;
        const float a = std::abs(wNew);
        const float lim = m_engine->maxRotationSpeed / std::abs(ratio);
        const float v = a <= 0.0f ? 0.0f : (a < lim ? a : lim);
        wNew = v * s;
    }
    for (int i = 0; i < n; ++i)
        m_wheels[static_cast<std::size_t>(i)]->rotationSpeed = wNew;
    rotationSpeed = wNew;
    if (m_engine) {
        const float e = (ratio * w0.rotationSpeed) * -1.0f;
        m_engine->rotationSpeed = e <= 0.0f ? 0.0f : e;
    }
    for (int i = 0; i < n; ++i)
        m_wheels[static_cast<std::size_t>(i)]->updateMm1(env);
}

} // namespace mm2::phys
