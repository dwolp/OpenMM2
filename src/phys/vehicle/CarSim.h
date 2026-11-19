#pragma once

#include "phys/World.h"
#include "phys/vehicle/Aero.h"
#include "phys/vehicle/Drivetrain.h"
#include "phys/vehicle/Engine.h"
#include "phys/vehicle/Gyro.h"
#include "phys/vehicle/Splash.h"
#include "phys/vehicle/Stuck.h"
#include "phys/vehicle/Transmission.h"
#include "phys/vehicle/TuneParams.h"
#include "phys/vehicle/VehicleBody.h"
#include "phys/vehicle/VehicleGeometry.h"
#include "phys/vehicle/Wheel.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace mm2::phys {

// One impact as vehCarDamage::ApplyImpact hands it on (vehDamageImpactInfo
// plus what ApplyImpact decided): to AudImpact (sound), and to the sparks,
// shards, texel damage, breakables and the game's impact callback
// (damaging).
struct CarImpact {
    const Collider* other = nullptr; // the collider hit
    Body* otherBody = nullptr;       // its body, when it is one
    Vec3 localPosition;              // the impact point in the car's model space
    Vec3 position;                   // the impact point (world)
    Vec3 normal;                     // the impact's normal (from its B towards its A)
    Vec3 impulse;                    // the impulse the car took
    float value = 0.0f;              // |impulse| * damage modifier * the other body's mass share
    float total = 0.0f;              // the values summed while the impact lasts
    // AudImpact::Play(|x| + |y| + |z| of the impulse, the other collider's
    // id), when value > 0.001.
    bool sound = false;
    float soundStrength = 0.0f;
    int audioId = 0;
    // value > ImpactThreshold at 10 mph or more, or against a body: sparks
    // (above 15 mph), shards, damage, texel damage, breakables and the game
    // callback.
    bool damaging = false;
    bool otherIsBody = false; // the other collider has an InertialCS
};

// vehCarDamage: the impact list and the damage bookkeeping.
//
// Each collision impulse the car takes (vehCarDamage::Impact, the collider's
// impact callback) is worth |impulse| times the other body's share of the
// two masses (1 against the world). The first impact from a collider, and
// any later one worth more than 1.25 times the last, is applied
// (ApplyImpact: a sound above 0.001; above ImpactThreshold, at 10 mph or
// more or against a body, damage and effects); the collider then stays in
// the list (12 entries) until RelaxTime (0.2 s) passes without such a
// re-trigger, its weaker impacts adding damage silently. CurrentDamage falls
// by RegenerateRate per second; damage is the 0..1 fraction between
// MedDamage and MaxDamage and the car is wrecked at MaxDamage. (MM2 has no
// global damage scale; Midtown Madness 1's GlobalDamageScale is gone.)
struct CarDamage {
    // ?RelaxTime@vehCarDamage@@2MA and the impact list's size.
    static constexpr float kRelaxTime = 0.2f;
    static constexpr int kMaxImpacts = 12;

    struct ImpactInfo {
        const Collider* other = nullptr; // null: free
        Vec3 localPosition, position, normal, impulse;
        float value = 0.0f;
        float total = 0.0f;
        float timer = 0.0f;
    };

    CarDamageParams params;
    float currentDamage = 0.0f;
    float damage = 0.0f; // 0..1
    // vehCarDamage's enable flag: impacts are recorded (the game turns it on for a
    // race unless damage is off).
    bool enabled = true;
    std::array<ImpactInfo, kMaxImpacts> impacts{};

    float maxDamage() const { return params.maxDamage; }
    float medDamage() const { return params.medDamage; }
    // vehCarDamage::ClearDamage (and Reset).
    void reset();
    // vehCarDamage::AddDamage.
    void addDamage(float value);
    // vehCarDamage::Update's bookkeeping: regeneration, the damage fraction
    // and the impact timers (per sample).
    void update(float dt);
    // vehCarDamage::Update's test (it ejects the car's one-shot parts from
    // then on): damage enabled and MaxDamage reached.
    bool wrecked() const { return enabled && params.maxDamage <= currentDamage; }
    // mmPlayer::IsMaxDamaged: strictly past MaxDamage.
    bool maxDamaged() const { return params.maxDamage < currentDamage; }
};

