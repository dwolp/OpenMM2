#pragma once

// Ambient traffic, after MM2's aiVehicleAmbient / aiVehicleSpline /
// aiRailSet and its goals (build 3393, MM2Recomp; documentation only):
//
//   aiMap::AdjustAmbients, NumCars       population of the roads near the player
//   aiGoalRandomDrive::Reset / Update /  speed control, junctions, lane changes,
//     SolveVelocity / AvoidCollision /     moving along the lane and turn curves
//     OkayToEnterIntersection / AnyVehiclesComingThisWay / SolveRailType /
//     SolveLane / ChangeLanes / SpeedLimit / UpcomingAccident
//   aiMap::ChooseNextLaneLink            next road (ai/AmbientRoute)
//   aiPath::Push/Pop/Add/RemoveAmbVehicle, RoadCapacity, ResetVehicleReactTicks
//   aiIntersection stop sign control     four-way stops
//   aiVehicleSpline::DistanceToVehicle / DistanceToIntersection /
//     DetectPlayerCollision / DetectPlayerZoneCollision /
//     IsThePlayerInFrontOfMe / IsAmbientBlockingPlayer
//   aiGoalAvoidPlayer, aiGoalRegainRail, aiGoalCollision, aiVehicleAmbient::Impact
//
// Cars ride cubic Hermite curves in XZ: one per lane section, one through
// each intersection. docs/ai.md lists what is verified and what is inferred.

#include "ai/AmbientRoute.h"
#include "ai/MapView.h"
#include "ai/PlayerCar.h"
#include "ai/Random.h"
#include "ai/RoadNetwork.h"
#include "ai/TrafficLights.h"
#include "ai/VehicleData.h"

#include <array>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mm2::ai {

inline constexpr int kAmbientPoolSize = 300;          // MMSTATE ambient vehicle count (mmStatePack ctor)
inline constexpr float kAmbientDensityScale = 0.2f;   // aiMap::Init: clamp(density, 0, 1) * 0.2
inline constexpr float kAmbientCarSpacing = 8.0f;     // aiMap::AdjustAmbients / NumCars divisor
inline constexpr float kAmbientOpponentClearance = 50.0f; // AdjustAmbients: 2500 (squared)
inline constexpr float kIntersectionReactDist = 25.0f;    // aiRailSet::Reset, aiGoalRandomDrive ctor
inline constexpr float kRestartReactDist = 5.0f;          // SolveVelocity restart
inline constexpr float kFollowReactDist = 20.0f;          // SolveVelocity
inline constexpr float kPlayerZoneDistance = 25.0f;       // aiGoalRandomDrive::Update: 625 (squared)
inline constexpr float kTireRotationWrap = 6.28f;         // aiVehicleSpline::Update
inline constexpr float kRegainDistance = 30.0f;           // aiGoalRegainRail
inline constexpr int kMaxPhysicalCars = 32;               // aiVehicleManager slots

// Indicator lights (aiVehicleInstance +0x1a): 1 left, 2 right, 3 hazards.
enum class TurnSignal : std::uint8_t { None = 0, Left = 1, Right = 2, Hazard = 3 };

// Goal states (aiVehicleSpline +0xec).
enum class AmbientGoal : std::uint8_t {
    RandomDrive = 0, // on its rail
    Collision = 2,   // hit: physical, or a wreck
    RegainRail = 3,  // driving a curve back onto its lane
    AvoidPlayer = 4, // off the rail, swerving round the player
    Parked = 6,      // gave up: stays where it is
};

