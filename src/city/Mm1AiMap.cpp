#include "city/Mm1AiMap.h"

#include "city/Reader.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::city {
namespace {

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

// Generous limits for a corrupt file.
constexpr int kMaxCount = 1 << 16;

} // namespace

std::optional<Mm1AiMap> parseMm1Bai(std::span<const std::byte> data, std::string* error) {
    detail::Reader r(data);
    Mm1AiMap map;
    const int intersections = r.u16();
    const int paths = r.u16();
    auto vec3 = [&] { return r.vec3(); };
    auto vecs = [&](std::size_t n) {
        std::vector<Vec3> v(n);
        for (auto& x : v)
            x = vec3();
        return v;
    };

    // aiPath::ReadBinary.
    map.paths.resize(static_cast<std::size_t>(paths));
    for (int k = 0; k < paths && r.ok(); ++k) {
        Mm1AiPath& p = map.paths[static_cast<std::size_t>(k)];
        p.id = static_cast<std::int16_t>(r.u16());
        p.vertexCount = static_cast<std::int16_t>(r.u16());
        p.numLanes = static_cast<std::int16_t>(r.u16());
        p.numSidewalks = static_cast<std::int16_t>(r.u16());
        p.stopLightIndex = static_cast<std::int16_t>(r.u16());
        p.intersectionType = static_cast<std::int16_t>(r.u16());
        p.alwaysBlocked = r.u16() != 0;
        p.pedBlocked = r.u16() != 0;
        p.divided = r.u16() != 0;
        p.flat = r.u16() != 0;
        p.bridge = r.u16() != 0;
        p.alley = r.u16() != 0;
        p.length = r.f32();
        p.speedLimit = r.f32();
        p.stopLight = str::lower(r.fixedString(32));
        p.oncoming = static_cast<std::int32_t>(r.u32());
        p.edgeIndex = static_cast<std::int32_t>(r.u32());
        p.pathIndex = static_cast<std::int32_t>(r.u32());
        const int rows = p.numLanes + p.numSidewalks;
        if (p.vertexCount < 2 || p.vertexCount > 1024 || p.numLanes < 0 || p.numSidewalks < 0 || rows > 16) {
            setError(error, std::format("path {}: bad counts ({} vertices, {} lanes, {} sidewalks)", k,
                                        p.vertexCount, p.numLanes, p.numSidewalks));
            return std::nullopt;
        }
        const auto vc = static_cast<std::size_t>(p.vertexCount);
        r.skip(4 * static_cast<std::size_t>(rows) * vc); // SubSectionOffsets
        r.skip(4 * vc);                                   // CenterOffsets
        p.sink = static_cast<std::int32_t>(r.u32());
        p.source = static_cast<std::int32_t>(r.u32());
        p.rows.resize(static_cast<std::size_t>(rows));
        for (auto& row : p.rows)
            row = vecs(vc); // LaneVertices, lane-major
        p.center = vecs(vc);
        p.xAxis = vecs(vc);
        p.normal = vecs(vc);
        p.zAxis = vecs(vc);
        r.skip(12 * vc); // SubSectionDirs
        p.edge = vecs(vc); // Boundaries: the outer edge row, then the curb row
        p.curb = vecs(vc);
        p.leftCurb = vecs(vc);
        p.stopLightPos[0] = r.vec3();
        p.stopLightPos[1] = r.vec3();
        for (auto& w : p.laneWidths)
            w = r.f32();
        r.skip(40); // LaneLengths
        if (p.id != k) {
            setError(error, std::format("path {} has id {}", k, p.id));
            return std::nullopt;
        }
    }

    // aiIntersection::ReadBinary.
    map.intersections.resize(static_cast<std::size_t>(intersections));
    for (int k = 0; k < intersections && r.ok(); ++k) {
        Mm1AiIntersection& n = map.intersections[static_cast<std::size_t>(k)];
        n.id = static_cast<std::int16_t>(r.u16());
        n.position = vec3();
        const int sinks = static_cast<std::int16_t>(r.u16());
        if (sinks < 0 || sinks > 64) {
            setError(error, std::format("intersection {}: {} sink paths", k, sinks));
            return std::nullopt;
        }
        for (int i = 0; i < sinks; ++i)
            n.sinkPaths.push_back(static_cast<std::int32_t>(r.u32()));
        const int sources = static_cast<std::int16_t>(r.u16());
        if (sources < 0 || sources > 64) {
            setError(error, std::format("intersection {}: {} source paths", k, sources));
            return std::nullopt;
        }
        for (int i = 0; i < sources; ++i)
            n.sourcePaths.push_back(static_cast<std::int32_t>(r.u32()));
        for (int i = 0; i < sinks + sources; ++i)
            n.paths.push_back(static_cast<std::int32_t>(r.u32()));
        for (int i = 0; i < sinks + sources; ++i)
            n.directions.push_back(r.f32());
        if (n.id != k) {
            setError(error, std::format("intersection {} has id {}", k, n.id));
            return std::nullopt;
        }
    }

    // aiMap::ReadBinary: the cells' two path lists.
    const std::uint32_t cells = r.u32();
    if (!r.ok() || cells > static_cast<std::uint32_t>(kMaxCount)) {
        setError(error, "bad cell count");
        return std::nullopt;
    }
    for (auto* lists : {&map.cellPathsNear, &map.cellPathsIn}) {
        lists->resize(cells);
        for (auto& list : *lists) {
            const std::uint32_t n = r.u32();
            if (n > static_cast<std::uint32_t>(kMaxCount)) {
                setError(error, "bad cell list");
                return std::nullopt;
            }
            for (std::uint32_t i = 0; i < n; ++i)
                list.push_back(static_cast<std::int32_t>(r.u32()));
        }
    }
    if (!r.ok()) {
        setError(error, "truncated file");
        return std::nullopt;
    }
    if (r.remaining() != 0) {
        setError(error, std::format("{} bytes after the cell lists", r.remaining()));
        return std::nullopt;
    }
    for (const auto& p : map.paths)
        if (p.sink < 0 || p.sink >= intersections || p.source < 0 || p.source >= intersections) {
            setError(error, std::format("path {} joins intersections {} and {} of {}", p.id, p.source, p.sink,
                                        intersections));
            return std::nullopt;
        }
    return map;
}