// vehAxle: anti-roll coupling between an axle's wheels (TorqueCoef,
// DampCoef) and the axle's visual roll.
struct Axle {
    AxleParams params;
    float stiffness = 0.0f; // TorqueCoef * Izz
    float damping = 0.0f;   // 2 sqrt(stiffness * Izz) * DampCoef
    float roll = 0.0f;      // visual roll of the wheels (rad)
    // The "axle0/1" pivot (model space; identity without one). vehAxle::Update
    // writes the roll into its m0.y and the mean travel into its m2.y.
    Mat34 matrix;
    float rollFactor = 1.0f;  // 1 / the left wheel's offset along the pivot's X (1 without a pivot)
    float pitchFactor = 1.0f; // 1 / its offset along the pivot's Z
};

struct CarSimOptions {
    // ?WeatherFriction@@3MA: 1, or 0.8 in rain (0.75 in rain at night)
    // (mmGame::InitWeather, weather type 3).
    float weatherFriction = 1.0f;
    // Damage can wreck the car (which then brakes and stops responding).
    bool damage = true;
    // vehGyro and vehAxle coupling (the original always runs them; kept
    // switchable for comparisons).
    bool gyro = true;
    bool axleCoupling = true;
    // The player's car: mmPlayer::Update's input overrides apply (handbrake
    // below 4 mph without throttle, wrecked and finished states).
    bool player = false;
    // vehCarModel::InitBound's choice: the polygonal bound of
    // bound/<car>_bound.bnd (vehBound; the player's and the network cars),
    // or a box around it (dgBoundBox; AI opponents and police).
    bool polygonalBound = false;
};

// OpenMM2 (network prediction, docs/multiplayer.md "Players' cars"):
// everything a car's simulation changes from one sample to the next, so
// that a machine can put its car back where an earlier sample left it and
// run the samples after it again (CarSim::saveState / restoreState). It
// restores into the car it was saved from only: the parts keep their
// pointers to that car's wheels, engine and gearbox.
struct CarSimState {
    InertialCS ics;
    Mat34 boundMatrix;
    Collider collider;
    int room = 0;
    std::array<Wheel, 4> wheels;
    Engine engine;
    Transmission trans;
    std::array<Drivetrain, 3> drivetrains;
    std::array<Axle, 2> axles;
    Aero aero;
    Gyro gyro;
    Stuck stuck;
    Splash splash;
    CarDamage damage; // its params are not restored
    float brakes = 0.0f;
    float handBrake = 0.0f;
    float steering = 0.0f;
    bool raceFinished = false;
    bool drivable = true;
    int undrivableMode = 0;
    float speed = 0.0f;
    float speedMph = 0.0f;
    std::uint32_t resets = 0;
    std::uint32_t random = 1;
    Vec3 resetPos;
    float resetRotation = 0.0f;
    std::optional<float> waterLevel;
};

// The turn about Y of a spawn transform built by Mat34::rotationY (the
// session's starts, posts and checkpoints keep MM2's angle only in this
// form): the reset rotation (vehCarSim +0x250) that faces the car the same
// way, equal to the original angle up to a float rounding.
float resetRotationOf(const Mat34& spawn);

// vehCarSim (Midtown Madness 2): a car, verified against the build 3393
// code. Construct, init(), add body() to a World, set inputs each frame.
//
// Coordinate conventions: car model space has its origin near the ground
// under the body centre and faces -Z. The model's origin is the rigid body's
// position plus R * CenterOfGravity (vehCarSim::SetWorldMatrix), so the
// centre of mass sits at -CenterOfGravity in model space. Steering +1 turns
// right.
class CarSim final : public BodyController, public ImpactHandler {
public:
    using Options = CarSimOptions;

    CarSim() = default;
    CarSim(const CarSim&) = delete;
    CarSim& operator=(const CarSim&) = delete;

