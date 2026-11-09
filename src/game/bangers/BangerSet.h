/*
    OpenMM2 - runtime breakable props (bangers) on MM2's collision manager:
    dgBangerData's bounds, dgUnhitBangerInstance, dgHitBangerInstance,
    dgBangerManager, dgBangerActive and dgBangerActiveManager, ported from
    the code of midtown2.exe build 3393 (MM2Recomp). Structure first ported
    from Open1560 (mmbangers/banger.cpp, active.cpp), Copyright (C) 2020
    Brick, GPL-3.0-or-later.
*/
#pragma once

#include "game/Camera.h"
#include "game/CityLevel.h"
#include "game/MeshDraw.h"
#include "game/ModelLibrary.h"
#include "game/RoomVisibility.h"
#include "game/TextureLibrary.h"
#include "game/bangers/BangerData.h"
#include "game/bangers/PropPlacement.h"
#include "game/fx/EffectLibrary.h"
#include "game/fx/ParticleRenderer.h"
#include "game/fx/Particles.h"
#include "net/PropState.h"
#include "phys/World.h"
#include "render/Device.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mm2::game::bangers {

// All props of a city that can be knocked over, as MM2's collision manager
// (phys::World) sees them.
//
// A prop standing where the city placed it is an unhit instance listed in
// its room. When a body touches it, the world attaches one of 32 actives
// (dgBangerActive: a rigid body) to it and resolves the impacts with
// dgImpact: up to sqrt(ImpulseLimit2) the prop holds like a wall and the
// active goes back to the pool; beyond, the prop breaks loose. It then
// leaves its room and one of a ring of 40 hit instances takes its place,
// keeping the active and the impulses (dgUnhitBangerInstance::Impact), or
// each of its BREAKnn parts becomes a hit instance with its own active that
// carries the prop's change of motion. Actives are movers of the world until
// they sleep or fall below the city; their hit instance then rests where it
// stopped, an ordinary object that can be hit again. When the ring wraps,
// the oldest hit instance disappears.
//
// Per frame: World::advance*, then update(). Register the set with the
// level (CityLevel::addSource) so the world finds the props in their rooms.
class BangerSet final : public InstanceSource {
public:
    static constexpr int kMaxActive = 32; // dgBangerActiveManager
    static constexpr int kMaxHit = 40;    // dgBangerManager::Init(40)

    explicit BangerSet(const BangerDataLibrary& data);
    ~BangerSet() override;
    BangerSet(const BangerSet&) = delete;
    BangerSet& operator=(const BangerSet&) = delete;

    // Adds props that have banger data (others are ignored and counted).
    void add(const std::vector<PlacedProp>& props);
    // Adds one prop and returns its instance index (nullopt without banger
    // data). Call setWorld (again) afterwards for a prop without a room.
    std::optional<std::size_t> addOne(const PlacedProp& prop);
    // The physics world whose movers the actives become and whose level
    // places props without a room. Must outlive this set (or call
    // setWorld(nullptr) first).
    void setWorld(phys::World* world);
    const phys::World* world() const { return m_world; }

    // Once per frame, after the physics world's step: dgBangerActive::
    // PostUpdate (actives that sleep or fell below the city detach), then
    // dgBangerActiveManager::Update for the next frame (the actives join the
    // world as its CollisionType says), and the debris.
    void update(float dt);

    // InstanceSource: the props standing or resting in `room` (actives'
    // instances are not collidable and not listed).
    void instancesIn(int room, std::vector<phys::Instance*>& out) const override;

