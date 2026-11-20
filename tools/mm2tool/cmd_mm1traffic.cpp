// mm1traffic: runs the AI of an MM1 city headless and reports cars that move
// backwards, too fast, jump, or leave their lane (OpenMM2 Enhanced).
#include "Command.h"
#include "Common.h"

#include "ai/World.h"
#include "city/CityData.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <print>

namespace mm2::tool {
namespace {

struct Track {
    Vec3 last;
    int seen = 0;
    int backward = 0, fast = 0, jumps = 0, offLane = 0, still = 0;
    float maxSpeed = 0.0f, maxOff = 0.0f;
    int prevLane = -1, prevNext = -1;
    std::string model;
    Vec3 worst;
};

int cmdMm1Traffic(std::span<char* const> args) {
    if (args.empty())
        return 2;
    const float seconds = args.size() > 1 ? static_cast<float>(std::atof(args[1])) : 120.0f;
    // The player parked somewhere (cars spawn around it): x z, default the start.
    Vec3 player{-94.0f, 2.0f, -444.0f};
    if (args.size() > 3)
        player = {static_cast<float>(std::atof(args[2])), 2.0f, static_cast<float>(std::atof(args[3]))};
    vfs::Vfs v;
    std::string err;
    if (!vfs::mountMm1Source(v, str::toPath(args[0]), &err)) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    auto city = city::loadCity(v, "chicago", &err);
    if (!city || !city->mm1) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    auto world = ai::World::create(*city, v, {}, nullptr, &err);
    if (!world) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    if (city->mm1->cellLocator) {
        const city::Mm1CellLocator* loc = &*city->mm1->cellLocator;
        world->setRoomFinder([loc](const Vec3& p, int hint) { return loc->find(p, hint); });
    }
    const auto& net = world->network();
    if (const char* n = std::getenv("MM1TRAFFIC_NODE")) {
        const int node = std::atoi(n);
        const auto& inter = net.intersections()[static_cast<std::size_t>(node)];
        std::println("node {} at ({:.1f}, {:.1f}), {} roads:", node, inter.centre.x, inter.centre.z, inter.paths.size());
        for (std::size_t k = 0; k < inter.paths.size(); ++k) {
            const auto& pi = net.paths()[static_cast<std::size_t>(inter.paths[k])];
            std::println("  [{}] path {}: ends {} / {}, roadIndex {} / {}, centre ({:.1f}, {:.1f}) -> ({:.1f}, {:.1f})", k,
                         inter.paths[k], pi.intersection[0], pi.intersection[1], pi.roadIndex[0], pi.roadIndex[1],
                         pi.centreStart.x, pi.centreStart.z, pi.centreEnd.x, pi.centreEnd.z);
        }
    }
    // The frames against the lanes: each side's first lane and the centre
    // line should run the same way (increasing section index).
    if (city->aiMap) {
        int checked = 0, rightBad = 0, leftBad = 0, zBad = 0, wBad = 0;
        for (const auto& path : city->aiMap->paths) {
            if (path.center.size() < 2)
                continue;
            ++checked;
            const Vec3 dc = path.center.back() - path.center.front();
            for (int sd = 0; sd < 2; ++sd) {
                const auto& side = sd == 0 ? path.right : path.left;
                if (side.numLanes == 0 || side.polylines.empty() || side.polylines[0].size() < 2)
                    continue;
                const Vec3 dl = side.polylines[0].back() - side.polylines[0].front();
                if (dl.x * dc.x + dl.z * dc.z < 0.0f)
                    ++(sd == 0 ? rightBad : leftBad);
            }
            // z points back: against the centre line's direction.
            if (path.zAxis.size() >= 2 && path.zAxis[0].x * dc.x + path.zAxis[0].z * dc.z > 0.0f)
                ++zBad;
            if (path.wAxis.size() >= 2 && path.wAxis[0].x * dc.x + path.wAxis[0].z * dc.z < 0.0f)
                ++wBad;
        }
        std::println("frames: {} paths; right lanes against the centre line {}, left {}, z forward {}, w backward {}",
                     checked, rightBad, leftBad, zBad, wBad);
    }
    // mm1traffic <src> <sec> <x> <z> <car id>: one car's trace.
    const int traceId = args.size() > 4 ? std::atoi(args[4]) : -1;
    bool described = false;
    std::println("{} lanes, {} intersections, ambient types:", net.lanes().size(), net.intersections().size());
    std::map<int, Track> tracks;
    std::map<std::string, int> models;
    const int steps = static_cast<int>(seconds / ai::kAiStepSeconds);
    for (int i = 0; i < steps; ++i) {
        world->step(player, Vec3{});
        for (const auto& car : world->cars()) {
            Track& t = tracks[car.id * 1000 + car.spawns];
            if (t.seen == 0)
                models[car.model]++;
            t.model = car.model;
            if (t.seen > 0) {
                const Vec3 d = car.transform.m3 - t.last;
                const float move = std::sqrt(d.x * d.x + d.z * d.z);
                if (move > 4.0f) {
                    ++t.jumps; // teleports (respawns count as new tracks)
                    static int logged = 0;
                    if (logged++ < 12) {
                        const auto dbg = world->traffic().debug(car.id);
                        std::println("jump {:.1f} m: car {} at t {:.1f} s, ({:.1f}, {:.1f}) -> ({:.1f}, {:.1f}), lane {} next {} "
                                     "turning {} change {} goal {} speed {:.1f}",
                                     move, car.id, static_cast<float>(i) * ai::kAiStepSeconds, t.last.x, t.last.z,
                                     car.transform.m3.x, car.transform.m3.z, dbg.lane, dbg.nextLane, dbg.turning,
                                     dbg.changingLane, static_cast<int>(dbg.goal), car.speed);
                        if (t.prevNext >= 0)
                            std::println("    it had planned lane {} (path {} dir {}) and is now on lane {}", t.prevNext,
                                         net.lanes()[static_cast<std::size_t>(t.prevNext)].path,
                                         net.lanes()[static_cast<std::size_t>(t.prevNext)].dir, dbg.lane);
                        if (t.prevLane >= 0 && dbg.lane >= 0) {
                            const auto& la = net.lanes()[static_cast<std::size_t>(t.prevLane)];
                            const auto& lb = net.lanes()[static_cast<std::size_t>(dbg.lane)];
                            const auto& node = net.intersections()[static_cast<std::size_t>(la.toIntersection)];
                            std::println("    left lane {} (path {} dir {}, {} -> {}, ends ({:.1f}, {:.1f}) heading ({:.2f}, {:.2f})) "
                                         "for lane {} (path {} dir {}, {} -> {}, starts ({:.1f}, {:.1f})); intersection {} at "
                                         "({:.1f}, {:.1f}) has {} roads",
                                         la.id, la.path, la.dir, la.fromIntersection, la.toIntersection,
                                         la.line.points.back().x, la.line.points.back().z,
                                         la.line.points.back().x - la.line.points[la.line.points.size() - 2].x,
                                         la.line.points.back().z - la.line.points[la.line.points.size() - 2].z, lb.id,
                                         lb.path, lb.dir, lb.fromIntersection, lb.toIntersection,
                                         lb.line.points.front().x, lb.line.points.front().z, la.toIntersection,
                                         node.centre.x, node.centre.z, node.paths.size());
                        }
                        if (dbg.lane >= 0 && dbg.nextLane >= 0) {
                            const auto& a = net.lanes()[static_cast<std::size_t>(dbg.lane)];
                            const auto& b = net.lanes()[static_cast<std::size_t>(dbg.nextLane)];
                            std::println("    lane {}: length {:.1f} car at s {:.1f}, end ({:.1f}, {:.1f}); next lane {} start "
                                         "({:.1f}, {:.1f}) gap {:.1f} m, {} -> {}",
                                         a.id, a.line.length, dbg.s, a.line.points.back().x, a.line.points.back().z, b.id,
                                         b.line.points.front().x, b.line.points.front().z,
                                         a.line.points.back().dist(b.line.points.front()), a.fromIntersection,
                                         a.toIntersection);
                        }
                    }
                }
                else if (move > 0.05f) {
                    // Facing is -Z of the model matrix.
                    const Vec3 face{-car.transform.m2.x, 0, -car.transform.m2.z};
                    if (face.x * d.x + face.z * d.z < -0.5f * move * face.mag())
                        ++t.backward;
                } else {
                    ++t.still;
                }
            }
            if (car.id == traceId && !described) {
                described = true;
                const auto dbg = world->traffic().debug(car.id);
                if (dbg.lane >= 0) {
                    const auto& ln = net.lanes()[static_cast<std::size_t>(dbg.lane)];
                    const auto& src = city->aiMap->paths[static_cast<std::size_t>(ln.path)];
                    std::println("lane {}: path {} side {} dir {} index {}/{}, {} points, first ({:.1f}, {:.1f}) last ({:.1f}, {:.1f}), "
                                 "from {} to {}; path centre first ({:.1f}, {:.1f}) last ({:.1f}, {:.1f}), {} sections, "
                                 "z[0] ({:.2f}, {:.2f}) w[0] ({:.2f}, {:.2f}) flags {:#x}",
                                 ln.id, ln.path, ln.side, ln.dir, ln.index, ln.count, ln.line.points.size(),
                                 ln.line.points.front().x, ln.line.points.front().z, ln.line.points.back().x,
                                 ln.line.points.back().z, ln.fromIntersection, ln.toIntersection,
                                 src.center.front().x, src.center.front().z, src.center.back().x, src.center.back().z,
                                 src.center.size(), src.zAxis[0].x, src.zAxis[0].z, src.wAxis[0].x, src.wAxis[0].z, src.flags);
                }
            }
            if (car.id == traceId && (i % 3 == 0 || std::getenv("MM1TRAFFIC_ALL"))) {
                const auto dbg = world->traffic().debug(car.id);
                std::println("t {:6.2f} pos ({:8.1f} {:7.1f} {:8.1f}) face ({:5.2f} {:5.2f}) vel ({:5.1f} {:5.1f}) speed {:5.1f} "
                             "lane {} turning {} change {} goal {}",
                             static_cast<float>(i) * ai::kAiStepSeconds, car.transform.m3.x, car.transform.m3.y,
                             car.transform.m3.z, -car.transform.m2.x, -car.transform.m2.z, car.velocity.x, car.velocity.z,
                             car.speed, dbg.lane, dbg.turning, dbg.changingLane, static_cast<int>(dbg.goal));
            }
            t.last = car.transform.m3;
            {
                const auto d2 = world->traffic().debug(car.id);
                if (d2.lane >= 0) {
                    t.prevLane = d2.lane;
                    t.prevNext = d2.nextLane;
                }
            }
            ++t.seen;
            t.maxSpeed = std::max(t.maxSpeed, car.speed);
            const auto dbg = world->traffic().debug(car.id);
            // Over 1.3 times its lane's limit (MM2's cars cruise at the limit).
            if (dbg.lane >= 0 && car.speed > 1.3f * net.lanes()[static_cast<std::size_t>(dbg.lane)].speedLimit)
                ++t.fast;
            if (!dbg.turning && !dbg.changingLane && dbg.goal == ai::AmbientGoal::RandomDrive && dbg.lane >= 0) {
                float dist = 0.0f;
                net.lanes()[static_cast<std::size_t>(dbg.lane)].line.project(car.transform.m3, &dist);
                if (dist > t.maxOff) {
                    t.maxOff = dist;
                    t.worst = car.transform.m3;
                }
                if (dist > 2.0f)
                    ++t.offLane;
            }
        }
    }
    for (const auto& [m, n] : models)
        std::println("  {} x{}", m, n);
    int cars = 0, backward = 0, fast = 0, jumpy = 0, off = 0;
    for (const auto& [id, t] : tracks) {
        ++cars;
        backward += t.backward > 5 ? 1 : 0;
        fast += t.fast > 5 ? 1 : 0;
        jumpy += t.jumps > 0 ? 1 : 0;
        off += t.offLane > 20 ? 1 : 0;
        if (t.backward > 5 || t.fast > 5 || t.offLane > 20)
            std::println("  car {} {}: {} steps, backward {}, over 1.3x the limit {} (max {:.1f} m/s), off-lane {} (max {:.1f} m at "
                         "({:.0f}, {:.0f}))",
                         id, t.model, t.seen, t.backward, t.fast, t.maxSpeed, t.offLane, t.maxOff, t.worst.x,
                         t.worst.z);
    }
    std::println("{} car tracks over {:.0f} s: {} moved backwards, {} went over 1.3x the limit, {} jumped, {} left their lane",
                 cars, seconds, backward, fast, jumpy, off);
    return 0;
}

// aimapframes <game source> <city>: whether each side's lane lines run with
// or against the centre line, in a retail (MM2) city, for comparison.
int cmdAiMapFrames(std::span<char* const> args) {
    if (args.size() < 2)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    vfs::Vfs v;
    v.mount(std::shared_ptr<const vfs::FileSystem>(std::move(fs)));
    std::string err;
    auto city = city::loadCity(v, args[1], &err);
    if (!city || !city->aiMap) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    int same[2] = {0, 0}, opposite[2] = {0, 0};
    for (const auto& path : city->aiMap->paths) {
        if (path.center.size() < 2)
            continue;
        const Vec3 dc = path.center.back() - path.center.front();
        for (int sd = 0; sd < 2; ++sd) {
            const auto& side = sd == 0 ? path.left : path.right;
            if (side.numLanes == 0 || side.polylines.empty() || side.polylines[0].size() < 2)
                continue;
            const Vec3 dl = side.polylines[0].back() - side.polylines[0].front();
            ++(dl.x * dc.x + dl.z * dc.z >= 0.0f ? same : opposite)[sd];
        }
    }
    std::println("{}: left lane lines with the centre line {}, against {}; right with {}, against {}", args[1], same[0],
                 opposite[0], same[1], opposite[1]);
    return 0;
}

const Registrar r2({"aimapframes", "<game source> <city>", "lane line directions against the centre line",
                    &cmdAiMapFrames});
const Registrar r({"mm1traffic", "<mm1 source> [seconds] [x z]",
                   "run Chicago's traffic headless and report erratic cars", &cmdMm1Traffic});

} // namespace
} // namespace mm2::tool