    // vehCarSim::Init + ConfigureDrivetrain.
    void init(const CarSimParams& params, const VehicleGeometry& geometry, const Options& options = {});
    void setGyroParams(const GyroParams& p) { gyro.configure(p); }
    void setStuckParams(const StuckParams& p) { stuck.configure(p); }
    void setDamageParams(const CarDamageParams& p) { damage.params = p; }

    // Places the car's model origin at `model` and resets all state
    // (vehCarSim::Reset and vehCar::Reset; OpenMM2's placement, for the
    // OpenMM2-only repositioning of AI cars and network cars).
    void reset(const Mat34& model);
    // vehCarSim::SetResetPos: from now on vehCar::Reset puts the body's
    // centre at `position` + CenterOfGravity (vehCarSim +0x210). Every
    // caller passes a point on or above the road (a race start, a police
    // post, a checkpoint), so the model origin lands at position +
    // CenterOfGravity + R * CenterOfGravity.
    void setResetPos(const Vec3& position);
    const Vec3& resetPos() const { return m_resetPos; }
    // vehCarSim +0x250: the reset body's turn about Y (radians; the game
    // writes it next to each SetResetPos).
    float resetRotation = 0.0f;
    // vehCarSim::Reset with vehCar::Reset's resets: the body at resetPos(),
    // turned by resetRotation (Matrix34::Rotate about Y), at rest.
    void reset();
    // MM2's placement in one call: setResetPos(position), resetRotation =
    // rotation, reset().
    void resetAt(const Vec3& position, float rotation);

    // Inputs (vehCarSim brake/handbrake/steering, vehEngine throttle).
    void setInputs(float throttle, float brakes, float steering, float handBrake);

    // World matrix of the car's model origin (vehCarSim world matrix).
    Mat34 modelMatrix() const;
    // World matrix of a wheel (steered, displaced, spun, rolled with its axle).
    Mat34 wheelMatrix(int i) const;

    // vehCarSim::OnGround: number of wheels touching the ground.
    int wheelsOnGround() const;
    // vehCarSim::BottomedOut: number of wheels that bottomed out this sample.
    int bottomedOut() const;
    // vehCar::RequiresTerrainCollision, which dgPhysManager::CollideTerrain
    // asks before colliding the body with the room's terrain: not while the
    // car stands upright (up.y > 0.5) with the mean of its wheels' probe
    // normals within sqrt(0.1) of its up axis and no wheel bottomed out.
    bool requiresTerrainCollision() const;
    // mmPlayer::UpdateRegen (Cops and Robbers, mmPlayer::EnableRegen; once a
    // frame, only with damage enabled): above 5 m/s the damage heals by
    // MaxDamage / 2000 a frame; once that empties it, mmPlayer::ResetDamage
    // clears it. Returns true then (the caller also clears the model's
    // visual damage, as ResetDamage does).
    bool regenerate();
    bool onGround() const { return wheelsOnGround() > 0; }
    // vehCarSim::GetSSSFactor.
    float sssFactor(float speed) const;
    float speed() const { return m_speed; }       // m/s, |velocity . car Z axis|
    float speedMph() const { return m_speedMph; } // m_speed * MetricFactor (mph)

    // The water level of the room the car is in, if it is a water room
    // (vehCar::Update activates vehSplash once the car's origin is below it).
    void setWaterLevel(std::optional<float> level) { m_waterLevel = level; }

    // Ground used by the wheels; defaults to the World the body is in.
    void setGround(const GroundQuery* ground) { m_ground = ground; }
    // Called with every impact vehCarDamage::ApplyImpact applies (sounds,
    // effects, game logic): the effects MM2 runs in ApplyImpact itself and
    // the game callback (vehCarDamage::SetGameCallback, which only
    // mmPlayer::Init sets: mmPlayer::ImpactCallback).
    std::function<void(const CarImpact&)> onImpactCallback;

    // vehCarModel::InitBound again with another choice of bound (see
    // CarSimOptions::polygonalBound).
    void setPolygonalBound(bool polygonal);
    const Bound* bound() const { return m_bound.get(); }
    // phBound::SetElasticity on the car's bound (mmGame::SendChatMessage's
    // "/blubber" sets 4 on the player's).
    void setBoundElasticity(float elasticity);
    // Half the size of the collision bound's box (model space), for the AI.
    Vec3 halfExtents() const;