// One ambient vehicle as the renderer, audio and physics see it.
struct AmbientCar {
    int id = 0;
    const VehicleData* data = nullptr;
    std::string model; // geometry/<model>.pkg
    float paint = 0.0f; // aiVehicleInstance::SetColor: paint job = trunc(paint * (paint jobs - 1))
    Mat34 transform;    // model origin on the ground, facing -Z
    Vec3 velocity;
    float speed = 0.0f;
    float tireRotation = 0.0f; // radians, wraps at 6.28 (aiVehicleSpline::Update)
    float steer = 0.0f;        // rail cars have no steering angle in MM2
    // aiVehicleInstance::DrawGlow lights the TLIGHT part when the car
    // decelerates (aiVehicleSpline +0x54 below 0) or stands (speed 0).
    bool braking = false;
    // The indicators (aiVehicleInstance +0x1a) and their blink phase
    // (+0x18): DrawGlow lights SLIGHT0 (bit 1) and SLIGHT1 (bit 2) while
    // bit 3 of (phase + aiVehicleManager's clock) is set; the clock is the
    // AI time x 16 (aiVehicleManager::Update), so they blink once a second.
    TurnSignal signal = TurnSignal::None;
    int blinkPhase = 0;
    bool horn = false; // tried to honk this step (aiGoalAvoidPlayer::Reset)
    AmbientGoal goal = AmbientGoal::RandomDrive;
    bool physical = false; // handed over to the physics simulation
    bool wreck = false;    // can never regain its rail (aiVehicleInstance flag 0x02)
    // dgPhysManager::DeclareMover flags of the car's rail instance (no
    // body) from its last update: 0x0a while avoiding the player or
    // regaining its rail (aiGoalAvoidPlayer / aiGoalRegainRail::Update),
    // 0x08 for a wreck (aiGoalCollision::Update); 0 not declared.
    unsigned moverFlags = 0;
    // OpenMM2: how many times the pool slot was put on a road (a network
    // cruise's shared traffic tells a recycled slot from the car before it).
    int spawns = 0;
};

// The players the traffic sees: MM2's aiMap player list (aiMap::AddPlayer,
// aiMap::RemovePlayer, aiVehiclePlayer). A single-player game has one, the
// local player in slot 0. aiMap keeps four slots, one bit each in a road's
// populated mask (aiPath::AddAmbPlayer / RemAmbPlayer); OpenMM2 keeps 16
// for the shared traffic of its network cruise (an OpenMM2 extra, see
// docs/parity/openmm2-only.md), whose other players take the slots of their
// network ids.
inline constexpr int kMaxTrafficPlayers = 16;
struct TrafficPlayer {
    int slot = 0; // aiVehiclePlayer +8: its place in the list (0..kMaxTrafficPlayers - 1)
    PlayerCar car;
    int room = 0; // the PSDL room it is in (0: outside every room)
};

struct TrafficSettings {
    float density = 1.0f; // menu traffic density (MMSTATE trafficDensity), 0..1
    int poolSize = kAmbientPoolSize;
    bool laneChanges = true; // [AmbientLaneChanges] of the race's AI map
    // Vehicle types and cumulative spawn probabilities ([Ambient Types/Density]).
    std::vector<city::AiAmbientType> types;
};

