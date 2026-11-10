// Bangers on MM2's collision manager: dgBangerData::InitBound / AdjustPrim,
// dgBangerInstance (GetBound, GetEntity, AttachEntity), dgUnhitBangerInstance
// (Impact, Reset), dgHitBangerInstance, dgBangerManager (GetBanger, Reset),
// dgBangerActive (Attach, Detach, DetachMe, Update, PostUpdate),
// dgBangerActiveManager (Attach, Detach, Update, Reset) and
// vehBreakableMgr::Eject, from the code of midtown2.exe build 3393
// (MM2Recomp). See docs/physics.md, "Collision".

#include "game/bangers/BangerSet.h"

#include "core/Log.h"
#include "phys/AgeMath.h"
#include "phys/Sleep.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <tuple>

namespace mm2::game::bangers {
namespace {

// cityLevel::DrawRooms' alpha test: GREATER than 100 (bangers flagged unlit
// use 140, trees 120). The shader keeps alpha >= ref.
constexpr float kAlphaRef = 101.0f / 255.0f;
constexpr float kUnlitAlphaRef = 141.0f / 255.0f;
constexpr float kTreeAlphaRef = 121.0f / 255.0f;

// dgBangerActive's constructor: its phSleep goes to sleep below these
// (speed^2, spin^2) instead of phSleep::Init's 0.005 and 0.01.
constexpr float kSleepSpeed2 = 0.1f;
constexpr float kSleepSpin2 = 0.5f;
// dgBangerActive::Attach: phSleep::SmoothAngInertia(ics, 40).
constexpr float kInertiaRatio = 40.0f;
// dgBangerActive::PostUpdate / dgBangerActiveManager::Update: below this
// height an active is dropped.
constexpr float kLowestY = -100.0f;

// dgBangerActiveManager::Update: the mover each CollisionType bit declares,
// checked in this order.
constexpr int kCollideNone = 0x2;    // updated by the manager, without collisions
constexpr int kCollideAlways = 0x40; // DeclareMover(type 2, 0x1b)
constexpr int kCollideAll = 0x10;    // DeclareMover(type 1, 0x1b)
constexpr int kCollideCity = 0x4;    // DeclareMover(type 1, 3): the city only
// DeclareMover's flags: 0x1 update, 0x2 the city, 0x8 the rooms' instances,
// 0x10 the other movers.
constexpr unsigned kMoverAll = 0x1b;
constexpr unsigned kMoverCity = 0x3;
// The age mode (dgBangerDataManager +0x2a8a8, which mmGame::Init clears)
// declares by the active's age instead: (1, 0x1b) while the age is at most
// the second age, (1, 0x3) while at most the first, else the manager updates
// it without collisions. mmGame::Init sets the ages to 6 and 30000 s (the
// constructor's 3 and 1.5 are replaced), so the first test decides.
constexpr float kAgeFirst = 6.0f;      // dgBangerDataManager +0x2a8a0
constexpr float kAgeSecond = 30000.0f; // dgBangerDataManager +0x2a8a4

// vehBreakableMgr's ejection: momentum speed +- 1, angular impulse 2 +- 1
// (vehBreakableMgr's three ejection settings).
constexpr float kEjectSpeedSpread = 1.0f;
constexpr float kEjectSpin = 2.0f;
constexpr float kEjectSpinSpread = 1.0f;

float reciprocalOrMax(float v) {
    return v <= 0.0f ? FLT_MAX : 1.0f / v;
}

// phSleep::SmoothAngInertia: no principal moment below the largest one over
// `ratio`, then phInertialCS::Init(Mass, ...) for the reciprocals.
void smoothAngInertia(phys::InertialCS& ics, float ratio) {
    float ix = ics.inertia.x, iy = ics.inertia.y, iz = ics.inertia.z;
    float largest;
    if (ix <= iy)
        largest = iy < iz ? iz : iy;
    else
        largest = ix < iz ? iz : ix;
    largest = largest / ratio;
    if (ix < largest)
        ix = largest;
    if (iy < largest)
        iy = largest;
    if (iz < largest)
        iz = largest;
    ics.invMass = reciprocalOrMax(ics.mass);
    ics.inertia = {ix, iy, iz};
    ics.invInertia = {reciprocalOrMax(ix), reciprocalOrMax(iy), reciprocalOrMax(iz)};
}

// phInertialCS::GetInverseInertiaMatrix: R^T diag(1 / I) R in world space.
Mat34 inverseInertiaMatrix(const phys::InertialCS& ics) {
    const Mat34& r = ics.matrix;
    const Vec3& inv = ics.invInertia;
    Mat34 d;
    d.m0 = {r.m0.x * inv.x, r.m0.y * inv.x, r.m0.z * inv.x};
    d.m1 = {r.m1.x * inv.y, r.m1.y * inv.y, r.m1.z * inv.y};
    d.m2 = {r.m2.x * inv.z, r.m2.y * inv.z, r.m2.z * inv.z};
    d.m3 = {};
    return phys::age::dot3x3(phys::age::transpose(r), d);
}

// A row vector times a matrix's 3x3 part, summed z, y, x as
// dgUnhitBangerInstance::Impact does.
Vec3 rowTimes(const Vec3& v, const Mat34& m) {
    return {(v.z * m.m2.x + v.y * m.m1.x) + m.m0.x * v.x, (v.z * m.m2.y + v.y * m.m1.y) + m.m0.y * v.x,
            (v.z * m.m2.z + v.y * m.m1.z) + m.m0.z * v.x};
}

// dgBangerInstance::SetVariant: the variant modulo the geometry's number of
// variants (paint jobs).
int variantOf(const GpuModel& model, int variant) {
    const int count = static_cast<int>(model.paintjobs.size());
    return count > 0 ? variant % count : 0;
}

} // namespace

// dgBangerData's bound and what dgBangerInstance::GetBound derives from it.
struct BangerSet::DataBounds {
    std::unique_ptr<phys::Bound> bound;
    // GetBound(1) of a bound that is not a box: the box around it (MM2
    // alternates two static phBoundBoxes; one per data here).
    std::unique_ptr<phys::BoundBox> box;
    // lvlInstance::GetRadius: the bound's sphere about the instance's origin
    // (MM2 returns the geometry's radius; inferred, as phys::Body).
    float radius = 0.0f;
};

// dgBangerActive: the rigid body simulating a knocked-over prop (its
// collider, phInertialCS and phSleep), the instance it moves, and the
// prop's debris. The physics world drives it as a mover's entity.
// An active's body as the collision manager's mover. Detach is the hit
// instance's (dgHitBangerInstance::Detach), which dgPhysManager::Update
// calls on a type-1 mover outside the active rooms.
struct BangerSet::ActiveBody final : phys::Body {
    Active* owner = nullptr;
    void detach() override;
    // OpenMM2: a replayed car meets the prop this piece broke off as it
    // stood (a replay ghost), not the piece.
    bool acceptsContact(const phys::Instance& other) const override;
};

// Its body's phInertialCS is dgBangerActive::GetICS.
struct BangerSet::Active final : phys::BodyController {
    BangerSet* set = nullptr;
    int index = 0;
    ActiveBody body;
    phys::Sleep sleep;
    int instance = -1;
    float age = 0.0f;
    fx::ParticleSystem particles;
    fx::BirthRule rule;
    Mat34 particleMatrix;
    int texNumber = 0;

    // dgBangerActive::Update, as one sample of the world: dgPhysEntity::
    // Update (gravity, which the world's integration adds) and phSleep::
    // Update before the integration ...
    void beforeIntegrate(phys::Body&, float, const phys::World&) override {
        sleep.update(phys::sampleTime().invSeconds);
    }
    // ... then the instance follows the body (SetMatrix) and ages.
    void afterIntegrate(phys::Body& b, float dt, const phys::World&) override {
        if (instance >= 0)
            set->m_instances[static_cast<std::size_t>(instance)].matrix = b.ics.matrix;
        age = dt + age;
    }
    // lvlLevel::MoveToRoom of the instance: the world found the body's room
    // after the integration.
    void afterCollisions(phys::Body& b, float, const phys::World&) override {
        if (instance >= 0 && set->m_instances[static_cast<std::size_t>(instance)].room != b.room)
            set->moveToRoom(static_cast<std::size_t>(instance), b.room);
    }
};

// The lvlInstance face of a placed prop (dgUnhitBangerInstance, flags 0x13)
// or of a hit instance (dgHitBangerInstance, flags 0x12).
class BangerSet::Prop final : public phys::Instance {
public:
    Prop(BangerSet& set, std::size_t index) : m_set(set), m_index(index) {}

