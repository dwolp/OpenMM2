// Midtown Madness 1 content (for bringing MM1's Chicago into the fork):
// bmsinfo, bmscheck, mm1map.
#include "Command.h"
#include "Common.h"

#include "asset/Bms.h"
#include "asset/Image.h"
#include "asset/Mm1Bound.h"
#include "asset/Mm1TexSheet.h"
#include "city/Mm1AiMap.h"
#include "city/Mm1City.h"
#include "city/Mm1Instances.h"
#include "core/File.h"
#include "game/Mm1Level.h"
#include "phys/vehicle/Mm1CarParams.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <optional>
#include <print>

namespace mm2::tool {
namespace {

int cmdBmsInfo(std::span<char* const> args) {
    if (args.size() < 2)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    auto data = readFile(*fs, args[1]);
    if (!data)
        return 1;
    std::string err;
    auto m = asset::parseBms(*data, &err);
    if (!m) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    const auto b = m->bounds();
    std::println("flags 0x{:02X}{}{}{}{}{}", m->flags, (m->flags & asset::BmsFlag::Uv) ? " uv" : "",
                 (m->flags & asset::BmsFlag::Normal) ? " normal" : "", (m->flags & asset::BmsFlag::Color) ? " color" : "",
                 (m->flags & asset::BmsFlag::Offset) ? " offset" : "", (m->flags & asset::BmsFlag::Planes) ? " planes" : "");
    std::println("vertices {}  adjuncts {}  surfaces {} ({} triangles)", m->positions.size(), m->adjunctCount(),
                 m->surfaces.size(), m->triangleCount());
    std::println("radius {:.3f}  bounds ({:.2f}, {:.2f}, {:.2f}) .. ({:.2f}, {:.2f}, {:.2f})", m->radius, b.min.x,
                 b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    for (std::size_t i = 0; i < m->textures.size(); ++i) {
        const auto& t = m->textures[i];
        std::println("  tex {:2} {:<32} flags 0x{:02X} props 0x{:X} color 0x{:08X}", i + 1, t.name, t.flags, t.props,
                     t.color);
    }
    return 0;
}

int cmdBmsCheck(std::span<char* const> args) {
    if (args.empty())
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    const std::string pattern = args.size() > 1 ? args[1] : "*.bms";
    std::vector<std::string> paths;
    fs->forEachFile([&](const vfs::EntryInfo& e) {
        if (globMatch(pattern, e.path))
            paths.push_back(e.path);
    });
    std::size_t ok = 0, failed = 0, triangles = 0, headerNonZero = 0;
    std::map<int, std::size_t> flagCounts;
    for (const auto& p : paths) {
        auto data = readFile(*fs, p);
        if (!data) {
            ++failed;
            continue;
        }
        std::string err;
        auto m = asset::parseBms(*data, &err);
        if (!m) {
            ++failed;
            std::println("FAIL {}: {}", p, err);
            continue;
        }
        if (data->size() >= 16 && std::any_of(data->begin() + 4, data->begin() + 16, [](std::byte b) { return b != std::byte{0}; }))
            ++headerNonZero;
        ++ok;
        ++flagCounts[m->flags];
        triangles += m->triangleCount();
    }
    std::println("{} meshes: {} parsed, {} failed, {} triangles", paths.size(), ok, failed, triangles);
    std::println("header bytes 4..15 nonzero in {} files", headerNonZero);
    for (const auto& [f, n] : flagCounts)
        std::println("  flags 0x{:02X}: {}", f, n);
    return failed ? 1 : 0;
}

// Top-down picture of every mesh matching a glob, untextured: each triangle
// filled with its slope shading, the highest surface winning. For checking
// that a city's cell meshes sit where they belong.
int cmdMm1Map(std::span<char* const> args) {
    if (args.size() < 3)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    // Several globs may be given, separated by commas.
    std::vector<std::string> patterns;
    for (std::string_view rest = args[1]; !rest.empty();) {
        const auto comma = rest.find(',');
        patterns.emplace_back(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    const std::string pattern = args[1];
    int size = 2048;
    std::string fcdPath;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::string_view(args[i]) == "--fcd" && i + 1 < args.size())
            fcdPath = args[++i];
        else
            size = std::max(64, std::atoi(args[i]));
    }

    struct Tri {
        std::array<Vec3, 3> v;
    };
    std::vector<Tri> tris;
    Aabb bounds;
    std::size_t meshes = 0;
    auto addMesh = [&](const asset::BmsMesh& m, const Mat34* xf) {
        ++meshes;
        auto pos = [&](std::uint16_t adj) {
            const Vec3 p = m.positions[m.adjunctPosition[adj]];
            return xf ? xf->transform(p) : p;
        };
        for (const auto& s : m.surfaces) {
            const auto& c = s.corners;
            std::array<std::array<std::uint16_t, 3>, 2> parts{};
            int n = 0;
            if (s.quad()) {
                parts[n++] = {c[1], c[2], c[3]};
                parts[n++] = {c[1], c[3], c[0]};
            } else {
                parts[n++] = {c[0], c[1], c[2]};
            }
            for (int k = 0; k < n; ++k) {
                Tri t{{pos(parts[k][0]), pos(parts[k][1]), pos(parts[k][2])}};
                for (const auto& v : t.v)
                    bounds.expand(v);
                tris.push_back(t);
            }
        }
    };
    fs->forEachFile([&](const vfs::EntryInfo& e) {
        if (std::ranges::none_of(patterns, [&](const std::string& p) { return globMatch(p, e.path); }))
            return;
        auto data = readFile(*fs, e.path);
        if (!data)
            return;
        if (auto m = asset::parseBms(*data))
            addMesh(*m, nullptr);
    });

    // Facades and buildings from a .fcd, at their highest level of detail.
    std::size_t placed = 0, missing = 0;
    if (!fcdPath.empty()) {
        auto data = readFile(*fs, fcdPath);
        if (!data)
            return 1;
        std::string err;
        auto fcd = city::parseMm1Fcd(*data, &err);
        if (!fcd) {
            std::println(stderr, "error: {}: {}", fcdPath, err);
            return 1;
        }
        std::map<std::string, std::optional<asset::BmsMesh>> cache;
        // GetMeshSetSet names a group's levels "<GROUP>_H" .. "<GROUP>_VL",
        // with plain "H" .. "VL" as the fallback (freewaywall04 has only
        // those). For the map: the most detailed level that exists (some,
        // such as tunnel02, have only the low ones).
        auto group = [&](const std::string& name, std::string_view g) -> const asset::BmsMesh* {
            std::vector<std::string> candidates;
            for (const char* lod : {"h", "m", "l", "vl"})
                candidates.push_back(std::format("bms/{}/{}_{}.bms", name, g, lod));
            candidates.push_back(std::format("bms/{}/{}.bms", name, g));
            if (g == "facade")
                for (const char* lod : {"h", "m", "l", "vl"})
                    candidates.push_back(std::format("bms/{}/{}.bms", name, lod));
            for (const auto& path : candidates) {
                auto it = cache.find(path);
                if (it == cache.end()) {
                    std::optional<asset::BmsMesh> m;
                    if (fs->exists(path))
                        if (auto bytes = fs->open(path)->readAll(); !bytes.empty())
                            m = asset::parseBms(bytes);
                    it = cache.emplace(path, std::move(m)).first;
                }
                if (it->second)
                    return &*it->second;
            }
            return nullptr;
        };
        using namespace city::Mm1InstFlag;
        for (const auto& f : *fcd) {
            const std::string name = str::lower(f.name);
            Mat34 xf;
            std::vector<std::string_view> groups;
            if ((f.flags & Static) && !(f.flags & Shear) && (f.flags & Building)) {
                xf = city::mm1BuildingMatrix(f.start, f.end, f.sides);
                groups = {"facade", "grnd"};
            } else {
                // Facades (and, approximately, shear instances).
                xf = city::mm1FacadeMatrix(f.start, f.end, f.width);
                groups = {"facade", "grnd"};
                if (f.flags & FacadeTop)
                    groups.push_back("top");
                if (f.flags & FacadeLeft)
                    groups.push_back("left");
                if (f.flags & FacadeRight)
                    groups.push_back("right");
                if (f.flags & FacadeBack)
                    groups.push_back("back");
            }
            bool any = false;
            for (auto g : groups) {
                const asset::BmsMesh* m = group(name, g);
                if (!m && g == "facade")
                    m = group(name, "bldg");
                if (m) {
                    addMesh(*m, &xf);
                    any = true;
                }
            }
            any ? ++placed : ++missing;
        }
    }
    if (tris.empty()) {
        std::println(stderr, "error: no meshes match '{}'", pattern);
        return 1;
    }
    const float extent = std::max(bounds.max.x - bounds.min.x, bounds.max.z - bounds.min.z);
    const float scale = (size - 1) / extent;
    const int w = static_cast<int>((bounds.max.x - bounds.min.x) * scale) + 1;
    const int h = static_cast<int>((bounds.max.z - bounds.min.z) * scale) + 1;
    std::vector<float> depth(static_cast<std::size_t>(w) * h, -1e30f);
    asset::Image::Level img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        img.rgba[i] = 255;

    const float yRange = std::max(1.0f, bounds.max.y - bounds.min.y);
    for (const auto& t : tris) {
        // Screen: x right, image row 0 at the bottom (encodePng flips), so
        // larger z (south in Angel's -Z-forward world) ends up at the bottom.
        std::array<Vec2, 3> s;
        for (int k = 0; k < 3; ++k)
            s[k] = {(t.v[k].x - bounds.min.x) * scale, (bounds.max.z - t.v[k].z) * scale};
        const Vec3 e1 = t.v[1] - t.v[0], e2 = t.v[2] - t.v[0];
        Vec3 n = e1.cross(e2);
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len <= 0)
            continue;
        n = n / len;
        const float up = std::fabs(n.y);
        const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
        if (std::fabs(area) < 1e-6f)
            continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float b0 = ((s[1].x - px) * (s[2].y - py) - (s[2].x - px) * (s[1].y - py)) / area;
                const float b1 = ((s[2].x - px) * (s[0].y - py) - (s[0].x - px) * (s[2].y - py)) / area;
                const float b2 = 1.0f - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0)
                    continue;
                const float yv = b0 * t.v[0].y + b1 * t.v[1].y + b2 * t.v[2].y;
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (yv <= depth[i])
                    continue;
                depth[i] = yv;
                // Height tints blue (low) to warm (high); slope darkens walls.
                const float hgt = std::clamp((yv - bounds.min.y) / yRange, 0.0f, 1.0f);
                const float shade = 0.35f + 0.65f * up;
                const std::array<float, 3> rgb{0.45f + 0.55f * hgt, 0.55f + 0.25f * hgt, 0.75f - 0.45f * hgt};
                for (int c = 0; c < 3; ++c)
                    img.rgba[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(rgb[c] * shade * 255.0f, 0.0f, 255.0f));
            }
    }
    if (!file::writeAtomic(str::toPath(args[2]), asset::encodePng(img))) {
        std::println(stderr, "error: cannot write {}", args[2]);
        return 1;
    }
    if (!fcdPath.empty())
        std::println("{} facade records placed, {} without meshes", placed, missing);
    std::println("{} meshes, {} triangles, bounds x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.0f}..{:.0f}, {}x{} px",
                 meshes, tris.size(), bounds.min.x, bounds.max.x, bounds.min.y, bounds.max.y, bounds.min.z,
                 bounds.max.z, w, h);
    return 0;
}

