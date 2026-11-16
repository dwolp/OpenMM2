// mm1audit: audits a Midtown Madness 1 city's building meshes and its ground for what the player and the
// racers run into (OpenMM2 Enhanced).
//   A. building / facade models: every detail level (H, M, L, VL) of each mesh group against the high one:
//      fewer wall faces, a lower or narrower body (missing walls when seen from far), levels that are missing.
//   B. the roads: along every lane line, the collision ground every metre: holes (no ground), steps
//      (a lip over 0.2 m in 1 m) and steep slopes, and walls standing in the lane (a polygon the car's body
//      cannot pass), grouped in 8 m cells.
#include "Command.h"
#include "Common.h"

#include "asset/Bms.h"
#include "city/Mm1AiMap.h"
#include "city/Mm1City.h"
#include "core/StringUtil.h"
#include "game/Mm1Level.h"
#include "phys/Level.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <print>
#include <set>

namespace mm2::tool {
namespace {

struct LodStat {
    bool present = false;
    std::size_t tris = 0;
    float wall = 0.0f, height = 0.0f, width = 0.0f;
};

LodStat statOf(const asset::BmsMesh& m) {
    LodStat s;
    s.present = true;
    auto pos = [&](std::uint16_t adj) -> Vec3 {
        return adj < m.adjunctPosition.size() && m.adjunctPosition[adj] < m.positions.size()
                   ? m.positions[m.adjunctPosition[adj]]
                   : Vec3{};
    };
    auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 n = (b - a).cross(c - a);
        const float area2 = n.mag();
        ++s.tris;
        if (area2 > 1e-6f && std::fabs(n.y) / area2 < 0.35f)
            s.wall += area2 * 0.5f;
    };
    for (const auto& surf : m.surfaces) {
        tri(pos(surf.corners[0]), pos(surf.corners[1]), pos(surf.corners[2]));
        if (surf.quad())
            tri(pos(surf.corners[0]), pos(surf.corners[2]), pos(surf.corners[3]));
    }
    const Aabb b = m.bounds();
    if (b.valid()) {
        s.height = b.max.y - b.min.y;
        s.width = std::max(b.max.x - b.min.x, b.max.z - b.min.z);
    }
    return s;
}

int cmdMm1Audit(std::span<char* const> args) {
    if (args.empty())
        return 2;
    vfs::Vfs v;
    std::string err;
    if (!vfs::mountMm1Source(v, str::toPath(args[0]), &err)) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    const std::string cityName = args.size() > 1 && args[1][0] != '-' ? args[1] : "chicago";
    auto city = city::loadMm1City(v, cityName, &err);
    if (!city) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    const std::string prefix(vfs::kMm1Prefix);

    // ---- A. building and facade models --------------------------------------------------------------
    {
        std::map<std::string, int> uses;
        for (const auto& f : city->facades)
            ++uses[str::lower(f.name)];
        static const char* kLods[4] = {"h", "m", "l", "vl"};
        static const char* kGroups[] = {"facade", "bldg", "grnd", "top", "left", "right", "back", ""};
        struct Row {
            std::string text;
            int uses;
        };
        std::vector<Row> rows;
        int models = 0, groupsSeen = 0, gaps = 0, thin = 0, low = 0, narrow = 0;
        for (const auto& [name, count] : uses) {
            ++models;
            for (const char* g : kGroups) {
                std::array<LodStat, 4> st{};
                for (int l = 0; l < 4; ++l) {
                    const std::string path = g[0] ? std::format("{}bms/{}/{}_{}.bms", prefix, name, g, kLods[l])
                                                  : std::format("{}bms/{}/{}.bms", prefix, name, kLods[l]);
                    if (auto b = v.readAll(path))
                        if (auto m = asset::parseBms(*b))
                            st[static_cast<std::size_t>(l)] = statOf(*m);
                }
                if (!st[0].present)
                    continue;
                ++groupsSeen;
                std::string why;
                for (int l = 1; l < 4; ++l) {
                    const LodStat& s = st[static_cast<std::size_t>(l)];
                    if (!s.present) {
                        ++gaps;
                        continue;
                    }
                    if (st[0].wall >= 30.0f && s.wall < 0.4f * st[0].wall) {
                        why += std::format(" {}: walls {:.0f}% of H", kLods[l], 100.0f * s.wall / st[0].wall);
                        ++thin;
                    }
                    if (st[0].height >= 6.0f && s.height < 0.8f * st[0].height) {
                        why += std::format(" {}: height {:.0f} of {:.0f} m", kLods[l], s.height, st[0].height);
                        ++low;
                    }
                    if (st[0].width >= 6.0f && s.width < 0.8f * st[0].width) {
                        why += std::format(" {}: width {:.0f} of {:.0f} m", kLods[l], s.width, st[0].width);
                        ++narrow;
                    }
                }
                if (!why.empty())
                    rows.push_back({std::format("  {} / {} (x{}):{}", name, g[0] ? g : "(plain)", count, why), count});
            }
        }
        std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.uses > b.uses; });
        std::println("A. facade / building models: {} models, {} mesh groups; LOD levels absent below H {}; thin walls {}, lower {}, narrower {}",
                     models, groupsSeen, gaps, thin, low, narrow);
        for (std::size_t i = 0; i < rows.size() && i < 60; ++i)
            std::println("{}", rows[i].text);
        if (rows.size() > 60)
            std::println("  ... {} more", rows.size() - 60);
    }

    // ---- B. the roads' ground ---------------------------------------------------------------------------
    auto bytes = v.readAll(std::format("{}city/{}/{}.bai", prefix, cityName, cityName));
    if (!bytes) {
        std::println("B. no road network ({}.bai)", cityName);
        return 0;
    }
    auto bai = city::parseMm1Bai(*bytes);
    if (!bai)
        return 0;
    game::Mm1Level level(*city, v, [](std::string_view) { return -1; });
    struct Cluster {
        std::string type;
        int count = 0, path = -1;
        Vec3 sum{};
        float worst = 0.0f;
    };
    std::map<std::tuple<int, int, std::string>, Cluster> clusters;
    std::map<std::string, int> totals;
    auto note = [&](const char* type, const Vec3& p, int path, float amount) {
        auto& c = clusters[{static_cast<int>(std::floor(p.x / 8.0f)), static_cast<int>(std::floor(p.z / 8.0f)), type}];
        c.type = type;
        ++c.count;
        c.path = path;
        c.sum = c.sum + p;
        c.worst = std::max(c.worst, amount);
        ++totals[type];
    };
    std::size_t samples = 0;
    int skipped = 0;
    for (const auto& path : bai->paths) {
        if (path.alwaysBlocked) { // AI-only helper paths (closed to traffic): not roads a car drives
            ++skipped;
            continue;
        }
        for (int lane = 0; lane < path.numLanes && lane < static_cast<int>(path.rows.size()); ++lane) {
            const auto& row = path.rows[static_cast<std::size_t>(lane)];
            float prevH = 0.0f;
            bool havePrev = false;
            for (std::size_t i = 1; i + 1 < row.size(); ++i) {
                const Vec3& a = row[i];
                const Vec3& b = row[i + 1];
                const float len = std::hypot(b.x - a.x, b.z - a.z);
                const float lx = len > 1e-3f ? (b.x - a.x) / len : 0.0f, lz = len > 1e-3f ? (b.z - a.z) / len : 0.0f;
                const int n = std::max(1, static_cast<int>(len));
                for (int k = 0; k < n; ++k) {
                    const Vec3 p = a + (b - a) * (static_cast<float>(k) / static_cast<float>(n));
                    ++samples;
                    const int room = level.findRoom(p + Vec3{0, 1, 0}, 0);
                    int touched[16];
                    const int nt = level.touchedNeighbors(touched, 16, room, p, 1.2f);
                    std::vector<int> rooms{room};
                    rooms.insert(rooms.end(), touched, touched + nt);
                    phys::LevelBound out;
                    level.collect(rooms.data(), static_cast<int>(rooms.size()), p, 1.2f, out);
                    // The ground under the lane point (the polygon plane nearest the lane's own height).
                    bool found = false;
                    float h = 0.0f, bestD = 1e9f;
                    for (const auto& poly : out.polygons) {
                        if (poly.normal.y < 0.7f)
                            continue;
                        const int cnt = poly.v[3] ? 4 : 3;
                        bool inside = true;
                        float sign = 0.0f;
                        for (int c = 0; c < cnt && inside; ++c) {
                            const Vec3& va = out.vertices[poly.v[static_cast<std::size_t>(c)]];
                            const Vec3& vb = out.vertices[poly.v[static_cast<std::size_t>((c + 1) % cnt)]];
                            const float cr = (vb.x - va.x) * (p.z - va.z) - (vb.z - va.z) * (p.x - va.x);
                            // A little slack: a lane point on the seam between two polygons is on the ground.
                            if (cr * sign < -0.05f * std::hypot(vb.x - va.x, vb.z - va.z) * std::fabs(sign) / std::max(std::fabs(sign), 1e-6f))
                                inside = false;
                            else if (cr != 0.0f)
                                sign = cr;
                        }
                        if (!inside)
                            continue;
                        const Vec3& v0 = out.vertices[poly.v[0]];
                        const float y = v0.y - (poly.normal.x * (p.x - v0.x) + poly.normal.z * (p.z - v0.z)) / poly.normal.y;
                        if (std::fabs(y - p.y) < bestD && std::fabs(y - p.y) < 3.0f) {
                            bestD = std::fabs(y - p.y);
                            h = y;
                            found = true;
                        }
                    }
                    if (!found) {
                        note("hole", p, path.id, 1.0f);
                        havePrev = false;
                        continue;
                    }
                    if (havePrev) {
                        const float dh = std::fabs(h - prevH);
                        if (dh > 0.20f && dh < 1.2f)
                            note("step", p, path.id, dh);
                        else if (dh > 0.35f)
                            note("steep", p, path.id, dh);
                    }
                    prevH = h;
                    havePrev = true;
                    // A wall the car's body cannot pass: a near-vertical polygon over the lane point from
                    // wheel height (0.3 m) up to 0.9 m above the ground.
                    for (const auto& poly : out.polygons) {
                        if (std::fabs(poly.normal.y) > 0.5f)
                            continue;
                        const int cnt = poly.v[3] ? 4 : 3;
                        float lo = 1e9f, hi = -1e9f, tmin = 1e9f, tmax = -1e9f;
                        const float nl = std::hypot(poly.normal.x, poly.normal.z);
                        if (nl < 1e-4f)
                            continue;
                        const float dx = -poly.normal.z / nl, dz = poly.normal.x / nl;
                        for (int c = 0; c < cnt; ++c) {
                            const Vec3& q = out.vertices[poly.v[static_cast<std::size_t>(c)]];
                            lo = std::min(lo, q.y);
                            hi = std::max(hi, q.y);
                            const float t = (q.x - p.x) * dx + (q.z - p.z) * dz;
                            tmin = std::min(tmin, t);
                            tmax = std::max(tmax, t);
                        }
                        const Vec3& q0 = out.vertices[poly.v[0]];
                        const float dist = (p.x - q0.x) * poly.normal.x / nl + (p.z - q0.z) * poly.normal.z / nl;
                        // Only a wall across the lane blocks it: one running along it (an edge barrier) is
                        // within 25 degrees of the lane's direction.
                        const float along = std::fabs(lx * dx + lz * dz);
                        if (along > 0.9f)
                            continue;
                        if (std::fabs(dist) < 0.9f && tmin - 0.3f <= 0.0f && tmax + 0.3f >= 0.0f && hi >= h + 0.9f &&
                            lo <= h + 0.3f)
                            note("wall", p, path.id, hi - h);
                    }
                }
            }
        }
    }
    std::println("B. roads: {} always-blocked helper paths skipped; {} lane samples (1 m apart): holes {}, steps {}, steep {}, walls across the lane {}", skipped, samples,
                 totals["hole"], totals["step"], totals["steep"], totals["wall"]);
    std::vector<Cluster> list;
    for (auto& [k, c] : clusters)
        list.push_back(c);
    std::ranges::sort(list, [](const Cluster& a, const Cluster& b) { return a.count > b.count; });
    FILE* csv = args.size() > 2 ? std::fopen(args[2], "wb") : nullptr;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Cluster& c = list[i];
        const Vec3 m = c.sum * (1.0f / static_cast<float>(c.count));
        if (i < 40)
            std::println("  {:5s} x{:<3d} at ({:.0f}, {:.1f}, {:.0f}) path {} worst {:.2f}", c.type, c.count, m.x, m.y, m.z,
                         c.path, c.worst);
        if (csv)
            std::fprintf(csv, "%s,%d,%.1f,%.1f,%.1f,%d,%.2f\n", c.type.c_str(), c.count, m.x, m.y, m.z, c.path, c.worst);
    }
    if (csv)
        std::fclose(csv);
    return 0;
}

const Registrar r({"mm1audit", "<mm1 source> [city] [out.csv]",
                   "building LOD meshes (missing walls) and the roads' ground (holes, steps, walls in lanes)",
                   &cmdMm1Audit});

} // namespace
} // namespace mm2::tool
