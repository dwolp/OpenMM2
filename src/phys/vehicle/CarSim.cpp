// vehCarSim (Init, ConfigureDrivetrain, Reset, Update, SetWorldMatrix,
// GetSSSFactor, OnGround) and the per-sample parts of vehCar::Update (gyro,
// stuck, damage) and vehAxle from Midtown Madness 2, verified against the
// build 3393 code (MM2Recomp). See docs/physics.md.

#include "phys/vehicle/CarSim.h"

#include "core/Libm.h"
#include "phys/AgeMath.h"

#include <algorithm>
#include <cmath>

namespace mm2::phys {
namespace {

// dgPhysEntity::Update: vehicles fall at 19.6 m/s^2.
constexpr float kVehicleGravity = 19.6f;
// vehCarSim::Init: the body's angular velocity is limited to 4 pi per axis.
constexpr float kMaxAngVelocity = 12.566371f;

} // namespace

void CarDamage::reset() {
    // vehCarDamage::ClearDamage: no damage and an empty impact list.
    currentDamage = 0.0f;
    damage = 0.0f;
    for (ImpactInfo& e : impacts)
        e.other = nullptr;
}

void CarDamage::addDamage(float value) {
    // vehCarDamage::AddDamage.
    const float d = value + currentDamage;
    currentDamage = d;
    if (d < 0.0f)
        currentDamage = 0.0f;
}

void CarDamage::update(float dt) {
    // vehCarDamage::Update: regeneration, the damage fraction, and the
    // impact list's relax timers.
    currentDamage = currentDamage - dt * params.regenerateRate;
    if (currentDamage < 0.0f)
        currentDamage = 0.0f;
    const float med = params.medDamage, max = params.maxDamage;
    float f = (currentDamage - med) / (max - med);
    damage = f < 0.0f ? 0.0f : (1.0f < f ? 1.0f : f);
    for (ImpactInfo& e : impacts) {
        if (!e.other)
            continue;
        const float t = e.timer - dt;
        e.timer = t;
        if (t < 0.0f)
            e.other = nullptr;
    }
}

void CarSim::init(const CarSimParams& given, const VehicleGeometry& g, const Options& o) {
    CarSimParams p = given;
    mm1Params.reset();
    if (p.mm1) {
        mm1Params = *p.mm1;
        p.mm1 = nullptr;
        // OpenMM2 Enhanced: an MM1 car's body numbers go where MM2's are read.
        const Mm1CarParams& m = *mm1Params;
        p.mass = m.mass;
        p.inertiaBox = m.inertiaBox;
        p.centerOfGravity = m.bodyCG;
        p.boundFriction = m.boundFriction;
        p.boundElasticity = m.boundElasticity;
        p.drivetrainType = m.drivetrainType;
        p.carFrictionHandling = m.carFrictionHandling != 0.0f ? m.carFrictionHandling : 1.0f;
    }
    params = p;
    options = o;
    centerOfGravity = p.centerOfGravity;
    damage.enabled = o.damage;

    body.ics = InertialCS{};
    body.controller = this;
    body.joint = nullptr;
    InertialCS& ics = body.ics;
    ics.setMass(p.inertiaBox.x, p.inertiaBox.y, p.inertiaBox.z, p.mass);
    // (MM1: mmPhysicsMGR's gravity, -19.8.)
    ics.gravity = {0.0f, mm1Params ? -19.8f : -kVehicleGravity, 0.0f};
    ics.setMaxAngVelocity(kMaxAngVelocity);
    ics.state = InertialCS::Off;

    // The collision bound (vehCarModel::InitBound) in model space, whose
    // origin sits at CenterOfGravity in the body's frame.
    m_boundData = g.bound;
    m_boundBox = g.body;
    if (!m_boundBox.valid()) {
        for (const auto& w : g.wheels) {
            m_boundBox.expand(w.center + Vec3{w.radius, w.radius, w.radius});
            m_boundBox.expand(w.center - Vec3{w.radius, 0.0f, w.radius});
        }
        m_boundBox.max.y += 0.8f;
    }
    body.boundOrigin = centerOfGravity;
    body.collider.handler = this;
    buildBound();

    if (mm1Params) {
        initMm1Parts(g);
        const Vec3 half = p.inertiaBox * 0.5f;
        splash.init(centerOfGravity - half, half + centerOfGravity);
        setResetPos({});
        reset();
        return;
    }

    // Wheels (vehWheel::Init): the tune file's WheelFront / WheelBack are
    // the left wheels'; the right ones start from the constructor's values
    // and take the left ones' by vehWheel::CopyVars (all but HandbrakeCoef
    // and WobbleLimit).
    wheels[0].init(p.wheelFront, g.wheels[0], p.mass, true, centerOfGravity.z);
    wheels[1].init(WheelParams{}, g.wheels[1], p.mass, true, centerOfGravity.z);
    wheels[2].init(p.wheelBack, g.wheels[2], p.mass, true, centerOfGravity.z);
    wheels[3].init(WheelParams{}, g.wheels[3], p.mass, true, centerOfGravity.z);
    wheels[1].copyVars(wheels[0]);
    wheels[3].copyVars(wheels[2]);

    engine.configure(p.engine);
    engine.pivot = g.enginePivot;
    trans.configure(p.trans);

    drivetrains = {};
    drivetrains[0].configure(p.freetrain);
    drivetrains[1].configure(p.freetrain);
    drivetrains[2].configure(p.drivetrain);
    drivetrains[2].attach(&engine, &trans);

    // vehCarSim::ConfigureDrivetrain (vehCarSim::Init reaches it through
    // vehCarSim::ReconfigureDrivetrain, which first runs
    // vehCarSim::UnconfigureDrivetrain on the still empty drivetrains).
    Wheel& fl = wheels[0];
    Wheel& fr = wheels[1];
    Wheel& bl = wheels[2];
    Wheel& br = wheels[3];
    const int type = std::clamp(p.drivetrainType, 0, 2);
    switch (type) {
    case 0: // rear-wheel drive
        drivetrains[0].addWheel(&fl);
        drivetrains[1].addWheel(&fr);
        drivetrains[2].addWheel(&bl);
        drivetrains[2].addWheel(&br);
        m_drivetrainOrder = {0, 1, 2};
        m_numDrivetrains = 3;
        break;
    case 1: // front-wheel drive
        drivetrains[0].addWheel(&bl);
        drivetrains[1].addWheel(&br);
        drivetrains[2].addWheel(&fl);
        drivetrains[2].addWheel(&fr);
        m_drivetrainOrder = {0, 1, 2};
        m_numDrivetrains = 3;
        break;
    default: // all-wheel drive
        for (Wheel* w : {&fl, &fr, &bl, &br})
            drivetrains[2].addWheel(w);
        m_drivetrainOrder = {2, 0, 0};
        m_numDrivetrains = 1;
        break;
    }

    // vehCarSim::Init: ComputeConstants of the engine, transmission and axles.
    engine.computeConstants();
    trans.computeConstants(engine, primary().wheel(0)->radius);
    for (std::size_t a = 0; a < 2; ++a) {
        Axle& axle = axles[a];
        axle.params = a == 0 ? p.axleFront : p.axleBack;
        axle.stiffness = axle.params.torqueCoef * ics.inertia.z;
        const float d = std::sqrt(axle.stiffness * ics.inertia.z) * axle.params.dampCoef;
        axle.damping = d + d;
        // vehAxle::Init: with an "axle0/1" pivot, the left wheel's offset
        // from it along the pivot's Z and X axes scales the visual pitch and
        // roll. (MM2 divides unguarded; a zero offset keeps the factor 1.)
        axle.matrix = g.axlePivots[a].value_or(Mat34::identity());
        axle.pitchFactor = 1.0f;
        axle.rollFactor = 1.0f;
        if (g.axlePivots[a]) {
            const Mat34& m = axle.matrix;
            const Vec3 off = wheels[a * 2].center - m.m3;
            const float along = (off.z * m.m2.z + off.y * m.m2.y) + off.x * m.m2.x;
            if (along != 0.0f)
                axle.pitchFactor = 1.0f / along;
            const float across = (off.z * m.m0.z + off.y * m.m0.y) + off.x * m.m0.x;
            if (across != 0.0f)
                axle.rollFactor = 1.0f / across;
        }
    }
    aero.configure(p.aero);
    // vehCar::Init: the splash points span the InertiaBox around the model
    // origin (in the body's frame, the origin is at +CenterOfGravity).
    const Vec3 half = p.inertiaBox * 0.5f;
    splash.init(centerOfGravity - half, half + centerOfGravity);
    // vehCarSim::Init ends with SetResetPos(origin) and Reset: the body at
    // CenterOfGravity, unturned (the constructor's reset rotation is 0).
    setResetPos({});
    reset();
}

void CarSim::reset(const Mat34& model) {
    Mat34 m = model;
    m.m3 = model.m3 - model.transformDir(centerOfGravity);
    resetBody(m);
}

float resetRotationOf(const Mat34& spawn) {
    // Mat34::rotationY(a) has m2 = (sin a, 0, cos a).
    return libm::atan2(spawn.m2.x, spawn.m2.z);
}

void CarSim::setResetPos(const Vec3& position) {
    // vehCarSim::SetResetPos: CenterOfGravity plus the position.
    const Vec3& cg = centerOfGravity;
    m_resetPos = {cg.x + position.x, cg.y + position.y, cg.z + position.z};
}

void CarSim::reset() {
    // vehCarSim::Reset: phInertialCS::Reset (identity, at rest), the
    // position from the reset position, then Matrix34::Rotate about the Y
    // axis by the reset rotation (the 3x3 part only).
    Mat34 m = Mat34::identity();
    m.m3 = m_resetPos;
    age::rotate(m, {0.0f, 1.0f, 0.0f}, resetRotation);
    resetBody(m);
}

void CarSim::resetAt(const Vec3& position, float rotation) {
    setResetPos(position);
    resetRotation = rotation;
    reset();
}

void CarSim::resetBody(const Mat34& bodyMatrix) {
    ++resets;
    InertialCS& ics = body.ics;
    ics.zero();
    ics.matrix = bodyMatrix;

    engine.reset();
    trans.reset();
    for (Drivetrain& d : drivetrains)
        d.reset();
    const Mat34 world = modelMatrix();
    for (Wheel& w : wheels) {
        w.reset();
        w.matrix = Mat34::mul(Mat34::translation(w.center), world);
    }
    stuck.reset();
    // vehCar::Reset only clears the splash's active flag: a car reset after
    // sinking keeps its lowered buoyancy (vehSplash::Reset runs once, from
    // its constructor).
    splash.deactivate();
    damage.reset();
    raceFinished = false;
    steering = 0.0f;
    brakes = 0.0f;
    handBrake = 0.0f;
    m_speed = 0.0f;
    m_speedMph = 0.0f;
    // vehCarSim::RestoreImpactParams: the bound's friction and elasticity.
    if (m_bound) {
        m_bound->setElasticity(params.boundElasticity);
        m_bound->setFriction(params.boundFriction);
    }
    // vehCar::Reset resets the collider: the next sweep starts here.
    body.syncBoundMatrix();
    body.collider.reset();
}

Vec3 CarSim::halfExtents() const {
    if (!m_bound)
        return m_boundBox.extent();
    const Vec3& lo = m_bound->boxMin;
    const Vec3& hi = m_bound->boxMax;
    return (hi - lo) * 0.5f;
}

void CarSim::setBoundElasticity(float elasticity) {
    if (m_bound)
        m_bound->setElasticity(elasticity);
}

void CarSim::setPolygonalBound(bool polygonal) {
    options.polygonalBound = polygonal;
    buildBound();
}

void CarSim::buildBound() {
    // vehCarModel::InitBound: bound/<car>_bound.bnd (phBoundGeometry::Load
    // into a vehBound, whose single own material every polygon uses:
    // vehBound::vehBound, vehBound::Init); AI cars replace it with a
    // dgBoundBox of its box (SetOffset to the box's centre, SetSize to its
    // extent). vehCar::Init then gives the bound BoundFriction and
    // BoundElasticity (vehBound::SetFriction, vehBound::SetElasticity).
    std::unique_ptr<BoundGeometry> geometry;
    if (m_boundData)
        geometry = makeGeometryBound(*m_boundData);
    Vec3 lo = m_boundBox.min, hi = m_boundBox.max;
    if (geometry) {
        geometry->makeOwnMaterial();
        lo = geometry->boxMin;
        hi = geometry->boxMax;
    }
    if (geometry && options.polygonalBound) {
        m_bound = std::move(geometry);
    } else {
        const Vec3 size{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        const Vec3 centre{size.x * 0.5f + lo.x, size.y * 0.5f + lo.y, size.z * 0.5f + lo.z};
        auto box = std::make_unique<BoundBox>();
        box->makeOwnMaterial();
        box->setOffset(centre);
        box->setSize(size);
        m_bound = std::move(box);
    }
    m_bound->setFriction(params.boundFriction);
    m_bound->setElasticity(params.boundElasticity);
    body.collisionBound = m_bound.get();
    body.resetCollider();
}

void CarSim::setInputs(float throttle, float brakeInput, float steer, float handBrakeInput) {
    engine.throttle = clampf(throttle, 0.0f, 1.0f);
    brakes = clampf(brakeInput, 0.0f, 1.0f);
    steering = clampf(steer, -1.0f, 1.0f);
    handBrake = clampf(handBrakeInput, 0.0f, 1.0f);
}

void CarSim::setDrivable(bool on, int mode) {
    if (on) {
        undrivableMode = 0;
        drivable = true;
        trans.setDrive();
        return;
    }
    drivable = false;
    undrivableMode = mode;
    if (mode == 1 || mode == 3)
        trans.setNeutral();
}

void CarSim::preUpdate() {
    if (drivable)
        return;
    if (undrivableMode == 1) {
        brakes = 1.0f;
        trans.setNeutral();
    } else if (undrivableMode == 2 || undrivableMode == 3) {
        brakes = 1.0f;
        engine.throttle = 0.0f;
        steering = 0.0f;
        handBrake = 0.0f;
    }
}

CarSimState CarSim::saveState() const {
    CarSimState s;
    s.ics = body.ics;
    s.boundMatrix = body.boundMatrix;
    s.collider = body.collider;
    s.room = body.room;
    s.wheels = wheels;
    s.engine = engine;
    s.trans = trans;
    s.drivetrains = drivetrains;
    s.axles = axles;
    s.aero = aero;
    s.gyro = gyro;
    s.stuck = stuck;
    s.splash = splash;
    s.damage = damage;
    s.brakes = brakes;
    s.handBrake = handBrake;
    s.steering = steering;
    s.raceFinished = raceFinished;
    s.drivable = drivable;
    s.undrivableMode = undrivableMode;
    s.speed = m_speed;
    s.speedMph = m_speedMph;
    s.resets = resets;
    s.random = randomState;
    s.resetPos = m_resetPos;
    s.resetRotation = resetRotation;
    s.waterLevel = m_waterLevel;
    return s;
}

void CarSim::restoreState(const CarSimState& s) {
    body.ics = s.ics;
    body.boundMatrix = s.boundMatrix;
    body.collider = s.collider;
    body.room = s.room;
    wheels = s.wheels;
    engine = s.engine;
    trans = s.trans;
    drivetrains = s.drivetrains;
    axles = s.axles;
    aero = s.aero;
    gyro = s.gyro;
    stuck = s.stuck;
    splash = s.splash;
    const CarDamageParams limits = damage.params;
    damage = s.damage;
    damage.params = limits;
    brakes = s.brakes;
    handBrake = s.handBrake;
    steering = s.steering;
    raceFinished = s.raceFinished;
    drivable = s.drivable;
    undrivableMode = s.undrivableMode;
    m_speed = s.speed;
    m_speedMph = s.speedMph;
    resets = s.resets;
    randomState = s.random;
    m_resetPos = s.resetPos;
    resetRotation = s.resetRotation;
    m_waterLevel = s.waterLevel;
}

Mat34 CarSim::modelMatrix() const {
    // vehCarSim::SetWorldMatrix: the body matrix moved by R * CenterOfGravity
    // (summed in its order).
    Mat34 m = body.ics.matrix;
    const Vec3& cg = centerOfGravity;
    m.m3 = {((m.m1.x * cg.y + m.m2.x * cg.z) + cg.x * m.m0.x) + m.m3.x,
            ((m.m1.y * cg.y + m.m0.y * cg.x) + m.m2.y * cg.z) + m.m3.y,
            ((m.m1.z * cg.y + m.m0.z * cg.x) + m.m2.z * cg.z) + m.m3.z};
    return m;
}

Mat34 CarSim::wheelMatrix(int i) const {
    return wheels[static_cast<std::size_t>(i)].matrix;
}

int CarSim::wheelsOnGround() const {
    return static_cast<int>(std::ranges::count_if(wheels, [](const Wheel& w) { return w.onGround; }));
}

int CarSim::bottomedOut() const {
    return static_cast<int>(std::ranges::count_if(wheels, [](const Wheel& w) { return w.bottomedOut; }));
}

bool CarSim::requiresTerrainCollision() const {
    const Mat34& m = body.ics.matrix;
    if (!(0.5f < m.m1.y))
        return true;
    // The probe normals (front pair, back pair, then both), less the up axis.
    const Vec3& fl = wheels[0].intersection.normal;
    const Vec3& fr = wheels[1].intersection.normal;
    const Vec3& bl = wheels[2].intersection.normal;
    const Vec3& br = wheels[3].intersection.normal;
    const Vec3 f{(fl.x + fr.x) * 0.5f, (fl.y + fr.y) * 0.5f, (fl.z + fr.z) * 0.5f};
    const Vec3 b{(bl.x + br.x) * 0.5f, (bl.y + br.y) * 0.5f, (bl.z + br.z) * 0.5f};
    const Vec3 d{(f.x + b.x) * 0.5f - m.m1.x, (b.y + f.y) * 0.5f - m.m1.y, (b.z + f.z) * 0.5f - m.m1.z};
    return !((d.z * d.z + d.y * d.y) + d.x * d.x < 0.1f && bottomedOut() == 0);
}

bool CarSim::regenerate() {
    const Vec3& v = body.ics.linearVelocity;
    if (!((v.y * v.y + v.z * v.z) + v.x * v.x > 25.0f))
        return false;
    const float current = damage.currentDamage;
    if (current < 0.01f)
        return false;
    const float heal = damage.params.maxDamage * -0.0005f;
    damage.addDamage(heal);
    if (heal + current <= 0.0f) {
        damage.reset(); // mmPlayer::ResetDamage -> vehCarDamage::ClearDamage
        return true;
    }
    return false;
}

float CarSim::sssFactor(float speed) const {
    const float threshold = params.sssThreshold;
    if (threshold == 0.0f)
        return 1.0f;
    if (speed < threshold)
        return (speed / threshold) * (params.sssValue - 1.0f) + 1.0f;
    return params.sssValue;
}

WheelEnv CarSim::makeEnv(float dt, const World& world) {
    WheelEnv env;
    env.ics = &body.ics;
    env.ground = m_ground ? m_ground : &world;
    env.dt = dt;
    env.invDt = 1.0f / dt;
    env.weatherFriction = options.weatherFriction;
    env.hasCar = true;
    env.carFrictionHandling = params.carFrictionHandling;
    env.randomSeed = ownRandom ? &randomState : world.randomSeed();
    env.self = &body;
    env.realism = realism;
    env.longSlideMultiplier = mm1Params && mm1Params->longSlideMultiplier != 0.0f ? mm1Params->longSlideMultiplier : 1.0f;
    if (mm1Params && mm1Params->slipPercentThresh != 0.0f)
        env.slipPercentThresh = mm1Params->slipPercentThresh;
    // mmCarSim::ShouldSkid (Open1560).
    env.shouldSkid = m_speed > 7.0f || engine.throttle > 0.5f || brakes > 0.7f;
    env.carSpeed = m_speed;
    return env;
}

void CarSim::beforeIntegrate(Body& b, float, const World&) {
    InertialCS& ics = b.ics;
    // mmPlayer::Update: after the finish the car brakes with the wheel turned
    // full left; a wrecked car gets no throttle, steering or brake; and below
    // 4 mph without throttle the handbrake holds it (the speed being the one
    // shown, from the previous sample).
    if (options.player) {
        if (raceFinished) {
            engine.throttle = 0.0f;
            brakes = 1.0f;
            steering = -1.0f;
        }
        if (damage.enabled && damage.maxDamaged()) {
            engine.throttle = 0.0f;
            steering = 0.0f;
            brakes = 0.0f;
        }
        if (m_speedMph < 4.0f && engine.throttle == 0.0f)
            handBrake = 1.0f;
    }

    // vehCarSim::Update: forward speed, then the wheel inputs.
    const Vec3& v = ics.linearVelocity;
    const Vec3& z = ics.matrix.m2;
    m_speed = std::abs((z.z * v.z + z.y * v.y) + z.x * v.x);
    m_speedMph = m_speed * kMetersPerSecondToMph;
    if (mm1Params)
        return; // MM1: the wheel inputs come from updateMm1 (mmCarSim::Update)
    const float steer = sssFactor(m_speed) * steering;
    const float front = handBrake < 0.0f ? -handBrake : 0.0f;
    wheels[0].setInputs(steer, brakes, front);
    wheels[1].setInputs(steer, brakes, front);
    const float hand = handBrake <= 0.0f ? 0.0f : handBrake;
    // Burnout: full brake and throttle at a standstill frees the back wheels.
    float rearBrake = brakes;
    if (0.95f < brakes && 0.95f < engine.throttle && m_speed < 1.0f)
        rearBrake = 0.0f;
    // The handbrake eases off the back wheel on the inside of the turn.
    wheels[2].setInputs(-steer, rearBrake, (steering <= 0.0f ? 1.0f : 1.0f - steering) * hand);
    wheels[3].setInputs(-steer, rearBrake, (0.0f <= steering ? 1.0f : steering + 1.0f) * hand);
}

void CarSim::afterIntegrate(Body& b, float dt, const World& world) {
    InertialCS& ics = b.ics;
    WheelEnv env = makeEnv(dt, world);
    const Mat34 frame = modelMatrix();
    env.frame = &frame;

    // vehCarSim's children, in order.
    const int type = std::clamp(params.drivetrainType, 0, 2);
    if (mm1Params) {
        updateMm1(dt, env);
    } else {
        engine.update(dt, trans, primary(), ics, type);
        trans.update(dt, engine, wheelsOnGround());
        aero.update(ics, m_speed, dt, env.invDt);
        for (int i = 0; i < m_numDrivetrains; ++i)
            drivetrains[static_cast<std::size_t>(m_drivetrainOrder[static_cast<std::size_t>(i)])].update(
                env, params.mass);
        updateAxles();
    }

    // vehCar::Update: vehGyro, vehStuck, vehCarDamage.
    if (options.gyro && !mm1Params) {
        Gyro::Inputs gi;
        gi.steering = steering;
        gi.sssFactor = sssFactor(m_speed);
        gi.drivetrainSpeed = primary().rotationSpeed;
        gi.brake = brakes;
        gi.handbrake = handBrake;
        gi.wheelsOnGround = wheelsOnGround();
        gyro.update(ics, gi);
    }
    Stuck::Inputs si;
    si.throttle = engine.throttle;
    si.maxThrottle = engine.maxThrottle;
    si.steering = steering;
    si.gear = trans.currentGear;
    si.wheelsOnGround = wheelsOnGround();
    if (drivable && stuck.update(ics, dt, si)) {
        engine.throttle = 0.0f;
        brakes = 1.0f;
    }
    if (m_waterLevel && modelMatrix().m3.y < *m_waterLevel)
        splash.activate(*m_waterLevel);
    if (drivable)
        splash.update(ics, dt);
    damage.update(dt);
    // vehCarDamage::Update (bWobble, on): damaged wheels wobble, less as the
    // front-left wheel spins faster; the front-left and back-right ones one
    // way, the other two the other way.
    float spin = std::abs(wheels[0].rotationSpeed) * dt;
    spin = (spin + spin) * 0.31830987f;
    spin = spin < 0.0f ? 0.0f : (1.0f < spin ? 1.0f : spin);
    const float wobble = (1.0f - spin) * damage.damage;
    wheels[0].wobble = wobble * -0.15f;
    wheels[2].wobble = wobble * 0.35f;
    wheels[1].wobble = wobble * 0.35f;
    wheels[3].wobble = wobble * -0.15f;
}

void CarSim::updateAxles() {
    // vehAxle::Update: anti-roll torque about the body's length, and the
    // wheels rolled with the axle (or by CamberLimit when both set one).
    InertialCS& ics = body.ics;
    const Mat34 world = modelMatrix();
    for (std::size_t a = 0; a < 2; ++a) {
        Axle& axle = axles[a];
        Wheel& l = wheels[a * 2];
        Wheel& r = wheels[a * 2 + 1];
        const float dl = l.suspension - l.visualDispVert();
        const float dr = r.suspension - r.visualDispVert();
        const float diff = dl - dr;
        axle.roll = diff * axle.rollFactor * 0.5f;
        // The axle's own matrix takes the roll and the mean travel (its m0.y
        // and m2.y), tilting the axis the wheels are rolled about.
        axle.matrix.m2.y = (dr + dl) * axle.pitchFactor * 0.5f;
        axle.matrix.m0.y = axle.roll;
        if (options.axleCoupling && axle.stiffness != 0.0f) {
            const float t =
                -(diff * axle.stiffness + (l.suspensionVelocity - r.suspensionVelocity) * axle.damping);
            ics.applyTorque({t * ics.matrix.m2.x, t * ics.matrix.m2.y, t * ics.matrix.m2.z});
        }
        const Mat34 axleWorld = Mat34::mul(axle.matrix, world);
        const bool camber = 0.0f <= l.params.camberLimit && 0.0f <= r.params.camberLimit;
        for (Wheel* w : {&l, &r}) {
            const float angle = camber ? w->camber : axle.roll;
            age::rotate(w->matrix, axleWorld.m2, angle); // Matrix34::Rotate
        }
    }
}

void CarSim::onImpact(Collider& self, const Impact& impact, const Vec3& impulse) {
    // vehCarDamage::Impact: the stuck watcher, then the impact list.
    stuck.impact(body.ics);
    const Collider* other = impact.colliderA == &self ? impact.colliderB : impact.colliderA;
    if (damage.enabled)
        insertImpact(impact, impulse, other);
}

void CarSim::insertImpact(const Impact& impact, const Vec3& impulse, const Collider* other) {
    // vehCarDamage::InsertImpact. The impact is worth the impulse times
    // vehCarDamage::GetDamageModifier (1; no class overrides it) times the
    // other body's share of the masses.
    float share = 1.0f;
    if (other && other->ics) {
        share = other->ics->mass;
        share = share / (body.ics.mass + share);
    }
    const float j2 = (impulse.x * impulse.x + impulse.y * impulse.y) + impulse.z * impulse.z;
    const float value = std::sqrt(j2) * 1.0f * share;
    for (CarDamage::ImpactInfo& e : damage.impacts) {
        if (e.other != other)
            continue;
        if (e.value * 1.25f < value) {
            // Hit again harder: applied again (with the first contact's
            // point and impulse), the relax time restarts.
            e.total = value + e.total;
            e.value = value;
            e.timer = CarDamage::kRelaxTime;
            applyImpact(e);
            return;
        }
        if (value <= damage.params.impactThreshold)
            return;
        if (m_speedMph < 10.0f && !(other && other->ics))
            return;
        e.total = value + e.total;
        damage.addDamage(value);
        return;
    }
    for (CarDamage::ImpactInfo& e : damage.impacts) {
        if (e.other)
            continue;
        e.other = other;
        // The point in the car's model space (the world matrix's inverse,
        // applied in InsertImpact's order).
        const Mat34 inv = modelMatrix().fastInverse();
        const Vec3& p = impact.position;
        e.localPosition = {((inv.m2.x * p.z + inv.m1.x * p.y) + inv.m0.x * p.x) + inv.m3.x,
                           ((inv.m2.y * p.z + inv.m1.y * p.y) + inv.m0.y * p.x) + inv.m3.y,
                           ((inv.m2.z * p.z + inv.m1.z * p.y) + inv.m0.z * p.x) + inv.m3.z};
        e.position = impact.position;
        e.normal = impact.normal;
        e.impulse = impulse;
        e.value = value;
        e.total = value;
        e.timer = CarDamage::kRelaxTime;
        applyImpact(e);
        return;
    }
}

void CarSim::applyImpact(CarDamage::ImpactInfo& e) {
    // vehCarDamage::ApplyImpact: a sound for anything above 0.001; above
    // ImpactThreshold, at 10 mph or more or against a body, the damage, its
    // effects and the game's callback.
    CarImpact out;
    out.other = e.other;
    out.otherBody = nullptr;
    out.localPosition = e.localPosition;
    out.position = e.position;
    out.normal = e.normal;
    out.impulse = e.impulse;
    out.value = e.value;
    out.total = e.total;
    out.otherIsBody = e.other && e.other->ics;
    if (0.001f < e.value) {
        out.sound = true;
        // The other collider's id (AudImpact plays the WALL entry for ids
        // beyond its table; MM2 passes 1000 when the collider has no
        // instance data, 0 above 1000).
        int id = e.other ? e.other->id : 1000;
        if (1000 < id)
            id = 0;
        out.audioId = id;
        out.soundStrength = std::abs(e.impulse.z) + std::abs(e.impulse.y) + std::abs(e.impulse.x);
    }
    if (damage.params.impactThreshold < e.value && (10.0f <= m_speedMph || out.otherIsBody)) {
        out.damaging = true;
        damage.addDamage(e.value);
    }
    out.otherBody = out.other ? out.other->body : nullptr;
    if ((out.sound || out.damaging) && onImpactCallback)
        onImpactCallback(out);
}

} // namespace mm2::phys