class Traffic {
public:
    // Called when a rail car is hit and must become physical
    // (aiVehicleInstance::AttachEntity -> aiVehicleManager::Attach). The
    // handler takes over the car until detach().
    using ImpactHandler = std::function<void(AmbientCar&, const Vec3& impulse)>;
    // Vertical ground probe from `from` down to `to`; returns the hit point.
    using GroundProbe = std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit)>;

    // The traffic's own random stream, set to `seed` again by reset().
    Traffic(const RoadNetwork& network, TrafficLights& lights, std::vector<VehicleData> types,
            const TrafficSettings& settings, std::uint64_t seed);
    // MM2's global stream, shared with the pedestrians (ai::World):
    // aiMap::Init's draws for the pool come from it as it stands, and its
    // owner sets it back to 1 at a reset (aiMap::Reset's ResetRandomSeed).
    Traffic(const RoadNetwork& network, TrafficLights& lights, std::vector<VehicleData> types,
            const TrafficSettings& settings, Random& random);
    Traffic(const Traffic&) = delete;
    Traffic& operator=(const Traffic&) = delete;

    // One update (aiMap::Update's ambient part). `playerRoom` is the PSDL room
    // the player is in (0: outside every room, nothing changes).
    void step(float dt, const PlayerCar& player, int playerRoom);
    // The same for every player in `players` (MM2's list order; slots must
    // differ): the roads round each are populated, a player new to the list
    // populates from room 0 (aiMap::AddPlayer) and one gone from it gives
    // its roads up (aiMap::RemovePlayer), and the cars avoid and let pass
    // any of them (aiGoalRandomDrive::Update, aiGoalRegainRail::Update,
    // aiGoalAvoidPlayer for the player it avoids). One player in slot 0 is
    // the single-player step.
    void step(float dt, std::span<const TrafficPlayer> players);
    // aiMap::Reset's ambient part (mmGame::Init after the AI map loads, and
    // mmGame::Reset when a race restarts): the own random stream back to its
    // seed (ResetRandomSeed; a shared one is its owner's to set), every road
    // and intersection list emptied
    // (aiPath::Reset, aiIntersection::Reset), every car back in the pool in
    // index order with its rail reset (aiMap::AddAmbient, aiRailSet::Reset).
    // The next step populates the roads around the player's room. A car's
    // wreck flag (aiVehicleInstance flag 2) survives, as in MM2.
    void reset();
    // aiMap::Reset's AdjustAmbients from room 0 to the player's room, which
    // OpenMM2 makes on the first step after a reset (step() calls it; ai::World
    // calls it before the pedestrians' population, as aiMap::Reset orders
    // them, and before either updates). False when already populated.
    bool populate(int playerRoom);
    // aiMap::Reset's AdjustAmbients for every player (`players` as step()).
    bool populate(std::span<const TrafficPlayer> players);
    // Convenience for tools: a player of default size at `pos` moving at `vel`.
    void step(float dt, const Vec3& pos, const Vec3& vel, int playerRoom);

    const std::vector<AmbientCar>& cars() const { return m_public; }
    // The vehicle types of the pool (aiMap::Init's ambient types, in the AI
    // map's order).
    const std::vector<VehicleData>& types() const { return m_types; }
    // Ids of the cars that started avoiding the player since the last call
    // (aiGoalAvoidPlayer::Reset, where MM2 plays the avoidance horn and, if
    // it sounds, the driver's reaction).
    std::vector<int> takeAvoidEvents() { return std::exchange(m_avoidEvents, {}); }
    std::size_t activeCount() const;

    void setImpactHandler(ImpactHandler h) { m_onImpact = std::move(h); }
    void setGroundProbe(GroundProbe probe) { m_probe = std::move(probe); }
    void setOpponents(std::span<const Vec3> positions) {
        m_opponents.assign(positions.begin(), positions.end());
    }

    // A vehicle hit rail car `carId` (aiVehicleAmbient::Impact(1)).
    void impact(int carId, const Vec3& impulse);
    // The physical car came to rest at `transform` (aiVehicleActive::Detach):
    // `upright` when it stands on the ground. It regains its rail, or stays a
    // wreck.
    void detach(int carId, const Mat34& transform, bool upright);
    // Pose of a physical car, kept in the AI matrix (aiGoalCollision::Update).
    void setPhysicalTransform(int carId, const Mat34& transform);
    // Returns a car to the pool (recycled by the game).
    void release(int carId);
    // The pedestrians' accident queries (aiPedestrian::UpcomingAccident and
    // Accident): a car out of normal driving (InAccident: any goal but
    // driving its rail) in the obstacle list of `intersection`, or, for a
    // road (`path` >= 0), in its two section lists at index 1 (`dir` 1) or
    // n - 1 (as coded: the lists are counted each in its own direction, so
    // this reads both ends of the road).
    bool accidentAt(int intersection, int path, int dir) const;

    // MM2's obstacle map: every ambient car is listed, by
    // aiVehicleSpline::UpdateObstacleMap after each update, either in the
    // vehicle list of the intersection it is in or in a per-section list of
    // the road it is on, by side (1: driving with the vertex index) and
    // section (counted in that side's direction, 1 .. n - 1). The entries
    // are car indices. `map` gives the rooms' components.
    void setMap(const MapView* map) { m_map = map; }
    std::span<const int> roadVehicles(int path, int side, int bucket) const;
    std::span<const int> intersectionVehicles(int node) const;
    // aiVehicleSpline::CurrentRoadIdx: the slot of car `car`'s road in another
    // car's window of three roads (and the section there); 0 when it is on
    // none of them (it never answers "none").
    int currentRoadIdx(int car, const int roads[3], const bool dirs[3], int* vert) const;

    // aiIntersection::StopSources: every road of `intersection` whose end
    // there has a stop sign or a traffic light is told to always stop
    // (aiPath::AllwaysStop, aiPath +0x162) or let go again: its ambient cars
    // do not enter an intersection while it is set (OkayToEnterIntersection).
    // The racers set it on the intersections ahead of them while the ambient
    // traffic updates (aiMap::StopRoadTraffic) and on their first waypoint
    // when they start.
    void stopSources(int intersection, bool stop);
    bool alwaysStop(int path) const;

    // A rail vehicle of another system (aiCableCar) that MM2 keeps in the
    // same obstacle map and four-way stop queues as the ambient cars: the
    // drivers then see it (IsTargetBlocked) and the stop signs take turns
    // with it.
    class ExternalVehicle {
    public:
        virtual ~ExternalVehicle() = default;
        // aiObstacle::CurrentRoadIdx (see currentRoadIdx).
        virtual int currentRoadIdx(const int roads[3], const bool dirs[3], int* vert) const = 0;
        // aiObstacle::InAccident.
        virtual bool inAccident() const { return false; }
    };
    // Registers `vehicle` (not owned); returns its entry in the obstacle
    // lists and stop queues, beyond every ambient car's index.
    int addExternal(const ExternalVehicle* vehicle);
    bool isExternal(int entry) const { return entry >= static_cast<int>(m_cars.size()); }
    // aiPath::AddVehicle / RemoveVehicle (newest first), aiIntersection::
    // AddVehicle / RemoveVehicle, for an external entry.
    void listOnRoad(int entry, int path, int side, int bucket);
    void unlistFromRoad(int entry, int path, int side, int bucket);
    void listAtIntersection(int entry, int node);
    void unlistFromIntersection(int entry, int node);
    // aiIntersection::AddToStopSignCntl, StopSignOkayToGo and
    // RemoveFromStopSignCntl for an external entry (not of type 0: it takes
    // no other vehicle of its road along).
    void joinStopSign(int node, int entry);
    bool stopSignTurn(int node, int entry) { return stopSignOkayToGo(node, entry); }
    void leaveStopSign(int node, int entry) { removeFromStopSign(node, entry); }

    // Diagnostics for tests and tools.
    struct DebugCar {
        int lane = -1;     // network lane id of the logical lane
        int nextLane = -1; // network lane id of the next lane
        float s = 0.0f;    // distance along the lane (RoadDist)
        bool turning = false;
        bool changingLane = false;
        bool entered = false;
        float accel = 0.0f;
        float targetVelocity = 0.0f;
        int reactTicks = 0;
        int totReactTicks = 0;
        int lead = -1;
        float leadDistance = 0.0f;
        AmbientGoal goal = AmbientGoal::RandomDrive;
    };
    DebugCar debug(int carId) const;
    // Fills every road open to traffic regardless of the player's room.
    void populateAll();
    // OpenMM2: the menu density changed during a race. The pool is built again at it (init), and the
    // owner repopulates the cars round the player (ai::World::setTrafficDensity).
    void setDensity(float density) {
        m_settings.density = density;
        init();
    }
    float density() const { return m_settings.density; }
    int poolFree() const;