AiMap mm1ToAiMap(const Mm1AiMap& mm1) {
    AiMap out;
    const std::size_t n = mm1.paths.size();
    // The MM2 path of every MM1 path, and which side it is (1 right: the
    // pair's first path; 0 left: its partner).
    std::vector<int> pathOf(n, -1), sideOf(n, 1);
    std::vector<std::pair<int, int>> pairs; // (right, left or -1)
    for (std::size_t i = 0; i < n; ++i) {
        if (pathOf[i] >= 0)
            continue;
        const int other = mm1.paths[i].oncoming;
        const bool paired = other >= 0 && static_cast<std::size_t>(other) < n && other != static_cast<int>(i) &&
                            pathOf[static_cast<std::size_t>(other)] < 0 &&
                            mm1.paths[static_cast<std::size_t>(other)].oncoming == static_cast<int>(i);
        pathOf[i] = static_cast<int>(pairs.size());
        if (paired) {
            pathOf[static_cast<std::size_t>(other)] = static_cast<int>(pairs.size());
            sideOf[static_cast<std::size_t>(other)] = 0;
        }
        pairs.emplace_back(static_cast<int>(i), paired ? other : -1);
    }

    // Drops a row's first and last points (both at the intersections'
    // centres) when the row is long enough to keep two.
    auto trimmed = [](const std::vector<Vec3>& row) {
        if (row.size() >= 4)
            return std::vector<Vec3>(row.begin() + 1, row.end() - 1);
        return row;
    };
    auto reversed = [&](const std::vector<Vec3>& row) {
        std::vector<Vec3> r = trimmed(row);
        std::ranges::reverse(r);
        return r;
    };
    auto ruleOf = [](const Mm1AiPath& p) -> std::uint16_t {
        // MM1's IntersectionType at the path's sink, as MM2's EntryRule: 1 a
        // traffic light (aiTrafficLightSet ctor: the paths of type 1 get a
        // light), 0 a stop sign; anything else (3) uncontrolled. Its stop
        // light name is not the rule: uncontrolled ends keep the default
        // tplttrafc with no position.
        return p.intersectionType == 1 ? 1 : p.intersectionType == 0 ? 0 : 3;
    };

    out.paths.reserve(pairs.size());
    for (std::size_t k = 0; k < pairs.size(); ++k) {
        const Mm1AiPath& a = mm1.paths[static_cast<std::size_t>(pairs[k].first)];
        const Mm1AiPath* b = pairs[k].second >= 0 ? &mm1.paths[static_cast<std::size_t>(pairs[k].second)] : nullptr;
        AiPath p;
        p.id = static_cast<std::uint16_t>(k);
        if (a.divided || (b && b->divided))
            p.flags |= 0x1;
        if (a.alley || (b && b->alley))
            p.flags |= 0x2;
        if (a.speedLimit > 20.0f || (b && b->speedLimit > 20.0f))
            p.flags |= 0x4; // inferred: MM1's 28 m/s roads are its freeway
        p.speedLimit = a.speedLimit;
        p.center = trimmed(a.center);
        p.xAxis = trimmed(a.xAxis);
        p.yAxis = trimmed(a.normal);
        // z points back, against increasing section index (MM2's frames).
        const std::size_t sections = p.center.size();
        p.zAxis.resize(sections);
        for (std::size_t i = 0; i < sections; ++i) {
            const Vec3 ahead = p.center[std::min(i + 1, sections - 1)] - p.center[i == sections - 1 ? i - 1 : i];
            p.zAxis[i] = ahead.mag2() > 1e-9f ? ahead.normalized() * -1.0f : Vec3{0, 0, 1};
        }
        p.wAxis = p.zAxis;
        float run = 0.0f;
        for (std::size_t i = 1; i < sections; ++i) {
            run += p.center[i].dist(p.center[i - 1]);
            p.centerLengths.push_back(run);
        }
        const std::vector<Vec3> curb = trimmed(a.curb);
        float half = 0.0f;
        for (std::size_t i = 0; i < sections && i < curb.size(); ++i)
            half += Vec2{curb[i].x - p.center[i].x, curb[i].z - p.center[i].z}.mag();
        p.halfWidth = sections ? half / static_cast<float>(sections) : 0.0f;

        // MM2: ends[0] is at center.back() (A's sink), ends[1] at the front.
        p.ends[0].intersection = static_cast<std::uint32_t>(a.sink);
        p.ends[1].intersection = static_cast<std::uint32_t>(a.source);
        // Side 1 (A) arrives at ends[0], side 0 (B) at ends[1].
        p.ends[0].vehicleRule = ruleOf(a);
        p.ends[1].vehicleRule = b ? ruleOf(*b) : 3;
        // The lights: MM1's pole and axis points, as MM2's are (both
        // aiTrafficLightInstance::Init), and MM1's model for each.
        p.ends[0].trafficLightPos = a.stopLightPos[0];
        p.ends[0].trafficLightAxis = a.stopLightPos[1];
        if (p.ends[0].vehicleRule == 1)
            p.ends[0].trafficLightModel = a.stopLight;
        if (b) {
            p.ends[1].trafficLightPos = b->stopLightPos[0];
            p.ends[1].trafficLightAxis = b->stopLightPos[1];
            if (p.ends[1].vehicleRule == 1)
                p.ends[1].trafficLightModel = b->stopLight;
        }

        // Right side: A's lanes, its sidewalk, curb and edge.
        auto fillSide = [&](AiRoadSide& side, const Mm1AiPath& src, bool reverse) {
            side.numLanes = static_cast<std::uint16_t>(src.numLanes);
            side.numSidewalks = static_cast<std::uint16_t>(src.numSidewalks > 0 ? 1 : 0);
            side.roadType = static_cast<std::uint16_t>((src.alwaysBlocked ? 1 : 0) | (src.pedBlocked ? 2 : 0));
            auto row = [&](const std::vector<Vec3>& r) { return reverse ? reversed(r) : trimmed(r); };
            for (int l = 0; l < src.numLanes; ++l)
                side.polylines.push_back(row(src.rows[static_cast<std::size_t>(l)]));
            if (side.numSidewalks)
                side.polylines.push_back(row(src.rows[static_cast<std::size_t>(src.numLanes)]));
            side.polylines.push_back(row(src.curb));
            side.polylines.push_back(row(src.edge));
        };
        fillSide(p.right, a, false);
        if (b) {
            fillSide(p.left, *b, true);
            // The far curb on A's own sections (LBoundaries), so both curbs
            // have the centre line's points (aiPath::CalcRoadTurns needs it).
            const std::size_t curbIndex = p.left.polylines.size() - 2;
            p.left.polylines[curbIndex] = trimmed(a.leftCurb);
        } else {
            p.left.polylines = {trimmed(a.leftCurb), trimmed(a.leftCurb)}; // one way: curbs only
        }
        out.paths.push_back(std::move(p));
    }

    // Intersections: every MM2 path once, in MM1's order by direction.
    out.intersections.resize(mm1.intersections.size());
    for (std::size_t i = 0; i < mm1.intersections.size(); ++i) {
        const auto& src = mm1.intersections[i];
        AiIntersection& dst = out.intersections[i];
        dst.id = static_cast<std::uint16_t>(i);
        dst.center = src.position;
        for (int id : src.paths) {
            if (id < 0 || static_cast<std::size_t>(id) >= n)
                continue;
            const auto mm2 = static_cast<std::uint32_t>(pathOf[static_cast<std::size_t>(id)]);
            if (std::ranges::find(dst.paths, mm2) == dst.paths.end())
                dst.paths.push_back(mm2);
        }
    }
    // Each path end's index in its intersection's list.
    for (auto& p : out.paths)
        for (auto& e : p.ends) {
            if (e.intersection >= out.intersections.size())
                continue;
            const auto& list = out.intersections[e.intersection].paths;
            const auto it = std::ranges::find(list, static_cast<std::uint32_t>(p.id));
            e.roadIndex = static_cast<std::uint16_t>(it == list.end() ? 0 : it - list.begin());
        }

    // The cells' path lists, in MM2 path ids.
    auto convert = [&](const std::vector<std::vector<int>>& lists) {
        std::vector<std::vector<std::uint16_t>> res(lists.size());
        for (std::size_t c = 0; c < lists.size(); ++c)
            for (int id : lists[c]) {
                if (id < 0 || static_cast<std::size_t>(id) >= n)
                    continue;
                const auto mm2 = static_cast<std::uint16_t>(pathOf[static_cast<std::size_t>(id)]);
                if (std::ranges::find(res[c], mm2) == res[c].end())
                    res[c].push_back(mm2);
            }
        return res;
    };
    out.roomPathsNear = convert(mm1.cellPathsNear);
    out.roomPathsIn = convert(mm1.cellPathsIn);
    return out;
}

void adaptMm1AiMapConfig(AiMapConfig& config) {
    if (config.speedLimit)
        *config.speedLimit *= 0.44704f;
    for (auto& o : config.opponents)
        if (!o.params.empty())
            o.params[0] = std::clamp(0.85f + 0.15f * (o.params[0] - 0.3f) / 0.7f, 0.85f, 1.0f);
}

} // namespace mm2::city