    struct DrawParams {
        ObjectDetail detail;
        // dgBangerManager::InitGlow is called at night only (mmGame::InitWeather).
        bool glows = false;
        // The rooms the city listed for the view (CityRenderer::rooms()):
        // props are then drawn from their rooms (cityLevel_drawObjects) and
        // their lamp glows by the room alone (cityLevel_drawLights).
        const RoomVisibility* rooms = nullptr;
        // OpenMM2 presentation: where a prop an active simulates is drawn,
        // from its index and current matrix (between the physics' last two
        // samples, game::StepHistory); without it, at its matrix.
        std::function<Mat34(std::size_t index, const Mat34& matrix)> drawnMatrix;
    };
    // OpenMM2 night lighting: the lamp glows (bulbs) of the props still
    // standing within `radius` of `eye`, whatever is in view, so the light
    // they cast does not pop as the camera turns.
    void lampPositions(const Vec3& eye, float radius, std::vector<Vec3>& out) const;

    // Inside a scene pass after setFrameConstants().
    void draw(render::Device& device, ModelLibrary& models, TextureLibrary& textures, fx::ParticleRenderer& cards,
              const Frustum& frustum, const Camera& camera, const DrawParams& params);

    // Resets every prop to its original place (race restart):
    // dgBangerActiveManager::Reset, dgBangerManager::Reset and
    // dgUnhitBangerInstance::Reset.
    void reset();

    enum class State : std::uint8_t {
        Unhit,  // standing where the city placed it
        Active, // simulated by an active
        Hit,    // knocked over, at rest (can be hit again)
        Gone,   // in no room: a prop that broke loose, or an unused hit instance
    };
    struct Instance {
        const BangerData* data = nullptr; // dgBangerInstance::GetData
        std::string model;
        int part = -1;   // -1 whole model, k = mesh "BREAK{k+1:02}"
        std::string mesh; // a car part's mesh (ejected parts), else empty
        int paint = 0;
        Mat34 ground;    // placement of the model's ground origin
        Mat34 matrix;    // current frame at the CG (meshes are centred on it)
        State state = State::Unhit;
        bool everHit = false; // one of the ring of hit instances rather than a placed prop
        bool ownerDrawn = false; // PlacedProp::ownerDrawn: not drawn here while it stands
        int active = -1; // index into the active pool
        int room = 0;
        int roomHint = 0; // with room 0: where FindRoomId starts (an xref's parent room)
        // OpenMM2 network games (game/net/PropSync). A hit instance: the
        // placed prop it came from (whole, or its BREAKnn `part`), else -1;
        // the caller's tag of an ejected car part (0: none).
        int source = -1;
        std::uint32_t tag = 0;
        // One of the host's ring slots shown on a network client
        // (showMirror), where it is drawn while the host drives it and its
        // motion there (an active attached to it here starts with it).
        bool mirror = false;
        // OpenMM2: one the host sends in full (net::PropFull), simulated here
        // from its states: it meets the pieces simulated here as the host's.
        bool driven = false;
        std::optional<Mat34> drawn;
        Vec3 mirrorVelocity, mirrorSpin;
    };
    // The placed props (in add() order) and the hit instances (created as
    // the ring first hands them out).
    const std::deque<Instance>& instances() const { return m_instances; }
    // Instance i as the collision manager sees it (lvlInstance).
    phys::Instance& prop(std::size_t i);
    // The body simulating instance i (its active's), or null.
    const phys::Body* body(std::size_t i) const;
    // dgBangerData's bound for `data`, built on first use.
    const phys::Bound* bound(const BangerData& data) const;
    // dgBangerInstance::GetBound(which) and lvlInstance::GetRadius for any
    // dgUnhitBangerInstance of `data` (the gizmos own theirs): the bound, or
    // for 1 the box around a bound that is not a box; the bound's radius.
    const phys::Bound* boundOf(const BangerData& data, int which) const;
    float boundRadius(const BangerData& data) const;
    std::size_t skipped() const { return m_skipped; } // props without banger data
    const BangerDataLibrary& dataLibrary() const { return m_data; }
    int activeCount() const { return m_attached; }
    // dgBangerDataManager's age mode: actives declared by age rather than by
    // CollisionType. mmGame::Init turns it off; kept for completeness.
    void setAgeMode(bool on) { m_ageMode = on; }
    int hitCount() const;

