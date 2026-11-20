#pragma once

#include "core/Math.h"
#include "vfs/Vfs.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace mm2::ai {

// tune/vehicle/<model>.aivehicledata: physical description of an ambient
// vehicle type (aiVehicleData). Size is the full extent of the collision box
// (x width, y height, z length); CG is relative to the model origin.
struct VehicleData {
    std::string model; // e.g. "va_taxi_f"
    float mass = 1000.0f;
    Vec3 size{2.0f, 1.5f, 4.5f};
    Vec3 maxAng;
    float elasticity = 0.5f;
    float friction = 0.5f;
    float maxDamage = 0.0f;
    float ptxThresh = 0.0f;
    float spring = 0.0f;
    float damping = 0.0f;
    float limit = 0.0f;
    float rubberSpring = 0.0f;
    float rubberDamp = 0.0f;
    Vec3 cg;
    // aiVehicleManager::AddVehicleDataEntry: wheel pivots from
    // geometry/<model>_whlN.mtx and the wheel radius from WHL0's box.
    std::array<Vec3, 6> wheels{};
    int wheelCount = 0; // pivots found, in order WHL0..
    float wheelRadius = 0.0f;

    float length() const { return size.z; }
    float width() const { return size.x; }
};

// Loads tune/vehicle/<model>.aivehicledata. Missing files return defaults
// sized like a family car (logged).
std::optional<VehicleData> loadVehicleData(const vfs::Vfs& vfs, std::string_view model,
                                           std::string* error = nullptr);

// OpenMM2 Enhanced: Midtown Madness 1's traffic cars (mm1/tune/<model>.
// aivehicledata, an aiVehicleData block). The fields MM2 shares (Size,
// Mass, Friction, ...; SuspensionLimit for Limit), the wheels from FLOff,
// FROff, BLOff and BROff (WHL0-3 in that order) and TireRadius. MM1 gives
// no CG: inferred halfway up the car (MM2's run 0.5-0.65 of the height).
std::optional<VehicleData> parseMm1VehicleData(std::string_view model, std::string_view text);

// MM1's traffic: aiMap::Init hands out its ambient cars from this list in
// turn (no cheat), so each is as common as the others.
inline constexpr std::array<std::string_view, 13> kMm1AmbientModels{
    "vasedans", "vasedanl", "vavan",   "vadiesels",   "vacompact", "vapickup",   "vabus",
    "vadelivery", "valimo", "valimoblack", "vataxi", "vataxicheck", "valimoangel"};

} // namespace mm2::ai
