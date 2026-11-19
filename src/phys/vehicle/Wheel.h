#pragma once

#include "phys/Level.h"
#include "phys/PolygonSoup.h"
#include "phys/vehicle/TuneParams.h"
#include "phys/vehicle/VehicleGeometry.h"

#include <cstdint>

namespace mm2::phys {

class InertialCS;
class GroundQuery;
struct Material;

// Per-sample inputs a wheel needs from its car.
struct WheelEnv {
    InertialCS* ics = nullptr;
    // The car's model matrix (vehCarSim world matrix), or the body matrix for
    // wheels without a car (vehTrailer passes no vehCarSim).
    const Mat34* frame = nullptr;
    const GroundQuery* ground = nullptr;
    float dt = 0;
    float invDt = 0;
    // WeatherFriction (mmGame::InitWeather: 0.8 in rain, 0.75 in rain at night).
    float weatherFriction = 1.0f;
    // vehCarSim CarFrictionHandling; wheels without a car do not apply it.
    bool hasCar = true;
    float carFrictionHandling = 1.0f;
    // State of the game's random generator (World::randomSeed); null uses a
    // private stream.
    std::uint32_t* randomSeed = nullptr;
    // The instance the probes never hit: the car's own (vehWheel passes its
    // vehCarSim's instance; a trailer's wheels have no vehCarSim and pass
    // none).
    const Instance* self = nullptr;
};

// vehWheel (Midtown Madness 2), verified against the build 3393 code:
// ComputeConstants, SetNormalLoad, ComputeFriction, SetInputs,
// CalcSuspensionForce, ComputeDwtdw, GetBumpDisplacement, Update.
//
// Suspension: a probe from SuspensionLimit + 0.3 above the wheel centre to
// SuspensionExtent + Radius below it finds the ground. Displacement drives a
// progressive spring and damper on top of the static load; the contact
// registers its stiffness with the body (phInertialCS::ApplyContactForce),
// which integrates it implicitly. Past SuspensionLimit the wheel bottoms out:
// an impulse stops the closing velocity and a push removes the overlap.
//
// Tyre: a displacement ("rubber") model per direction. Each sample the
// contact patch displacement moves with the slip velocity, limited to the
// displacement at which the tyre force reaches friction(slip) * load, and
// relaxes towards that limit while sliding. The force is -stiffness * disp -
// damping * rate, clipped to the friction circle. friction(slip) is
// StaticFric (2s/s0 - s^2/s0^2) up to the optimum slip s0, falling to
// SlidingFric past it. Stiffness = 2 * static load / TireDispLimit, damping =
// 2 sqrt(stiffness * load / g) * TireDampCoef.
class Wheel {
public:
    enum Flags : int {
        // vehWheel flag 4: the wheel does not spin; the contact slides with
        // the body and the brakes hold it at full friction.
        kFixed = 4,
    };

    // vehWheel::Init + ComputeConstants. `center` (the wheel pivot), radius
    // and width come from the geometry, in the car's model space. With a car
    // the static load is mass * g/4 scaled by |z - cg.z| / |z| (vehCarSim's
    // CenterOfGravity z); without one it is mass * g / 4.
    void init(const WheelParams& p, const WheelGeometry& g, float mass, bool hasCar, float cgZ, int flags = 0);
    // vehWheel::CopyVars (the right wheel of each axle copies the left one):
    // every tune field but HandbrakeCoef and WobbleLimit, which keep this
    // wheel's own.
    void copyVars(const Wheel& other);
    void computeConstants();
    // vehWheel::SetNormalLoad: derives the spring, damper and tyre constants.
    void setNormalLoad(float load);
    // vehWheel::AddNormalLoad: SetNormalLoad(static load + load), at least 1 N
    // (vehTrailer::Init adds the trailer's share to the tractor's wheels).
    void addNormalLoad(float load);
    void reset();

    // vehWheel::SetInputs: steering (-1..1, before SteeringLimit), foot brake
    // and handbrake (0..1).
    void setInputs(float steer, float brake, float handbrake);

    // vehWheel::ComputeFriction: friction coefficient at a slip ratio and the
    // sliding fraction (0 below the optimum slip, 1 fully sliding).
    float computeFriction(float slip, float& slide) const;

