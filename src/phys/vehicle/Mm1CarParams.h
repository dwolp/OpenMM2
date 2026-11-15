/*
    OpenMM2 Enhanced - Midtown Madness 1's car tuning: tune/<car>.mmcarsim
    (mmCarSim::FileIO and its nested mmEngine, mmTransmission, asAero and
    mmWheel records, read from Open1560's game.asm declarations and the
    retail files). See docs/mm1physics.md.
*/
#pragma once

#include "core/Math.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::phys {

// mmWheel's FileIO fields. MM1's files hold only FrontLeft and BackLeft; the
// right wheels copy them (inferred from the files).
struct Mm1WheelParams {
    float spring = 0.0f;                    // Spring (N/m)
    float damping = 0.0f;                  // Damping (N s/m)
    float steeringRatio = 0.0f;            // SteeringRatio
    float suspensionBlend = 0.0f;          // SuspensionBlend
    float brakeRatio = 0.0f;               // BrakeRatio
    float staticFriction = 0.0f;           // StaticFriction
    float suspensionLimit = 0.0f;          // SuspensionLimit (m)
    float renderableSuspensionLimit = 0.0f; // RenderableSuspensionLimit (m)
    float rubberSpring = 0.0f;             // RubberSpring
    float rubberDamp = 0.0f;               // RubberDamp
    float optimumSlipPercent = 0.0f;       // OptimumSlipPercent
    float staticFric = 0.0f;               // StaticFric
    float slidingFric = 0.0f;              // SlidingFric
    // RubberSpringLat / RubberDampLat: absent from some files (kept as read
    // from the others' values when missing: the long ones; inferred).
    float rubberSpringLat = 0.0f;
    float rubberDampLat = 0.0f;
    bool hasLat = false;
};

struct Mm1EngineParams {
    float maxHorsePower = 0.0f; // MaxHorsePower
    float optRPM = 0.0f;        // OptRPM
    float maxRPM = 0.0f;        // MaxRPM
    float gcl = 0.0f;           // GCL
};

struct Mm1TransmissionParams {
    int numGears = 0;               // NumGears
    float upshiftThreshold = 0.0f;  // UpshiftThreshold
    float downshiftThreshold = 0.0f; // DownshiftThreshold
    std::vector<float> gearRatios;   // GearRatios: reverse, neutral, 1st ...
    std::vector<float> upshiftRPM;   // UpshiftRPM (per gear)
    std::vector<float> downshiftRPM; // DownshiftRPM (per gear)
    int manualNumGears = 0;          // ManualNumGears
    std::vector<float> manualGearRatios; // ManualGearRatios
    float downshiftBias = 0.0f;      // DownshiftBias
};

struct Mm1AeroParams {
    Vec3 cDamp, velDamp, vel2Damp;       // CDamp, VelDamp, Vel2Damp
    Vec3 angCDamp, angVelDamp, angVel2Damp; // AngCDamp, AngVelDamp, AngVel2Damp
};

// mmCarSim::FileIO. A field a file does not set stays 0 (the constructor's
// defaults are not read from the asm yet).
struct Mm1CarParams {
    std::string className; // "mmCarSim"
    float mass = 0.0f;
    Vec3 inertiaBox;
    Vec3 bodyCG;
    float drag = 0.0f;
    float downforce = 0.0f;
    float boundFriction = 0.0f;
    float boundElasticity = 0.0f;
    float driftTorque = 0.0f;
    int drivetrainType = 0;
    float carFrictionHandling = 0.0f;
    float maxDamage = 0.0f;
    float medDamage = 0.0f;
    float redistHeight = 0.0f;
    float redistLongRatio = 0.0f;
    float slipPercentThresh = 0.0f;
    // The drift and spin assist (mmCarSim::Update's friction multipliers and
    // its drift/spin state machine).
    float longSlideMultiplier = 0.0f;
    float steerMultiplier = 0.0f;
    float driftThreshold = 0.0f;
    float spinThreshold = 0.0f;
    float spinStart = 0.0f, spinStop = 0.0f;
    float spinFromMin = 0.0f, spinFromMax = 0.0f;
    float spinToMin = 0.0f, spinToMax = 0.0f;
    float spinStartTime = 0.0f, spinEndTime = 0.0f;
    float frontDriftFricMultiplier = 0.0f, backDriftFricMultiplier = 0.0f;
    float frontSpinFricMultiplier = 0.0f, backSpinFricMultiplier = 0.0f;
    float brakeFrontFricMultiplier = 0.0f, brakeBackFricMultiplier = 0.0f;
    float weebleTorque = 0.0f;
    Vec3 damageSmokeOffset;
    std::optional<Vec3> dashCamHeadlightOffset, povCamHeadlightOffset;
    Mm1EngineParams engine;
    Mm1TransmissionParams trans;
    Mm1AeroParams aero;
    Mm1WheelParams frontLeft, backLeft; // the right wheels copy these
};

// Parses the text of a tune/<car>.mmcarsim. Object headers carry a class name
// and an address ("Engine mmEngine :075acba0 {"); lists are "Name [ ... ]".
// Unknown fields (the particle rules, the *Ptx values) are skipped. Returns
// nullopt with `error` set when the text has no mmCarSim block.
std::optional<Mm1CarParams> parseMm1CarParams(std::string_view text, std::string* error = nullptr);

} // namespace mm2::phys
