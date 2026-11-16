#pragma once

// MM2's driving controller for AI physics cars (race opponents and police),
// aiVehiclePhysics, ported from MM2 build 3393 (function names from its
// linker map; see docs/ai.md for what is verified and what is inferred):
//
//   DriveRoute / Forward / Backup / FinishedBackingUp / Shortcut / Stop /
//   Mirror               the per-frame state machine and control laws
//                        (Driving.cpp)
//   RegisterRoute / PlanRoute / LocateWayPtFromRoad / SolveRoadTargetPoint /
//   SolveShortcutTargetPoint
//                        the waypoint intersections and the window of three
//                        aiPath roads round the car (DrivingRoute.cpp)
//   CalcRoute / EnumRoutes / IsTargetBlocked / CalcObstacleAvoidPoints /
//   EnumTargets / ContinueCheck / DetermineBestRoute
//                        the route planner: a chain of target points down the
//                        window's roads, branching round obstacles (the
//                        traffic's per-section obstacle lists), the
//                        straightest route chosen (DrivingRoute.cpp)
//   CalcRoadTarget / CalcDestinationTarget / InitRoadTurns / CalcRoadTurns /
//   CalcTurnIntersection / InSharpTurn / CalcSharpTurnTarget / SaveTurnTarget
//                        the next target down a road and the turn circles of
//                        the window's junctions and the roads' sharp turns
//                        (DrivingTargets.cpp)
//   CalcSpeed / CalcRoadSpeed
//                        braking for the turns ahead and at the destination
//   aiStuck              turning a car that is stuck in place
//
// The roads, intersections and rooms come from an ai::MapView (aiMap).
//
// The drivers write a phys::CarSim's inputs directly (as MM2 wrote
// vehCarSim Steering / Brakes / HandBrake / Engine Throttle) and select
// reverse through its transmission; do not also drive the car through
// phys::ArcadeControls.
//
// Parts of this file (aiStuck, the per-frame scaling, the OpenMM2 recovery)
// were first ported from Open1560 (MM1). Open1560 - An Open Source
// Re-Implementation of Midtown Madness 1 Beta, Copyright (C) 2020 Brick.
// GPL-3.0-or-later; OpenMM2 port under the same licence.

#include "ai/Course.h"
#include "core/Math.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace mm2::city {
class Mm1RacerAi;
}

namespace mm2::phys {
class Body;
class CarSim;
class GroundQuery;
struct CarImpact;
} // namespace mm2::phys

namespace mm2::ai {

class PhysicsDriver;

// A car the AI should know about this frame: the player, opponents, police
// and ambient traffic. The game fills one list per frame and passes it to
// every driver (trackedCar() / trackedAmbient() fill one as MM2's aiVehicle
// interface describes the car).
struct TrackedCar {
    int id = -1;              // unique per car; drivers skip their own id
    // aiVehicle::Position and GetMatrix: a physics car's inertial frame (its
    // vehCarSim ICS, about the centre of gravity), an ambient car's AI
    // matrix (model origin, on the ground).
    Vec3 position;
    Vec3 forward{0, 0, -1};   // -m2 of that matrix
    Vec3 right;               // m0 of that matrix (zero: right of `forward` in XZ)
    Vec3 velocity;            // m/s
    float speed = -1.0f;      // aiVehicle::Speed (vehCarSim Speed); < 0: |velocity|
    float halfWidth = 1.0f;   // body half extents (m)
    float halfLength = 2.3f;
    // aiVehicle::FrontBumperDistance, BackBumperDistance, LSideDistance,
    // RSideDistance: from `position` along the matrix; < 0 uses halfLength
    // or halfWidth.
    float frontBumper = -1.0f;
    float backBumper = -1.0f;
    float leftSide = -1.0f;
    float rightSide = -1.0f;
    // The box of its collision bound (vehCarModel GetBound(0), model space),
    // which aiPoliceOfficer::Block reads.
    Vec3 boundMin, boundMax;
    bool hasBound = false;
    const phys::Body* body = nullptr; // physics body, if simulated (impact matching)
    bool collided = false;    // hit something since the previous frame
    bool isPlayer = false;
    bool isPolice = false;
    bool suspect = false;     // police may pursue it (player, racers)
    bool reversing = false;   // in reverse gear (aiPoliceOfficer::Update looks at the player's)
    // Where the car is on the AI's roads, for aiObstacle::CurrentRoadIdx:
    // a racer asks its driver (aiVehiclePhysics), an ambient car the traffic
    // (aiVehicleSpline), a player carries the road and raw vertex it was last
    // found on (aiVehiclePlayer, see MapView::trackPlayer).
    const PhysicsDriver* racer = nullptr;
    int racerIndex = -1; // aiRouteRacer index (IsTargetBlocked skips its own)
    int ambient = -1;    // Traffic car index
    int playerRoad = -1;
    int playerVert = 0;
    // A prop of the roads' obstacle lists (aiBanger): its index in the
    // pedestrians' prop list, and the road (or intersection) whose list
    // holds it. `position` is aiBanger::Position (its ground origin).
    int prop = -1;
    int propComponent = -1;
    bool propOnRoad = false;
    float propRadius = 0.0f; // dgBangerData YRadius
    Vec3 propCentre;         // lvlInstance::GetPosition (centre of gravity)