    // vehBreakableMgr::Eject: a car part flies off as a knocked-over banger.
    // `mesh` is the part of `model` (the car's PKG) to draw with paint job
    // `paint`; `frame` its world placement (the part's pivot); `room` the
    // car's room (-1: found from the world's level). It is given momentum
    // `speed` +- 1 in a random upward direction and an angular impulse of
    // 1-3 (as momentum, not velocity: see the .cpp). Returns the hit
    // instance it became (vehBreakable +0x44 keeps it for Reset).
    // `tag` (OpenMM2): the caller's name for the part across machines
    // (Instance::tag), 0 for none.
    std::size_t ejectPart(const BangerData& data, const std::string& model, const std::string& mesh, int paint,
                          const Mat34& frame, float speed, int room = -1, std::uint32_t tag = 0);

    // dgHitBangerInstance::Detach of instance i, as vehBreakableMgr::Reset
    // calls it for an ejected car part when the car's damage is cleared: its
    // active (if any) detaches and it leaves its room, so it disappears. MM2
    // keeps the instance's address, not its prop: when the ring has handed
    // the slot out again since, whatever prop it now holds disappears (kept).
    // A placed prop (dgUnhitBangerInstance) keeps lvlInstance's empty Detach.
    void detachHit(std::size_t i);

    // The debris of instance i's active (dgBangerActive's asParticles) and
    // the fxpt sheet it draws with (0: none), or nullopt without an active.
    struct Debris {
        const fx::ParticleSystem* particles = nullptr;
        int sheet = 0;
    };
    std::optional<Debris> debris(std::size_t i) const;

    // lvlInstance flag 1 of instance i: a placed prop that has not broken
    // loose (also while an active holds it, before dgUnhitBangerInstance::
    // Impact clears the flag). dgBangerInstance::DrawGlow tests it.
    bool standing(std::size_t i) const;

    // --- OpenMM2: network games (game/net/PropSync) ---------------------------------------
    //
    // MM2 sends nothing about props: every machine knocks its own with its
    // own simulation of every car. OpenMM2's host simulates them for
    // everyone; its clients show the host's and simulate only what their
    // own car touches (a prediction the host confirms or corrects).

    // A placed prop that broke loose (dgUnhitBangerInstance::Impact) and
    // what broke it (the instance whose impact did; null when unknown, and
    // only valid until the next physics step).
    struct Knock {
        std::size_t prop = 0;
        const phys::Instance* by = nullptr;
    };
    // Whether knocks are kept for takeKnocks (off by default: a
    // single-player race never takes them).
    void recordKnocks(bool on) { m_recordKnocks = on; }
    // The knocks since the last call, oldest first.
    std::vector<Knock> takeKnocks() { return std::exchange(m_knocks, {}); }
    // dgBangerManager's ring as handed out so far: slot k's hit instance,
    // and how many times slot k has been handed out (a new prop in the slot
    // has a new generation).
    const std::vector<std::size_t>& ring() const { return m_ring; }
    std::uint32_t generation(std::size_t slot) const {
        return slot < m_ringGeneration.size() ? m_ringGeneration[slot] : 0;
    }
    // OpenMM2 (network games): when the ring is about to wrap onto a slot
    // whose prop still moves or was handed out less than `busySeconds` ago
    // (a pile-up of more knocks than the ring holds), it grows by kMaxHit
    // slots instead, up to `limit` (MM2's ring of 40 makes those props
    // disappear in mid-flight). Off (the limit kMaxHit) in single player.
    void setRingGrowth(int limit, double busySeconds);
    int ringSize() const { return m_ringSize; }
    // Whether instance i has an active whose body is moving in the world.
    bool moving(std::size_t i) const;
    // Whether `i` is the body of one of this set's actives.
    bool isActiveBody(const phys::Instance* i) const;
    // Whether `other` is the body of a piece the host sends in full (net::
    // PropFull), simulated here from its states.
    bool drivenBody(const phys::Instance& other) const;
    // The body an active would give instance i if a car hit it now
    // (phys::Instance::heldInertia, for World::replaySample); false for an
    // instance with an active or out of the world.
    bool heldInertia(std::size_t i, phys::InertialCS& ics) const;
    // A client's replay of its car (phys::World::replaying) meets a placed
    // prop its car broke loose in the last kGhostSeconds as it stood there
    // (a "ghost", standing again for the replay only), and not the pieces it
    // became, in the samples up to the one that broke it loose: the replayed
    // car takes the knock the real one took. The later samples meet the
    // pieces.
    bool replayGhost(std::size_t i) const;
    bool replayGhostPiece(std::size_t i) const;
    static constexpr double kGhostSeconds = 1.0;