    // BodyController.
    void beforeIntegrate(Body& body, float dt, const World& world) override;
    void afterIntegrate(Body& body, float dt, const World& world) override;
    // ImpactHandler: vehCarDamage::Impact.
    void onImpact(Collider& self, const Impact& impact, const Vec3& impulse) override;

    VehicleBody body;
    CarSimParams params;
    Options options;
    Vec3 centerOfGravity; // vehCarSim CenterOfGravity
    Engine engine;
    Transmission trans;
    // 0, 1: Freetrain (front or back wheels, one each); 2: Drivetrain
    // (engine-driven). Updated in that order, as vehCarSim's children.
    std::array<Drivetrain, 3> drivetrains;
    std::array<Wheel, 4> wheels; // FL, FR, BL, BR
    std::array<Axle, 2> axles;   // front, back
    Aero aero;
    Gyro gyro;
    Stuck stuck;
    Splash splash;
    CarDamage damage;

    float brakes = 0.0f;
    float handBrake = 0.0f;
    float steering = 0.0f;
    // mmPlayer +0x2258: the player has finished the race (brakes on, wheel
    // turned full left from then on). Set by the game.
    bool raceFinished = false;
    // OpenMM2 presentation: how many times the car has been reset (put
    // somewhere at rest). What is drawn between two simulation steps is not
    // blended across a reset (game::StepHistory); the simulation never
    // reads it.
    std::uint32_t resets = 0;
    // OpenMM2 (network prediction): the state after the last sample, and
    // that state put back (CarSimState).
    CarSimState saveState() const;
    void restoreState(const CarSimState& state);
    // OpenMM2 (network races): the wheels draw their bump numbers from the
    // car's own stream (`randomState`) instead of the world's, which every
    // simulated object shares (MM2: one rand() for the whole game). The
    // host and the car's own machine then simulate the car alike whatever
    // else each one simulates.
    bool ownRandom = false;
    std::uint32_t randomState = 1;
    // The reset position as vehCarSim +0x210 holds it (CenterOfGravity
    // added), set as it is (another machine's car, placed where that machine
    // put it).
    void setResetPosRaw(const Vec3& position) { m_resetPos = position; }

    // vehCar's drivable flag (vehCar +0xe8 bit 2, vehCar::SetDrivable): the
    // game clears it while a car is held before the start. vehCar::Update
    // then runs neither vehStuck nor vehSplash.
    bool drivable = true;
    // vehCar +0xec: how a car that is not drivable is held (see preUpdate).
    int undrivableMode = 0;
    // vehCar::SetDrivable(on, mode): drivable again (mode 0, out of neutral
    // into first: vehTransmission::SetForward), or held in `mode`; modes 1
    // and 3 also select neutral.
    void setDrivable(bool on, int mode);
    // vehCar::PreUpdate's hold of a car that is not drivable, every frame
    // before the physics: mode 1 brakes on and neutral (the throttle is left
    // alone, so the engine revs); modes 2 and 3 brakes on with the throttle,
    // steering and handbrake off.
    void preUpdate();

private:
    WheelEnv makeEnv(float dt, const World& world);
    void resetBody(const Mat34& bodyMatrix);
    void updateAxles();
    Drivetrain& primary() { return drivetrains[2]; }
    void buildBound();
    // vehCarDamage::InsertImpact / ApplyImpact.
    void insertImpact(const Impact& impact, const Vec3& impulse, const Collider* other);
    void applyImpact(CarDamage::ImpactInfo& entry);

    Vec3 m_resetPos; // vehCarSim +0x210 (SetResetPos)
    std::array<int, 3> m_drivetrainOrder{0, 1, 2};
    int m_numDrivetrains = 3;
    const GroundQuery* m_ground = nullptr;
    std::optional<float> m_waterLevel;
    std::optional<GeometryData> m_boundData; // bound/<car>_bound.bnd
    Aabb m_boundBox;                         // its box (or the geometry's fallback)
    std::unique_ptr<Bound> m_bound;
    float m_speed = 0.0f;
    float m_speedMph = 0.0f;
};

} // namespace mm2::phys