    // dgBangerInstance::GetBound. An instance that left the rooms has none:
    // dgPhysManager::NewMover takes a prop that broke loose off the lists
    // the movers gathered this sample; OpenMM2's world keeps its lists, so
    // the prop stops colliding this way instead.
    const phys::Bound* bound(int which) const override {
        const BangerSet::Instance& inst = m_set.m_instances[m_index];
        if ((inst.state == State::Gone && !m_set.replayGhost(m_index)) || !inst.data)
            return nullptr;
        const DataBounds& b = m_set.boundsOf(*inst.data);
        if (which == 1 && b.box)
            return b.box.get();
        return b.bound.get();
    }
    // dgUnhitYBangerInstance::GetMatrix (rebuilt from the Y form),
    // dgUnhitMtxBangerInstance::GetMatrix, dgHitBangerInstance::GetMatrix:
    // the frame at the CG, which their SetMatrix store (GetPosition: its
    // last row).
    const Mat34& matrix() const override { return m_set.m_instances[m_index].matrix; }
    float radius() const override {
        const BangerSet::Instance& inst = m_set.m_instances[m_index];
        return inst.data ? m_set.boundsOf(*inst.data).radius : 0.0f;
    }
    // dgBangerInstance::GetEntity: the active's body.
    phys::Body* entity() override {
        Active* a = m_set.activeOf(m_index);
        return a ? &a->body : nullptr;
    }
    phys::Body* attachEntity() override { return m_set.attachEntity(m_index); }

    bool isBanger() const override { return banger || m_set.replayGhost(m_index); }
    float bangerImpulseLimit2() const override {
        const BangerSet::Instance& inst = m_set.m_instances[m_index];
        return inst.data ? inst.data->impulseLimit2 : 0.0f;
    }
    // dgPhysManager::TrivialCollideInstances: a prop with a YRadius touches
    // within that radius of the ground point under its CG.
    bool bangerSphere(Vec3& centre, float& r) const override {
        const BangerSet::Instance& inst = m_set.m_instances[m_index];
        const BangerData* d = inst.data;
        if (!d || d->yRadius == 0.0f)
            return false;
        const Mat34& m = inst.matrix;
        const Vec3& cg = d->cg;
        centre = {m.m3.x - (m.m0.x * cg.x + cg.y * m.m1.x + cg.z * m.m2.x),
                  m.m3.y - (m.m0.y * cg.x + cg.y * m.m1.y + cg.z * m.m2.y),
                  m.m3.z - (m.m0.z * cg.x + cg.y * m.m1.z + cg.z * m.m2.z)};
        r = d->yRadius;
        return true;
    }
    void bangerHit(phys::Instance& by, const Vec3& /*position*/) override { m_set.unhitImpact(m_index, &by); }
    // The world's call on a prop's entity that held: dgBangerActive::DetachMe.
    void bangerHeld() override {
        if (Active* a = m_set.activeOf(m_index))
            m_set.detachMe(*a);
    }
    // OpenMM2: a network client's props take contacts from its own car and
    // its own props only.
    bool acceptsContact(const phys::Instance& other) const override {
        return !m_set.replayGhostPiece(m_index) && m_set.acceptsFrom(m_index, other);
    }
    // OpenMM2: the active a car's hit would attach (World::replaySample).
    bool heldInertia(phys::InertialCS& out) const override { return m_set.heldInertia(m_index, out); }

    bool banger = false; // lvlInstance flag 1: a placed prop still standing
    bool listed = false; // in m_rooms[room]

private:
    BangerSet& m_set;
    std::size_t m_index;
};

// dgBangerManager::dgBangerManager (an empty ring) and the 32 actives of
// dgBangerActiveManager.
BangerSet::BangerSet(const BangerDataLibrary& data) : m_data(data) {
    for (int i = 0; i < kMaxActive; ++i) {
        auto a = std::make_unique<Active>();
        a->set = this;
        a->index = i;
        a->body.controller = a.get();
        a->body.owner = a.get();
        // dgBangerActive's constructor: phSleep::Init, then its thresholds.
        a->sleep.init(&a->body.ics);
        a->sleep.speed2 = kSleepSpeed2;
        a->sleep.spin2 = kSleepSpin2;
        a->particles.init(64, 2, 2); // dgBangerActive: asParticles::Init(64, 2, 2)
        a->particles.rng().seed(0xBA9u + static_cast<std::uint32_t>(i));
        m_active.push_back(std::move(a));
        m_list[static_cast<std::size_t>(i)] = i;
    }
}

BangerSet::~BangerSet() { setWorld(nullptr); }

std::size_t BangerSet::newInstance() {
    m_instances.emplace_back();
    const std::size_t i = m_instances.size() - 1;
    m_props.push_back(std::make_unique<Prop>(*this, i));
    return i;
}

phys::Instance& BangerSet::prop(std::size_t i) { return *m_props[i]; }

const phys::Body* BangerSet::body(std::size_t i) const {
    const int a = m_instances[i].active;
    return a >= 0 ? &m_active[static_cast<std::size_t>(a)]->body : nullptr;
}

const phys::Bound* BangerSet::bound(const BangerData& data) const { return boundsOf(data).bound.get(); }

const phys::Bound* BangerSet::boundOf(const BangerData& data, int which) const {
    const DataBounds& b = boundsOf(data);
    if (which == 1 && b.box)
        return b.box.get();
    return b.bound.get();
}

float BangerSet::boundRadius(const BangerData& data) const { return boundsOf(data).radius; }

int BangerSet::hitCount() const {
    return static_cast<int>(std::ranges::count_if(m_instances, [](const Instance& i) { return i.state == State::Hit; }));
}

// --- Bounds ----------------------------------------------------------------------------------

const BangerSet::DataBounds& BangerSet::boundsOf(const BangerData& d) const {
    if (auto it = m_bounds.find(&d); it != m_bounds.end())
        return *it->second;
    auto b = std::make_unique<DataBounds>();
    // dgBangerData::InitBound: every dg* bound carries its own material.
    switch (d.collisionPrim) {
    case 0:
        // phBoundGeometry::Load of "<name>_bound", moved so that the CG is
        // its origin (ShiftCentroid by -CG); a box of Size without it.
        if (auto file = loadBoundFile(m_data.vfs(), d.name)) {
            if (auto geometry = phys::makeGeometryBound(toGeometryData(*file))) {
                geometry->makeOwnMaterial();
                geometry->shiftCentroid({-d.cg.x, -d.cg.y, -d.cg.z});
                b->bound = std::move(geometry);
                break;
            }
        }
        [[fallthrough]];
    case 1: {
        auto box = std::make_unique<phys::BoundBox>(); // dgBoundBox: centred on the CG
        box->makeOwnMaterial();
        box->setSize(d.size);
        b->bound = std::move(box);
        break;
    }
    case 2: {
        auto hotdog = std::make_unique<phys::BoundHotdog>();
        hotdog->makeOwnMaterial();
        hotdog->setSize(d.yRadius, d.size.y);
        b->bound = std::move(hotdog);
        break;
    }
    case 3: {
        auto sphere = std::make_unique<phys::BoundSphere>();
        sphere->makeOwnMaterial();
        sphere->setRadius(d.yRadius);
        b->bound = std::move(sphere);
        break;
    }
    default: {
        // OpenMM2: MM2 leaves the data without a bound (and AdjustPrim then
        // fails); no retail data does this. A box of Size instead.
        auto box = std::make_unique<phys::BoundBox>();
        box->makeOwnMaterial();
        box->setSize(d.size);
        b->bound = std::move(box);
        break;
    }
    }
    // dgBangerData::AdjustPrim.
    b->bound->setElasticity(d.elasticity);
    b->bound->setFriction(d.friction);

    // dgBangerInstance::GetBound(1): the box around a bound that is not a
    // box (a plain phBoundBox: the default material).
    const phys::Bound& made = *b->bound;
    if (made.type != phys::BoundType::Box) {
        const Vec3& lo = made.boxMin;
        const Vec3& hi = made.boxMax;
        const Vec3 size{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        const Vec3 centre{size.x * 0.5f + lo.x, size.y * 0.5f + lo.y, size.z * 0.5f + lo.z};
        auto box = std::make_unique<phys::BoundBox>();
        box->setOffset(centre);
        box->setSize(size);
        b->box = std::move(box);
    }
    const Vec3& c = made.centroid;
    b->radius = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z) + made.radius;
    const DataBounds& result = *b;
    m_bounds.emplace(&d, std::move(b));
    return result;
}

// --- Rooms -----------------------------------------------------------------------------------

bool BangerSet::roomsTracked() const { return m_world && m_world->level(); }

int BangerSet::findRoom(const Vec3& position, int hint) const {
    return roomsTracked() ? m_world->level()->findRoom(position, hint) : 0;
}

void BangerSet::moveToRoom(std::size_t i, int room) {
    // lvlLevel::MoveToRoom: off its room's list and onto the head of the new
    // room's (room 0: none), so a room lists its props newest first. (Only
    // the city's static instances, lvlInstance flag 0x400, go after the
    // room's last static one instead; no banger has that flag.)
    Prop& p = *m_props[i];
    if (p.listed) {
        std::erase(m_rooms[static_cast<std::size_t>(p.room)], &p);
        p.listed = false;
    }
    p.room = room;
    m_instances[i].room = room;
    if (room > 0) {
        if (m_rooms.size() <= static_cast<std::size_t>(room))
            m_rooms.resize(static_cast<std::size_t>(room) + 1);
        auto& list = m_rooms[static_cast<std::size_t>(room)];
        list.insert(list.begin(), &p);
        p.listed = true;
    }
}

void BangerSet::placeUnroomed() {
    // Props placed without a room (path sets) go in the room containing
    // their placement point (cityLevel::LoadPath: the level's FindRoomId of
    // the path matrix's position, before the CG offset) once the level is
    // known.
    if (!roomsTracked())
        return;
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const Instance& inst = m_instances[i];
        if (!inst.everHit && inst.state == State::Unhit && inst.room == 0 && !m_props[i]->listed)
            moveToRoom(i, findRoom(inst.ground.m3, inst.roomHint));
    }
}

