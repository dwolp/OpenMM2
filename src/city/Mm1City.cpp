#include "city/Mm1City.h"

#include "core/File.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <charconv>
#include <format>

namespace mm2::city {
namespace {

constexpr std::size_t kPortalRecord = 0x24;
constexpr int kMaxCellIndex = 1 << 16;

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

std::optional<int> toInt(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    int v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || s.empty())
        return std::nullopt;
    return v;
}

Vec3 vec3At(const std::byte* p) { return {loadLE<float>(p), loadLE<float>(p + 4), loadLE<float>(p + 8)}; }

std::string_view textOf(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

} // namespace

std::optional<std::vector<Mm1Cell>> parseMm1Cells(std::string_view text, int* maxCells, std::string* error) {
    std::vector<std::string_view> lines;
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
    }
    if (lines.size() < 2) {
        setError(error, "missing the cell counts");
        return std::nullopt;
    }
    const auto count = toInt(lines[0]);
    const auto maxId = toInt(lines[1]);
    if (!count || !maxId || *count < 0 || *maxId < 0 || *maxId >= kMaxCellIndex ||
        lines.size() < 2 + static_cast<std::size_t>(*count)) {
        setError(error, "bad cell counts");
        return std::nullopt;
    }
    if (maxCells)
        *maxCells = *maxId + 1; // LoadCells: MaxCells = atoi + 1
    std::vector<Mm1Cell> cells;
    cells.reserve(static_cast<std::size_t>(*count));
    for (int i = 0; i < *count; ++i) {
        // "id,cullFlags" then, separated by commas or spaces, roomFlags,
        // tagCount and the tags (LoadCells' arts_strtok with ", \n").
        const std::string_view line = lines[2 + static_cast<std::size_t>(i)];
        std::vector<std::string_view> f;
        std::size_t start = 0;
        for (std::size_t j = 0; j <= line.size(); ++j) {
            if (j == line.size() || line[j] == ',' || line[j] == ' ') {
                if (j > start)
                    f.push_back(line.substr(start, j - start));
                start = j + 1;
            }
        }
        Mm1Cell c;
        const auto id = f.size() > 0 ? toInt(f[0]) : std::nullopt;
        const auto cull = f.size() > 1 ? toInt(f[1]) : std::nullopt;
        if (!id || !cull || *id < 1 || *id > *maxId) {
            setError(error, std::format("cell line {}: '{}'", i + 3, line));
            return std::nullopt;
        }
        c.index = *id;
        c.cullFlags = *cull;
        if (f.size() > 2)
            c.roomFlags = toInt(f[2]).value_or(0);
        if (f.size() > 3) {
            const int tags = toInt(f[3]).value_or(0);
            for (int t = 0; t < tags && 4 + static_cast<std::size_t>(t) < f.size(); ++t)
                c.visitTags.push_back(toInt(f[4 + static_cast<std::size_t>(t)]).value_or(0));
        }
        cells.push_back(std::move(c));
    }
    return cells;
}

std::optional<std::vector<Mm1Portal>> parseMm1Portals(std::span<const std::byte> data, std::string* error) {
    if (data.size() < 8) {
        setError(error, "file too small");
        return std::nullopt;
    }
    if (loadLE<std::int32_t>(data.data()) != 0) {
        setError(error, "unexpected PTL vector data");
        return std::nullopt;
    }
    const auto count = loadLE<std::int32_t>(data.data() + 4);
    if (count < 0 || count > (1 << 20)) {
        setError(error, std::format("bad portal count {}", count));
        return std::nullopt;
    }
    std::vector<Mm1Portal> out;
    out.reserve(static_cast<std::size_t>(count));
    std::size_t pos = 8;
    for (int i = 0; i < count; ++i) {
        if (pos + kPortalRecord > data.size()) {
            setError(error, std::format("portal {} truncated", i));
            return std::nullopt;
        }
        const std::byte* p = data.data() + pos;
        Mm1Portal portal;
        portal.flags = std::to_integer<std::uint8_t>(p[0]);
        portal.edgeCount = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(p[1]));
        portal.cell1 = loadLE<std::int16_t>(p + 4);
        portal.cell2 = loadLE<std::int16_t>(p + 6);
        portal.height = loadLE<float>(p + 8);
        const Vec3 mn = vec3At(p + 12), mx = vec3At(p + 24);
        pos += kPortalRecord;
        if (portal.edgeCount < 2 || portal.edgeCount > 3) {
            setError(error, std::format("portal {} has {} edges", i, portal.edgeCount));
            return std::nullopt;
        }
        // LoadPortals' polygon: the top edge, then down the far side.
        portal.polygon = {Vec3{mn.x, mn.y + portal.height, mn.z}, Vec3{mx.x, mx.y + portal.height, mx.z}, mx};
        if (portal.edgeCount == 3) {
            if (pos + 12 > data.size()) {
                setError(error, std::format("portal {} truncated", i));
                return std::nullopt;
            }
            portal.polygon.push_back(vec3At(data.data() + pos));
            pos += 12;
        }
        portal.polygon.push_back(mn);
        out.push_back(std::move(portal));
    }
    if (pos != data.size()) {
        setError(error, std::format("{} bytes after the last portal", data.size() - pos));
        return std::nullopt;
    }
    return out;
}