    // MM2 obstacle classes (aiVehiclePhysics::IsTargetBlocked).
    bool isOpponent() const { return suspect && !isPlayer && !isPolice; }
    bool isAmbient() const { return !suspect && !isPlayer && !isPolice; }

    float currentSpeed() const { return speed >= 0.0f ? speed : velocity.mag(); }
    float front() const { return frontBumper >= 0.0f ? frontBumper : halfLength; }
    float back() const { return backBumper >= 0.0f ? backBumper : halfLength; }
    float leftDistance() const { return leftSide >= 0.0f ? leftSide : halfWidth; }
    float rightDistance() const { return rightSide >= 0.0f ? rightSide : halfWidth; }
    float boundBack() const { return hasBound ? boundMax.z : back(); }
    float boundLeft() const { return hasBound ? -boundMin.x : leftDistance(); }
    Vec3 rightAxis() const;
};

struct AmbientCar;

// A physics car as MM2's AI sees it: its ICS position and frame, its speed
// (vehCarSim Speed) and its bumper and side distances. Players
// (aiVehiclePlayer) use half the car's InertiaBox, which vehCarSim::Init
// copies into Size; AI cars (aiVehiclePhysics::Init) the box of their
// collision bound, measured from the model origin.
TrackedCar trackedCar(const phys::CarSim& car, int id, bool player);
// An ambient car (aiVehicleSpline): its AI matrix and speed, and the bumper
// and side distances aiVehicleSpline::Init takes from its aiVehicleData box
// (centred on CG).
TrackedCar trackedAmbient(const AmbientCar& car, int id);

// MM2 applies its per-frame "cheats" every frame at whatever rate it runs;
// OpenMM2 scales them to the frame time as for a 30 Hz frame:
// factor^(30 dt), with the exponent clamped to [0.01, 2].
float perFrame(float factor, float dt);

// Heading error to a target, as MM2 measures it everywhere but in Backup:
// atan2(d . m0, d . -m2) in XZ with d = target - position (radians,
// + = right).
float headingError(const Mat34& m, const Vec3& target);

// Signed speed along the car's facing direction (m/s).
float forwardSpeed(const phys::CarSim& car);

// The deceleration, in m/s^2, that aiVehiclePhysics::CalcSpeed /
// CalcRoadSpeed assume for cornering and for braking: MM2's AI grip factor
// 1.2 times 19.8. (MM1's aiGoalFollowWayPts used the same 23.76.)
inline constexpr float kAiGripFactor = 1.2f;
inline constexpr float kAiGrip = kAiGripFactor * 19.8f;
// Bends and turns sharper than this (radians) are braked for (MM2's AI
// sharp-turn angle).
inline constexpr float kSharpTurn = 0.7f;

// aiVehiclePhysics::RegisterRoute's per-route settings. Racers take them
// from their .aimap [Opponent] line (aiRaceData), police set them in code.
struct RouteParams {
    float maxThrottle = 1.0f;       // written to the car's MaxThrottle every frame
    float cornerSpeedFactor = 1.0f; // scales every corner speed (0.89 - 2.29 in the data)
    float brakeThreshold = 0.7f;    // brake when (v - vmax) / (a t) exceeds this
    float lookAhead = 50.0f;        // the route is planned this far (m)
    bool avoidTraffic = true;       // steer round ambient vehicles
    bool avoidProps = true;         // steer round the props of the roads' obstacle lists
    bool avoidPlayers = true;       // steer round the players
    bool avoidOpponents = true;     // steer round other racers (after the third waypoint)
    bool preferSidewalk = false;    // DetermineBestRoute: take a route over the sidewalk first
    float destinationSpeed = 0.0f;  // speed wanted at the destination (m/s)
    float stopShort = 0.0f;         // aim this far short of the destination (m)
};

// aiVehiclePhysics::Init sorts its car into types by name (vppanoz 0,
// vpford 1, vpmustang99 2, vppanozgt 3, vpsemi 4, vpcaddie 5, vpbug 6,
// vppolice 7, vpbullet 8, vpbus 9, others none). EnumRoutes lets every type
// but 3, the Panoz GTR-1 (vppanozgt), go over the sidewalk round an obstacle.
bool goesOverSidewalks(std::string_view vehicle);

// vehStuck settings MM2 gives AI cars (aiVehiclePhysics::Init: TimeThresh
// 0.5 s, PosThresh 1 m, no rotation recovery; aiPoliceOfficer::Init raises
// TimeThresh to 0.75 s for police).
void configureAiVehStuck(phys::CarSim& car, float timeThresh);

// aiStuck: when the car's own vehStuck starts watching (after an impact),
// the position is watched; a car that stays within PosThresh for TimeThresh
// seconds while "pegged" (throttle above 3/4 of MaxThrottle and steering
// under 0.5) is stuck, and is turned in place (RotAmount rad/s, in the
// steering direction) until it moves MoveThresh away or eases off.
class AiStuck {
public:
    enum State : int { Idle = 0, Watching = 1, Stuck = 2 };
    void reset();
    // aiVehiclePhysics::InitForward / InitShortcut clear the state only.
    void clearState() { m_state = Idle; }
    void update(phys::CarSim& car, float dt);
    int state() const { return m_state; }