void BangerSet::instancesIn(int room, std::vector<phys::Instance*>& out) const {
    if (room <= 0 || static_cast<std::size_t>(room) >= m_rooms.size())
        return;
    for (Prop* p : m_rooms[static_cast<std::size_t>(room)])
        if (p->collidable)
            out.push_back(p);
    // OpenMM2: in a network client's replay, the props its car broke loose
    // lately, where they stood.
    if (m_world && m_world->replaying())
        for (const Ghost& g : m_ghosts)
            if (ghostStands(g) && m_instances[g.prop].room == room &&
                m_instances[g.prop].state == State::Gone)
                out.push_back(m_props[g.prop].get());
}

// --- Setup -----------------------------------------------------------------------------------

void BangerSet::add(const std::vector<PlacedProp>& props) {
    for (const auto& p : props)
        addOne(p);
    placeUnroomed();
}

std::optional<std::size_t> BangerSet::addOne(const PlacedProp& p) {
    const BangerData* d = m_data.find(p.model);
    if (!d) {
        ++m_skipped;
        return std::nullopt;
    }
    // dgBangerDataManager::AddBangerDataEntry: the data's bound, and its
    // parts' (dgUnhitBangerInstance::InitBreakables).
    boundsOf(*d);
    for (int k = 0; k < d->numParts; ++k)
        if (const BangerData* part = m_data.part(p.model, k))
            boundsOf(*part);
    const std::size_t i = newInstance();
    Instance& inst = m_instances[i];
    inst.data = d;
    inst.model = p.model;
    inst.ground = p.transform;
    if (!p.fullMatrix) {
        // dgUnhitYBangerInstance keeps only the X row's x and z: rows
        // (c, 0, s), (0, 1, 0), (-s, 0, c), not renormalised.
        const float c = p.transform.m0.x, s = p.transform.m0.z;
        inst.ground.m0 = {c, 0.0f, s};
        inst.ground.m1 = {0.0f, 1.0f, 0.0f};
        inst.ground.m2 = {-s, 0.0f, c};
    }
    // The banger's frame sits at its CG (dgUnhitBangerInstance::Init: the
    // CG offset turned by the matrix as placed, before SetMatrix keeps
    // only the Y rotation), with the instance data's variant
    // (SetVariant).
    inst.matrix = inst.ground;
    inst.matrix.m3 = p.transform.transform(d->cg);
    inst.paint = p.variant;
    inst.roomHint = p.roomHint;
    inst.ownerDrawn = p.ownerDrawn;
    inst.state = State::Unhit;
    Prop& prop = *m_props[i];
    prop.banger = true;
    prop.collidable = true;
    prop.audioId = d->colliderId;
    moveToRoom(i, p.room);
    return i;
}

void BangerSet::setWorld(phys::World* world) {
    if (m_world)
        for (auto& a : m_active)
            if (m_world->contains(&a->body))
                m_world->remove(&a->body);
    m_world = world;
    if (!m_world)
        return;
    placeUnroomed();
    for (int n = 0; n < m_attached; ++n) {
        Active& a = *m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(n)])];
        const int type = m_instances[static_cast<std::size_t>(a.instance)].data->collisionType;
        if (!(type & kCollideNone) && (type & (kCollideAlways | kCollideAll | kCollideCity)))
            m_world->add(&a.body);
    }
}

void BangerSet::reset() {
    // lvlLevel::ResetInstances: every instance's own Reset first, then
    // dgBangerManager::Reset, then dgBangerActiveManager::Reset.
    // dgUnhitBangerInstance::Reset: every placed prop back in its room,
    // standing.
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        Instance& inst = m_instances[i];
        if (inst.everHit)
            continue;
        Prop& prop = *m_props[i];
        const int room = inst.room;
        if (room > 0) {
            if (prop.banger)
                moveToRoom(i, 0);
            moveToRoom(i, room);
        }
        prop.banger = true;
        prop.collidable = true;
        inst.state = State::Unhit;
    }
    // dgBangerManager::Reset: the hit instances leave their rooms.
    m_ringNext = 0;
    for (const std::size_t i : m_ring) {
        if (m_instances[i].room != 0 || m_props[i]->listed)
            moveToRoom(i, 0);
        m_instances[i].state = State::Gone;
    }
    // dgBangerActiveManager::Reset: every active detached, the pool back in
    // its original order.
    while (m_attached > 0) {
        --m_attached;
        activeDetach(*m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(m_attached)])]);
    }
    for (int k = 0; k < kMaxActive; ++k)
        m_list[static_cast<std::size_t>(k)] = k;
    syncActiveList();
    m_ticker.reset();
}

// --- The ring of hit instances ---------------------------------------------------------------

std::size_t BangerSet::getBanger() {
    // dgBangerManager::GetBanger: the next hit instance of the ring. When
    // the index reaches the end it restarts at 0 without counting that use,
    // so after each wrap slot 0 is handed out twice in a row (MM2's
    // behaviour, kept).
    std::size_t slot;
    if (m_ringNext == m_ringSize && m_ringSize < m_ringLimit && ringSlotBusy(0)) {
        // OpenMM2 (network games): the oldest prop is still busy: the ring
        // grows rather than wrapping onto it (setRingGrowth).
        m_ringSize = std::min(m_ringSize + kMaxHit, m_ringLimit);
        slot = static_cast<std::size_t>(m_ringNext++);
    } else if (m_ringNext == m_ringSize) {
        slot = 0;
        m_ringNext = 0;
    } else {
        slot = static_cast<std::size_t>(m_ringNext++);
    }
    // dgBangerManager::Init makes all 40 up front
    // (dgHitBangerInstance::dgHitBangerInstance: flags 0x12, identity, and
    // dgHitBangerInstance::SetMatrix stores the whole matrix); OpenMM2
    // makes each when the ring first reaches it.
    if (slot >= m_ring.size()) {
        const std::size_t i = newInstance();
        m_instances[i].everHit = true;
        m_instances[i].state = State::Gone;
        m_ring.push_back(i);
        m_ringGeneration.push_back(0);
        m_ringHandedAt.push_back(0.0);
    }
    ++m_ringGeneration[slot]; // OpenMM2: a new prop in the slot (network games)
    m_instances[m_ring[slot]].driven = false;
    m_ringHandedAt[slot] = m_world ? m_world->time() : 0.0;
    const std::size_t i = m_ring[slot];
    // Its previous prop disappears.
    if (Active* a = activeOf(i))
        detachMe(*a);
    if (m_instances[i].room != 0 || m_props[i]->listed)
        moveToRoom(i, 0);
    m_instances[i].state = State::Gone;
    return i;
}

void BangerSet::setRingGrowth(int limit, double busySeconds) {
    m_ringLimit = std::max(limit, m_ringSize);
    m_ringBusySeconds = busySeconds;
}

bool BangerSet::ringSlotBusy(std::size_t slot) const {
    if (slot >= m_ring.size())
        return false;
    const std::size_t i = m_ring[slot];
    if (m_instances[i].state == State::Gone)
        return false;
    if (m_instances[i].active >= 0)
        return true; // still moving
    return m_world && m_world->time() - m_ringHandedAt[slot] < m_ringBusySeconds;
}

// --- Actives ---------------------------------------------------------------------------------

BangerSet::Active* BangerSet::activeOf(std::size_t i) {
    // dgBangerActiveManager::GetActive.
    const int a = m_instances[i].active;
    return a >= 0 ? m_active[static_cast<std::size_t>(a)].get() : nullptr;
}

bool BangerSet::inWorld(const Active& a) const { return m_world && m_world->contains(&a.body); }