std::optional<asset::Mm1TexSheet> loadTexSheet(const vfs::FileSystem& fs) {
    auto data = readFile(fs, "mtl/global.tsh");
    if (!data)
        return std::nullopt;
    std::string err;
    auto sheet = asset::Mm1TexSheet::parse(
        std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), &err);
    if (!sheet)
        std::println(stderr, "error: mtl/global.tsh: {}", err);
    return sheet;
}

// The file MM1's hardware renderer loads for a texture name.
std::optional<std::string> mm1TexturePath(const vfs::FileSystem& fs, std::string_view name) {
    for (auto dir : asset::kMm1TexSearchPath) {
        const std::string path = std::format("{}/{}.dds", dir, str::lower(name));
        if (fs.exists(path))
            return path;
    }
    return std::nullopt;
}

int cmdMm1Tex(std::span<char* const> args) {
    if (args.size() < 3)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    bool night = false;
    int variation = 0;
    auto weather = asset::Mm1TexWeather::Clear;
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--night")
            night = true;
        else if (a == "--rain")
            weather = asset::Mm1TexWeather::Rain;
        else if (a == "--snow")
            weather = asset::Mm1TexWeather::Snow;
        else if (a == "--variation" && i + 1 < args.size())
            variation = std::atoi(args[++i]);
    }
    auto sheet = loadTexSheet(*fs);
    if (!sheet)
        return 1;
    const std::string name = sheet->resolve(args[1], variation, night, weather);
    if (name.empty()) {
        std::println(stderr, "error: '{}' is not in the texture sheet", args[1]);
        return 1;
    }
    const auto path = mm1TexturePath(*fs, name);
    if (!path) {
        std::println(stderr, "error: no file for texture '{}'", name);
        return 1;
    }
    auto data = readFile(*fs, *path);
    if (!data)
        return 1;
    std::string err;
    auto img = asset::decodeDds(*data, &err);
    if (!img) {
        std::println(stderr, "error: {}: {}", *path, err);
        return 1;
    }
    if (!file::writeAtomic(str::toPath(args[2]), asset::encodePng(img->levels[0], false))) {
        std::println(stderr, "error: cannot write {}", args[2]);
        return 1;
    }
    const auto* prop = sheet->lookup(name);
    std::println("{} -> {} ({}x{}, {} levels{}, flags 0x{:X})", args[1], *path, img->width(), img->height(),
                 img->levels.size(), img->alphaFormat ? ", alpha" : "", prop ? prop->flags : 0);
    return 0;
}

int cmdMm1TexCheck(std::span<char* const> args) {
    if (args.empty())
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    auto sheet = loadTexSheet(*fs);
    if (!sheet)
        return 1;
    for (const auto& w : sheet->warnings())
        std::println("warning: {}", w);

    // Every .dds in the 16-bit folders decodes.
    std::size_t decoded = 0, failed = 0;
    std::map<std::string, std::size_t> formats;
    fs->forEachFile([&](const vfs::EntryInfo& e) {
        if (!e.path.starts_with("tex16") || !e.path.ends_with(".dds"))
            return;
        auto data = readFile(*fs, e.path);
        std::string err;
        auto img = data ? asset::decodeDds(*data, &err) : std::nullopt;
        if (!img) {
            ++failed;
            std::println("FAIL {}: {}", e.path, err);
            return;
        }
        ++decoded;
        ++formats[img->alphaFormat ? "alpha" : "opaque"];
    });

    // Every sheet entry, and its night alternate, has a file.
    std::size_t entries = 0, missing = 0, alternates = 0, variations = 0;
    for (const auto& p : sheet->entries()) {
        ++entries;
        if (!p.alternate.empty())
            ++alternates;
        for (const std::string* n : {&p.name, &p.alternate}) {
            if (n->empty())
                continue;
            if (!mm1TexturePath(*fs, *n)) {
                ++missing;
                std::println("missing file for '{}'", *n);
            }
        }
        if (sheet->variationCount(p.name) > 1)
            ++variations;
    }
    std::println("{} textures decoded ({} opaque, {} with alpha), {} failed", decoded, formats["opaque"],
                 formats["alpha"], failed);
    std::println("sheet: {} entries, {} night alternates, {} with variations; {} names without a file", entries,
                 alternates, variations, missing);
    return failed || missing ? 1 : 0;
}