    // aiStuck::aiStuck: TimeThresh 0.3 s, PosThresh 0.6 m, MoveThresh 1 m,
    // RotAmount 1 rad/s.
    float timeThresh = 0.3f;
    float posThresh = 0.6f;
    float moveThresh = 1.0f;
    float rotAmount = 1.0f;

private:
    bool pegged(const phys::CarSim& car) const;

    int m_state = Idle;
    float m_time = 0.0f;
    Vec3 m_lastPos;
};

// One target point of a route (aiRouteNode, 0x24 bytes). Node 0 is the car.
struct RouteNode {
    int obstacle = -1;     // +0x00 TrackedCar id of the obstacle this point avoids, -1 = none
    Vec3 pos;              // +0x04 the target (1 m above the road)
    float turnSum = 0.0f;  // +0x10 turning from the car's heading to here (rad), the route's cost
    float dist = 0.0f;     // +0x14 XZ path length from the car (9999: the destination)
    int road = 0;          // +0x18 window slot (0..2) of the road it lies on
    int vert = 0;          // +0x1a its vertex, counted in the direction of travel
    int kind = 0;          // +0x1c 0 road target, 1 turn target, 2 avoid point, 3 no way round
    int turnCode = 0;      // +0x1e 0/1 junction turn w, s + 2 sharp turn s (also the road slot)
    int blockKind = 0;     // +0x20 what blocked it: 0 player, 1 traffic, 2 racer, 5 prop
    int surface = 0;       // +0x22 1 road, 2 sidewalk
};

// aiVehiclePhysics::RegisterRoute's arguments: the waypoint intersections,
// the laps and the destination.
struct RouteRegistration {
    std::vector<int> wayPoints; // intersection ids
    int laps = 0;               // -1: endless
    Vec3 destination;
    Vec3 destinationHeading;    // the way the car should face there (unit, XZ; zero = any)
};

// dgPhysManager::DeclareMover's arguments for an AI car this frame: the
// mover type (2 plain, 3 also activating its room and the rooms round it;
// 0: not declared) and what it collides with (0x1 update, 0x2 the city,
// 0x8 instances, 0x10 other movers). The game hands them to its physics
// manager.
struct MoverDeclaration {
    int type = 0;
    unsigned flags = 0;
};

// The global switch the race game's setup sets, which keeps the instances
// out of the collisions of racers away from the players (and of police
// 200 - 250 m from them); inferred from the function that also registers
// the opponents' HUD icons.
inline constexpr bool kRaceGameMovers = true;

// The nearest player's squared distance (3D, ICS positions) in `cars`;
// false when there is none.
bool nearestPlayer2(const Vec3& position, std::span<const TrackedCar> cars, float& distance2);

// What the game tells a driver each frame.
struct DriveContext {
    bool repairWhenWrecked = false; // aiVehiclePhysics::Init's repair flag (circuits)
    bool touchingPlayer = false;    // collided with the player since the last frame
};

class MapView;

// aiVehiclePhysics: one AI car's controller and route planner.
class PhysicsDriver {
public:
    enum class State : std::uint8_t { Forward, Backup, Shortcut, Stop };
    static constexpr int kMaxNodes = 40;  // aiRouteNode m_Nodes[40]
    static constexpr int kMaxRoutes = 25; // m_Routes[25][40]