phys::Body* BangerSet::attachEntity(std::size_t i) {
    // dgBangerInstance::AttachEntity.
    Active* a = activeOf(i);
    const bool attached = a == nullptr;
    if (!a)
        a = managerAttach(i);
    if (!a) {
        log::error("bangers: AttachEntity failed for {}", m_instances[i].model);
        return nullptr;
    }
    // OpenMM2: a network client's mirror of a host's piece that its car
    // touched goes on from the host's motion (dgBangerActive::Attach starts a
    // prop at rest: a placed or resting prop is).
    if (const Instance& inst = m_instances[i]; attached && inst.mirror) {
        phys::InertialCS& ics = a->body.ics;
        ics.linearVelocity = inst.mirrorVelocity;
        const float m = ics.mass;
        const Vec3& v = inst.mirrorVelocity;
        ics.linearMomentum = {m * v.x, m * v.y, m * v.z};
        ics.angularVelocity = inst.mirrorSpin;
        ics.angularMomentum = rowTimes(inst.mirrorSpin, ics.worldInertia());
    }
    return &a->body;
}

BangerSet::Active* BangerSet::managerAttach(std::size_t i) {
    // dgBangerActiveManager::Attach: the next free active; with all 32 in
    // use, the first of the list is detached and reused.
    for (int n = 0; n < m_attached; ++n) {
        const Active& attached = *m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(n)])];
        if (attached.instance == static_cast<int>(i)) {
            log::error("bangers: {} already attached", m_instances[i].model);
            return nullptr;
        }
    }
    Active* a;
    if (m_attached < kMaxActive) {
        a = m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(m_attached)])].get();
        activeAttach(*a, i);
        ++m_attached;
    } else {
        a = m_active[static_cast<std::size_t>(m_list[0])].get();
        activeDetach(*a);
        activeAttach(*a, i);
    }
    syncActiveList();
    return a;
}

void BangerSet::managerDetach(const Active& a) {
    // dgBangerActiveManager::Detach: swapped with the last attached one.
    for (int n = 0; n < m_attached; ++n) {
        if (m_list[static_cast<std::size_t>(n)] != a.index)
            continue;
        --m_attached;
        m_list[static_cast<std::size_t>(n)] = m_list[static_cast<std::size_t>(m_attached)];
        m_list[static_cast<std::size_t>(m_attached)] = a.index;
        syncActiveList();
        return;
    }
}

void BangerSet::activeAttach(Active& a, std::size_t i) {
    // dgBangerActive::Attach.
    Instance& inst = m_instances[i];
    Prop& prop = *m_props[i];
    const BangerData& d = *inst.data;
    const bool standing = prop.banger; // lvlInstance flag 1, which the debris reads
    prop.collidable = false;
    inst.active = a.index;
    inst.state = State::Active;
    inst.drawn.reset(); // OpenMM2: a client's mirror is drawn from the body now
    a.instance = static_cast<int>(i);

    // The collider: the data's bound at the instance's matrix, with the
    // prop's collider id (the instance data's). New movers collide with
    // everything until the manager declares them (dgPhysManager::NewMover).
    const DataBounds& b = boundsOf(d);
    phys::Body& sim = a.body;
    sim.collisionBound = b.bound.get();
    sim.terrainBound = b.box.get();
    sim.boundOrigin = {};
    sim.audioId = d.colliderId;
    sim.kinematic = false;
    sim.collideTerrain = sim.collideInstances = sim.collideMovers = true;
    sim.room = inst.room;
    // phInertialCS::Zero, the instance's matrix, phCollider::Init + Reset.
    sim.place(inst.matrix);
    phys::InertialCS& ics = sim.ics;
    ics.setMass(d.size.x, d.size.y, d.size.z, d.mass); // InitBoxMass
    // dgBangerInstance::GetVelocity is the active's velocity, which Zero just
    // cleared: a prop always starts at rest (momentum Mass * 0).
    const Vec3 v = ics.linearVelocity;
    const float m = ics.mass;
    ics.linearMomentum = {m * v.x, m * v.y, m * v.z};
    smoothAngInertia(ics, kInertiaRatio);
    ics.gravity = {0.0f, -phys::kGravity, 0.0f};
    // MM2's phInertialCS has no sleep test of its own (phSleep does it).
    ics.state = phys::InertialCS::Off;
    a.sleep.wakeUp();
    a.age = 0.0f;

    // Debris (dgBangerActive::Attach): InitialBlast particles of the prop's
    // rule from fxpt<TexNumber>, born in the prop's frame; stationary rules
    // are born at the prop's CG in world space, and only from a prop that
    // was still standing.
    a.particles.reset(); // asParticles::Reset: no particles, no birth matrix
    const int sheet = fx::EffectLibrary::bangerSheetNumber(d.texNumber);
    if (sheet == 0) {
        // MM2 stops here: the active keeps the rule and the sheet it last
        // had (asParticles' rule and texture), so a reused active whose old
        // rule spews (SpewRate, e.g. a mailbox's letters) goes on spewing
        // from that rule's own Position with no birth matrix, around the
        // world origin, for its SpewTimeLimit. Kept.
        return;
    }
    a.texNumber = sheet; // asParticles::SetTexture
    // Every MM2 dgBangerData has a rule (the asBirthRule defaults without one).
    const fx::BirthRule& rule = d.birthRule ? *d.birthRule : fx::BirthRule{};
    if (rule.birthFlags & fx::BirthRule::kStationary) {
        // The prop's CG in world space. MM2 writes it into the data's shared
        // rule; OpenMM2 into the active's copy (no retail rule is stationary).
        // A prop no longer standing gets no rule and no blast: the old rule
        // stays, as above.
        if (!standing)
            return;
        a.rule = rule;
        a.rule.position = inst.matrix.m3;
    } else {
        a.rule = rule;
        a.particleMatrix = inst.matrix;
        a.particles.setMatrix(&a.particleMatrix);
    }
    a.particles.setBirthRule(&a.rule);
    a.particles.blast(a.rule.initialBlast);
}

void BangerSet::activeDetach(Active& a) {
    // dgBangerActive::Detach: no longer a mover (dgPhysManager::IgnoreMover),
    // the instance collidable again where it stopped; its particles go.
    if (a.instance < 0)
        return;
    if (inWorld(a))
        m_world->remove(&a.body);
    a.particles.reset();
    const auto i = static_cast<std::size_t>(a.instance);
    Instance& inst = m_instances[i];
    Prop& prop = *m_props[i];
    prop.collidable = true;
    inst.active = -1;
    if (inst.state == State::Active)
        inst.state = prop.banger ? State::Unhit : State::Hit;
    a.instance = -1;
}

void BangerSet::detachMe(Active& a) {
    // dgBangerActive::DetachMe.
    activeDetach(a);
    managerDetach(a);
}

void BangerSet::ActiveBody::detach() { owner->set->worldDetach(*owner); }

bool BangerSet::ActiveBody::acceptsContact(const phys::Instance&) const {
    return owner->instance < 0 || !owner->set->replayGhostPiece(static_cast<std::size_t>(owner->instance));
}

void BangerSet::worldDetach(Active& a) {
    // dgPhysManager::Update detaches a type-1 mover outside the active rooms
    // through its instance (lvlInstance vtable +0x28).
    if (a.instance >= 0)
        detachHit(static_cast<std::size_t>(a.instance));
}

void BangerSet::detachHit(std::size_t i) {
    // dgHitBangerInstance::Detach: the instance's entity detaches
    // (dgBangerActive::DetachMe) and the instance leaves its room, so the
    // knocked-over prop disappears. A placed prop still standing
    // (dgUnhitBangerInstance) keeps lvlInstance::Detach, which does nothing.
    if (i >= m_instances.size() || !m_instances[i].everHit)
        return;
    if (Active* a = activeOf(i))
        detachMe(*a);
    if (m_instances[i].room != 0 || m_props[i]->listed)
        moveToRoom(i, 0);
    m_instances[i].state = State::Gone;
}

bool BangerSet::standing(std::size_t i) const { return i < m_props.size() && m_props[i]->banger; }

std::optional<BangerSet::Debris> BangerSet::debris(std::size_t i) const {
    if (i >= m_instances.size() || m_instances[i].active < 0)
        return std::nullopt;
    const Active& a = *m_active[static_cast<std::size_t>(m_instances[i].active)];
    return Debris{&a.particles, a.texNumber};
}

void BangerSet::newMover(Active& a) {
    // dgPhysManager::NewMover: the active's body joins the movers and
    // collides with everything from the next sample (0x100, then 0x1b). It
    // refuses an instance in no room.
    if (a.instance < 0)
        return;
    const Instance& inst = m_instances[static_cast<std::size_t>(a.instance)];
    if (roomsTracked() && inst.room == 0) {
        log::error("bangers: NewMover: {} is not in a room", inst.model);
        return;
    }
    a.body.collideTerrain = a.body.collideInstances = a.body.collideMovers = true;
    if (m_world)
        m_world->addNewMover(&a.body);
}