    // A network client: only this machine's cars (`localToucher`: its own
    // and the other players' it simulates) may touch a prop, and the props
    // this set simulates itself one another (phys::Instance::
    // acceptsContact); everything else passes through them, since the host
    // decides what the other cars do to them (and what its pieces knock).
    void setReplica(std::function<bool(const phys::Instance&)> localToucher);
    // Back to simulating every contact (a client that cannot follow the
    // host's props).
    void clearReplica() {
        m_replica = false;
        m_localToucher = nullptr;
    }
    bool replica() const { return m_replica; }
    // Client: placed prop i broke loose on the host. It leaves its room as
    // dgUnhitBangerInstance::Impact makes it leave, without a body (its
    // pieces are the host's ring slots, showMirror).
    void breakPlaced(std::size_t i);
    // Client: a knock this machine predicted that the host did not make.
    // Placed prop i stands in its room again and the hit instances it became
    // here disappear (dgUnhitBangerInstance::Reset for one prop).
    void restoreStanding(std::size_t i);
    // Client: what one of the host's ring slots holds.
    struct MirrorSpec {
        const BangerData* data = nullptr;
        std::string model;
        int part = -1;
        std::string mesh;
        int paint = 0;
        int source = -1;
        std::uint32_t tag = 0;
    };
    // The instance showing host ring slot `slot` (made on first use, hidden).
    std::size_t mirror(std::size_t slot);
    // Shows host ring slot `slot` at `matrix` (the frame at the CG), drawn at
    // `drawn`, moving at `velocity` and `spin` there. This machine's car may
    // touch it: an active then simulates it here from that motion until it
    // rests again. Not while the instance has an active: the local
    // simulation drives it then.
    void showMirror(std::size_t slot, const MirrorSpec& spec, const Mat34& matrix, const Mat34& drawn,
                    const Vec3& velocity, const Vec3& spin);
    // Takes it out of the world (not drawn, not collidable, no body).
    void hideMirror(std::size_t slot);
    // Ends this machine's simulation of it (its active detaches; it stays
    // where it is).
    void releaseMirror(std::size_t slot);
    std::size_t mirrorCount() const { return m_mirrors.size(); }

    // OpenMM2 (network games, net::PropFull): instance i's body as the host
    // sends it in full (nullopt without an active the world's last step ran);
    // `car`: the car of the client it is for (net::PropPusher::Car).
    std::optional<net::PropBodyState> bodyState(std::size_t i, const phys::Body* car = nullptr) const;
    // What a collider key (a body's hardest pusher) is in those terms.
    std::pair<net::PropPusher, std::uint8_t> pusherOf(const void* key, const phys::Body* car) const;
    // Where instance i's body runs in the world's movers (World::order;
    // SIZE_MAX without one).
    std::size_t worldOrder(std::size_t i) const;
    // Client: instance i (a mirror shown, or a piece this machine simulates)
    // simulated here, given an active first when it has none (from the
    // motion it has); its body, or null when it cannot be (not shown).
    phys::Body* simulate(std::size_t i);
    void setDriven(std::size_t i, bool driven) {
        if (i < m_instances.size())
            m_instances[i].driven = driven;
    }
    // ... and put to the host's state (given an active first), `pusher`
    // the key of its hardest pusher here.
    bool setBodyState(std::size_t i, const net::PropBodyState& s, const void* pusher = nullptr);
    // Client: the host's state of piece i is known (it knocked the prop
    // over before then, whatever knocked it), so the replays starting from
    // it meet the piece and not the prop standing (no replayGhost).
    void forgetGhostOf(std::size_t i);

private:
    struct Active;
    struct ActiveBody;
    class Prop;
    struct DataBounds;