private:
    enum class Rail : std::uint8_t { Lane = 0, Turn = 1, LaneChange = 2, Regain = 3 };

    void init(); // aiMap::Init's part for the ambient pool

    struct Car {
        // Pool slot, fixed for the session (aiVehicleAmbient ctor / Init).
        int type = 0;
        float paint = 0.0f;
        int blinkPhase = 0; // aiVehicleInstance +0x18 (DrawGlow reads its low byte)
        float laneRandomness = 0.0f; // aiRailSet +0x24
        int totReactTicks = 8;
        float exceedLimit = 0.0f; // +0x48
        float accelFactor = 5.0f; // +0x54
        float separation = 1.0f;  // +0x58
        float frontBumper = 2.0f, backBumper = 2.0f, leftSide = 1.0f, rightSide = 1.0f;
        bool active = false;
        bool wreck = false;
        int spawns = 0; // placeCar count (AmbientCar::spawns)
        int regainAttempts = 1; // +0xea
        // aiVehicleSpline::UpdateObstacleMap's state (+0xdc id, +0xde type,
        // +0xe0 room hint, +0xe2 section, +0xe4 side).
        int mapId = -1, mapType = -1, mapRoom = 0, mapVert = -1, mapSide = 0;
        Vec3 regainStart;

        // Rail (aiRailSet).
        int path = -1, dir = 1;
        int lane = 0;     // logical lane (+0x30)
        int drawLane = 0; // lane whose list and vertices it uses (+0x2c)
        int nextPath = -1, nextDir = 1, nextLane = 0;
        Rail rail = Rail::Lane;
        int section = 1;            // +0x2a: end vertex of the current segment
        float roadDist = 0.0f;      // +0x14
        float laneChangeDist = 0.0f; // +0x18
        float segDist = 0.0f;       // +0x1c
        float segLen = 1.0f;        // +0x20
        float curve[2][4] = {};     // x and z cubic coefficients (+0x60, +0x70)
        float turnY0 = 0.0f, turnY1 = 0.0f; // heights at the ends of a turn / regain curve
        bool enterInt = false;
        float speed = 0.0f, accel = 0.0f, target = 0.0f;
        float reactDist = kIntersectionReactDist;
        int curReactTicks = 0;
        float tireRotation = 0.0f;

        // Goals.
        AmbientGoal goal = AmbientGoal::RandomDrive;
        int goalTicks = 0;        // the running goal's counter; Reset runs at 0
        bool laneChangeOk = false; // aiGoalRandomDrive +0x10
        bool atStopSign = false;   // aiGoalRandomDrive +0x12
        // aiGoalAvoidPlayer: the player it avoids (aiVehicleSpline +0xe6, a
        // slot of the player list), its heading and offsets.
        int avoidSlot = 0;
        float heading = 0.0f, passOffset = 0.0f;
        bool centred = false;
        // aiGoalRegainRail.
        float regainBase = 0.0f, regainLength = kRegainDistance;

        TurnSignal signal = TurnSignal::None;
        bool horn = false;
        bool physical = false;
        unsigned moverFlags = 0; // DeclareMover of the rail instance this step
        bool fitted = false; // +0xe8
        Mat34 transform;
    };

    // Lane geometry (aiPath helpers) for (path, dir, lane).
    const Lane* laneOf(int path, int dir, int lane) const;
    float laneLength(int path, int dir, int lane) const;
    Vec3 xAxisAt(int path, int dir, int i) const;
    Vec3 lanePoint(const Car& c, int path, int dir, int lane, int i) const;
    Vec3 subSectionDir(int path, int dir, int i, float scale) const;
    Vec3 entryVector(int path, int dir, float scale) const;
    Vec3 exitVector(int path, int dir, float scale) const;
    Vec3 entryPoint(int path, int dir, int lane, float d) const;
    Vec3 subSectionPoint(int path, int dir, int lane, int i, float d) const;
    int index(int path, int dir, int lane, float dist) const;
    float subSectionDist(int path, int dir, int lane, float dist) const;
    float turnLength(const Car& c) const;
    void setCurve(Car& c, const Vec3& p0, const Vec3& p1, const Vec3& m0, const Vec3& m1);
    Vec3 curvePoint(const Car& c, float t, Vec3* direction = nullptr) const;
    // aiRailSet::CalcRailPosition (and, with `direction`, CalcRailPosOrient:
    // the rail's tangent there).
    Vec3 railPosition(const Car& c, float dist, Vec3* direction = nullptr) const;
    // aiPath::DetermineRoadPosInfo: the first vertex of `path` the car's
    // position lies before (along the section's z axis) within the road's
    // half width, the side it drives (from its matrix), the lane whose
    // lateral bounds hold it (else lane 0) and its distance along that lane.
    // False ("Position is not on road segment") leaves the outputs alone.
    bool roadPosInfo(int path, const Mat34& m, int& vert, float& dist, int& lane, int& dir) const;
    // aiPath::RoadDistance on `path`: from vertex 1 on, the first vertex of
    // row `lane` of side `side` (the side's sidewalk row when `lane` is its
    // lane count; higher lanes are clamped to it) that the position lies
    // before along the section's z axis, with the position within the road's
    // half width + 5 m of the centre line: `vert` that vertex, `lateral` the
    // position's offset (along -x), `dist` the row's length to the vertex
    // less how far before it the position is. `dist` and `lateral` are left
    // alone when no vertex qualifies (`vert` is then 1).
    void pathRoadDistance(int path, const Vec3& pos, int lane, int side, int& vert, float& dist,
                          float& lateral) const;
    // aiMap::DetermineRoadPosInfo: RoadDistance on lane 0, the lane (or
    // sidewalk) whose lateral bounds hold that offset, read from the side
    // `side` of `railPath` (the car's own road, as coded; the counts are
    // `path`'s; none: the last lane), then RoadDistance again on it.
    void determineRoadPosInfo(const Vec3& pos, int railPath, int path, int side, int& vert, float& dist,
                              int& lane, float& lateral) const;
    // aiMap::PredictAmbIntersectionPath / PredictAmbFreewayIntersectionPath:
    // the road leaving `node` that best matches the heading of `m`.
    int predictIntersectionPath(int node, const Mat34& m, bool freeway) const;

    // Lane queues (aiPath per-lane vehicle lists, front-most first).
    std::vector<int>& queue(int path, int dir, int lane);
    const std::vector<int>* queueOf(int path, int dir, int lane) const;
    void pushVehicle(int car, int path, int dir, int lane);
    void popVehicle(int car, int path, int dir, int lane);
    void addVehicle(int car, int path, int dir, int lane, float dist);
    void removeVehicle(int car, int path, int dir, int lane);
    int ahead(int car, int lane) const;
    void resetReactTicks(int car);

    // Population.
    // aiMap::AdjustAmbients for player `slot` (its bit in the roads' masks).
    void adjustAmbients(int oldRoom, int newRoom, int slot);
    void activate(int path);
    void clearPath(int path);
    bool placeCar(int slot, int path, int dir, int lane, float dist);
    void returnToPool(int car);
    int pickType();

    // aiGoalRandomDrive.
    bool chooseNext(Car& c);
    void resetRandomDrive(Car& c);
    void updateRandomDrive(int idx, float dt);
    float speedLimit(const Car& c) const;
    float distanceToIntersection(const Car& c) const;
    float distanceToVehicle(const Car& a, const Car& b) const;
    bool okayToEnter(int idx, float dist);
    bool upcomingAccident(const Car& c) const;
    bool roadCapacity(int idx) const;
    bool anyVehiclesComingThisWay(const Car& c) const;
    void avoidCollision(Car& c, const Car& lead, float d);
    void solveVelocity(int idx, float dt);
    bool solveRailType(int idx);
    void solveLane(Car& c);
    void changeLanes(int idx);
    void solvePose(Car& c);

    // Stop signs (aiIntersection).
    bool stopSignOkayToGo(int node, int car);
    void removeFromStopSign(int node, int car);

    // Player reactions.
    bool detectPlayerCollision(const Car& c, const PlayerCar& p) const;
    bool detectPlayerZoneCollision(const Car& c, const PlayerCar& p) const;
    bool playerInFront(const Car& c, const PlayerCar& p) const;
    bool ambientBlockingPlayer(int idx, const PlayerCar& p) const;
    void updateAvoidPlayer(int idx, float dt);
    void resetRegainRail(int idx);
    void updateRegainRail(int idx, float dt);
    void fitOffRail(Car& c);

    void updateCar(int idx, float dt);
    void publish();

    const RoadNetwork& m_net;
    TrafficLights& m_lights;
    std::vector<VehicleData> m_types;
    TrafficSettings m_settings;
    float m_density = 0.0f; // aiMap +0x3c
    std::uint64_t m_seed = 1; // ResetRandomSeed's value for the own stream
    Random m_ownRng;          // the stream when none is shared
    Random* m_rng;            // the stream drawn from
    std::vector<Car> m_cars;
    std::vector<int> m_pool; // free cars, last = next to use (aiMap +0x44)
    std::vector<AmbientCar> m_public;
    std::vector<int> m_avoidEvents;
    // The obstacle map: per path, the side -1 and side 1 lists by section;
    // per intersection one list (most recent first, as MM2 pushes them).
    std::vector<std::array<std::vector<std::vector<int>>, 2>> m_roadObstacles;
    std::vector<std::vector<int>> m_nodeObstacles;
    const MapView* m_map = nullptr;
    std::vector<const ExternalVehicle*> m_externals; // entries m_cars.size() + k
    bool inAccident(int entry) const;
    void updateObstacleMap(int idx);
    std::vector<int>* obstacleList(int path, int side, int bucket);
    std::vector<std::vector<int>> m_queues;      // per network lane
    std::vector<std::uint16_t> m_pathActive;     // aiPath AddAmbPlayer mask: a bit per player slot
    std::vector<int> m_activePaths;              // aiMap +0x17c, most recent first
    std::vector<std::vector<int>> m_stopWaiting; // per intersection (aiIntersection +0x8)
    std::vector<std::uint8_t> m_alwaysStop;      // per path (aiPath +0x162)
    std::vector<std::vector<int>> m_stopAllowed; // per intersection (aiIntersection +0xc)
    std::vector<Vec3> m_opponents;
    ImpactHandler m_onImpact;
    GroundProbe m_probe;
    bool m_started = false;
    bool m_populateAll = false;
    // The player list of this step (aiMap +0x170) with their slots and, per
    // slot, the last car seen there (a car avoiding a player who left keeps
    // avoiding where it was, inferred), the room its roads were populated
    // for and whether it is in the list.
    std::vector<PlayerCar> m_players;
    std::vector<int> m_playerSlots;
    std::array<PlayerCar, kMaxTrafficPlayers> m_slotCars{};
    std::array<int, kMaxTrafficPlayers> m_slotRooms{};
    std::array<bool, kMaxTrafficPlayers> m_slotPresent{};
    bool anyPlayerWithin(const Vec3& p, float radius2) const;
};

} // namespace mm2::ai