void BangerSet::syncActiveList() {
    m_activeList.assign(m_list.begin(), m_list.begin() + m_attached);
}

// --- Breaking loose --------------------------------------------------------------------------

void BangerSet::unhitImpact(std::size_t i, const phys::Instance* by) {
    // dgUnhitBangerInstance::Impact: called by the world once dgImpact's
    // impulses broke the prop loose; its active holds them.
    const Mat34 frame = m_instances[i].matrix;
    Active* parent = activeOf(i);
    const BangerData* data = m_instances[i].data;
    if (!parent || !data)
        return; // OpenMM2 guard: AttachEntity always gave it an active
    const BangerData& d = *data;
    Prop& unhitProp = *m_props[i];
    if (m_recordKnocks && !m_instances[i].everHit)
        m_knocks.push_back({i, by}); // OpenMM2: network games (game/net/PropSync)
    // OpenMM2: a network client's own car broke it loose (or a host's piece
    // it simulates from the host's states): its replays meet it where it
    // stood for a while (replayGhost).
    if (m_replica && m_world && !m_instances[i].everHit && by &&
        ((m_localToucher && m_localToucher(*by)) || drivenBody(*by)))
        m_ghosts.push_back({i, m_world->time()});

    if (d.numParts == 0) {
        // A hit instance from the ring takes the prop's place in its room
        // (dgUnhitBangerInstance::ImpactCB, for glass, does nothing here).
        const std::size_t h = getBanger();
        moveToRoom(h, m_instances[i].room);
        // The prop leaves the room lists but remembers its room, and is no
        // longer a banger (flag 1).
        const int room = m_instances[i].room;
        moveToRoom(i, 0);
        unhitProp.banger = false;
        unhitProp.room = room;
        Instance& unhit = m_instances[i];
        unhit.room = room;
        unhit.state = State::Gone;

        Instance& hit = m_instances[h];
        hit.data = unhit.data;
        hit.model = unhit.model;
        hit.part = unhit.part;
        hit.mesh = unhit.mesh;
        hit.paint = unhit.paint;
        hit.ground = unhit.ground;
        hit.everHit = true;
        hit.source = unhit.everHit ? unhit.source : static_cast<int>(i);
        hit.tag = unhit.tag;
        Prop& hitProp = *m_props[h];
        hitProp.collidable = false;
        hitProp.audioId = d.colliderId;
        // The active moves to the hit instance with its body, motion and
        // pending impulses (phCollider::Init with the hit instance's bound,
        // the collider id, phCollider::Reset).
        parent->instance = static_cast<int>(h);
        hit.active = parent->index;
        hit.state = State::Active;
        const DataBounds& b = boundsOf(d);
        parent->body.collisionBound = b.bound.get();
        parent->body.terrainBound = b.box.get();
        parent->body.audioId = d.colliderId;
        parent->body.resetCollider();
        hit.matrix = frame; // SetMatrix
        unhit.active = -1;
        newMover(*parent);
        unhitProp.collidable = true;
        return;
    }

    // With parts: the prop leaves the room lists and each BREAKnn part
    // becomes a hit instance with its own active, given the prop's change
    // of motion at its own CG.
    const int room = m_instances[i].room;
    moveToRoom(i, 0);
    unhitProp.banger = false;
    unhitProp.room = room;
    m_instances[i].room = room;
    m_instances[i].state = State::Gone;
    const std::string model = m_instances[i].model;
    const int paint = m_instances[i].paint;
    const Mat34 m = m_instances[i].matrix;
    // The ground point under the CG.
    Vec3 ground{m.m3.x - ((m.m2.x * d.cg.z + m.m1.x * d.cg.y) + m.m0.x * d.cg.x),
                m.m3.y - ((m.m2.y * d.cg.z + m.m1.y * d.cg.y) + m.m0.y * d.cg.x),
                m.m3.z - ((m.m2.z * d.cg.z + m.m1.z * d.cg.y) + m.m0.z * d.cg.x)};
    // The prop's velocity change from the impulses dgImpact gave it.
    const phys::InertialCS& pics = parent->body.ics;
    const float invMass = pics.invMass;
    const Vec3& impulse = pics.linearImpulse;
    const Vec3 dv{impulse.x * invMass, impulse.y * invMass, impulse.z * invMass};
    const Vec3 dw = rowTimes(pics.angularImpulse, inverseInertiaMatrix(pics));

    for (int p = 0; p < d.numParts; ++p) {
        const std::size_t h = getBanger();
        moveToRoom(h, room);
        Instance& hit = m_instances[h];
        // dgBangerDataManager's entry after the prop's: "<model>_BREAKnn".
        hit.data = m_data.part(model, p);
        hit.model = model;
        hit.part = p;
        hit.mesh.clear();
        hit.paint = paint;
        hit.everHit = true;
        hit.source = m_instances[i].everHit ? m_instances[i].source : static_cast<int>(i);
        hit.tag = 0;
        Prop& hitProp = *m_props[h];
        hitProp.collidable = false;
        if (!hit.data) {
            // OpenMM2: MM2 requires every part's data (InitBreakables fails
            // without it); retail data has them all.
            moveToRoom(h, 0);
            continue;
        }
        hitProp.audioId = hit.data->colliderId;
        // The part's frame: the prop's rotation, at the ground point plus
        // the part's CG.
        const Vec3& cg = hit.data->cg;
        const Vec3 c{(m.m2.x * cg.z + m.m1.x * cg.y) + m.m0.x * cg.x,
                     (m.m0.y * cg.x + m.m2.y * cg.z) + m.m1.y * cg.y,
                     (m.m0.z * cg.x + m.m2.z * cg.z) + m.m1.z * cg.y};
        hit.ground = m;
        hit.ground.m3 = ground;
        ground = {ground.x + c.x, ground.y + c.y, ground.z + c.z};
        hit.matrix = m;
        hit.matrix.m3 = ground; // SetMatrix
        ground = {ground.x - c.x, ground.y - c.y, ground.z - c.z};

        if (!attachEntity(h))
            continue;
        Active& part = *activeOf(h);
        phys::InertialCS& ics = part.body.ics;
        // The prop's velocity at the part's CG, as an impulse on the part
        // (the prop's active is read as it now stands: with all 32 actives
        // busy, attaching the part took it over).
        const Vec3& from = parent->body.ics.matrix.m3;
        const Vec3& to = ics.matrix.m3;
        Vec3 j = Vec3{from.x - to.x, from.y - to.y, from.z - to.z}.cross(dw);
        j = {j.x + dv.x, j.y + dv.y, j.z + dv.z};
        const float mass = ics.mass;
        j = {j.x * mass, j.y * mass, j.z * mass};
        ics.linearImpulse = {j.x + ics.linearImpulse.x, j.y + ics.linearImpulse.y, j.z + ics.linearImpulse.z};
        // And the prop's spin.
        const Vec3 l = rowTimes(dw, ics.worldInertia());
        const Vec3& spin = ics.angularImpulse;
        ics.angularImpulse = {l.x + spin.x, l.y + spin.y, l.z + spin.z};
        newMover(part);
    }
    // The prop's active goes back to the pool (vtable 0x1c: DetachMe).
    detachMe(*parent);
}

// --- Per frame -------------------------------------------------------------------------------

void BangerSet::directUpdate(Active& a, float dt) {
    // dgBangerActive::Update called by the manager once per frame, outside
    // the physics world (no collisions): sleep, integration, the instance
    // follows, its room, its age.
    const float invDt = 1.0f / dt;
    a.sleep.update(invDt);
    a.body.ics.update(dt, invDt);
    a.body.syncBoundMatrix();
    const auto i = static_cast<std::size_t>(a.instance);
    m_instances[i].matrix = a.body.ics.matrix;
    if (roomsTracked()) {
        const int room = findRoom(a.body.ics.matrix.m3, m_instances[i].room);
        if (room != m_instances[i].room)
            moveToRoom(i, room);
        a.body.room = room;
    }
    a.age = dt + a.age;
}

void BangerSet::declare(Active& a, float dt) {
    // dgBangerActiveManager::Update: each active declared to the physics
    // manager by its data's CollisionType.
    if (a.instance < 0)
        return;
    int type = m_instances[static_cast<std::size_t>(a.instance)].data->collisionType;
    phys::Body& sim = a.body;
    if (m_ageMode) {
        // By age: all, then the city only, then the manager's own update.
        type = a.age <= kAgeFirst ? (a.age <= kAgeSecond ? kCollideAll : kCollideCity) : kCollideNone;
    }
    if (type & kCollideNone) {
        // Updated here, without collisions, then dgBangerActive::PostUpdate.
        if (inWorld(a))
            m_world->remove(&sim);
        if (dt > 0.0f)
            directUpdate(a, dt);
        if (a.sleep.state == phys::Sleep::Asleep || sim.ics.matrix.m3.y < kLowestY)
            detachMe(a);
        return;
    }
    bool mover = true;
    if (type & kCollideAlways) {
        sim.declare(2, kMoverAll);
    } else if (type & kCollideAll) {
        sim.declare(1, kMoverAll);
    } else if (type & kCollideCity) {
        sim.declare(1, kMoverCity);
    } else {
        // Not declared: neither simulated nor collided with.
        mover = false;
    }
    if (!m_world)
        return;
    if (mover && !inWorld(a))
        m_world->add(&sim);
    else if (!mover && inWorld(a))
        m_world->remove(&sim);
}

