#include "city/Mm1CellLocator.h"

#include "asset/Mm1Bound.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::city {
namespace {

// Whether (x, z) lies inside the convex polygon in the ground plane (either winding).
bool insideXZ(const std::array<Vec3, 4>& v, int n, float x, float z) {
    int sign = 0;
    for (int i = 0; i < n; ++i) {
        const Vec3& a = v[static_cast<std::size_t>(i)];
        const Vec3& b = v[static_cast<std::size_t>((i + 1) % n)];
        const float c = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
        const int s = c > 1e-4f ? 1 : (c < -1e-4f ? -1 : 0);
        if (s == 0)
            continue;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return true;
}

} // namespace

float Mm1CellLocator::Footprint::heightAt(float x, float z) const {
    return std::fabs(normal.y) > 0.01f ? -(normal.x * x + normal.z * z + d) / normal.y : v[0].y;
}

std::optional<Mm1CellLocator> Mm1CellLocator::load(const vfs::Vfs& vfs, std::string_view city, std::string* error) {
    const std::string path = std::format("{}bnd/{}_hitid.bnd", vfs::kMm1Prefix, city);
    auto bytes = vfs.readAll(path);
    if (!bytes) {
        if (error)
            *error = path + " not found";
        return std::nullopt;
    }
    std::string err;
    auto hit = asset::parseMm1Bound(*bytes, &err);
    if (!hit) {
        if (error)
            *error = std::format("{}: {}", path, err);
        return std::nullopt;
    }
    Mm1CellLocator loc;
    Aabb box;
    for (const auto& p : hit->polygons) {
        Footprint f;
        f.cell = p.room;
        f.count = p.quad() ? 4 : 3;
        for (int i = 0; i < f.count; ++i) {
            f.v[static_cast<std::size_t>(i)] =
                hit->vertices[static_cast<std::size_t>(p.vertices[static_cast<std::size_t>(i)])];
            box.expand(f.v[static_cast<std::size_t>(i)]);
        }
        f.normal = p.normal;
        f.d = p.d;
        loc.m_footprints.push_back(f);
    }
    if (!box.valid())
        return loc;
    loc.m_gridMin = box.min;
    loc.m_gridW = static_cast<int>((box.max.x - box.min.x) / loc.m_gridCell) + 1;
    loc.m_gridH = static_cast<int>((box.max.z - box.min.z) / loc.m_gridCell) + 1;
    loc.m_grid.resize(static_cast<std::size_t>(loc.m_gridW) * static_cast<std::size_t>(loc.m_gridH));
    for (std::size_t i = 0; i < loc.m_footprints.size(); ++i) {
        Aabb fb;
        for (int k = 0; k < loc.m_footprints[i].count; ++k)
            fb.expand(loc.m_footprints[i].v[static_cast<std::size_t>(k)]);
        const int x0 = static_cast<int>((fb.min.x - loc.m_gridMin.x) / loc.m_gridCell);
        const int x1 = static_cast<int>((fb.max.x - loc.m_gridMin.x) / loc.m_gridCell);
        const int z0 = static_cast<int>((fb.min.z - loc.m_gridMin.z) / loc.m_gridCell);
        const int z1 = static_cast<int>((fb.max.z - loc.m_gridMin.z) / loc.m_gridCell);
        for (int z = std::max(z0, 0); z <= std::min(z1, loc.m_gridH - 1); ++z)
            for (int x = std::max(x0, 0); x <= std::min(x1, loc.m_gridW - 1); ++x)
                loc.m_grid[static_cast<std::size_t>(z) * static_cast<std::size_t>(loc.m_gridW) +
                           static_cast<std::size_t>(x)]
                    .push_back(static_cast<int>(i));
    }
    return loc;
}

const std::vector<int>* Mm1CellLocator::bucket(float x, float z) const {
    if (m_grid.empty())
        return nullptr;
    const int gx = static_cast<int>(std::floor((x - m_gridMin.x) / m_gridCell));
    const int gz = static_cast<int>(std::floor((z - m_gridMin.z) / m_gridCell));
    if (gx < 0 || gz < 0 || gx >= m_gridW || gz >= m_gridH)
        return nullptr;
    return &m_grid[static_cast<std::size_t>(gz) * static_cast<std::size_t>(m_gridW) + static_cast<std::size_t>(gx)];
}

int Mm1CellLocator::find(const Vec3& position, int hint) const {
    const auto* list = bucket(position.x, position.z);
    if (!list)
        return 0;
    int best = 0, above = 0;
    float bestY = -1e30f, aboveY = 1e30f;
    for (int i : *list) {
        const Footprint& f = m_footprints[static_cast<std::size_t>(i)];
        if (!insideXZ(f.v, f.count, position.x, position.z))
            continue;
        const float y = f.heightAt(position.x, position.z);
        const bool preferHint = f.cell == hint;
        if (y <= position.y + 1.0f) {
            if (y > bestY + (preferHint ? -0.5f : 0.0f)) {
                bestY = y;
                best = f.cell;
            }
        } else if (y < aboveY) {
            aboveY = y;
            above = f.cell;
        }
    }
    return best ? best : above;
}

bool Mm1CellLocator::footprintNear(const Vec3& p, float y) const {
    const auto* list = bucket(p.x, p.z);
    if (!list)
        return false;
    for (int i : *list) {
        const Footprint& f = m_footprints[static_cast<std::size_t>(i)];
        if (insideXZ(f.v, f.count, p.x, p.z) && std::fabs(f.heightAt(p.x, p.z) - y) <= 3.0f)
            return true;
    }
    return false;
}

} // namespace mm2::city