// Mounts an MM1 source as the game does and loads one of its cities.
int cmdMm1CityInfo(std::span<char* const> args) {
    if (args.empty())
        return 2;
    const std::string city = args.size() > 1 ? args[1] : "chicago";
    vfs::Vfs v;
    std::string err;
    if (!vfs::mountMm1Source(v, str::toPath(args[0]), &err)) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    auto c = city::loadMm1City(v, city, &err);
    if (!c) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    for (const auto& w : c->warnings)
        std::println("warning: {}", w);
    std::size_t open = 0, threeEdge = 0;
    for (const auto& p : c->portals) {
        open += (p.flags & 0x2) ? 1 : 0;
        threeEdge += p.edgeCount == 3 ? 1 : 0;
    }
    std::println("{}: {} cells (ids below {}), {} portals ({} open, {} with three edges), {} facades, {} props",
                 c->name, c->cells.size(), c->maxCells, c->portals.size(), open, threeEdge, c->facades.size(),
                 c->bangers.size());
    if (c->extent)
        std::println("extent x {} .. {}, z {} .. {}", (*c->extent)[0], (*c->extent)[2], (*c->extent)[1],
                     (*c->extent)[3]);
    // The shear instances (mmShearInstance): which meshes each model has.
    {
        std::map<std::string, int> shear;
        const city::Mm1Facade* steepest = nullptr;
        for (const auto& f : c->facades)
            if ((f.flags & city::Mm1InstFlag::Static) && (f.flags & city::Mm1InstFlag::Shear)) {
                ++shear[str::lower(f.name)];
                if (!steepest || f.start.y - f.end.y > steepest->start.y - steepest->end.y)
                    steepest = &f;
            }
        if (steepest)
            std::println("shear piece dropping most: {} from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f})",
                         steepest->name, steepest->start.x, steepest->start.y, steepest->start.z, steepest->end.x,
                         steepest->end.y, steepest->end.z);
        for (const auto& [name, count] : shear) {
            auto has = [&](std::string_view file) {
                return v.exists(std::format("{}bms/{}/{}.bms", vfs::kMm1Prefix, name, file));
            };
            std::println("shear {} x{}: plain {} facade {} bldg {}", name, count, has("h") || has("m") || has("l"),
                         has("facade_h") || has("facade_m") || has("facade"), has("bldg_h") || has("bldg_m"));
        }
    }
    // The road network, converted to MM2's and checked as MM2's are.
    if (auto b = v.readAll(std::format("{}city/{}/{}.bai", vfs::kMm1Prefix, city, city))) {
        if (auto bai = city::parseMm1Bai(*b, &err)) {
            // The paths over the drawbridges (as loadCity flags them).
            std::vector<city::Mm1LeafDeck> decks;
            for (const auto& set : c->bridges)
                for (const auto& part : set.parts)
                    if (part.leaf())
                        if (auto bb = v.readAll(std::format("{}bnd/{}_bnd.bnd", vfs::kMm1Prefix, part.model)))
                            if (auto bound = asset::parseMm1Bound(*bb))
                                decks.push_back({part.matrix(), bound->box});
            if (const char* bn = std::getenv("MM1BND"))
                if (auto bb = v.readAll(std::string_view(bn).find('/') != std::string_view::npos
                                            ? std::format("{}{}", vfs::kMm1Prefix, bn)
                                            : std::format("{}bnd/{}_bnd.bnd", vfs::kMm1Prefix, bn)))
                    if (auto bound = asset::parseMm1Bound(*bb)) {
                        if (std::getenv("MM1BND_POLYS"))
                            for (const auto& pl : bound->polygons) {
                                std::string vs;
                                for (int k = 0; k < (pl.quad() ? 4 : 3); ++k) {
                                    const Vec3& q = bound->vertices[static_cast<std::size_t>(pl.vertices[static_cast<std::size_t>(k)])];
                                    vs += std::format(" ({:.1f},{:.1f},{:.1f})", q.x, q.y, q.z);
                                }
                                std::println("    n({:.2f},{:.2f},{:.2f}) mat {} flags 0x{:X}{}", pl.normal.x, pl.normal.y, pl.normal.z,
                                             pl.material, pl.flags, vs);
                            }
                        std::println("{} box ({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f}) polys {}", bn, bound->box.min.x,
                                     bound->box.min.y, bound->box.min.z, bound->box.max.x, bound->box.max.y,
                                     bound->box.max.z, bound->polygons.size());
                    } else
                        std::println("{}: unparsable", bn);
                else
                    std::println("{}: no bound", bn);
            if (const char* tn = std::getenv("MM1TUNE"))
                if (auto tb = v.readAll(std::format("{}tune/{}", vfs::kMm1Prefix, tn)))
                    std::println("{}", std::string(reinterpret_cast<const char*>(tb->data()), tb->size()));
            if (std::getenv("MM1BRIDGES"))
                for (const auto& set : c->bridges)
                    for (const auto& part : set.parts) {
                        const Mat34 m = part.matrix();
                        std::println("  {} {} pos ({:.1f},{:.1f},{:.1f}) x ({:.2f},{:.2f},{:.2f}) z ({:.2f},{:.2f},{:.2f})",
                                     set.name, part.model, part.position.x, part.position.y, part.position.z, m.m0.x,
                                     m.m0.y, m.m0.z, m.m2.x, m.m2.y, m.m2.z);
                        if (auto bb = v.readAll(std::format("{}bnd/{}_bnd.bnd", vfs::kMm1Prefix, part.model)))
                            if (auto bound = asset::parseMm1Bound(*bb))
                                std::println("    bound ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})", bound->box.min.x,
                                             bound->box.min.y, bound->box.min.z, bound->box.max.x, bound->box.max.y,
                                             bound->box.max.z);
                        if (std::getenv("MM1BRIDGES")[0] == '2')
                            if (auto bb = v.readAll(std::format("{}bnd/{}_bnd.bnd", vfs::kMm1Prefix, part.model)))
                                if (auto bound = asset::parseMm1Bound(*bb)) {
                                    for (const auto& pl : bound->polygons) {
                                        std::string vs;
                                        for (int k = 0; k < (pl.quad() ? 4 : 3); ++k) {
                                            const Vec3& q = bound->vertices[static_cast<std::size_t>(pl.vertices[static_cast<std::size_t>(k)])];
                                            vs += std::format(" ({:.1f},{:.1f},{:.1f})", q.x, q.y, q.z);
                                        }
                                        std::println("      n({:.2f},{:.2f},{:.2f}) mat {}{}", pl.normal.x, pl.normal.y,
                                                     pl.normal.z, pl.material, vs);
                                    }
                                }
                    }
            const int marked = city::markMm1BridgePaths(*bai, decks);
            std::string ids;
            for (const auto& p : bai->paths)
                if (p.hasBridge)
                    ids += std::format(" {}({}->{})", p.id, p.source, p.sink);
            std::println("drawbridges: {} leaves, {} paths over them:{}", decks.size(), marked, ids);
            std::size_t pairs = 0;
            for (const auto& p : bai->paths)
                pairs += p.oncoming >= 0 ? 1 : 0;
            const auto map = city::mm1ToAiMap(*bai);
            std::size_t lanes = 0, walks = 0;
            for (const auto& p : map.paths) {
                lanes += p.left.numLanes + p.right.numLanes;
                walks += p.left.numSidewalks + p.right.numSidewalks;
            }
            std::println("road network: {} MM1 paths ({} with an oncoming path), {} intersections -> {} MM2 paths, "
                         "{} lanes, {} sidewalks",
                         bai->paths.size(), pairs, bai->intersections.size(), map.paths.size(), lanes, walks);
            const auto issues = city::validateAiMap(map, static_cast<std::size_t>(c->maxCells));
            std::println("  {} validation issues", issues.size());
            for (std::size_t i = 0; i < issues.size() && i < 8; ++i)
                std::println("    {}", issues[i]);
        } else {
            std::println("road network: {}", err);
        }
    }
    // Which of mmCellRenderer's groups exist, with and without landmarks.
    for (bool lm : {true, false}) {
        std::array<std::size_t, city::kMm1CellGroups.size()> have{};
        std::size_t noHigh = 0;
        for (const auto& cell : c->cells) {
            bool high = v.exists(std::format("{}bms/{}/cull{:02}.bms", vfs::kMm1Prefix,
                                             c->cellMeshDir(cell.index, lm), cell.index));
            for (std::size_t g = 0; g < city::kMm1CellGroups.size(); ++g)
                if (v.exists(c->cellMeshPath(cell.index, city::kMm1CellGroups[g], lm))) {
                    ++have[g];
                    high = high || g == 0;
                }
            noHigh += high ? 0 : 1;
        }
        std::string line;
        for (std::size_t g = 0; g < have.size(); ++g)
            line += std::format(" {}={}", city::kMm1CellGroups[g], have[g]);
        if (const char* mp = std::getenv("MM1MESH"); mp && lm)
            if (auto mb = v.readAll(mp))
                if (auto mesh = asset::parseBms(*mb)) {
                    Aabb bx;
                    for (const Vec3& q : mesh->positions)
                        bx.expand(q);
                    std::println("  mesh {}: {} positions, ({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f})", mp,
                                 mesh->positions.size(), bx.min.x, bx.min.y, bx.min.z, bx.max.x, bx.max.y, bx.max.z);
                }
        if (const char* cz = std::getenv("MM1COVER"); cz && lm) {
            // The cells whose meshes have a triangle over (x, z), and its height.
            float px = 0, pz = 0;
            std::sscanf(cz, "%f,%f", &px, &pz);
            for (const auto& cell : c->cells)
                for (std::size_t g = 0; g < city::kMm1CellGroups.size(); ++g) {
                    auto mb = v.readAll(c->cellMeshPath(cell.index, city::kMm1CellGroups[g], lm));
                    if (!mb && g == 0)
                        mb = v.readAll(std::format("{}bms/{}/cull{:02}.bms", vfs::kMm1Prefix, c->cellMeshDir(cell.index, lm),
                                                   cell.index));
                    if (!mb)
                        continue;
                    auto mesh = asset::parseBms(*mb);
                    if (!mesh)
                        continue;
                    for (const auto& sf : mesh->surfaces) {
                        const int k = sf.quad() ? 4 : 3;
                        for (int t = 0; t < k - 2; ++t) {
                            const Vec3 A = mesh->positions[mesh->adjunctPosition[sf.corners[0]]];
                            const Vec3 B = mesh->positions[mesh->adjunctPosition[sf.corners[static_cast<std::size_t>(t + 1)]]];
                            const Vec3 C = mesh->positions[mesh->adjunctPosition[sf.corners[static_cast<std::size_t>(t + 2)]]];
                            auto e = [&](const Vec3& a, const Vec3& b) { return (b.x - a.x) * (pz - a.z) - (b.z - a.z) * (px - a.x); };
                            const float e0 = e(A, B), e1 = e(B, C), e2 = e(C, A);
                            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                                const Vec3 n = (B - A).cross(C - A);
                                if (std::abs(n.y) < 1e-4f)
                                    continue;
                                const float y = A.y - (n.x * (px - A.x) + n.z * (pz - A.z)) / n.y;
                                std::println("  cover cell {} group {} y {:.2f} tex {}", cell.index, city::kMm1CellGroups[g], y,
                                             sf.texture > 0 && sf.texture <= mesh->textures.size()
                                                 ? mesh->textures[sf.texture - 1u].name
                                                 : "-");
                            }
                        }
                    }
                }
        }
        if (const char* pf = std::getenv("MM1PROPFLAG"); pf && lm) {
            const int want = static_cast<int>(std::strtol(pf, nullptr, 0));
            std::map<std::string, int> names;
            for (const auto& b : c->bangers)
                if (b.flags & want)
                    ++names[b.name];
            for (const auto& [n, k] : names)
                std::println("  propflag {} x{}", n, k);
        }
        if (const char* pp = std::getenv("MM1PROPS"); pp && lm) {
            float px = 0, pz = 0, pr = 5;
            std::sscanf(pp, "%f,%f,%f", &px, &pz, &pr);
            for (const auto& b : c->bangers)
                if (std::hypot(b.position.x - px, b.position.z - pz) < pr)
                    std::println("  prop {} flags 0x{:X} ({:.1f},{:.1f},{:.1f}) ahead ({:.1f},{:.1f},{:.1f})", b.name, b.flags,
                                 b.position.x, b.position.y, b.position.z, b.ahead.x, b.ahead.y, b.ahead.z);
        }
        if (const char* fl = std::getenv("MM1FLAG"); fl && lm) {
            const int want = static_cast<int>(std::strtol(fl, nullptr, 0));
            std::map<std::string, int> names;
            for (const auto& f : c->facades)
                if (f.flags & want) {
                    if (names[f.name]++ < 2 || (std::getenv("MM1FLAGNAME") && f.name == std::getenv("MM1FLAGNAME")))
                        std::println("  flagged {} flags 0x{:X} cell {} ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) sides ({:.1f},{:.1f},{:.1f}) w {:.1f}",
                                     f.name, f.flags, f.cell, f.start.x, f.start.y, f.start.z, f.end.x, f.end.y, f.end.z,
                                     f.sides.x, f.sides.y, f.sides.z, f.width);
                }
            for (const auto& [n, k] : names)
                std::println("  flagged total {} x{}", n, k);
        }
        if (const char* cp = std::getenv("MM1CAT"); cp && lm)
            if (auto cb = v.readAll(cp))
                if (const char* out = std::getenv("MM1CAT_OUT")) {
                    std::FILE* fo = std::fopen(out, "wb");
                    if (fo) {
                        std::fwrite(cb->data(), 1, cb->size(), fo);
                        std::fclose(fo);
                    }
                }
        if (const char* pat = std::getenv("MM1LS"); pat && lm)
            for (const auto& e : v.listFiles())
                if (e.path.find(pat) != std::string::npos)
                    std::println("  file {}", e.path);
        if (const char* fz = std::getenv("MM1FIND"); fz && lm) {
            float gx = 0, gz = 0;
            std::sscanf(fz, "%f,%f", &gx, &gz);
            for (const auto& f : c->facades) {
                const float d0 = std::hypot(f.start.x - gx, f.start.z - gz), d1 = std::hypot(f.end.x - gx, f.end.z - gz);
                if (std::min(d0, d1) < 40.0f)
                    std::println("  facade {} flags 0x{:X} cell {} ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) sides ({:.1f},{:.1f},{:.1f}) w {:.1f}",
                                 f.name, f.flags, f.cell, f.start.x, f.start.y, f.start.z, f.end.x, f.end.y, f.end.z,
                                 f.sides.x, f.sides.y, f.sides.z, f.width);
            }
        }
        if (const char* fz = std::getenv("MM1FIND"); fz && lm) {
            // The cells whose meshes have vertices within 3 m (x, z) of a point.
            float fx = 0, fzz = 0;
            std::sscanf(fz, "%f,%f", &fx, &fzz);
            for (const auto& cell : c->cells)
                for (std::size_t g = 0; g < city::kMm1CellGroups.size(); ++g)
                    if (auto mb = v.readAll(c->cellMeshPath(cell.index, city::kMm1CellGroups[g], lm)))
                        if (auto mesh = asset::parseBms(*mb)) {
                            int n = 0;
                            float lo = 1e9f, hi = -1e9f;
                            for (const Vec3& q : mesh->positions)
                                if (std::abs(q.x - fx) < 15.0f && std::abs(q.z - fzz) < 15.0f) {
                                    ++n;
                                    lo = std::min(lo, q.y);
                                    hi = std::max(hi, q.y);
                                }
                            if (n)
                                std::println("  cell {} group {} flags cull 0x{:X} room 0x{:X}: {} vertices, y {:.1f}..{:.1f}",
                                             cell.index, city::kMm1CellGroups[g], cell.cullFlags, cell.roomFlags, n, lo, hi);
                        }
        }
        if (std::getenv("MM1NOMESH") && lm)
            for (const auto& cell : c->cells) {
                bool any = v.exists(std::format("{}bms/{}/cull{:02}.bms", vfs::kMm1Prefix, c->cellMeshDir(cell.index, lm),
                                                cell.index));
                for (std::size_t g = 0; g < city::kMm1CellGroups.size(); ++g)
                    any = any || v.exists(c->cellMeshPath(cell.index, city::kMm1CellGroups[g], lm));
                if (!any)
                    std::println("  no mesh: cell {}", cell.index);
            }
        std::println("{}:{}; {} cells without CULL<n> or _H", lm ? "landmarks on " : "landmarks off", line,
                     noHigh);
    }
    return 0;
}