void BangerSet::update(float dt) {
    // OpenMM2: a client's replays meet a prop its car broke loose for this
    // long (as long as the host's correction of the car may reach back).
    if (m_world)
        std::erase_if(m_ghosts,
                      [now = m_world->time()](const Ghost& g) { return now - g.time > kGhostSeconds; });
    // dgBangerActive::PostUpdate, which the physics manager calls on its
    // movers after the frame's samples: an active that fell asleep or below
    // the city detaches.
    std::vector<Active*> movers;
    for (int n = 0; n < m_attached; ++n) {
        Active* a = m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(n)])].get();
        if (inWorld(*a))
            movers.push_back(a);
    }
    for (Active* a : movers)
        if (a->instance >= 0 &&
            (a->sleep.state == phys::Sleep::Asleep || a->body.ics.matrix.m3.y < kLowestY))
            detachMe(*a);

    // dgBangerActiveManager::Update for the next frame. Actives asleep,
    // below the city or in no room leave the list (MM2 only takes them off
    // its list, leaving the instance linked to a free active; OpenMM2
    // detaches them, and one in no room disappears as MM2 draws by room).
    for (int n = 0; n < m_attached;) {
        Active& a = *m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(n)])];
        if (a.instance < 0) {
            managerDetach(a); // not reached: listed actives always have an instance
            continue;
        }
        const auto i = static_cast<std::size_t>(a.instance);
        const bool noRoom = roomsTracked() && m_instances[i].room == 0;
        if (a.sleep.state == phys::Sleep::Asleep || a.body.ics.matrix.m3.y < kLowestY || noRoom) {
            detachMe(a); // the last one moves into slot n
            if (noRoom)
                m_instances[i].state = State::Gone;
            continue;
        }
        ++n;
    }
    // The rest are declared in list order (one detached by its own update
    // lets the last take its place, which waits for the next frame).
    for (int n = 0; n < m_attached; ++n)
        declare(*m_active[static_cast<std::size_t>(m_list[static_cast<std::size_t>(n)])], dt);

    // Debris at the original's per-frame rate, born in the body's frame
    // (asParticles' matrix is the active's phInertialCS matrix).
    for (const int k : m_activeList) {
        Active& a = *m_active[static_cast<std::size_t>(k)];
        a.particleMatrix = a.body.ics.matrix;
    }
    for (int steps = m_ticker.advance(dt); steps > 0; --steps)
        for (const int k : m_activeList)
            m_active[static_cast<std::size_t>(k)]->particles.update(fx::FixedTicker::kStep);
}

// --- Car parts -------------------------------------------------------------------------------

std::size_t BangerSet::ejectPart(const BangerData& data, const std::string& model, const std::string& mesh,
                                 int paint, const Mat34& frame, float speed, int room, std::uint32_t tag) {
    // vehBreakableMgr::Eject: a hit instance from the ring in the car's room,
    // attached, then pushed off. MM2 writes the random "velocity" into the
    // body's momentum (phInertialCS's) and adds the random spin to its
    // angular impulse, so heavy parts move off slowly: kept.
    boundsOf(data);
    const std::size_t h = getBanger();
    if (room < 0)
        room = findRoom(frame.m3, 0);
    moveToRoom(h, room);
    Instance& inst = m_instances[h];
    inst.data = &data;
    inst.model = model;
    inst.part = -1;
    inst.mesh = mesh;
    inst.paint = paint;
    inst.ground = frame;
    inst.everHit = true;
    inst.source = -1;
    inst.tag = tag;
    m_props[h]->audioId = data.colliderId;
    inst.matrix = frame; // SetMatrix
    if (!attachEntity(h))
        return h;
    Active& a = *activeOf(h);
    phys::InertialCS& ics = a.body.ics;

    // A random direction with an upward component.
    float y = m_ejectRand.frand();
    float x = m_ejectRand.frand();
    x = (x + x) - 1.0f;
    float z = m_ejectRand.frand();
    z = (z + z) - 1.0f;
    float n2 = (z * z + x * x) + y * y;
    float inv = n2 == 0.0f ? 0.0f : 1.0f / std::sqrt(n2);
    Vec3 dir{x * inv, y * inv, z * inv};
    {
        const float hi = speed + kEjectSpeedSpread;
        const float lo = speed - kEjectSpeedSpread;
        const float s = (hi - lo) * m_ejectRand.frand() + lo;
        ics.linearMomentum = {s * dir.x, dir.y * s, dir.z * s};
    }
    // A random spin axis, also upward.
    y = m_ejectRand.frand();
    x = m_ejectRand.frand();
    x = (x + x) - 1.0f;
    z = m_ejectRand.frand();
    z = (z + z) - 1.0f;
    n2 = (z * z + y * y) + x * x;
    inv = n2 == 0.0f ? 0.0f : 1.0f / std::sqrt(n2);
    dir = {inv * x, inv * y, inv * z};
    {
        const float hi = kEjectSpinSpread + kEjectSpin;
        const float lo = kEjectSpin - kEjectSpinSpread;
        const float s = (hi - lo) * m_ejectRand.frand() + lo;
        ics.angularImpulse = {s * dir.x + ics.angularImpulse.x, dir.y * s + ics.angularImpulse.y,
                              dir.z * s + ics.angularImpulse.z};
    }
    ics.matrix = frame;
    a.body.syncBoundMatrix();
    newMover(a);
    return h;
}

// --- OpenMM2: network games ------------------------------------------------------------------

bool BangerSet::moving(std::size_t i) const {
    if (i >= m_instances.size() || m_instances[i].active < 0)
        return false;
    return inWorld(*m_active[static_cast<std::size_t>(m_instances[i].active)]);
}

bool BangerSet::heldInertia(std::size_t i, phys::InertialCS& ics) const {
    // dgBangerActive::Attach's body for instance i, as AttachEntity would
    // make it now (a network client's mirror moving with the host's motion).
    if (i >= m_instances.size())
        return false;
    const Instance& inst = m_instances[i];
    if (!inst.data || (inst.state == State::Gone && !replayGhost(i)) || inst.active >= 0)
        return false;
    const BangerData& d = *inst.data;
    ics = phys::InertialCS{};
    ics.matrix = inst.matrix;
    ics.setMass(d.size.x, d.size.y, d.size.z, d.mass);
    smoothAngInertia(ics, kInertiaRatio);
    if (inst.mirror) {
        ics.linearVelocity = inst.mirrorVelocity;
        ics.linearMomentum = inst.mirrorVelocity * ics.mass;
        ics.angularVelocity = inst.mirrorSpin;
        ics.angularMomentum = rowTimes(inst.mirrorSpin, ics.worldInertia());
    }
    return true;
}

bool BangerSet::ghostStands(const Ghost& g) const {
    // The samples up to the one that broke it loose (it started at g.time);
    // the later ones meet its pieces where they were (the race screen puts
    // them back there for each sample). A millisecond is far under a sample
    // and far over the clocks' rounding.
    return m_world && m_world->replaying() && m_world->replayTime() < g.time + 0.001;
}

bool BangerSet::replayGhost(std::size_t i) const {
    if (!m_world || !m_world->replaying() || m_ghosts.empty())
        return false;
    return std::ranges::any_of(m_ghosts, [this, i](const Ghost& g) { return g.prop == i && ghostStands(g); });
}

bool BangerSet::replayGhostPiece(std::size_t i) const {
    if (!m_world || !m_world->replaying() || m_ghosts.empty() || i >= m_instances.size())
        return false;
    const int source = m_instances[i].source;
    if (!m_instances[i].everHit || source < 0)
        return false;
    return std::ranges::any_of(m_ghosts, [this, source](const Ghost& g) {
        return g.prop == static_cast<std::size_t>(source) && ghostStands(g);
    });
}

bool BangerSet::drivenBody(const phys::Instance& other) const {
    for (const auto& a : m_active)
        if (&a->body == &other)
            return a->instance >= 0 && m_instances[static_cast<std::size_t>(a->instance)].driven;
    return false;
}

bool BangerSet::isActiveBody(const phys::Instance* i) const {
    for (const auto& a : m_active)
        if (&a->body == i)
            return true;
    return false;
}

void BangerSet::setReplica(std::function<bool(const phys::Instance&)> localToucher) {
    m_replica = true;
    m_localToucher = std::move(localToucher);
}

