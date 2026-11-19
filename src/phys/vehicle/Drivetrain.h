#pragma once

#include "phys/vehicle/TuneParams.h"
#include "phys/vehicle/Wheel.h"

#include <array>

namespace mm2::phys {

class Engine;
class Transmission;

// vehDrivetrain (Midtown Madness 2), verified against the build 3393 code.
//
// A drivetrain is a set of wheels that spin together. The engine-driven one
// is attached to the engine and transmission while the clutch is engaged
// (vehEngine::Update attaches and detaches it); the others (Freetrain) only
// feel brakes and tyre forces. Each sample:
//
//   brake = 50 + sum(wheel brake torque) * (BrakeStaticCoef if stopped,
//           BrakeDynamicCoef otherwise)
//   net   = engine coupling + ratio * engine torque - sum(tyre torque)
//           (the tyre torques of the previous sample: an explicit coupling)
//   the brake opposes the rotation and cannot reverse it
//   w'    = w + dt * (-net) / (dt * AngInertia + I),
//           I = ratio^2 * Engine.AngInertia + 0.02 attached, Mass * 0.005 free
//
// AngInertia acts as a damping term (it is multiplied by dt). Paired wheels
// spin at w * diffRatio and w / diffRatio: a limited-slip differential that
// moves towards the ratio balancing their tyre torques, limited to
// diffRatioMax (1.25) at rest and diffRatioMaxHighSpeed (1.03) from
// diffRatioHighSpeedLevel (50 rad/s). The wheel speed is limited by the
// engine's MaxRPM in gear, and the engine speed follows the wheels.
class Drivetrain {
public:
    // vehDrivetrain::FileIO fields / vehDrivetrain::CopyVars.
    void configure(const DrivetrainParams& p);
    void reset();

    bool addWheel(Wheel* w);
    void attach(Engine* engine, Transmission* trans);
    void attach(); // re-attach to the last engine/transmission
    void detach(); // vehDrivetrain::Detach
    bool attached() const { return m_engine != nullptr; }

    // vehDrivetrain::Update, then the wheels' own updates (its children).
    void update(const WheelEnv& env, float carMass);

    int numWheels() const { return m_numWheels; }
    Wheel* wheel(int i) const { return m_wheels[static_cast<std::size_t>(i)]; }

    float angInertia = 5000.0f;
    float brakeDynamicCoef = 1.0f;
    float brakeStaticCoef = 1.2f;
    float rotationSpeed = 0.0f; // rad/s, wheel convention (negative forward)
    float diffRatio = 1.0f;

    static constexpr float kDiffRatioMax = 1.25f;          // vehDrivetrain::diffRatioMax
    static constexpr float kDiffRatioMaxHighSpeed = 1.03f; // diffRatioMaxHighSpeed
    static constexpr float kDiffRatioHighSpeedLevel = 50.0f;

private:
    std::array<Wheel*, 4> m_wheels{};
    int m_numWheels = 0;
    Engine* m_engine = nullptr;
    Transmission* m_trans = nullptr;
    Engine* m_lastEngine = nullptr;
    Transmission* m_lastTrans = nullptr;
};

} // namespace mm2::phys