// Collision at a point: the cell found there and the polygons collected.
// Parses MM1's tune/<car>.mmcarsim files and prints what they hold (every car
// when none is named).
int cmdMm1CarSim(std::span<char* const> args) {
    if (args.empty())
        return 2;
    vfs::Vfs v;
    std::string err;
    if (!vfs::mountMm1Source(v, str::toPath(args[0]), &err)) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    std::vector<std::string> names;
    for (std::size_t i = 1; i < args.size(); ++i)
        names.emplace_back(args[i]);
    if (names.empty())
        for (const auto& e : v.listFiles())
            if (e.path.starts_with("mm1/tune/") && e.path.ends_with(".mmcarsim"))
                names.push_back(e.path.substr(9, e.path.size() - 9 - 9));
    int bad = 0;
    for (const auto& name : names) {
        const auto bytes = v.readAll("mm1/tune/" + name + ".mmcarsim");
        if (!bytes) {
            std::println("{}: not found", name);
            ++bad;
            continue;
        }
        std::string perr;
        const auto p = phys::parseMm1CarParams(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &perr);
        if (!p) {
            std::println("{}: {}", name, perr);
            ++bad;
            continue;
        }
        std::println("{}: mass {} hp {} rpm {}/{} drive {} gears {} (manual {}) FL spring {} damp {} fric {}/{} BL spring {}"
                     " lat {}/{} spin {} drift {} steerMul {}",
                     name, p->mass, p->engine.maxHorsePower, p->engine.optRPM, p->engine.maxRPM, p->drivetrainType,
                     p->trans.numGears, p->trans.manualNumGears, p->frontLeft.spring, p->frontLeft.damping,
                     p->frontLeft.staticFric, p->frontLeft.slidingFric, p->backLeft.spring, p->frontLeft.hasLat,
                     p->backLeft.hasLat, p->spinThreshold, p->driftThreshold, p->steerMultiplier);
    }
    return bad ? 1 : 0;
}