bool BangerSet::acceptsFrom(std::size_t i, const phys::Instance& other) const {
    if (!m_replica)
        return true;
    if (m_localToucher && m_localToucher(other))
        return true;
    // The props this machine simulates (its predictions) meet one another,
    // but knock no placed prop: the host's pieces of the same knock fly
    // differently, and such a knock was more often undone than not (5 of 9
    // in the measured runs; one left the client's car tangled in pieces the
    // host never had). The host's knocks bring those it makes. The host's
    // pieces move only for this machine's cars (two of them lying together
    // would otherwise keep each other moving here while the host's rest).
    // A host's piece the host sends in full (net::PropFull) flies as the
    // host's: it knocks a placed prop as the host's does (a prediction like
    // its car's; measured on a car pushing a pile of 18 props through the
    // rest: no correction with, 10 without).
    if (!m_instances[i].everHit)
        return drivenBody(other);
    if (!m_instances[i].mirror)
        return isActiveBody(&other);
    // A host's piece the host sends in full (net::PropFull) meets the pieces
    // simulated here, and they it, as on the host.
    for (const auto& a : m_active)
        if (&a->body == &other)
            return m_instances[i].driven ||
                   (a->instance >= 0 && m_instances[static_cast<std::size_t>(a->instance)].driven);
    return false;
}

void BangerSet::breakPlaced(std::size_t i) {
    // As dgUnhitBangerInstance::Impact leaves the prop: out of the room
    // lists, remembering its room, no longer a banger (flag 1).
    if (i >= m_instances.size() || m_instances[i].everHit || !m_props[i]->banger)
        return;
    if (Active* a = activeOf(i))
        detachMe(*a);
    Prop& prop = *m_props[i];
    const int room = m_instances[i].room;
    moveToRoom(i, 0);
    prop.banger = false;
    prop.room = room;
    m_instances[i].room = room;
    m_instances[i].state = State::Gone;
}

void BangerSet::restoreStanding(std::size_t i) {
    if (i >= m_instances.size() || m_instances[i].everHit)
        return;
    // The pieces it became on this machine (its own ring) disappear.
    for (const std::size_t h : m_ring)
        if (m_instances[h].source == static_cast<int>(i) && m_instances[h].state != State::Gone)
            detachHit(h);
    Prop& prop = *m_props[i];
    if (prop.banger)
        return;
    std::erase_if(m_ghosts, [i](const Ghost& g) { return g.prop == i; });
    // dgUnhitBangerInstance::Reset: back in its room, standing.
    Instance& inst = m_instances[i];
    if (Active* a = activeOf(i))
        detachMe(*a);
    const int room = inst.room;
    if (prop.listed)
        moveToRoom(i, 0);
    moveToRoom(i, room);
    prop.banger = true;
    prop.collidable = true;
    inst.state = State::Unhit;
}

std::size_t BangerSet::mirror(std::size_t slot) {
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    if (slot >= m_mirrors.size())
        m_mirrors.resize(slot + 1, none);
    if (m_mirrors[slot] == none) {
        const std::size_t i = newInstance();
        m_instances[i].everHit = true;
        m_instances[i].mirror = true;
        m_instances[i].state = State::Gone;
        m_props[i]->collidable = false;
        m_mirrors[slot] = i;
    }
    return m_mirrors[slot];
}

void BangerSet::showMirror(std::size_t slot, const MirrorSpec& spec, const Mat34& matrix, const Mat34& drawn,
                           const Vec3& velocity, const Vec3& spin) {
    const std::size_t i = mirror(slot);
    Instance& inst = m_instances[i];
    if (inst.active >= 0 || !spec.data)
        return;
    boundsOf(*spec.data);
    inst.data = spec.data;
    inst.model = spec.model;
    inst.part = spec.part;
    inst.mesh = spec.mesh;
    inst.paint = spec.paint;
    inst.source = spec.source;
    inst.tag = spec.tag;
    inst.matrix = matrix;
    inst.ground = matrix;
    inst.drawn = drawn;
    inst.mirrorVelocity = velocity;
    inst.mirrorSpin = spin;
    inst.state = State::Hit;
    Prop& prop = *m_props[i];
    prop.audioId = spec.data->colliderId;
    prop.collidable = true;
    // Its room, as an active's body finds its own after each sample.
    const int room = roomsTracked() ? findRoom(matrix.m3, inst.room) : inst.room;
    if (room != inst.room || (room > 0 && !prop.listed))
        moveToRoom(i, room);
}

std::pair<net::PropPusher, std::uint8_t> BangerSet::pusherOf(const void* key, const phys::Body* car) const {
    using net::PropPusher;
    if (!key)
        return {PropPusher::None, 0};
    if (m_world && key == m_world->cityKey())
        return {PropPusher::City, 0};
    if (car && key == car->collider.key())
        return {PropPusher::Car, 0};
    if (m_world && key == m_world->staticKey(false))
        return {PropPusher::StaticA, 0};
    if (m_world && key == m_world->staticKey(true))
        return {PropPusher::StaticB, 0};
    for (std::size_t k = 0; k < m_ring.size() && k < net::kMaxPropSlots; ++k)
        if (const phys::Body* b = body(m_ring[k]); b && b->collider.key() == key)
            return {PropPusher::Piece, static_cast<std::uint8_t>(k)};
    return {PropPusher::Other, 0};
}

std::optional<net::PropBodyState> BangerSet::bodyState(std::size_t i, const phys::Body* car) const {
    // A body the world's last step did not run (beyond its movers, or not
    // declared) is not simulated: a client must not run it either.
    const phys::Body* b = i < m_instances.size() ? body(i) : nullptr;
    if (!b || !m_world || !m_world->isActive(b))
        return std::nullopt;
    const Active& a = *m_active[static_cast<std::size_t>(m_instances[i].active)];
    const phys::InertialCS& ics = b->ics;
    net::PropBodyState s;
    s.matrix = ics.matrix;
    s.linearMomentum = ics.linearMomentum;
    s.angularMomentum = ics.angularMomentum;
    s.linearVelocity = ics.linearVelocity;
    s.angularVelocity = ics.angularVelocity;
    s.force = ics.linearForce;
    s.torque = ics.angularTorque;
    s.lastPush = ics.lastPush;
    s.linearImpulse = ics.linearImpulse;
    s.angularImpulse = ics.angularImpulse;
    s.linearPush = ics.linearPush;
    s.turnForce = ics.turnForce;
    s.framePush = ics.framePush;
    const Vec3 zero{};
    s.contact = !(s.linearImpulse == zero && s.angularImpulse == zero && s.linearPush == zero &&
                  s.turnForce == zero && s.framePush == zero);
    s.sleepState = a.sleep.state;
    s.stillUpdates = std::clamp(a.sleep.stillUpdates, 0, 1023);
    s.dormantUpdates = std::clamp(a.sleep.dormantUpdates, 0, 1023);
    s.age = a.age;
    std::tie(s.pusher, s.pusherSlot) = pusherOf(b->collider.lastMaxPusher, car);
    // The bound where the collisions saw it, when the push moved the body on.
    Mat34 follows = ics.matrix;
    follows.m3 = ics.matrix.transform(b->boundOrigin);
    s.hasBound = !(b->boundMatrix.m0 == follows.m0 && b->boundMatrix.m1 == follows.m1 &&
                   b->boundMatrix.m2 == follows.m2 && b->boundMatrix.m3 == follows.m3);
    if (s.hasBound)
        s.bound = b->boundMatrix;
    return s;
}

phys::Body* BangerSet::simulate(std::size_t i) {
    if (i >= m_instances.size() || !m_instances[i].data || m_instances[i].state == State::Gone ||
        m_instances[i].state == State::Unhit)
        return nullptr;
    if (Active* a = activeOf(i))
        return &a->body;
    // As a car's touch would (World::collideInstances: AttachEntity, then
    // the new mover), the mirror from the host's motion.
    phys::Body* b = attachEntity(i);
    if (b && m_world && !m_world->contains(b))
        m_world->addNewMover(b);
    return b;
}

std::size_t BangerSet::worldOrder(std::size_t i) const {
    const phys::Body* b = body(i);
    return b && m_world ? m_world->order(b) : SIZE_MAX;
}

void BangerSet::forgetGhostOf(std::size_t i) {
    if (i >= m_instances.size() || !m_instances[i].everHit || m_instances[i].source < 0)
        return;
    const auto source = static_cast<std::size_t>(m_instances[i].source);
    std::erase_if(m_ghosts, [source](const Ghost& g) { return g.prop == source; });
}