    // `aiId` is the aiVehicle id (the racer's or the officer's index), which
    // IsTargetBlocked compares with the racers' indices; `vehicle` the car's
    // base name (aiVehiclePhysics::Init's type table).
    PhysicsDriver(phys::CarSim& car, int selfId, const MapView* map = nullptr, int aiId = 0,
                  std::string_view vehicle = {});

    // aiVehiclePhysics::Reset: forward, nothing planned.
    void reset();
    void setState(State s) { m_state = s; }
    State state() const { return m_state; }
    bool wrecked() const { return m_wrecked; }

    RouteParams params;

    // aiVehiclePhysics::RegisterRoute: the route, the destination and the
    // window of roads round the car; the state Forward, or Shortcut when the
    // car is on no road or intersection (a car backing up keeps backing up).
    void registerRoute(const RouteRegistration& route);
    // OpenMM2 recovery (not in MM2): after a registerRoute where the car was
    // put back on its route, carry on from waypoint `wayPtIdx` of lap `lap`.
    void resumeRoute(int wayPtIdx, int lap);
    int numWayPoints() const { return static_cast<int>(m_wayPts.size()); }
    int numLaps() const { return m_numLaps; }
    // aiVehiclePhysics::DriveRoute: one frame.
    void driveRoute(float dt, std::span<const TrackedCar> cars, const DriveContext& ctx);
    // aiVehiclePhysics::Mirror: match `target`'s heading at its speed - 3 m/s.
    void mirror(float dt, const TrackedCar& target);
    // aiVehiclePhysics::StopRoadTraffic: every road of the window tells the
    // intersection it drives into to hold (or release) its controlled roads.
    template <typename StopSources> void stopRoadTraffic(bool stop, StopSources&& stopSources) const {
        for (int w = 0; w < 3; ++w) {
            const int node = windowIntersectionAhead(w);
            if (node >= 0)
                stopSources(node, stop);
        }
    }
    // aiVehiclePhysics::CurrentRoadIdx: the slot of this car's road in another
    // car's window (and the vertex there), or -1.
    int currentRoadIdx(const int roads[3], const bool dirs[3], int* vert) const;