int cmdMm1Probe(std::span<char* const> args) {
    if (args.size() < 4)
        return 2;
    vfs::Vfs v;
    std::string err;
    if (!vfs::mountMm1Source(v, str::toPath(args[0]), &err)) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    auto c = city::loadMm1City(v, "chicago", &err);
    if (!c) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    game::Mm1Level level(*c, v, [](std::string_view) { return -1; });
    const Vec3 p{static_cast<float>(std::atof(args[1])), static_cast<float>(std::atof(args[2])),
                 static_cast<float>(std::atof(args[3]))};
    const int room = level.findRoom(p, 0);
    const float radius = args.size() > 4 ? static_cast<float>(std::atof(args[4])) : 2.0f;
    int touched[16];
    const int n = level.touchedNeighbors(touched, 16, room, p, radius);
    std::vector<int> rooms{room};
    rooms.insert(rooms.end(), touched, touched + n);
    phys::LevelBound out;
    level.collect(rooms.data(), static_cast<int>(rooms.size()), p, radius, out);
    std::println("room {} (+{} touched); {} polygons within {} m", room, n, out.polygons.size(), radius);
    if (std::getenv("MM1RAW")) {
        // The cell's raw polygons (material, flags) near the point.
        for (int r : rooms)
            if (auto bb = v.readAll(std::format("{}bnd/{}/bound{:02}.bnd", vfs::kMm1Prefix, c->cellMeshDir(r), r)))
                if (auto bound = asset::parseMm1Bound(*bb)) {
                    for (const auto& pl : bound->polygons) {
                        bool near = false;
                        float lo = 1e9f, hi = -1e9f;
                        for (int k = 0; k < (pl.quad() ? 4 : 3); ++k) {
                            const Vec3& q = bound->vertices[static_cast<std::size_t>(pl.vertices[static_cast<std::size_t>(k)])];
                            near = near || ((q - p).mag() < radius + 20.0f);
                            lo = std::min(lo, q.y);
                            hi = std::max(hi, q.y);
                        }
                        if (near && hi - lo > 3.0f)
                            std::println("  RAW room {} tall poly mat {} flags 0x{:X} n({:.2f},{:.2f},{:.2f}) y {:.1f}..{:.1f} v0 ({:.1f},{:.1f})",
                                         r, pl.material, pl.flags, pl.normal.x, pl.normal.y, pl.normal.z, lo, hi,
                                         bound->vertices[pl.vertices[0]].x, bound->vertices[pl.vertices[0]].z);
                    }
                }
    }
    for (const auto& poly : out.polygons) {
        float y = 0;
        int k = poly.v[3] ? 4 : 3;
        for (int i = 0; i < k; ++i)
            y += out.vertices[poly.v[static_cast<std::size_t>(i)]].y / static_cast<float>(k);
        std::println("  normal ({:.2f}, {:.2f}, {:.2f}) mean y {:.2f}", poly.normal.x, poly.normal.y, poly.normal.z, y);
        for (int i = 0; i < k; ++i) {
            const Vec3& q = out.vertices[poly.v[static_cast<std::size_t>(i)]];
            std::println("    ({:.1f}, {:.1f}, {:.1f})", q.x, q.y, q.z);
        }
    }
    return 0;
}

