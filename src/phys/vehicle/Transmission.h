#pragma once

#include "phys/vehicle/TuneParams.h"

#include <array>

namespace mm2::phys {

class Engine;

// vehTransmission (Midtown Madness 2), verified against the build 3393 code.
//
// Gear slots: 0 reverse, 1 neutral, 2.. forward. The automatic and manual
// boxes have their own ratio tables (AutoNumGears / ManualNumGears slots).
// Ratios come from gear speeds: GearRatioFromMPH(v) = OptRPM / (wheel RPM at
// v mph) on the primary drivetrain's first wheel; Low is first gear, High top
// gear, the gears between are spaced geometrically, pushed towards High by
// GearBias. The automatic box shifts up where the next gear would give at
// least as much power at full throttle (plus UpshiftBias), and down below a
// point blended between DownshiftBiasMin (full throttle) and
// DownshiftBiasMax (closed throttle), at most once per GearChangeTime and
// only while a wheel touches the ground.
class Transmission {
public:
    static constexpr int kSlots = 8;
    static constexpr int kReverse = 0;
    static constexpr int kNeutral = 1;
    static constexpr int kFirst = 2;

    // vehTransmission::vehTransmission's tables and vehTransmission::FileIO
    // (vehTransmission::Init only stores the vehCarSim).
    void configure(const TransmissionParams& p);
    // vehTransmission::ComputeConstants (needs the engine curve and the
    // driven wheel's radius).
    void computeConstants(const Engine& engine, float wheelRadius);
    void reset();

    float currentRatio() const { return ratio(currentGear); }
    float ratio(int gear) const { return isAutomatic ? gearRatios[gear] : manualGearRatios[gear]; }

    // vehTransmission::Upshift / vehTransmission::Downshift.
    int upshift();
    int downshift();
    void setReverse() { setCurrentGear(kReverse); } // vehTransmission::SetReverse
    void setDrive();                                // vehTransmission::SetForward
    void setNeutral() { setCurrentGear(kNeutral); } // vehTransmission::SetNeutral
    // -1 reverse, 0 neutral, 1.. forward.
    int getCurrentGear() const { return currentGear - 1; }
    int setCurrentGear(int gear);
    void automatic(bool on) { isAutomatic = on; }

    // vehTransmission::Update; `wheelsOnGround` is vehCarSim::OnGround().
    void update(float dt, const Engine& engine, int wheelsOnGround);

    TransmissionParams params;
    bool isAutomatic = true;
    bool gearChanged = false; // shift pending (vehEngine starts its lag on it)
    int currentGear = kFirst;
    float timeInGear = 0.0f;
    int numGears = 6;       // AutoNumGears
    int manualNumGears = 7; // ManualNumGears
    std::array<float, kSlots> gearRatios{};       // automatic
    std::array<float, kSlots> manualGearRatios{}; // manual
    std::array<float, kSlots> upshiftRPM{};
    std::array<float, kSlots> kickdownRPM{};  // downshift below this at full throttle
    std::array<float, kSlots> downshiftRPM{}; // ... and below this with the throttle closed
};

} // namespace mm2::phys