bool BangerSet::setBodyState(std::size_t i, const net::PropBodyState& s, const void* pusher) {
    phys::Body* b = simulate(i);
    if (!b)
        return false;
    Active& a = *m_active[static_cast<std::size_t>(m_instances[i].active)];
    phys::InertialCS& ics = b->ics;
    ics.matrix = s.matrix;
    ics.linearMomentum = s.linearMomentum;
    ics.angularMomentum = s.angularMomentum;
    ics.linearVelocity = s.linearVelocity;
    ics.angularVelocity = s.angularVelocity;
    ics.linearForce = s.force;
    ics.angularTorque = s.torque;
    ics.lastPush = s.lastPush;
    ics.linearImpulse = s.linearImpulse;
    ics.angularImpulse = s.angularImpulse;
    ics.linearPush = s.linearPush;
    ics.turnForce = s.turnForce;
    ics.framePush = s.framePush;
    a.sleep.state = std::clamp(s.sleepState, 0, 2);
    a.sleep.stillUpdates = s.stillUpdates;
    a.sleep.dormantUpdates = s.dormantUpdates;
    a.age = s.age;
    // The bound follows the body; the next sweep starts where it is; the
    // instance is drawn there.
    if (s.hasBound)
        b->boundMatrix = s.bound;
    else
        b->syncBoundMatrix();
    b->collider.lastMatrix = b->boundMatrix;
    b->collider.lastMaxPusher = pusher;
    b->collider.maxPusher = nullptr;
    b->collider.maxPush2 = 0.0f;
    m_instances[i].matrix = s.matrix;
    return true;
}

void BangerSet::releaseMirror(std::size_t slot) {
    if (slot >= m_mirrors.size() || m_mirrors[slot] == static_cast<std::size_t>(-1))
        return;
    if (Active* a = activeOf(m_mirrors[slot]))
        detachMe(*a);
}

void BangerSet::hideMirror(std::size_t slot) {
    if (slot >= m_mirrors.size() || m_mirrors[slot] == static_cast<std::size_t>(-1))
        return;
    const std::size_t i = m_mirrors[slot];
    if (Active* a = activeOf(i))
        detachMe(*a);
    if (m_instances[i].room != 0 || m_props[i]->listed)
        moveToRoom(i, 0);
    m_instances[i].state = State::Gone;
    m_instances[i].drawn.reset();
    m_props[i]->collidable = false;
}

// --- Drawing ---------------------------------------------------------------------------------

void BangerSet::lampPositions(const Vec3& eye, float radius, std::vector<Vec3>& out) const {
    const float r2 = radius * radius;
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const Instance& inst = m_instances[i];
        if (inst.state == State::Gone || !inst.data || inst.data->glowOffsets.empty() || !standing(i) ||
            inst.ownerDrawn) // traffic lights: their signals, not lamps (draw skips them too)
            continue;
        const Vec3 d = inst.matrix.m3 - eye;
        if (d.dot(d) > r2)
            continue;
        for (const Vec3& g : inst.data->glowOffsets)
            out.push_back(inst.matrix.transform(g));
    }
}

void BangerSet::draw(render::Device& device, ModelLibrary& models, TextureLibrary& textures, fx::ParticleRenderer& cards,
                     const Frustum& frustum, const Camera& camera, const DrawParams& params) {
    const Mat34& cam = camera.transform;
    std::vector<Vec3> glows;
    struct Tree {
        const Instance* instance;
        const GpuMesh* mesh;
        Mat34 matrix;
        float dither = 0.0f; // OpenMM2 fades
    };
    std::vector<Tree> trees;
    // cityLevel::DrawRooms draws the props from their rooms (lvlLevel::
    // MoveToRoom's, which BangerSet keeps) when the city listed the view's.
    const bool rooms = params.rooms && params.rooms->active() && roomsTracked();
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const Instance& inst = m_instances[i];
        if (inst.state == State::Gone)
            continue;
        // A traffic light standing is drawn with its signal (AiRenderer:
        // aiTrafficLightInstance::Draw / DrawGlow); once it breaks loose it
        // leaves the rooms (Gone) and its parts are ordinary hit bangers.
        if (inst.ownerDrawn && standing(i))
            continue;
        RoomVisibility::Passes passes;
        if (rooms) {
            passes = params.rooms->passes(inst.room);
            if (!passes.objects && !passes.shadowsAndGlows)
                continue;
        }
        const GpuModel* model = models.get(inst.model);
        if (!model)
            continue;
        const float radius = (model->bounds.max - model->bounds.min).mag() * 0.5f;
        const Mat34 matrix = inst.drawn                                  ? *inst.drawn
                             : inst.active >= 0 && params.drawnMatrix ? params.drawnMatrix(i, inst.matrix)
                                                                      : inst.matrix;
        // lvlInstance::IsVisible with the dynamic objects' NoDraw limit.
        const auto lod = objectLod(viewDepth(cam, matrix.m3), radius, params.detail, params.detail.noDraw);
        const auto fade = params.fades && lod ? objectLodFade(viewDepth(cam, matrix.m3), radius, params.detail,
                                                              params.detail.noDraw)
                                              : std::nullopt;
        const bool visible = lod && frustum.intersectsSphere(matrix.m3, radius);
        // Lamp glows of props still standing (dgBangerInstance::DrawGlow:
        // lvlInstance flag 1, which stays set while an active holds the prop
        // and only goes when it breaks loose). cityLevel_drawLights draws
        // them by the room alone, whatever IsVisible says; without the
        // city's room list, with the prop.
        if (params.glows && standing(i) && (rooms ? passes.shadowsAndGlows : visible))
            for (const Vec3& g : inst.data->glowOffsets)
                glows.push_back(matrix.transform(g));
        if (!visible || !passes.objects)
            continue;
        const std::string part = !inst.mesh.empty() ? inst.mesh
                                 : inst.part < 0 ? std::string()
                                                 : std::format("BREAK{:02}", inst.part + 1);
        const bool tree = inst.data->billFlags & BangerData::kTree;
        // Trees always draw their high LOD, after the other props.
        const GpuMesh* mesh = model->find(part, tree ? asset::Lod::High : fade ? fade->lod : *lod);
        if (!mesh)
            continue;
        // OpenMM2 fades: the level on the band's other side, dithered against
        // this one (a tree only thins out before NoDraw).
        const GpuMesh* other = fade && fade->other && !tree ? model->find(part, *fade->other) : nullptr;
        if (other == mesh)
            other = nullptr;
        const float dither = !fade ? 0.0f : other || fade->share < 1.0f ? fade->share : 0.0f;
        if (tree) {
            trees.push_back({&inst, mesh, matrix, dither});
            continue;
        }
        // dgBangerInstance::Draw. No local lights
        // (dgBangerInstance::SetupGfxLights answers 0), no reflection or
        // shadow of their own (dgBangerInstance::DrawReflected and
        // dgBangerInstance::DrawShadow are empty).
        MeshDrawOptions options;
        options.alphaRef = kAlphaRef;
        if (inst.data->billFlags & BangerData::kUnlit) {
            options.lighting = false;
            options.alphaRef = kUnlitAlphaRef;
        }
        options.dither = dither;
        drawGpuMesh(device, textures, *mesh, model->materials(variantOf(*model, inst.paint)),
                    Mat44::fromMat34(matrix), options);
        if (other) {
            options.dither = -dither;
            drawGpuMesh(device, textures, *other, model->materials(variantOf(*model, inst.paint)),
                        Mat44::fromMat34(matrix), options);
        }
    }
    // dgTreeRenderer::RenderTrees (dgBangerInstance::DrawTree): unlit, alpha
    // reference 120, with the prop's variant.
    for (const Tree& t : trees) {
        MeshDrawOptions options;
        options.lighting = false;
        options.alphaRef = kTreeAlphaRef;
        options.dither = t.dither;
        const GpuModel& model = *models.get(t.instance->model);
        drawGpuMesh(device, textures, *t.mesh, model.materials(variantOf(model, t.instance->paint)),
                    Mat44::fromMat34(t.matrix), options);
    }

    // Lamp glows: s_yel_glow cards of half size 1.5 m with a 1% flicker,
    // white, added, unfogged, without depth writes.
    if (!glows.empty()) {
        cards.begin();
        for (const Vec3& g : glows) {
            fx::SparkPos p;
            p.position = g;
            p.radius = (m_glowRand.frand() * 0.01f + 0.99f) * 1.5f;
            p.color = 0xFFFFFFFFu;
            cards.add(p, 1, 1, cam);
        }
        cards.flush(device, textures.get("s_yel_glow"), {render::BlendMode::Add, false, {1, 1, 1, 1}});
    }

    // Debris particles of active props.
    for (const int k : m_activeList) {
        const Active& a = *m_active[static_cast<std::size_t>(k)];
        if (!a.particles.count() || a.texNumber <= 0)
            continue;
        const auto sheet = fx::EffectLibrary::bangerSheet(a.texNumber);
        // OpenMM2: between the last two 60 Hz updates (FixedTicker::behind).
        cards.draw(device, cam, a.particles, textures.get(sheet.texture), {}, m_ticker.behind());
    }
}

} // namespace mm2::game::bangers
