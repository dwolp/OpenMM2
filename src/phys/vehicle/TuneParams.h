#pragma once

#include "core/Math.h"
#include "data/DatFile.h"

#include <optional>
#include <string>
#include <vector>

namespace mm2::phys {

// Parameters of the Angel vehicle classes as read from the tune files
// (tune/vehicle/<car>.vehCarSim, .vehGyro, .vehStuck, .vehCarDamage,
// .vehTrailer, .dgTrailerJoint). Field names match the files exactly.
//
// Defaults apply to fields a file does not set; they are the original
// constructors' values (verified against the build 3393 code).

struct AeroParams {
    Vec3 angCDamp{0, 0, 0};    // AngCDamp
    Vec3 angVelDamp{0, 0, 0};  // AngVelDamp
    Vec3 angVel2Damp{0, 0, 0}; // AngVel2Damp
    float drag = 0.0f;         // Drag
    float down = 0.0f;         // Down
};

struct EngineParams {
    float angInertia = 1.0f;      // AngInertia (kg m^2)
    float maxHorsePower = 200.0f; // MaxHorsePower (hp)
    float idleRPM = 750.0f;       // IdleRPM: clutch opens below it, engine-braking zero
    float optRPM = 5000.0f;       // OptRPM: RPM of peak power, reference RPM for gear speeds
    float maxRPM = 8000.0f;       // MaxRPM: rev limit
    float gcl = 0.25f;            // GCL: gear change lag (s), no torque after a shift
};

struct TransmissionParams {
    int manualNumGears = 7; // ManualNumGears: reverse + neutral + forward gears
    int autoNumGears = 6;   // AutoNumGears
    float reverse = 20.0f;  // Reverse: reverse gear speed at OptRPM (mph)
    float low = 20.0f;      // Low: first gear speed at OptRPM (mph)
    float high = 75.0f;     // High: top gear speed at OptRPM (mph)
    float gearBias = 0.5f;  // GearBias: pushes the intermediate gears towards High
    float upshiftBias = 0.05f;
    float downshiftBiasMin = 0.05f;
    float downshiftBiasMax = 0.3f;
    float gearChangeTime = 0.8f; // GearChangeTime: minimum time in gear before an automatic shift (s)
    // Midtown Madness 1 fields (GearRatios, UpshiftRPM, ...) left in a few
    // retail *_opp files are not read by MM2's vehTransmission.
};

struct DrivetrainParams {
    float angInertia = 5000.0f;    // AngInertia: damping of the wheel speed step (see Drivetrain)
    float brakeDynamicCoef = 1.0f; // BrakeDynamicCoef
    float brakeStaticCoef = 1.2f;  // BrakeStaticCoef
};

struct WheelParams {
    float suspensionExtent = 0.2f; // droop travel (m)
    float suspensionLimit = 0.1f;  // compression travel (m)
    float suspensionFactor = 1.0f; // spring progression (>= 0.75)
    float suspensionDampCoef = 0.1f;
    float steeringLimit = 0.39f; // rad at full lock
    float steeringOffset = 0.0f; // Ackermann-style inner/outer difference
    float brakeCoef = 1.0f;
    float handbrakeCoef = 1.0f;
    float camberLimit = -1.0f;
    float wobbleLimit = 0.0f;
    float tireDispLimitLong = 0.075f;
    float tireDampCoefLong = 0.25f;
    float tireDragCoefLong = 0.02f;
    float tireDispLimitLat = 0.075f;
    float tireDampCoefLat = 0.25f;
    float tireDragCoefLat = 0.05f;
    float optimumSlipPercent = 0.14f;
    float staticFric = 2.0f;
    float slidingFric = 1.9f;
};

struct AxleParams {
    float torqueCoef = 0.0f; // TorqueCoef
    float dampCoef = 0.0f;   // DampCoef
};

struct CarSimParams {
    float mass = 2000.0f;
    Vec3 inertiaBox{2, 1, 3};
    // vehCarSim adds R * CenterOfGravity to the body position to get the
    // model's origin, so the centre of mass sits at -CenterOfGravity in
    // model space.
    Vec3 centerOfGravity{0, 0, 0};
    float boundFriction = 0.3f;
    float boundElasticity = 0.2f;
    int drivetrainType = 0; // 0 rear, 1 front, 2 all-wheel drive
    float sssValue = 1.0f;
    float sssThreshold = 0.0f;
    float carFrictionHandling = 1.0f;
    AeroParams aero;
    EngineParams engine;
    TransmissionParams trans;
    DrivetrainParams drivetrain;
    DrivetrainParams freetrain;
    WheelParams wheelFront;
    WheelParams wheelBack;
    AxleParams axleFront;
    AxleParams axleBack;
};

struct GyroParams {
    float drift = 0.0f;
    float spin180 = 0.0f;
    float reverse180 = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

struct StuckParams {
    float turn = 1.57f;
    float rotation = 0.39f;
    float translation = 0.1f;
    float timeThresh = 0.3f;
    float posThresh = 1.25f;
    float moveThresh = 1.75f;
};

struct CarDamageParams {
    float maxDamage = 1000000.0f;
    float medDamage = 500000.0f;
    float impactThreshold = 100.0f;
    float regenerateRate = 0.0f;
    Vec3 smokeOffset{0.0f, 0.8f, -1.8f};
    Vec3 smokeOffset2;
    float textelDamageRadius = 0.4f;
    bool doublePivot = false;
    bool mirrorPivot = false;
};

// vehTrailer (vehTrailer::FileIO; defaults from its constructor).
struct TrailerParams {
    float mass = 3000.0f;
    Vec3 inertiaBox{3.0f, 4.0f, 9.0f};
    // Hitch points. vehTrailer::Init takes them from the models'
    // trailer_hitch pivots (TrailerGeometry); these fields replace them when
    // the file has them (vpsemi). CarHitchOffset is used in the tractor's
    // InertialCS space (relative to its centre of mass, not to its model
    // origin), TrailerHitchOffset in the trailer's, whose centre of mass is
    // its model origin.
    std::optional<Vec3> carHitchOffset;
    std::optional<Vec3> trailerHitchOffset;
    WheelParams wheelFront; // TWHL0 (TWHL1 copies it)
    WheelParams wheelBack;  // TWHL2 (TWHL3 copies it)
    // The four free drivetrains (the first one's block; the others copy it).
    DrivetrainParams drivetrain;
};

// dgTrailerJoint (dgTrailerJoint::FileIO; defaults from dgTrailerJoint::Init).
// See phys/TrailerJoint.h and docs/physics.md. MM2 reads nothing else:
// vpcentury's Offset0/Offset1 (an older layout) are ignored, and the roll
// limits are not loadable (Init's -0.3/+0.3).
struct TrailerJointParams {
    float forceLimit = 0.0f; // breaks above ForceLimit * 10000 N; 0 = never
    int jointStatus = 2;     // the joint's flags: 1 broken, 2 torques and limits on
    float restoreForceLean = 2.0f;
    float dampConstLean = 2.0f;
    float dampLinearLean = 0.9f; // read, but without effect in MM2
    float restoreForceRoll = 2.0f;
    float dampConstRoll = 0.1f;
    float dampLinearRoll = 2.0f;
    float leanLimit = 3.1415927f;
    float limitElasticityLean = 1.0f;
    float limitElasticityRoll = 0.0f;
    float freeRange = 0.15f; // hitch gap (m) allowed before the bodies are moved together
    float freeLean = 0.1f;   // lean (rad) inside which no restoring torque acts
    float freeRoll = 0.1f;   // |roll| (rad) above which the roll torque would act
};

// The records vehCarSim::FileIO registers, with the nested vehAero, vehEngine,
// vehTransmission, vehDrivetrain, vehWheel and vehAxle FileIO records. Parse
// .vehCarSim files with data::parseDat(text, carSimSchema()) to read exactly
// what MM2's datParser::Read reads (unknown names skip the rest of their line
// or the block after them).
const data::DatSchema& carSimSchema();

// Loaders take the top-level block ("vehCarSim { ... }"). Fields the Angel
// classes do not know (MM1-era leftovers such as RedistHeight) are skipped,
// as the original parser does; their names are appended to `ignored`.
bool loadCarSimParams(const data::DatNode& block, CarSimParams& out,
                      std::vector<std::string>* ignored = nullptr);
bool loadGyroParams(const data::DatNode& block, GyroParams& out);
bool loadStuckParams(const data::DatNode& block, StuckParams& out);
bool loadCarDamageParams(const data::DatNode& block, CarDamageParams& out);
bool loadTrailerParams(const data::DatNode& block, TrailerParams& out);
bool loadTrailerJointParams(const data::DatNode& block, TrailerJointParams& out);
void loadWheelParams(const data::DatNode& block, WheelParams& out);

} // namespace mm2::phys