    Vec3 target() const { return m_target; }
    int backups() const { return m_backups; }
    float throttle() const { return m_throttle; }
    float brake() const { return m_brake; }
    float steering() const { return m_steering; }
    // aiVehiclePhysics::LSideDistance / RSideDistance.
    float leftSide() const { return m_leftSide; }
    float rightSide() const { return m_rightSide; }
    phys::CarSim& car() { return m_car; }
    const phys::CarSim& car() const { return m_car; }
    int aiId() const { return m_aiId; }

    // The planner's state, for tests and diagnostics.
    int windowRoad(int slot) const { return m_roads[slot]; }
    bool windowForward(int slot) const { return m_roadDir[slot]; }
    // aiVehiclePhysics::FrontBumperDistance.
    float frontBumper() const { return m_frontBumper; }
    int wayPointIndex() const { return m_wayPtIdx; }
    int lap() const { return m_curLap; }
    int numRoutes() const { return m_numRoutes; }
    int bestRoute() const { return m_bestRoute; }
    std::span<const RouteNode> route(int r) const {
        return {m_routes[static_cast<std::size_t>(r)].data(),
                static_cast<std::size_t>(m_routeNodeCount[static_cast<std::size_t>(r)])};
    }
    // The best route's nodes (empty when none).
    std::span<const RouteNode> bestNodes() const {
        return m_bestRoute < 0 ? std::span<const RouteNode>{} : route(m_bestRoute);
    }

private:
    // State machine (Driving.cpp).
    void initForward();
    void forward(float dt, std::span<const TrackedCar> cars, const DriveContext& ctx);
    void initBackup();
    void backup(float dt);
    void finishedBackingUp();
    // OpenMM2 Enhanced, a car on MM1's physics: MM1's aiGoalBackup.
    void backupMm1();
    void initShortcut();
    void shortcut(float dt, std::span<const TrackedCar> cars);
    void stop();
    void calcSpeed(float dt);
    void calcRoadSpeed(float dt);
    float checkDistance(int turn) const;
    void applyBrake(float brake, float dt);
    void apply();
    bool handleStuck(float dt);
    bool undrivable();

    // Waypoints and the road window (DrivingRoute.cpp).
    void destMapComponent(const Vec3& pos, int& id, int& type) const;
    int planRoute();
    int locateWayPtFromRoad(int road);
    void solveRoadTargetPoint(std::span<const TrackedCar> cars);
    Vec3 mm1RailTarget(const Vec3& fallback); // MM1 look-ahead along the route
public:
    // OpenMM2 Enhanced: a racer on MM1's physics follows MM1's own road network and rail
    // (city::Mm1RacerAi); the pointer is the opponent's, null keeps the MM2 route driver.
    // Debug: whether MM2's avoidance replaced the rail's aim this frame, and the nearest ambient car
    // ahead (distance in m, -1 none within 60 m).
    bool debugAvoiding() const { return m_dbgAvoiding; }
    float debugNearestAmbientAhead() const { return m_dbgNearAmbient; }
    void setMm1Racer(city::Mm1RacerAi* racer) {
        m_mm1Racer = racer;
        m_mm1RacerInited = false;
    }

private:
    city::Mm1RacerAi* m_mm1Racer = nullptr;
    bool m_dbgAvoiding = false;
    float m_mm1Shift = 0.0f;        // the rail aim's sideways shift round a car in the way (m, right positive)
    bool m_mm1Blocked = false;      // a slower ambient car ahead in the lane
    float m_mm1BlockAhead = 0.0f, m_mm1BlockSpeed = 0.0f, m_mm1BlockSide = 0.0f;
    float m_dbgNearAmbient = -1.0f;
    bool m_mm1RacerInited = false;
    float m_mm1LaneOffset = 1e9f; // an MM1 racer's place right of the lane line (1e9: not taken yet)
    void solveShortcutTargetPoint();
    int windowIntersectionAhead(int slot) const;

