// OpenMM2 Enhanced: Midtown Madness 1's mmEngine and mmTransmission (Open1560's
// game.asm: mmEngine::ComputeConstants, CalcTorqueAtFullThrottle,
// CalcTorqueAtZeroThrottle, CalcTorque, Update; mmTransmission::Update,
// SetCurrentGear). See docs/mm1physics.md.

#include "phys/vehicle/Drivetrain.h"
#include "phys/vehicle/Engine.h"
#include "phys/vehicle/Transmission.h"

#include <algorithm>
#include <cmath>

namespace mm2::phys {

void Engine::configureMm1(const Mm1EngineParams& p) {
    *this = Engine{};
    mm1 = true;
    maxHorsePower = p.maxHorsePower;
    optRPM = p.optRPM;
    maxRPM = p.maxRPM;
    gcl = p.gcl;
    hpScale = 1.0f;
    // mmEngine::ComputeConstants. (6.28319 and 0.0166667 as the asm has them.)
    maxRotationSpeed = (maxRPM * 6.28319f) * 0.0166667f;
    optRotationSpeed = (optRPM * 6.28319f) * 0.0166667f;
    const float span = maxRotationSpeed - optRotationSpeed;
    taperCoef = 1.0f / (span * span);
    torqueCoef = ((hpScale * maxHorsePower) * 746.0f) / ((optRotationSpeed * optRotationSpeed) * optRotationSpeed);
    pullUpMod = (std::sqrt(5.0f) + 1.0f) * 0.5f;
    breakDownMod = (std::sqrt(5.0f) - 1.0f) * 0.5f;
    changingGear = false;
}

float Engine::calcTorqueAtFullThrottleMm1() const {
    const float w = rotationSpeed;
    const float o = optRotationSpeed;
    if (w <= o)
        return (((pullUpMod * o) - w) * ((breakDownMod * o) + w)) * torqueCoef;
    if (w <= maxRotationSpeed)
        return (((((((maxRotationSpeed + w) - (o + o)) * ((pullUpMod * o) - w)) * ((breakDownMod * o) + w)) *
                  (maxRotationSpeed - w)) *
                 taperCoef) *
                torqueCoef);
    return 0.0f;
}

float Engine::calcTorqueAtZeroThrottleMm1() const {
    const float o = optRotationSpeed;
    return (((((hpScale * maxHorsePower) * 746.0f) / o) * (160.0f - rotationSpeed)) * 0.5f) / (o - 160.0f);
}

float Engine::calcTorqueMm1(float t) const {
    const float zero = calcTorqueAtZeroThrottleMm1();
    const float scaled = zero * (1.0f - t);
    return calcTorqueAtFullThrottleMm1() * t + scaled;
}

void Engine::updateMm1(float dt, Transmission& trans, Drivetrain& primary, float carMass, float carDamage,
                       bool damageScalesTorque) {
    if (trans.gearChanged && !changingGear) {
        prevGearRPM = rpm;
        gearChangeTime = gcl;
        changingGear = true;
    }
    torque = calcTorqueMm1(throttle);
    if (damageScalesTorque) {
        float f = (1.0f - carDamage) - -0.3f;
        f = f <= 0.0f ? 0.0f : (f < 1.0f ? f : 1.0f);
        torque = f * torque;
    }
    const float ratio = trans.currentRatio();
    if (ratio == 0.0f) {
        // Neutral: the engine revs freely against the car's mass.
        primary.detach();
        const float rot = (torque / (carMass * 0.001f)) * dt + rotationSpeed;
        rotationSpeed = rot <= 0.0f ? 0.0f : (rot < maxRotationSpeed ? rot : maxRotationSpeed);
    } else {
        primary.attach();
    }
    rpm = rotationSpeed * 9.549296f;
    if (gearChangeTime > 0.0f) {
        torque = 0.0f;
        rpm = (((gcl - gearChangeTime) * rpm) + (prevGearRPM * gearChangeTime)) / gcl;
        gearChangeTime = gearChangeTime - dt;
    } else {
        changingGear = false;
        trans.gearChanged = false;
    }
    horsepower = (rotationSpeed * torque) * 0.0013404826f;
}

void Transmission::configureMm1(const Mm1TransmissionParams& p) {
    *this = Transmission{};
    mm1 = true;
    isAutomatic = true;
    numGears = p.numGears;
    manualNumGears = p.manualNumGears != 0 ? p.manualNumGears : p.numGears;
    for (int i = 0; i < kSlots; ++i) {
        const auto k = static_cast<std::size_t>(i);
        gearRatios[k] = k < p.gearRatios.size() ? p.gearRatios[k] : 0.0f;
        manualGearRatios[k] = k < p.manualGearRatios.size() ? p.manualGearRatios[k] : gearRatios[k];
        upshiftRPM[k] = k < p.upshiftRPM.size() ? p.upshiftRPM[k] : 0.0f;
        downshiftRPM[k] = k < p.downshiftRPM.size() ? p.downshiftRPM[k] : 0.0f;
    }
    downshiftBias = p.downshiftBias != 0.0f ? p.downshiftBias : 1.55f;
    currentGear = kFirst; // mmTransmission::Reset: gear 2
    timeInGear = 0.0f;
}

int Transmission::setCurrentGearMm1(int gear) {
    if (isAutomatic && gear >= numGears) {
        currentGear = numGears - 1;
        return currentGear;
    }
    timeInGear = 0.0f;
    gearChanged = true;
    currentGear = gear;
    return currentGear;
}

void Transmission::updateMm1(float dt, const Engine& engine, bool drivingDisabled) {
    if (drivingDisabled)
        return;
    const int cur = currentGear;
    if (isAutomatic && cur > 1 && timeInGear > gearChangeDelay) {
        const float rpm = engine.rpm;
        const auto k = static_cast<std::size_t>(cur);
        bool down = false;
        if (!(rpm <= upshiftRPM[k]) && cur != numGears - 1 && !gearChanged) {
            setCurrentGearMm1(cur + 1);
        } else if (rpm < downshiftRPM[k] && cur != 2 && !gearChanged) {
            down = true;
        } else if (!(engine.throttle <= 0.8f) && downshiftBias * downshiftRPM[k] > rpm && cur != 2 && !gearChanged) {
            // Kickdown: hard on the throttle below the biased downshift RPM.
            down = true;
        }
        if (down)
            setCurrentGearMm1(cur - 1);
    }
    timeInGear = dt + timeInGear;
}

} // namespace mm2::phys