Mat34 Mm1GizmoPart::matrix() const {
    Vec3 x{axisPoint.x - position.x, 0.0f, axisPoint.z - position.z};
    const float len = x.mag();
    x = len > 0.0f ? x * (1.0f / len) : Vec3{1, 0, 0};
    Mat34 m;
    m.m0 = x;
    m.m1 = {0.0f, 1.0f, 0.0f};
    m.m2 = x.cross(m.m1);
    m.m3 = position;
    return m;
}

std::vector<Mm1BridgeSet> parseMm1Gizmo(std::string_view text) {
    std::vector<Mm1BridgeSet> sets;
    Mm1BridgeSet* open = nullptr;
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        const bool indented = line.front() == '\t' || line.front() == ' ';
        if (!indented) {
            // A set's name opens it; the same name again closes it.
            if (open && open->name == line)
                open = nullptr;
            else
                open = &sets.emplace_back(Mm1BridgeSet{std::string(line), {}});
            continue;
        }
        if (!open)
            continue;
        while (!line.empty() && (line.front() == '\t' || line.front() == ' '))
            line.remove_prefix(1);
        const auto f = str::split(line, ',');
        if (f.size() < 8)
            continue;
        auto num = [&](std::size_t i) { return static_cast<float>(str::parseDouble(f[i]).value_or(0.0)); };
        open->parts.push_back(
            Mm1GizmoPart{str::lower(f[0]), Vec3{num(2), num(3), num(4)}, Vec3{num(5), num(6), num(7)}});
    }
    return sets;
}

std::string Mm1City::cellMeshDir(int index, bool landmarks) const {
    return std::format("{}{}", name, (landmarks && index < kMm1CityMeshStart) ? "lm" : "city");
}

std::string Mm1City::cellMeshPath(int index, std::string_view group, bool landmarks) const {
    // CULL%02d_<group>, lower case in the archive.
    return std::format("{}bms/{}/cull{:02}_{}.bms", vfs::kMm1Prefix, cellMeshDir(index, landmarks), index,
                       str::lower(group));
}

bool hasMm1City(const vfs::Vfs& vfs, std::string_view name) {
    return vfs.exists(std::format("{}city/{}.cells", vfs::kMm1Prefix, str::lower(name)));
}

std::optional<Mm1City> loadMm1City(const vfs::Vfs& vfs, std::string_view name, std::string* error) {
    Mm1City c;
    c.name = str::lower(name);
    const std::string base = std::format("{}city/{}", vfs::kMm1Prefix, c.name);
    std::string err;

    auto cells = vfs.readAll(base + ".cells");
    if (!cells) {
        setError(error, base + ".cells not found");
        return std::nullopt;
    }
    auto parsedCells = parseMm1Cells(textOf(*cells), &c.maxCells, &err);
    if (!parsedCells) {
        setError(error, std::format("{}.cells: {}", base, err));
        return std::nullopt;
    }
    c.cells = std::move(*parsedCells);

    auto ptl = vfs.readAll(base + ".ptl");
    if (!ptl) {
        setError(error, base + ".ptl not found");
        return std::nullopt;
    }
    auto portals = parseMm1Portals(*ptl, &err);
    if (!portals) {
        setError(error, std::format("{}.ptl: {}", base, err));
        return std::nullopt;
    }
    // LoadPortals skips portals to cells that do not exist ("Bad portal
    // index", "Cell doesn't exist").
    std::vector<bool> present(static_cast<std::size_t>(c.maxCells), false);
    for (const auto& cell : c.cells)
        present[static_cast<std::size_t>(cell.index)] = true;
    for (auto& p : *portals) {
        const bool ok = p.cell1 > 0 && p.cell2 > 0 && p.cell1 < c.maxCells && p.cell2 < c.maxCells &&
                        present[static_cast<std::size_t>(p.cell1)] && present[static_cast<std::size_t>(p.cell2)];
        if (ok)
            c.portals.push_back(std::move(p));
        else
            c.warnings.push_back(std::format("portal {} - {}: no such cell", p.cell1, p.cell2));
    }

    if (auto b = vfs.readAll(base + ".fcd")) {
        if (auto f = parseMm1Fcd(*b, &err))
            c.facades = std::move(*f);
        else
            c.warnings.push_back(std::format("{}.fcd: {}", base, err));
    } else {
        c.warnings.push_back(base + ".fcd: missing");
    }
    if (auto b = vfs.readAll(base + ".bng")) {
        if (auto f = parseMm1Bng(*b, &err))
            c.bangers = std::move(*f);
        else
            c.warnings.push_back(std::format("{}.bng: {}", base, err));
    } else {
        c.warnings.push_back(base + ".bng: missing");
    }
    if (auto b = vfs.readAll(base + ".gizmo"))
        c.bridges = parseMm1Gizmo(textOf(*b));
    if (auto b = vfs.readAll(base + ".ext")) {
        const auto parts = str::split(textOf(*b), ' ');
        std::array<float, 4> e{};
        int n = 0;
        for (const auto& part : parts) {
            if (n == 4)
                break;
            if (auto v = str::parseDouble(part))
                e[static_cast<std::size_t>(n++)] = static_cast<float>(*v);
        }
        if (n == 4)
            c.extent = e;
        else
            c.warnings.push_back(base + ".ext: expected four numbers");
    }
    return c;
}

} // namespace mm2::city