    // vehWheel::ComputeDwtdw: the ground probe, suspension and contact
    // velocities. `net` is the drivetrain's net torque (resistance positive);
    // returns the wheel speed B at which the slip reaches the optimum, which
    // the drivetrain uses as a breakpoint. (The original also reports two
    // slopes, always 0.)
    float computeDwtdw(float net, const WheelEnv& env);

    // vehWheel::Update: contact and tyre forces applied to the body, and the
    // wheel's visual state. Uses rotationSpeed set by the drivetrain.
    void update(const WheelEnv& env);

    // Visual displacements (vehWheel::GetVisualDispVert/Lat/Long).
    float visualDispVert() const;
    float visualDispLat() const;
    float visualDispLong() const;

    WheelParams params;

    // --- Geometry (model space) ---
    Vec3 center; // pivot
    float radius = 0.3f;
    float width = 0.1f;
    int flags = 0;
    bool hasCar = true;
    float cgZ = 0.0f; // vehCarSim CenterOfGravity z (static load split)
    float mass = 1000.0f;

    // --- Constants (SetNormalLoad) ---
    float normalLoad = 5000.0f;  // static load
    float spring = 0.0f;         // N/m
    float progressive = 0.0f;    // spring stiffening per metre of travel
    float damping = 0.0f;        // N s/m
    float stiffLong = 0.0f;      // tyre N/m
    float dampLong = 0.0f;       // tyre N s/m
    float stiffLat = 0.0f;
    float dampLat = 0.0f;
    float maxBrakeTorque = 0.0f; // StaticFric * Radius * load * BrakeCoef
    float maxHandbrakeTorque = 0.0f;
    float invOptSlip2 = 0.0f;

    // --- Inputs ---
    float steerAngle = 0.0f;  // rad
    float brakeTorque = 0.0f; // N m

    // --- State ---
    Mat34 matrix; // the wheel in world space (steered, displaced, spun)
    bool hit = false;
    RayHit intersection;
    // The probe's lvlSegmentInfo (rooms and cached polygon, vehWheel::Init).
    ProbeCache probeCache;
    Mat34 contactFrame; // rows right, ground normal, back; m3 the contact point
    Vec3 position;      // contact point
    const Material* material = nullptr;
    bool onGround = false;
    bool bottomedOut = false;
    bool skidding = false; // slide > 0.5
    float latVelocity = 0.0f;
    float fwdVelocity = 0.0f; // contactForwardVel
    float normalVelocity = 0.0f;
    float slipVelocity = 0.0f;
    float bump = 0.0f;
    float drag = 0.0f;     // surface drag (material)
    float friction = 1.0f; // surface friction after weather and CarFrictionHandling
    float depth = 0.0f;    // current sink into soft surfaces
    float bumpHeight = 0.0f;
    float bumpWidth = 0.0f;
    float bumpPhase = 0.0f;
    float rotation = 0.0f;           // accumulated spin (visual)
    float suspension = 0.0f;         // displacement, compression positive (m)
    float suspensionForce = 0.0f;    // N
    float currentLoad = 0.0f;        // normal force used for friction (N)
    float suspensionVelocity = 0.0f; // m/s
    float contactStiffness = 0.0f;   // d(force)/d(velocity) handed to the body
    float slide = 0.0f;
    float camber = 0.0f;
    float wobble = 0.0f;
    float currentTireDispLat = 0.0f;
    float currentTireDispLong = 0.0f;
    float tireGripLat = 0.0f;    // lateral force
    float tireGripLong = 0.0f;   // longitudinal force
    float tireResistance = 0.0f; // longitudinal force * radius
    float rotationSpeed = 0.0f;  // rad/s, negative when rolling forward
    float latSlipPercent = 0.0f;
    float longSlipPercent = 0.0f;

private:
    void calcSuspensionForce(float disp, bool contact, float cosNormal, const WheelEnv& env);
    float bumpDisplacement(float speed, float dt, std::uint32_t* seed);
};

// The game's frand (irand: MSVC rand(), times 2^-15) on the given state.
float physFrand(std::uint32_t& seed);

} // namespace mm2::phys
