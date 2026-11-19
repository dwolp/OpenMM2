#include "game/PlayerVehicle.h"

#include "asset/Bound.h"
#include "asset/Mtx.h"
#include "core/Log.h"
#include "game/CityLevel.h"
#include "core/StringUtil.h"
#include "data/DatFile.h"
#include "phys/vehicle/TuneParams.h"

#include <algorithm>
#include <format>

namespace mm2::game {
namespace {

// With a schema the file is read exactly as datParser::Read reads it for
// that class (see data::parseDat).
std::optional<data::DatFile> readDat(const vfs::Vfs& vfs, const std::string& path,
                                     const data::DatSchema* schema = nullptr) {
    auto bytes = vfs.readAll(path);
    if (!bytes)
        return std::nullopt;
    std::string error;
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    auto f = schema ? data::parseDat(text, *schema, &error) : data::parseDat(text, &error);
    if (!f)
        log::warn("vehicle: {}: {}", path, error);
    return f;
}

// GetPivot(<model>, <part>): the 12 floats of geometry/<model>_<part>.mtx as
// a matrix (rows min, max, centre, origin).
std::optional<Mat34> readPivot(const vfs::Vfs& vfs, const std::string& model, std::string_view part) {
    auto bytes = vfs.readAll(std::format("geometry/{}_{}.mtx", model, part));
    if (!bytes)
        return std::nullopt;
    auto mtx = asset::parseMtx(*bytes);
    if (!mtx)
        return std::nullopt;
    Mat34 m;
    m.m0 = mtx->min;
    m.m1 = mtx->max;
    m.m2 = mtx->center;
    m.m3 = mtx->origin;
    return m;
}

// The pivots vehCarSim::Init reads for `model`: the wheels (vehWheel::Init:
// centre, radius and width from <model>_whl0..3), the engine and the axles.
void readSimPivots(const vfs::Vfs& vfs, const std::string& model, phys::VehicleGeometry& geom) {
    for (int i = 0; i < 4; ++i)
        if (auto m = readPivot(vfs, model, std::format("whl{}", i)))
            geom.wheels[static_cast<std::size_t>(i)] = phys::VehicleGeometry::wheelFromPivot(*m);
    geom.enginePivot = readPivot(vfs, model, "engine");
    geom.axlePivots[0] = readPivot(vfs, model, "axle0");
    geom.axlePivots[1] = readPivot(vfs, model, "axle1");
}

// lvlInstance::GetGeomSet's radius of `part`: the largest modGetStatic
// radius over the part's levels of detail.
float geomSetRadius(const asset::VehicleModel& model, std::string_view part) {
    float radius = 0.0f;
    for (const auto& mesh : model.pkg.meshes)
        if (mesh.part == part)
            radius = std::max(radius, mesh.radius());
    return radius;
}

} // namespace

std::unique_ptr<SimVehicle> SimVehicle::loadPlayer(const vfs::Vfs& vfs, std::string_view baseName,
                                                   std::string* error, bool trailer) {
    return load(vfs, baseName, error, {}, true, trailer);
}

std::unique_ptr<SimVehicle> SimVehicle::load(const vfs::Vfs& vfs, std::string_view baseIn, std::string* error,
                                             std::string_view tuneSuffix, bool player, bool trailer) {
    const std::string base = str::lower(baseIn);
    auto v = std::make_unique<SimVehicle>();
    auto read = [&](std::string_view path) { return vfs.readAll(path); };
    auto model = asset::loadVehicleModel(base, read, error);
    if (!model)
        return nullptr;
    v->m_model = std::move(*model);

    // Tune: the requested variant first (e.g. vpbug_opp), else the base car.
    auto tunePath = [&](std::string_view ext) {
        const std::string variant = std::format("tune/vehicle/{}{}.{}", base, tuneSuffix, ext);
        return tuneSuffix.empty() || !vfs.exists(variant) ? std::format("tune/vehicle/{}.{}", base, ext) : variant;
    };
    phys::CarSimParams params;
    if (auto f = readDat(vfs, tunePath("vehcarsim"), &phys::carSimSchema()); f && f->top()) {
        phys::loadCarSimParams(*f->top(), params);
    } else {
        if (error)
            *error = std::format("missing {}", tunePath("vehcarsim"));
        return nullptr;
    }

    // Geometry: wheels from the model's pivots, body box from its bound.
    phys::VehicleGeometry geom = phys::VehicleGeometry::placeholder();
    for (const auto& w : v->m_model.wheels)
        if (w.index >= 4 && w.index < 6)
            geom.extraWheels[static_cast<std::size_t>(w.index - 4)] = {w.position, w.radius, w.width, true};
    readSimPivots(vfs, base, geom);
    // vehCarModel::InitBound: bound/<car>_bound.bnd (phBoundGeometry::Load
    // reads the text file; every retail car has one).
    std::optional<asset::BoundGeometry> bound = loadBoundFile(vfs, base, false);
    if (!bound)
        bound = loadBoundFile(vfs, base, true);
    if (bound && bound->bounds().valid()) {
        geom.body = bound->bounds();
        geom.hull = bound->vertices;
        geom.bound = toGeometryData(*bound);
    } else if (const auto* body = v->m_model.pkg.findBest("BODY")) {
        geom.body = body->bounds();
        log::warn("vehicle: {} has no collision bound; using the body mesh box", base);
    }

    // mmPlayer::Init: the player's vpcop runs vehCarSim::Init again with
    // "vpmustang99" (unless the -tune_car option is given), so its physics
    // are the Mustang's: tune, wheel, engine and axle pivots. The body,
    // bound, damage, gyro and stuck stay the police car's, and so does the
    // splash box vehCar::Init built from its InertiaBox before.
    const phys::CarSimParams copParams = params;
    if (player && base == "vpcop") {
        const std::string mustang = "tune/vehicle/vpmustang99.vehcarsim";
        if (auto f = readDat(vfs, mustang, &phys::carSimSchema()); f && f->top()) {
            params = phys::CarSimParams{};
            phys::loadCarSimParams(*f->top(), params);
            readSimPivots(vfs, "vpmustang99", geom);
        }
    }

    v->m_sim.init(params, geom);
    // The car instance's sphere radius: its "body" geometry's.
    v->m_sim.body.geometryRadius = geomSetRadius(v->m_model, "BODY");
    {
        const Vec3 half = copParams.inertiaBox * 0.5f;
        v->m_sim.splash.init(copParams.centerOfGravity - half, half + copParams.centerOfGravity);
    }

    // Semi trailer: vehTrailer + dgTrailerJoint tunes, <base>_trailer model.
    // vehCar::Init builds one only for a car with a trailer_hitch pivot.
    auto trailerTune = readDat(vfs, "tune/vehicle/" + base + ".vehtrailer");
    auto jointTune = readDat(vfs, "tune/vehicle/" + base + ".dgtrailerjoint");
    const asset::Mtx* carHitch = v->m_model.pivot("trailer_hitch");
    if (trailer && carHitch && trailerTune && trailerTune->top() && jointTune && jointTune->top()) {
        phys::TrailerParams tp;
        phys::TrailerJointParams jp;
        auto trailerModel = asset::loadVehicleModel(base + "_trailer", read, nullptr);
        if (trailerModel && phys::loadTrailerParams(*trailerTune->top(), tp) &&
            phys::loadTrailerJointParams(*jointTune->top(), jp)) {
            phys::TrailerGeometry tg;
            for (int i = 0; i < 4; ++i) {
                if (const auto* pivot = trailerModel->pivot(std::format("twhl{}", i)))
                    tg.wheels[static_cast<std::size_t>(i)] = {pivot->origin, pivot->halfExtent().y,
                                                              pivot->halfExtent().x * 2.0f, true};
            }
            // The trailer instance's bound (its materials resolve to the
            // bound's default material here; MM2 looks their names up in the
            // city's material manager).
            if (auto b = loadBoundFile(vfs, base + "_trailer", false)) {
                tg.body = b->bounds();
                tg.bound = toGeometryData(*b);
            }
            if (!tg.body.valid())
                if (const auto* body = trailerModel->pkg.findBest("TRAILER"))
                    tg.body = body->bounds();
            tg.carHitch = carHitch->origin;
            if (const auto* hitch = trailerModel->pivot("trailer_hitch"))
                tg.trailerHitch = hitch->origin;
            v->m_trailerModel = std::make_unique<asset::VehicleModel>(std::move(*trailerModel));
            v->m_trailer = std::make_unique<phys::Trailer>();
            v->m_trailer->init(tp, jp, tg, v->m_sim);
            // vehTrailerInstance's first geometry is "trailer".
            v->m_trailer->body.geometryRadius = geomSetRadius(*v->m_trailerModel, "TRAILER");
        }
    }
    // vehGyro::Init, vehStuck::Init, vehCarDamage::Init: each part loads
    // its own tune file.
    if (auto f = readDat(vfs, tunePath("vehgyro")); f && f->top()) {
        phys::GyroParams p;
        if (phys::loadGyroParams(*f->top(), p))
            v->m_sim.setGyroParams(p);
    }
    if (auto f = readDat(vfs, tunePath("vehstuck")); f && f->top()) {
        phys::StuckParams p;
        if (phys::loadStuckParams(*f->top(), p))
            v->m_sim.setStuckParams(p);
    }
    if (auto f = readDat(vfs, tunePath("vehcardamage")); f && f->top()) {
        phys::CarDamageParams p;
        if (phys::loadCarDamageParams(*f->top(), p))
            v->m_sim.setDamageParams(p);
    }
    return v;
}

void SimVehicle::addTo(phys::World& world) {
    world.add(&m_sim.body);
    if (m_trailer)
        m_trailer->addTo(world);
}

void SimVehicle::removeFrom(phys::World& world) {
    if (m_trailer)
        m_trailer->removeFrom(world);
    world.remove(&m_sim.body);
}

void SimVehicle::reset(const Mat34& model) {
    m_sim.reset(model);
    if (m_trailer)
        m_trailer->reset();
    m_controls.reset();
}

void SimVehicle::setResetPos(const Vec3& position, float rotation) {
    m_sim.setResetPos(position);
    m_sim.resetRotation = rotation;
}

void SimVehicle::setResetPos(const Mat34& spawn) {
    setResetPos(spawn.m3, phys::resetRotationOf(spawn));
}

void SimVehicle::reset() {
    m_sim.reset();
    if (m_trailer)
        m_trailer->reset();
    m_controls.reset();
}

bool SimVehicle::settleOnGround(const phys::World& world, const Vec3& from) {
    const Vec3 top{from.x, from.y + 2.0f, from.z};
    const Vec3 bottom{from.x, from.y - 10.0f, from.z};
    phys::RayHit hit;
    if (!world.wheelProbe(top, bottom, hit, nullptr, nullptr))
        return false;
    Vec3 p = hit.position;
    p.y = p.y + 0.9f;
    m_sim.setResetPos(p);
    reset();
    return true;
}

void SimVehicle::respawnAt(const Mat34& at) { respawnAt(at.m3, phys::resetRotationOf(at)); }

void SimVehicle::respawnAt(const Vec3& position, float rotation) {
    const Vec3 savedPos = m_sim.resetPos();
    const float savedRotation = m_sim.resetRotation;
    setResetPos(position, rotation);
    reset();
    // SetResetPos(the saved reset position): CenterOfGravity is added again.
    m_sim.setResetPos(savedPos);
    m_sim.resetRotation = savedRotation;
}

VehiclePose SimVehicle::trailerPose() const {
    VehiclePose pose;
    if (!m_trailer)
        return pose;
    pose.body = m_trailer->modelMatrix();
    for (std::size_t i = 0; i < 4; ++i) {
        pose.wheelWorld[i] = m_trailer->wheels[i].matrix;
        pose.wheelValid[i] = true;
        pose.wheelTurn[i] = m_trailer->wheels[i].rotation;
    }
    pose.hasWheelWorld = true;
    pose.hasWheelTurn = true;
    // vehTrailerInstance::Draw: TLIGHT while the tow car brakes over 0.1.
    pose.brakeLights = m_sim.brakes > 0.1f;
    return pose;
}

void SimVehicle::hold(const phys::PedalInput& in) {
    // vehCar::SetDrivable(0, 1): not drivable, and vehCar::PreUpdate then
    // puts the brake on and the gearbox in neutral every frame. The pedals
    // are not swapped while held (OpenMM2: the automatic reverse cannot
    // engage on the start line).
    m_held = true;
    m_sim.drivable = false;
    m_controls.reset();
    m_sim.trans.setNeutral();
    m_sim.setInputs(in.accelerator, 1.0f, in.steering, in.handbrake);
}

void SimVehicle::drive(const phys::PedalInput& in) {
    if (m_held) {
        // vehCar::SetDrivable(1, ...): drivable again, and
        // vehTransmission::SetForward takes it out of neutral.
        m_held = false;
        m_sim.drivable = true;
        m_sim.trans.setDrive();
    }
    m_controls.apply(m_sim, in);
}

bool SimVehicle::reversing() const { return m_sim.trans.getCurrentGear() < 0; }

VehiclePose SimVehicle::pose() const {
    VehiclePose pose;
    pose.body = m_sim.modelMatrix();
    for (int i = 0; i < 4; ++i) {
        pose.wheelWorld[static_cast<std::size_t>(i)] = m_sim.wheelMatrix(i);
        pose.wheelValid[static_cast<std::size_t>(i)] = m_model.wheel(i) != nullptr;
        pose.wheelTurn[static_cast<std::size_t>(i)] = m_sim.wheels[static_cast<std::size_t>(i)].rotation;
    }
    // vehCarModel::Draw: a WHL4 / WHL5 mesh (a second back axle) is drawn
    // with the WHL2 / WHL3 matrix moved back along the car's Z axis by
    // (0.2 + 2) times that wheel's radius (vehCarModel +0x2c holds the 0.2),
    // not at its own pivot.
    for (int i = 4; i < 6; ++i) {
        if (!m_model.pkg.findBest(std::format("WHL{}", i)))
            continue;
        const int follow = i - 2;
        Mat34 m = m_sim.wheelMatrix(follow);
        const float k = (0.2f + 2.0f) * m_sim.wheels[static_cast<std::size_t>(follow)].radius;
        const Vec3& back = pose.body.m2;
        m.m3 = {m.m3.x + back.x * k, m.m3.y + back.y * k, m.m3.z + back.z * k};
        pose.wheelWorld[static_cast<std::size_t>(i)] = m;
        pose.wheelValid[static_cast<std::size_t>(i)] = true;
        pose.wheelTurn[static_cast<std::size_t>(i)] = m_sim.wheels[static_cast<std::size_t>(follow)].rotation;
    }
    pose.hasWheelWorld = true;
    pose.hasWheelTurn = true;
    // vehCarModel::DrawGlow: brake input not zero; reverse gear.
    pose.brakeLights = m_sim.brakes != 0.0f;
    pose.reverseLights = reversing();
    return pose;
}

} // namespace mm2::game