int cmdMm1Bnd(std::span<char* const> args) {
    if (args.size() < 2)
        return 2;
    auto fs = openContainer(args[0]);
    if (!fs)
        return 1;
    auto data = readFile(*fs, args[1]);
    if (!data)
        return 1;
    std::string err;
    auto b = asset::parseMm1Bound(*data, &err);
    if (!b) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    std::println("vertices {}  polygons {}  radius {:.3f}  centre ({:.2f}, {:.2f}, {:.2f})", b->vertices.size(),
                 b->polygons.size(), b->radius, b->centre.x, b->centre.y, b->centre.z);
    std::println("box ({:.2f}, {:.2f}, {:.2f}) .. ({:.2f}, {:.2f}, {:.2f})", b->box.min.x, b->box.min.y, b->box.min.z,
                 b->box.max.x, b->box.max.y, b->box.max.z);
    for (std::size_t i = 0; i < b->vertices.size(); ++i)
        std::println("  v{:<3} ({:.2f}, {:.2f}, {:.2f})", i, b->vertices[i].x, b->vertices[i].y, b->vertices[i].z);
    for (const auto& p : b->polygons)
        std::println("  {} {} {} {} {} material {} flags 0x{:X} normal ({:.2f}, {:.2f}, {:.2f})", p.quad() ? "quad" : "tri ",
                     p.vertices[0], p.vertices[1], p.vertices[2], p.vertices[3], p.material, p.flags, p.normal.x, p.normal.y, p.normal.z);
    return 0;
}

