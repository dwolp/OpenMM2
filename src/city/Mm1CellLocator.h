#pragma once

// OpenMM2 Enhanced (not in MM2): which cell of a Midtown Madness 1 city a
// point is in, from the city's footprints (bnd/<city>_hitid.bnd, a polygon
// set per cell, asRenderWeb::LoadHitId). Shared by the collision
// (game::Mm1Level), the AI's room lookups and the road network's
// intersections.

#include "core/Math.h"
#include "vfs/Vfs.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::city {

class Mm1CellLocator {
public:
    struct Footprint {
        int cell = 0;
        std::array<Vec3, 4> v{};
        int count = 3;
        Vec3 normal;
        float d = 0;
        // The footprint's height at (x, z): its plane's.
        float heightAt(float x, float z) const;
    };

    // Loads mm1/bnd/<city>_hitid.bnd. Null when it is missing or broken.
    static std::optional<Mm1CellLocator> load(const vfs::Vfs& vfs, std::string_view city,
                                              std::string* error = nullptr);

    // The cell holding `position`: of the footprints under it, the highest at
    // most 1 m above it (a car on a bridge is in the bridge's cell, one below
    // it in the lower street's), `hint` preferred within 0.5 m; failing that
    // the lowest above it; 0 when none.
    int find(const Vec3& position, int hint = 0) const;
    // Whether a footprint holds `p` in the ground plane within 3 m of `y`.
    bool footprintNear(const Vec3& p, float y) const;

    const std::vector<Footprint>& footprints() const { return m_footprints; }

private:
    const std::vector<int>* bucket(float x, float z) const;

    std::vector<Footprint> m_footprints;
    // A uniform grid over the footprints in the ground plane.
    Vec3 m_gridMin;
    float m_gridCell = 32.0f;
    int m_gridW = 0, m_gridH = 0;
    std::vector<std::vector<int>> m_grid;
};

} // namespace mm2::city