    std::size_t newInstance();
    bool roomsTracked() const;
    int findRoom(const Vec3& position, int hint) const;
    void placeUnroomed();
    void moveToRoom(std::size_t i, int room);
    std::size_t getBanger();
    const DataBounds& boundsOf(const BangerData& data) const;

    Active* activeOf(std::size_t i);
    phys::Body* attachEntity(std::size_t i);
    Active* managerAttach(std::size_t i);
    void managerDetach(const Active& a);
    void activeAttach(Active& a, std::size_t i);
    void activeDetach(Active& a);
    void detachMe(Active& a);
    void worldDetach(Active& a);
    void newMover(Active& a);
    void declare(Active& a, float dt);
    void directUpdate(Active& a, float dt);
    bool inWorld(const Active& a) const;
    void unhitImpact(std::size_t i, const phys::Instance* by);
    void syncActiveList();
    bool acceptsFrom(std::size_t i, const phys::Instance& other) const;

    const BangerDataLibrary& m_data;
    phys::World* m_world = nullptr;
    // Stable addresses: the world holds pointers to the props during a step,
    // and breaking a prop adds hit instances in the middle of one.
    std::deque<Instance> m_instances;
    std::vector<std::unique_ptr<Prop>> m_props; // per instance
    std::vector<std::vector<Prop*>> m_rooms;    // lvlLevel's room lists (props only)
    mutable std::unordered_map<const BangerData*, std::unique_ptr<DataBounds>> m_bounds;
    std::vector<std::unique_ptr<Active>> m_active; // fixed pool of kMaxActive
    // dgBangerActiveManager's list: the first m_attached are attached, the
    // rest free in the order they are handed out.
    std::array<int, kMaxActive> m_list{};
    int m_attached = 0;
    std::vector<int> m_activeList; // the attached ones (the list's head), for drawing
    std::vector<std::size_t> m_ring; // dgBangerManager's hit instances by slot
    std::vector<std::uint32_t> m_ringGeneration; // times each slot was handed out
    std::vector<double> m_ringHandedAt;          // world time each slot was last handed out
    int m_ringNext = 0;
    int m_ringSize = kMaxHit;  // OpenMM2: grows in network games (setRingGrowth)
    int m_ringLimit = kMaxHit;
    double m_ringBusySeconds = 0.0;
    bool ringSlotBusy(std::size_t slot) const;
    // OpenMM2 network games.
    std::vector<Knock> m_knocks;
    bool m_recordKnocks = false;
    bool m_replica = false;
    std::function<bool(const phys::Instance&)> m_localToucher;
    std::vector<std::size_t> m_mirrors; // a client's instance per host ring slot (npos: none yet)
    struct Ghost {
        std::size_t prop = 0;
        double time = 0.0; // world time it broke loose
    };
    std::vector<Ghost> m_ghosts; // a client's own car's knocks, for its replays
    bool ghostStands(const Ghost& g) const; // in the sample being run again
    std::size_t m_skipped = 0;
    bool m_ageMode = false; // dgBangerDataManager +0x2a8a8 (cleared by mmGame::Init)
    fx::FixedTicker m_ticker;
    fx::Rand m_glowRand{1};
    fx::Rand m_ejectRand{0xE7EC7u};
};

} // namespace mm2::game::bangers