// The ground material under each traffic lane point of the converted road
// network: lanes over grass, dirt or water are listed.
int cmdMm1LaneCheck(std::span<char* const> args) {
    if (args.empty())
        return 2;
    vfs::Vfs v;
    std::string err;
    if (!vfs::mountMm1Source(v, str::toPath(args[0]), &err)) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    auto c = city::loadMm1City(v, "chicago", &err);
    if (!c) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    auto b = v.readAll(std::format("{}city/chicago/chicago.bai", vfs::kMm1Prefix));
    auto bai = b ? city::parseMm1Bai(*b, &err) : std::nullopt;
    if (!bai) {
        std::println(stderr, "error: {}", err);
        return 1;
    }
    const auto map = city::mm1ToAiMap(*bai);
    const std::vector<std::string> names{"_default", "grass", "dirt", "deepwater", "cobblestone"};
    game::Mm1Level level(*c, v, [&](std::string_view n) {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == n)
                return static_cast<int>(i);
        return -1;
    });
    // The highest upward polygon under p (within 1.5 m above it): its name.
    auto groundAt = [&](const Vec3& p) -> std::string {
        const int room = level.findRoom(p + Vec3{0, 1, 0}, 0);
        int touched[16];
        const int n = level.touchedNeighbors(touched, 16, room, p, 1.0f);
        std::vector<int> rooms{room};
        rooms.insert(rooms.end(), touched, touched + n);
        phys::LevelBound out;
        level.collect(rooms.data(), static_cast<int>(rooms.size()), p, 1.0f, out);
        float best = -1e30f;
        std::string name = "none";
        for (const auto& poly : out.polygons) {
            if (poly.normal.y < 0.5f)
                continue;
            const int k = poly.vertexCount();
            bool inside = true;
            float y = 0.0f;
            for (int i = 0; i < k && inside; ++i) {
                const Vec3& a = out.vertices[poly.v[static_cast<std::size_t>(i)]];
                const Vec3& bb = out.vertices[poly.v[static_cast<std::size_t>((i + 1) % k)]];
                // Counter-clockwise from above (normal up): p left of each edge.
                const float cross = (bb.x - a.x) * (p.z - a.z) - (bb.z - a.z) * (p.x - a.x);
                inside = cross <= 0.01f;
                y += a.y / static_cast<float>(k);
            }
            if (!inside) {
                inside = true;
                for (int i = 0; i < k && inside; ++i) {
                    const Vec3& a = out.vertices[poly.v[static_cast<std::size_t>(i)]];
                    const Vec3& bb = out.vertices[poly.v[static_cast<std::size_t>((i + 1) % k)]];
                    inside = (bb.x - a.x) * (p.z - a.z) - (bb.z - a.z) * (p.x - a.x) >= -0.01f;
                }
            }
            if (inside && y <= p.y + 1.5f && y > best) {
                best = y;
                name = poly.material > 0 && poly.material <= names.size() ? names[poly.material - 1] : "_default";
            }
        }
        return name;
    };
    if (args.size() >= 2 && std::string_view(args[1]) != "holes" && std::string_view(args[1]) != "facades" && std::string_view(args[1]) != "walls" && std::string_view(args[1]) != "groundgaps") {
        // One path's lanes, point by point.
        const int want = std::atoi(args[1]);
        for (const auto& path : map.paths)
            if (path.id == want)
                for (const city::AiRoadSide* side : {&path.right, &path.left})
                    for (int l = 0; l < side->numLanes; ++l) {
                        std::println("{} lane {}:", side == &path.right ? "right" : "left", l);
                        for (const Vec3& q : side->polylines[static_cast<std::size_t>(l)])
                            std::println("  ({:.1f}, {:.1f}, {:.1f}) {}", q.x, q.y, q.z, groundAt(q));
                    }
        return 0;
    }
    // Whether any cell has an up-facing polygon under p (the whole level).
    std::vector<int> everyRoom;
    for (int r = 1; r < level.roomCount(); ++r)
        everyRoom.push_back(r);
    auto groundAnywhere = [&](const Vec3& p, int* owner) {
        phys::LevelBound out;
        level.collect(everyRoom.data(), static_cast<int>(everyRoom.size()), p, 1.0f, out);
        for (const auto& poly : out.polygons) {
            if (poly.normal.y < 0.5f)
                continue;
            const int k = poly.vertexCount();
            bool pos = true, neg = true;
            for (int i = 0; i < k; ++i) {
                const Vec3& a = out.vertices[poly.v[static_cast<std::size_t>(i)]];
                const Vec3& b = out.vertices[poly.v[static_cast<std::size_t>((i + 1) % k)]];
                const float cross = (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
                pos = pos && cross <= 0.01f;
                neg = neg && cross >= -0.01f;
            }
            if (pos || neg) {
                if (owner)
                    *owner = static_cast<int>(poly.material);
                return true;
            }
        }
        return false;
    };
    // The game's own ground query: a vertical ray against the polygon soup.
    phys::MaterialTable soupMaterials;
    phys::PolygonSoup soup = level.buildSoup(soupMaterials);
    soup.finalize();
    auto soupGround = [&](const Vec3& p) {
        phys::RayHit hit;
        return soup.raycast({p.x, p.y + 3.0f, p.z}, {p.x, p.y - 3.0f, p.z}, hit);
    };
    // Ground gaps: points on the cells' drawn ground (upward triangles, not
    // water) with no collision polygon within 0.5 m of their height.
    if (args.size() >= 2 && std::string_view(args[1]) == "groundgaps") {
        std::map<std::string, std::pair<int, Vec3>> perCellGroup;
        int samples = 0, gaps = 0;
        for (const auto& cell : c->cells)
            for (std::size_t g = 0; g < city::kMm1CellGroups.size(); ++g) {
                auto mb = v.readAll(c->cellMeshPath(cell.index, city::kMm1CellGroups[g], true));
                if (!mb && g == 0)
                    mb = v.readAll(std::format("{}bms/{}/cull{:02}.bms", vfs::kMm1Prefix, c->cellMeshDir(cell.index, true),
                                               cell.index));
                if (!mb)
                    continue;
                auto mesh = asset::parseBms(*mb);
                if (!mesh)
                    continue;
                for (const auto& sf : mesh->surfaces) {
                    const std::string tex = sf.texture > 0 && sf.texture <= mesh->textures.size()
                                                ? str::lower(mesh->textures[sf.texture - 1u].name)
                                                : "";
                    if (tex.find("water") != std::string::npos)
                        continue;
                    const int k = sf.quad() ? 4 : 3;
                    for (int t = 0; t < k - 2; ++t) {
                        const Vec3 A = mesh->positions[mesh->adjunctPosition[sf.corners[0]]];
                        const Vec3 B = mesh->positions[mesh->adjunctPosition[sf.corners[static_cast<std::size_t>(t + 1)]]];
                        const Vec3 C = mesh->positions[mesh->adjunctPosition[sf.corners[static_cast<std::size_t>(t + 2)]]];
                        Vec3 n = (B - A).cross(C - A);
                        const float l = n.mag();
                        if (l < 1e-4f || std::abs(n.y) / l < 0.7f)
                            continue;
                        // Samples: the centroid and points toward each corner.
                        const Vec3 m = (A + B + C) * (1.0f / 3.0f);
                        for (const Vec3& q : {m, m + (A - m) * 0.8f, m + (B - m) * 0.8f, m + (C - m) * 0.8f}) {
                            if (const char* my = std::getenv("MM1GAP_MAXY"); my && q.y > std::atof(my))
                                continue;
                            ++samples;
                            if (level.groundNear(q, 0.5f, 0.5f))
                                continue;
                            ++gaps;
                            auto& r = perCellGroup[std::format("cell {} {} {}", cell.index, city::kMm1CellGroups[g], tex)];
                            if (r.first++ == 0)
                                r.second = q;
                        }
                    }
                }
            }
        std::println("{} of {} drawn-ground samples have no collision ground", gaps, samples);
        std::vector<std::pair<int, std::string>> sorted;
        for (const auto& [k, r] : perCellGroup)
            sorted.push_back({r.first, k});
        std::ranges::sort(sorted, std::greater{});
        for (std::size_t i = 0; i < sorted.size() && i < 50; ++i) {
            const Vec3 q = perCellGroup[sorted[i].second].second;
            std::println("  {:5} {} e.g. ({:.1f},{:.1f},{:.1f})", sorted[i].first, sorted[i].second, q.x, q.y, q.z);
        }
        return 0;
    }
    // Walls with nothing drawn: generated building walls with no facade or
    // building front within 4 m of their middle.
    if (args.size() >= 2 && std::string_view(args[1]) == "walls") {
        auto segDist = [](const Vec3& p, const Vec3& a, const Vec3& b) {
            const float dx = b.x - a.x, dz = b.z - a.z;
            const float l2 = dx * dx + dz * dz;
            float t = l2 > 0 ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / l2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            return std::hypot(p.x - (a.x + dx * t), p.z - (a.z + dz * t));
        };
        // The cells' vertical triangles (their H meshes), in a 16 m grid.
        struct Tri {
            Vec3 a, b, c, n;
            Aabb box;
        };
        std::vector<Tri> tris;
        for (const auto& cell : c->cells)
            for (std::string_view g : {"H", "H2"}) {
                auto mb = v.readAll(c->cellMeshPath(cell.index, g, true));
                if (!mb && g == "H")
                    mb = v.readAll(std::format("{}bms/{}/cull{:02}.bms", vfs::kMm1Prefix, c->cellMeshDir(cell.index, true),
                                               cell.index));
                if (!mb)
                    continue;
                auto mesh = asset::parseBms(*mb);
                if (!mesh)
                    continue;
                for (const auto& sf : mesh->surfaces) {
                    auto P = [&](int k) { return mesh->positions[mesh->adjunctPosition[sf.corners[static_cast<std::size_t>(k)]]]; };
                    const int k = sf.quad() ? 4 : 3;
                    for (int t = 0; t + 2 < k + (k == 4 ? 0 : 0) && t < k - 2; ++t) {
                        Tri tr{P(0), P(t + 1), P(t + 2), {}, {}};
                        Vec3 n = (tr.b - tr.a).cross(tr.c - tr.a);
                        const float l = n.mag();
                        if (l < 1e-6f)
                            continue;
                        tr.n = n * (1.0f / l);
                        if (std::abs(tr.n.y) > 0.5f)
                            continue;
                        tr.box.expand(tr.a);
                        tr.box.expand(tr.b);
                        tr.box.expand(tr.c);
                        tris.push_back(tr);
                    }
                }
            }
        auto drawnNear = [&](const Vec3& q) {
            for (const auto& tr : tris) {
                if (q.x < tr.box.min.x - 1.5f || q.x > tr.box.max.x + 1.5f || q.z < tr.box.min.z - 1.5f ||
                    q.z > tr.box.max.z + 1.5f || q.y < tr.box.min.y - 0.5f || q.y > tr.box.max.y + 0.5f)
                    continue;
                if (std::abs((q - tr.a).dot(tr.n)) < 1.5f)
                    return true;
            }
            return false;
        };
        int bare = 0;
        std::map<int, int> perCell;
        // MM1WALLS_BOX=x0,z0,x1,z1: every generated wall in the box.
        float bx0 = 0, bz0 = 0, bx1 = 0, bz1 = 0;
        const bool box = std::getenv("MM1WALLS_BOX") &&
                         std::sscanf(std::getenv("MM1WALLS_BOX"), "%f,%f,%f,%f", &bx0, &bz0, &bx1, &bz1) == 4;
        for (const auto& [wa, wb] : level.walls()) {
            const Vec3 mid = (wa + wb) * 0.5f;
            if (box && mid.x >= bx0 && mid.x <= bx1 && mid.z >= bz0 && mid.z <= bz1)
                std::println("wall ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) ground {} cell {}", wa.x, wa.y, wa.z, wb.x,
                             wb.y, wb.z, groundAt(mid + Vec3{0, 0.5f, 0}), level.findRoom(mid + Vec3{0, 1, 0}, 0));
            bool covered = false;
            for (const auto& f : c->facades) {
                if (segDist(mid, f.start, f.end) < 4.0f) {
                    covered = true;
                    break;
                }
                if ((f.flags & 0x4) && !(f.flags & 0x2)) {
                    // A building: rows end - start and sides - start from start.
                    const Vec3 c3 = f.start + (f.end - f.start) + (f.sides - f.start);
                    if (segDist(mid, f.start, f.end) < 4.0f || segDist(mid, f.end, c3) < 4.0f ||
                        segDist(mid, f.start, f.sides) < 4.0f || segDist(mid, f.sides, c3) < 4.0f) {
                        covered = true;
                        break;
                    }
                }
            }
            if (covered || drawnNear(mid + Vec3{0, 2.0f, 0}))
                continue;
            ++bare;
            const int cell = level.findRoom(mid + Vec3{0, 1, 0}, 0);
            ++perCell[cell];
            if (std::getenv("MM1WALLS_ALL"))
                std::println("bare wall ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) ground {} cell {}", wa.x, wa.y, wa.z, wb.x,
                             wb.y, wb.z, groundAt(mid + Vec3{0, 0.5f, 0}), cell);
        }
        std::println("{} of {} walls have no facade within 4 m", bare, level.walls().size());
        return 0;
    }
    // Facades across a lane: a facade's front line crossing a lane segment
    // with the lane between its foot and 10 m above it.
    if (args.size() >= 2 && std::string_view(args[1]) == "facades") {
        auto crosses = [](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
            auto side = [](const Vec3& p, const Vec3& q, const Vec3& r) {
                return (q.x - p.x) * (r.z - p.z) - (q.z - p.z) * (r.x - p.x);
            };
            return side(a, b, c) * side(a, b, d) < 0.0f && side(c, d, a) * side(c, d, b) < 0.0f;
        };
        int hits = 0;
        std::set<std::size_t> seen;
        for (std::size_t f = 0; f < c->facades.size(); ++f) {
            const auto& fc = c->facades[f];
            const float foot = std::min(fc.start.y, fc.end.y);
            for (const auto& path : map.paths) {
                for (const city::AiRoadSide* side : {&path.right, &path.left}) {
                    for (int r = 0; r < side->numLanes && r < static_cast<int>(side->polylines.size()); ++r) {
                        const auto& line = side->polylines[static_cast<std::size_t>(r)];
                        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                            const float y = (line[i].y + line[i + 1].y) * 0.5f;
                            if (y < foot - 1.0f || y > foot + 10.0f || !crosses(fc.start, fc.end, line[i], line[i + 1]))
                                continue;
                            if (seen.insert(f).second && hits++ < 60)
                                std::println("{} flags 0x{:X} cell {} ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) sides ({:.1f},{:.1f},{:.1f}) lane y {:.1f} path {}",
                                             fc.name, fc.flags, fc.cell, fc.start.x, fc.start.y, fc.start.z, fc.end.x,
                                             fc.end.y, fc.end.z, fc.sides.x, fc.sides.y, fc.sides.z, y, path.id);
                        }
                    }
                }
            }
        }
        std::println("{} facades cross a lane", hits);
        return 0;
    }
    // Holes: lane and sidewalk points with no ground polygon under them.
    if (args.size() >= 2 && std::string_view(args[1]) == "holes") {
        int checked = 0, holes = 0, gathering = 0, soupMisses = 0;
        std::map<int, std::pair<int, Vec3>> perPath;
        std::map<int, int> perCell;
        for (const auto& path : map.paths) {
            for (const city::AiRoadSide* side : {&path.right, &path.left}) {
                for (std::size_t r = 0; r < side->polylines.size(); ++r) {
                    // Lanes only (and not on a drawbridge road: its deck is a
                    // moving body, not in the level's polygons).
                    if (static_cast<int>(r) >= side->numLanes || (side->roadType & 2))
                        break;
                    const auto& line = side->polylines[r];
                    for (std::size_t i = 0; i + 1 < line.size(); ++i)
                        for (int h = 0; h < 2; ++h) {
                            const Vec3 q = h == 0 ? line[i] : (line[i] + line[i + 1]) * 0.5f;
                            ++checked;
                            if (!soupGround(q)) {
                                ++soupMisses;
                                auto& rec = perPath[path.id];
                                if (rec.first++ == 0)
                                    rec.second = q;
                            }
                            if (groundAt(q) == "none") {
                                if (groundAnywhere(q, nullptr)) {
                                    ++gathering;
                                    continue;
                                }
                                ++holes;
                                ++perCell[level.findRoom(q + Vec3{0, 1, 0}, 0)];
                                if (holes <= 40)
                                    std::println("hole at ({:.1f}, {:.1f}, {:.1f}) path {} cell {}", q.x, q.y, q.z, path.id,
                                                 level.findRoom(q + Vec3{0, 1, 0}, 0));
                            }
                        }
                }
            }
        }
        std::println("{} lane and sidewalk points checked: {} with no ground polygon in any cell (in {} cells), {} covered "
                     "only by another cell's polygon",
                     checked, holes, perCell.size(), gathering);
        std::println("the polygon soup (the game's ground query) misses {} of them, on {} roads:", soupMisses, perPath.size());
        std::vector<std::pair<int, int>> order;
        for (const auto& [id, rec] : perPath)
            order.emplace_back(rec.first, id);
        std::ranges::sort(order, std::greater<>());
        for (std::size_t i = 0; i < order.size() && i < 25; ++i) {
            const auto& rec = perPath[order[i].second];
            std::println("  path {}: {} points, the first at ({:.1f}, {:.1f}, {:.1f})", order[i].second, rec.first,
                         rec.second.x, rec.second.y, rec.second.z);
        }
        return 0;
    }
    int lanes = 0, bad = 0;
    for (const auto& path : map.paths) {
        for (const city::AiRoadSide* side : {&path.right, &path.left}) {
            for (int l = 0; l < side->numLanes && static_cast<std::size_t>(l) < side->polylines.size(); ++l) {
                ++lanes;
                const auto& line = side->polylines[static_cast<std::size_t>(l)];
                std::map<std::string, int> off;
                Vec3 at{};
                for (std::size_t i = 0; i < line.size(); ++i) {
                    // The points and the midpoints between them.
                    for (int h = 0; h < (i + 1 < line.size() ? 2 : 1); ++h) {
                        const Vec3 q = h == 0 ? line[i] : (line[i] + line[i + 1]) * 0.5f;
                        const std::string g = groundAt(q);
                        if (g == "grass" || g == "dirt" || g == "deepwater") {
                            if (off.empty())
                                at = q;
                            ++off[g];
                        }
                    }
                }
                if (off.empty())
                    continue;
                if (side->roadType & 1)
                    continue; // closed to ambient traffic (MM1's IsAlwaysBlocked, or a bridge)
                ++bad;
                std::string what;
                for (const auto& [g, n] : off)
                    what += std::format(" {} x{}", g, n);
                std::println("path {} {} lane {}: off the road at ({:.1f}, {:.1f}, {:.1f}):{}", path.id,
                             side == &path.right ? "right" : "left", l, at.x, at.y,
                             at.z, what);
            }
        }
    }
    std::println("{} lanes, {} open to traffic with points over grass, dirt or water", lanes, bad);
    return 0;
}

