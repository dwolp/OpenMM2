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
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
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
                xf = city::mm1MatrixFromPoints(f.start, f.end, f.width);
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
        std::println("{}:{}; {} cells without CULL<n> or _H", lm ? "landmarks on " : "landmarks off", line,
                     noHigh);
    }
    return 0;
}

// Collision at a point: the cell found there and the polygons collected.
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
    int touched[16];
    const int n = level.touchedNeighbors(touched, 16, room, p, 2.0f);
    std::vector<int> rooms{room};
    rooms.insert(rooms.end(), touched, touched + n);
    phys::LevelBound out;
    level.collect(rooms.data(), static_cast<int>(rooms.size()), p, 2.0f, out);
    std::println("room {} (+{} touched); {} polygons within 2 m", room, n, out.polygons.size());
    for (const auto& poly : out.polygons) {
        float y = 0;
        int k = poly.v[3] ? 4 : 3;
        for (int i = 0; i < k; ++i)
            y += out.vertices[poly.v[static_cast<std::size_t>(i)]].y / static_cast<float>(k);
        std::println("  normal ({:.2f}, {:.2f}, {:.2f}) mean y {:.2f}", poly.normal.x, poly.normal.y, poly.normal.z, y);
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
const Registrar r8({"mm1bnd", "<container> <bnd>", "describe an MM1 bound (2DNB)", &cmdMm1Bnd});

} // namespace
} // namespace mm2::tool
