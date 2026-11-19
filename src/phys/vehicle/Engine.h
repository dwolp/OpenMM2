#pragma once

#include "phys/vehicle/TuneParams.h"

#include <optional>

namespace mm2::phys {

class InertialCS;
class Transmission;
class Drivetrain;

// vehEngine (Midtown Madness 2), verified against the build 3393 code.
//
// Full-throttle torque peaks in power at OptRPM, T(w) = (phi w_opt - w)
// (w_opt/phi + w) * k (phi = 1.618034, k = MaxHorsePower * 746 / w_opt^3),
// tapering to 0 at MaxRPM above OptRPM. With the throttle closed the engine
// brakes: T0 = (w_idle - w) * Pmax/w_opt * 0.75 / (w_opt - w_idle); the
// throttle blends the two. The clutch (the primary drivetrain's attachment)
// opens in neutral or below IdleRPM and closes again above twice IdleRPM;
// while open the engine revs freely against its AngInertia.
class Engine {
public:
    void configure(const EngineParams& p);
    void reset();

    void computeConstants();
    float calcTorqueAtFullThrottle(float w) const;
    float calcTorqueAtZeroThrottle() const;
    float calcTorque(float throttle) const;
    // vehEngine::CalcHPAtFullThrottle (vehTransmission::ComputeConstants'
    // shift points).
    float calcHPAtFullThrottle(float w) const { return calcTorqueAtFullThrottle(w) * w; }

    // vehEngine::Update. `ics` receives the reaction torque that rocks the car
    // when revved in neutral; `drivetrainType` and the optional engine pivot
    // (the "engine" .mtx, rows m0..m2) give its axis.
    void update(float dt, Transmission& trans, Drivetrain& primary, InertialCS& ics, int drivetrainType);

    float maxHorsePower = 200.0f;
    float idleRPM = 750.0f;
    float optRPM = 5000.0f;
    float maxRPM = 8000.0f;
    float gcl = 0.25f; // gear change lag: no torque for this long after a shift
    float hpScale = 1.0f;
    float maxThrottle = 1.0f; // vehEngine +0x30; vehStuck::Pegged compares against 0.75 of it
    float angInertia = 1.0f;
    std::optional<Mat34> pivot; // GetPivot(<car>, "engine")

    float maxRotationSpeed = 0;  // MaxRPM in rad/s
    float optRotationSpeed = 0;  // OptRPM in rad/s
    float idleRotationSpeed = 0; // IdleRPM in rad/s
    float taperCoef = 0;         // 1 / (w_max - w_opt)^2
    float torqueCoef = 0;        // Pmax / w_opt^3
    static constexpr float kPhi = 1.618034f;
    static constexpr float kInvPhi = 0.618034f;

    float prevGearRPM = 0;
    float gearChangeTime = 0;
    bool changingGear = true;
    float throttle = 0;
    float rotationSpeed = 0; // rad/s
    float rpm = 0;
    float horsepower = 0;
    float torque = 0;
};

} // namespace mm2::phys