const Registrar r1({"bmsinfo", "<container> <bms>", "describe an MM1 mesh (.bms)", &cmdBmsInfo});
const Registrar r2({"bmscheck", "<container> [glob]", "parse every MM1 mesh (.bms) and report", &cmdBmsCheck});
const Registrar r3({"mm1map", "<container> <glob> <out.png> [size] [--fcd city/<name>.fcd]",
                    "top-down untextured render of the MM1 meshes matching glob", &cmdMm1Map});
const Registrar r4({"mm1tex", "<container> <name> <out.png> [--night] [--rain|--snow] [--variation N]",
                    "resolve an MM1 texture name through mtl/global.tsh and write it as PNG", &cmdMm1Tex});
const Registrar r5({"mm1texcheck", "<container>", "decode every MM1 texture and check the texture sheet",
                    &cmdMm1TexCheck});
const Registrar r6({"mm1cityinfo", "<mm1 source> [city]",
                    "mount an MM1 disc, folder or CORE.AR and summarise a city (cells, portals, meshes)",
                    &cmdMm1CityInfo});
const Registrar r7({"mm1probe", "<mm1 source> <x> <y> <z>", "Chicago's collision at a point (cell, polygons)",
                    &cmdMm1Probe});
const Registrar r10({"mm1carsim", "<mm1 source> [car...]", "parse MM1 tune/<car>.mmcarsim files and summarise them",
                     &cmdMm1CarSim});
const Registrar r8({"mm1bnd", "<container> <bnd>", "describe an MM1 bound (2DNB)", &cmdMm1Bnd});
const Registrar r9({"mm1lanecheck", "<mm1 source> [path]", "traffic lanes over grass, dirt or water in Chicago",
                    &cmdMm1LaneCheck});

} // namespace
} // namespace mm2::tool