    // The planner (DrivingRoute.cpp).
    void calcRoute(std::span<const TrackedCar> cars);
    void determineBestRoute();
    void enumRoutes(int i);
    int calcObstacleAvoidPoints(const TrackedCar& obstacle, int i, bool allowSidewalk, Vec3* pts, int* obs,
                                int* surface, int* kind);
    void enumTargets(const Vec3& pt, const TrackedCar& obstacle, int i, int roadSlot, int roadVert, int side,
                     bool allowSidewalk, int depth, Vec3* pts, int* obs, int* surface, int* kind, int* count);
    const TrackedCar* isTargetBlocked(const Vec3& from, const Vec3& to, int startVert, int startSlot,
                                      int endVert, int endSlot, float extra, int* kind);
    void saveTarget(int i, const Vec3& pt, const TrackedCar& obstacle, int surface, int blockKind, int kind);
    void continueCheck(int i);
    void setTargetPtToDestination(int i);
    void nodeTurnAndDistance(int i);
    int obstacleRoadIdx(const TrackedCar& o, int* vert) const;
    const TrackedCar* trackedById(int id) const;
    const TrackedCar* ambientObstacle(int index) const;
    // Prop `index` as listed by road or intersection `component` (one aiBanger
    // per listing, as AddBangersToObsMap makes them).
    const TrackedCar* propObstacle(int index, int component, bool onRoad);

    // Targets and turns (DrivingTargets.cpp).
    void calcRoadTarget(int i, Vec3& from);
    void calcDestinationTarget(int i, Vec3& from);
    void initRoadTurns();
    void calcRoadTurns();
    float calcTurnIntersection(int w);
    float calcCurrentMaxWidthAdjustment(int w) const;
    float calcCurrentRdOffset(int w) const;
    float calcNextMaxWidthAdjustment(int w) const;
    float calcNextRdOffset(int w) const;
    float laneTrafficIntrusion(int road, bool rightList, int bucket, const Vec3& base,
                               const Vec3& side) const;
    int inSharpTurn(int i);
    int calcSharpTurnTarget(int& i, int turnNode);
    void saveTurnTarget(int i, bool calcAngle);

    const city::AiPath* road(int slot) const;
    const PathInfo* roadInfo(int slot) const;

    phys::CarSim& m_car;
    int m_selfId = -1;
    const MapView* m_map = nullptr;
    int m_aiId = 0;
    int m_vehicleType = -1; // aiVehiclePhysics +0x968a
    // aiVehiclePhysics::Init: the car's bumper and side distances from the
    // box of its collision bound (model space): FrontBumperDistance -min z,
    // BackBumperDistance max z, LSideDistance -min x, RSideDistance max x.
    float m_frontBumper = 2.0f;
    float m_backBumper = 2.0f;
    float m_leftSide = 1.0f;
    float m_rightSide = 1.0f;
    State m_state = State::Forward;
    State m_lastState = State::Stop;
    AiStuck m_stuck;

    float m_throttle = 0.0f; // aiVehiclePhysics 0x9694
    float m_brake = 0.0f;    // 0x9690
    float m_steering = 0.0f; // 0x9698
    Vec3 m_target;           // 0x9668
    int m_backupFrames = 0;
    float m_backupTime = 0.0f;
    int m_backups = 0;
    bool m_wrecked = false;
    float m_wreckTime = 0.0f;

    // The cars of this frame (for the obstacle lookups).
    std::span<const TrackedCar> m_cars;
    std::map<std::tuple<int, int, bool>, TrackedCar> m_propObstacles; // by (prop, component, on a road)

    // Waypoints (RegisterRoute).
    std::vector<int> m_wayPts; // +0x9674
    int m_wayPtIdx = 1;        // +0x967a
    int m_curLap = 1;          // +0x9682
    int m_numLaps = 0;         // +0x9684
    int m_numWayPtRoads = 0;   // +0x968e
    int m_curRoom = 0;         // +0x967c
    int m_curCompType = 3;     // +0x967e
    int m_curCompId = 1;       // +0x9680
    Vec3 m_dest;               // +0x9648
    Vec3 m_destHeading;        // +0x9654
    int m_destCompType = 0;    // +0x9660
    int m_destCompId = 0;      // +0x9662
    bool m_toDestination = false; // +0x9664
    float m_roadOffset = 0.0f; // +0x9738

