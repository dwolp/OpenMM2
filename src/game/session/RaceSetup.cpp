#include "game/session/RaceSetup.h"

#include "city/Mm1AiMap.h"

#include "ai/Random.h"
#include "city/RoomInfo.h"
#include "core/Libm.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/session/Gate.h"

#include <cmath>
#include <cstdlib>
#include <format>

namespace mm2::game::session {
namespace {

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

std::optional<std::string> readText(const vfs::Vfs& vfs, const std::string& path) {
    auto bytes = vfs.readAll(path);
    if (!bytes)
        return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

std::optional<std::vector<Checkpoint>> loadCheckpoints(const vfs::Vfs& vfs, const std::string& path,
                                                       bool loop) {
    auto text = readText(vfs, path);
    if (!text)
        return std::nullopt;
    auto points = city::parseWaypoints(*text);
    if (!points || points->empty())
        return std::nullopt;
    return buildCheckpoints(*points, loop);
}

std::optional<city::RaceMode> raceMode(GameMode m) {
    switch (m) {
    case GameMode::Blitz: return city::RaceMode::Blitz;
    case GameMode::Circuit: return city::RaceMode::Circuit;
    case GameMode::Checkpoint: return city::RaceMode::Checkpoint;
    case GameMode::CrashCourse: return city::RaceMode::CrashCourse;
    default: return std::nullopt;
    }
}

// Heading that faces from `from` towards `to`, as mmWaypoints computes it
// for waypoints stored with heading 0.
float headingTowards(const Vec3& from, const Vec3& to) {
    return libm::atan2(from.x - to.x, from.z - to.z) * -57.295776f;
}

} // namespace

std::vector<Checkpoint> buildCheckpoints(const std::vector<city::Waypoint>& points, bool loop) {
    std::vector<Checkpoint> out;
    out.reserve(points.size());
    for (const auto& p : points) {
        // mmPositions::Load reads the radius with atoi; 0 becomes 15.
        int radius = static_cast<int>(p.radius);
        if (radius == 0)
            radius = 15;
        Checkpoint cp = makeCheckpoint(p.position, p.heading, static_cast<float>(radius));
        // Column 6 ("frame rate" in the headers) is the hit flag.
        cp.hitByRadius = !p.extra.empty() && std::atoi(p.extra[0].c_str()) != 0;
        out.push_back(cp);
    }
    // A previous waypoint (from the second on) with heading 0 turns towards
    // this one; circuits also turn a zero-heading last waypoint towards the
    // first (mmWaypoints::LoadCSV / ReInit).
    auto setHeading = [](Checkpoint& cp, float heading) {
        cp.headingDeg = heading;
        calculateGatePoints(cp.position, heading, cp.radius, cp.gateA, cp.gateB);
    };
    const std::size_t n = out.size();
    for (std::size_t i = 2; i < n; ++i)
        if (out[i - 1].headingDeg == 0.0f)
            setHeading(out[i - 1], headingTowards(out[i - 1].position, out[i].position));
    if (loop && n > 1 && out[n - 1].headingDeg == 0.0f)
        setHeading(out[n - 1], headingTowards(out[n - 1].position, out[0].position));
    // Which stand is the "pt_finish" depends on the waypoint type
    // (mmWaypoints::LoadCSV): the caller marks it.
    out.front().start = true;
    return out;
}

Vec3 headingDirection(float headingDeg) {
    const float h = headingDeg * kDegToRad;
    return {libm::sin(h), 0.0f, -libm::cos(h)};
}

Mat34 spawnAt(const Checkpoint& cp) {
    // Mat34::rotationY(a) faces (-sin a, 0, -cos a); the waypoint heading h
    // faces (sin h, 0, -cos h), so a = -h (the original's GetStartAngle *
    // -0.017453292).
    Mat34 m = Mat34::rotationY(-cp.headingDeg * kDegToRad);
    m.m3 = cp.position;
    return m;
}

ResetPlace startPlace(const Checkpoint& cp) {
    return {cp.position, cp.headingDeg * -0.017453292f};
}

Vec3 findGroundPos(const Vec3& p, const GroundProbe& probe) {
    if (!probe)
        return p;
    const Vec3 from{p.x, p.y + 7.5f, p.z};
    const Vec3 to{p.x, p.y - 15.0f, p.z};
    if (auto hit = probe(from, to))
        return *hit;
    return p;
}

int& respawnCounter() {
    static int counter = 0;
    return counter;
}

std::optional<RespawnPick> respawnXYZ(const city::CityData& city, const RoomLookup& findRoom,
                                      RespawnRules rules, std::uint32_t& stream, int draws) {
    if (!city.aiMap || city.aiMap->intersections.size() < 2)
        return std::nullopt;
    const auto& xs = city.aiMap->intersections;
    const auto& paths = city.aiMap->paths;
    // The level's room flags (lvlRoomInfo): water of death and terrain
    // instances always (0x24), subterranean and covered rooms (0x0A) with the
    // second rule.
    using namespace city::LevelRoomFlag;
    const std::uint16_t rejectedRooms = static_cast<std::uint16_t>(
        WaterOfDeath | TerrainInstance | (rules.noCovered ? Subterranean | Covered : 0));
    // aiPath +0xc: 0x4 a freeway, 0x2 an alley (mm2hook's names).
    const std::uint16_t rejectedRoads =
        static_cast<std::uint16_t>((rules.noFreeways ? 0x4 : 0) | (rules.noCovered ? 0x2 : 0));
    auto fits = [&](const city::AiIntersection& x) {
        const int room = findRoom ? findRoom(x.center) : x.room;
        if (room >= 0 && static_cast<std::size_t>(room) < city.levelRoomFlags.size() &&
            (city.levelRoomFlags[static_cast<std::size_t>(room)] & rejectedRooms))
            return false;
        for (const auto id : x.paths)
            if (id < paths.size() && (paths[id].flags & rejectedRoads))
                return false;
        return true;
    };
    // MM2 retries until one fits and would never return from a city where
    // none does: OpenMM2 checks that one exists first (the same pick whenever
    // MM2 returns at all).
    bool any = false;
    for (std::size_t i = 1; i < xs.size() && !any; ++i)
        any = fits(xs[i]);
    if (!any)
        return std::nullopt;
    ai::Random rng;
    rng.seed(stream);
    const int count = static_cast<int>(xs.size());
    int index = 0;
    for (;;) {
        for (int k = 0; k < draws; ++k)
            index = rng.irand() % (count - 1) + 1;
        if (fits(xs[static_cast<std::size_t>(index)]))
            break;
    }
    stream = rng.state();
    const Vec3& c = xs[static_cast<std::size_t>(index)].center;
    return RespawnPick{{c.x, c.y + 2.0f, c.z}, 0.0f, index};
}

std::optional<RespawnPick> cruiseStart(const city::CityData& city, const RoomLookup& findRoom,
                                       bool multiplayer, std::uint32_t globalSeed, std::uint32_t playerSeed) {
    const RespawnRules rules{true, true};
    int& counter = respawnCounter();
    if (!multiplayer) {
        std::uint32_t stream = globalSeed;
        return respawnXYZ(city, findRoom, rules, stream, counter + 1);
    }
    // mmMultiRoam / mmMultiCR::Reset, then InitNetworkPlayers: each seeds
    // the second stream with the player's id (DisableGlobalSeed, the
    // global stream itself untouched) and counts one more call.
    std::optional<RespawnPick> pick;
    for (int call = 0; call < 2 && city.aiMap; ++call) {
        std::uint32_t stream = playerSeed;
        pick = respawnXYZ(city, findRoom, rules, stream, counter + 1);
        counter = (counter + 1) % 100;
    }
    return pick;
}

Vec3 multiplayerGridOffset(int slot, bool longVehicle) {
    static constexpr std::array<Vec3, 8> kShort{{{2.25f, 0, 6}, {-2.25f, 0, 6}, {4.5f, 0, 0}, {0, 0, 0},
                                                  {-4.5f, 0, 0}, {4.5f, 0, -6}, {0, 0, -6}, {-4.5f, 0, -6}}};
    static constexpr std::array<Vec3, 8> kLong{{{2.75f, 0, 16}, {-2.75f, 0, 16}, {5.5f, 0, 16}, {0, 0, 16},
                                                 {2.75f, 0, 34}, {-2.75f, 0, 34}, {0, 0, 34}, {5.5f, 0, 34}}};
    if (slot < 0 || slot > 7)
        return {};
    return (longVehicle ? kLong : kShort)[static_cast<std::size_t>(slot)];
}

void applyRaceTableDefaults(RaceConfig& cfg, const city::RaceDefinition* race) {
    if (cfg.mode == GameMode::Cruise) {
        // RaceMenuBase::SetStateRace for cruise.
        cfg.timeOfDay = TimeOfDay::Noon;
        cfg.weather = Weather::Clear;
        cfg.pedestrianDensity = 0.25f;
        cfg.trafficDensity = 0.5f;
        cfg.copDensity = 1.0f;
        return;
    }
    if (!race || !race->settings)
        return;
    const auto& s = cfg.difficulty == Difficulty::Professional ? race->settings->professional
                                                               : race->settings->amateur;
    cfg.timeOfDay = static_cast<TimeOfDay>(std::clamp(s.timeOfDay, 0, 3));
    cfg.weather = static_cast<Weather>(std::clamp(s.weather, 0, 3));
    cfg.pedestrianDensity = std::clamp(s.pedDensity, 0.0f, 1.0f);
    if (cfg.mode == GameMode::CrashCourse) {
        // Lessons: the lesson table's time, weather and pedestrians, no
        // traffic, all cops (mmInterface::Update, Crash Course GO).
        cfg.trafficDensity = 0.0f;
        cfg.copDensity = 1.0f;
        cfg.opponents = 0;
        return;
    }
    cfg.trafficDensity = std::clamp(s.ambientDensity, 0.0f, 1.0f);
    // RaceMenuBase::SetStateRace keeps the race's cop count (0 to 8 in the
    // retail tables) in the cop density as it is: aiMap::Init clamps it to
    // 0..1 when it places the posts, the slider shows anything above 1 as
    // full, and the modes' RegisterFinish compare it with the count, so a
    // race with two or more cops is no longer recorded once the slider has
    // been moved.
    cfg.copDensity = static_cast<float>(s.cops);
    if (cfg.mode == GameMode::Checkpoint) {
        cfg.opponents = std::max(0, s.opponents);
        cfg.laps = 1;
    } else if (cfg.mode == GameMode::Circuit) {
        cfg.opponents = std::max(0, s.opponents);
        cfg.laps = std::max(1, s.numLaps);
    }
}

std::optional<RaceSetup> loadRaceSetup(const RaceConfig& config, const city::CityData& city, const vfs::Vfs& vfs,
                                       std::string* error) {
    RaceSetup s;
    s.config = config;
    const bool pro = config.difficulty == Difficulty::Professional;

    const auto mode = raceMode(config.mode);
    if (mode) {
        for (const auto& r : city.races)
            if (r.mode == *mode && r.index == config.raceIndex)
                s.race = &r;
        if (!s.race) {
            setError(error, std::format("{} has no {} race {}", city.info.mapName, city::raceModeName(*mode),
                                        config.raceIndex));
            return std::nullopt;
        }
        if (s.race->settings)
            s.settings = pro ? s.race->settings->professional : s.race->settings->amateur;
    }

    // Waypoints (crash courses keep theirs per event).
    if (s.race && config.mode != GameMode::CrashCourse) {
        const bool circuit = config.mode == GameMode::Circuit;
        auto cps = loadCheckpoints(vfs, s.race->waypoints, circuit);
        if (!cps) {
            setError(error, std::format("cannot read waypoints '{}'", s.race->waypoints));
            return std::nullopt;
        }
        s.checkpoints = std::move(*cps);
        // mmWaypoints::LoadCSV: circuits (type 1) make waypoint 0, the start
        // and finish of every lap, the "pt_finish"; checkpoint races (type 2)
        // the last waypoint; Blitz (type 3) uses "pt_check" for every one.
        if (circuit)
            s.checkpoints.front().finish = true;
        else if (config.mode == GameMode::Checkpoint)
            s.checkpoints.back().finish = true;
    }

    // Time limit: single player Blitz only (mmSingleBlitz::InitHUD); the
    // circuit and checkpoint tables carry 50 / 40, which their single player
    // rules never start a clock for.
    if (config.mode == GameMode::Blitz)
        s.timeLimit = s.settings.timeLimit;
    s.laps = config.mode == GameMode::Circuit ? (config.laps > 0 ? config.laps : s.settings.numLaps) : 0;
    if (config.mode == GameMode::Circuit && s.laps <= 0)
        s.laps = 1;

    // AI setup: race .aimap(_p), or roam.aimap(_p) for cruise.
    if (s.race) {
        const std::string& path = pro && !s.race->aiMapPro.empty() ? s.race->aiMapPro : s.race->aiMap;
        if (!path.empty())
            if (auto text = readText(vfs, path)) {
                s.aiMap = city::parseAiMapConfig(*text);
                if (s.aiMap && city.mm1) // OpenMM2 Enhanced: an MM1 race's
                    city::adaptMm1AiMapConfig(*s.aiMap);
            }
    } else {
        s.aiMap = pro && city.cruisePro ? city.cruisePro : city.cruise;
    }

    // Crash course events (mmSingleStunt::LoadEventFile).
    if (config.mode == GameMode::CrashCourse && s.race) {
        const std::string& table = pro && !s.race->crashEventsPro.empty() ? s.race->crashEventsPro : s.race->crashEvents;
        auto text = readText(vfs, table);
        auto events = text ? city::parseCrashEvents(*text) : std::nullopt;
        if (!events || events->empty()) {
            setError(error, std::format("cannot read crash course events '{}'", table));
            return std::nullopt;
        }
        const std::string dir = table.substr(0, table.rfind('/') + 1);
        for (const auto& e : *events) {
            LessonEvent le;
            le.rawType = e.event;
            le.type = static_cast<LessonType>(e.event);
            le.file = e.file;
            le.hasCheckpoints = e.checkpoints != 0;
            le.timeLimit = e.timeLimit;
            le.ambientDensity = e.ambientDensity;
            le.extra = e.extra;
            // cornerspeed is a float; chkflags and numopp are read with atoi.
            if (!e.extra.empty())
                le.minimumSpeedMph = e.extra[0];
            if (le.type == LessonType::MinimumSpeed && le.minimumSpeedMph < 1.0f)
                le.minimumSpeedMph = 50.0f; // mmSingleStunt::InitHUD
            // mmSingleStunt::InitNewEvent passes "chkflags != 0" to
            // mmWaypoints::ReInit as the show-only-the-next flag.
            le.singleCheckpoint = e.extra.size() > 1 && static_cast<int>(e.extra[1]) != 0;
            le.opponents = e.extra.size() > 2 ? static_cast<int>(e.extra[2]) : 0;
            auto cps = loadCheckpoints(vfs, dir + str::lower(e.file) + ".csv", false);
            if (!cps) {
                setError(error, std::format("cannot read crash course points '{}{}.csv'", dir, e.file));
                return std::nullopt;
            }
            le.checkpoints = std::move(*cps);
            for (auto& cp : le.checkpoints)
                cp.standDepth = 15.0f; // mmWaypoints::InitStatic's radius 15
            s.lessonEvents.push_back(std::move(le));
        }
        s.checkpoints = s.lessonEvents.front().checkpoints;
    }

    // Opponents: the aimap lists the field; the configuration says how many
    // race. Crash courses load all of theirs; each event's "numopp" says
    // which take part. Multiplayer has neither racers nor police:
    // mmGameMulti::Init sets the opponent and cop densities to 0 (and the race
    // modes load no AI map at all); OpenMM2's shared cruise traffic brings
    // the police back (below).
    const bool racing = config.mode == GameMode::Circuit || config.mode == GameMode::Checkpoint;
    if (s.aiMap && s.race && !config.multiplayer && (racing || config.mode == GameMode::CrashCourse)) {
        const std::string& any = !s.race->aiMap.empty() ? s.race->aiMap : s.race->waypoints;
        const std::string dir = any.substr(0, any.rfind('/') + 1);
        // aiMap::Init loads min(table count, OpponentDensity) racers; the
        // crash course sets OpponentDensity to 8 (CrashCourse::SetEnvironment).
        const int wanted = !racing ? 8 : (config.opponents >= 0 ? config.opponents : s.settings.opponents);
        for (const auto& o : s.aiMap->opponents) {
            if (static_cast<int>(s.opponents.size()) >= wanted)
                break;
            OpponentSetup op;
            op.vehicle = str::lower(o.car);
            op.pathFile = dir + str::lower(o.pathFile);
            op.params = o.params;
            if (auto text = readText(vfs, op.pathFile))
                if (auto path = city::parseOpponentPath(*text))
                    op.path = std::move(*path);
            // Grid place (aiRouteRacer::Init): the .opp's first row; its
            // fourth column (the parser's "brake") is the car's reset angle
            // in degrees, converted with the original's 0.017444445 and not
            // negated as the player's start angle is.
            if (!op.path.empty()) {
                op.spawn = Mat34::rotationY(op.path.front().brake * 0.017444445f);
                op.spawn.m3 = op.path.front().position;
            } else {
                op.spawn = s.checkpoints.empty() ? Mat34::identity() : spawnAt(s.checkpoints.front());
            }
            s.opponents.push_back(std::move(op));
        }
    }
    // OpenMM2 extra: a network cruise with the host's shared traffic has
    // the cruise's police too (the host drives them, the clients show them).
    const bool netPolice = config.multiplayer && config.mode == GameMode::Cruise && config.netTraffic;
    if (s.aiMap && (!config.multiplayer || netPolice)) {
        for (const auto& p : s.aiMap->police) {
            PoliceSetup ps;
            ps.vehicle = str::lower(p.car);
            // aiRaceData::aiRaceData stores the [Police] heading (degrees) as
            // heading x -0.017444445, MM2's own degree factor rather than
            // pi / 180 (as aiRouteRacer::Init's for the racers), and
            // aiPoliceOfficer::Reset copies it to the reset rotation.
            ps.spawn = Mat34::rotationY(p.heading * -0.017444445f);
            ps.spawn.m3 = p.position;
            ps.params = p.params;
            s.police.push_back(std::move(ps));
        }
    }

    // Player start: the first waypoint (mmWaypoints::GetStart /
    // GetStartAngle). Cruise and Cops and Robbers start where the mode's
    // InitGameObjects puts the car, and InitOtherPlayers then moves it to a
    // random AI intersection facing -Z (mmGame::RespawnXYZ, once the AI map
    // has been reset: Session::placeRespawnStart); without an AI map, the
    // city's first Blitz start.
    //
    // The car is placed there (the modes' InitGameObjects: SetResetPos and
    // vehCar::Reset); then the race modes' InitOtherPlayers settle it on the
    // ground (SimVehicle::settleOnGround), while the cruise modes leave it at
    // RespawnXYZ's point, 2 m above the intersection.
    s.playerDrop = StartDrop::OnGround;
    if (!s.checkpoints.empty()) {
        s.playerSpawn = spawnAt(s.checkpoints.front());
        s.playerPlace = startPlace(s.checkpoints.front());
        // mmMultiBlitz / mmMultiCircuit / mmMultiRace::InitNetworkPlayers:
        // the grid slot (StartXYZ, added by the race screen) goes through
        // mmGame::FindGroundPos before the one reset.
        if (config.multiplayer)
            s.playerDrop = StartDrop::FindGround;
    } else if (city.aiMap && city.aiMap->intersections.size() >= 2) {
        // mmSingleRoam::InitGameObjects: mmGame's start position, (0, 10, 0)
        // (mmGame::mmGame), and mmGameSingle's angle 0; mmMultiRoam /
        // mmMultiCR::InitGameObjects: the origin.
        const Vec3 place = config.multiplayer ? Vec3{} : Vec3{0.0f, 10.0f, 0.0f};
        s.playerSpawn = Mat34::identity();
        s.playerSpawn.m3 = place;
        s.playerPlace = {place, 0.0f};
        s.playerDrop = StartDrop::None;
        s.respawnStart = true;
    } else {
        // OpenMM2's fallback for a city without an AI map (RespawnXYZ would
        // use (0, 20, 0)): the city's first Blitz start.
        for (const auto& r : city.races) {
            if (r.mode != city::RaceMode::Blitz)
                continue;
            if (auto cps = loadCheckpoints(vfs, r.waypoints, false)) {
                s.playerSpawn = spawnAt(cps->front());
                s.playerPlace = startPlace(cps->front());
                break;
            }
        }
    }
    return s;
}

} // namespace mm2::game::session