    // The road window: the road the car is on and the next two (path ids,
    // -1 none) and their directions (true: with the vertex index).
    int m_roads[3] = {-1, -1, -1}; // +0x26c
    bool m_roadDir[3] = {};        // +0x268
    int m_turnsRoad = -1;          // +0x278

    // The window's junction turns (road w into road w + 1).
    float m_turnAngle[2] = {};  // +0x96a0
    float m_turnDir[2] = {};    // +0x96a8
    Vec3 m_turnRef;             // +0x96b0
    Vec3 m_turnRefStart;        // +0x96bc
    Vec3 m_turnCorner[2];       // +0x96c8
    Vec3 m_turnCenter[2];       // +0x96e0
    Vec3 m_turnStartDir[2];     // +0x96f8
    Vec3 m_turnEndDir[2];       // +0x9710
    float m_turnSetback[2] = {}; // +0x9728
    float m_turnRadius[2] = {};  // +0x9730

    // The working route and the candidates.
    std::array<RouteNode, kMaxNodes> m_nodes{};                                 // +0x2d4
    std::array<std::array<RouteNode, kMaxNodes>, kMaxRoutes> m_routes{};         // +0x874
    std::array<int, kMaxRoutes> m_routeBlocked{};                                // +0x9518
    std::array<int, kMaxRoutes> m_routeOnSidewalk{};                             // +0x957c
    std::array<int, kMaxRoutes> m_routeNodeCount{};                              // +0x95e0
    int m_numRoutes = 0;                                                         // +0x9644
    int m_bestRoute = -1;                                                        // +0x9514
    // m_Routes[-1] is m_Nodes in MM2 (best route -1 reads the working route).
    const std::array<RouteNode, kMaxNodes>& routeOrWorking(int r) const {
        return r < 0 ? m_nodes : m_routes[static_cast<std::size_t>(r)];
    }
};

// Rotates a car about the vertical axis through its centre of gravity.
void yawInPlace(phys::CarSim& car, float angle);

// Puts a car back on a course at arc length `s`, facing along it, at the
// place across the road (`side` preferred) farthest from the other cars
// (OpenMM2 recovery when an AI car has made no progress for a long time, or
// fell off the world; not in MM2). Keeps the car's damage. The car lands
// as MM2 places a racer: vehCarSim::SetResetPos at the road under the
// place raised by 0.9 m (mmGame::CollideAIOpponents' settling, the wheels'
// probe from 2 m above to 10 m below), turned about Y (vehCarSim::Reset);
// the car's own reset position is left alone.
void placeOnCourse(phys::CarSim& car, const Course& course, float s, float side, std::span<const TrackedCar> others,
                   int selfId, const std::function<void(const Mat34&)>& resetCar = {},
                   const phys::GroundQuery* world = nullptr);

// True when the straight line from `a` to `b`, `height` m above both, hits
// the static level geometry (false without `world`).
bool blocked(const phys::GroundQuery* world, const Vec3& a, const Vec3& b, float height = 1.0f);

// aiVehicle::IsBlockingTarget: how far along the way from `from` to `to`
// the box of `obstacle` lies in the path of a car `width` wide (-1: clear).
// A corner of the box blocks when it is ahead, within the way plus `extra`,
// within width / 2 + 1 m of the line and within 0.7 rad of it.
float blockingDistance(const TrackedCar& obstacle, const Vec3& from, const Vec3& to, float extra,
                       float width);

// aiVehicle::PreAvoid: the points to pass `obstacle` on the left and on the
// right, seen from `from` looking along `dir`: each corner of its box pushed
// `clearance` sideways, the leftmost and rightmost of the eight.
void avoidPoints(const TrackedCar& obstacle, const Vec3& from, const Vec3& dir, float clearance, Vec3& left,
                 Vec3& right);

} // namespace mm2::ai
