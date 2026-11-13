#pragma once

// OpenMM2 Enhanced (not in MM2): draws a Midtown Madness 1 city (Chicago)
// through the same mesh and texture paths as MM2's city, so the fork's
// graphics (ray traced shadows and reflections, water, glowing windows)
// apply to it. CityRenderer hands its draws to this when the city is MM1's.
//
// What is drawn, after Open1560's mmCellRenderer and mmFacadeInstance:
// every cell's prebuilt meshes (CULL<n> groups H/M/L/A and H2/M2/L2/A2) and
// the facades and buildings of city/<city>.fcd. Textures resolve through
// MM1's texture sheet (asset::Mm1TexSheet) for the time of day.
//
// First pass, still to port: MM1's portal culling (every cell in the
// frustum and within the far clip is drawn), its exact level-of-detail
// distances (StaticTerrainLodTable's use is inferred), the facades' side
// clipping, texture variations, and snow.

#include "asset/Mm1TexSheet.h"
#include "game/Camera.h"
#include "game/MeshDraw.h"
#include "game/ModelLibrary.h"
#include "game/TextureLibrary.h"
#include "city/Mm1City.h"
#include "render/Device.h"
#include "vfs/Vfs.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mm2::game {

struct DetailSettings;

class Mm1CityRenderer {
public:
    Mm1CityRenderer(render::Device& device, TextureLibrary& textures, ModelLibrary& models, const vfs::Vfs& vfs,
                    const city::Mm1City& city);

    struct Stats {
        int cellsDrawn = 0;
        int facadesDrawn = 0;
        int drawCalls = 0;
    };

    // Inside a scene pass, after the frame constants are set.
    void draw(const Camera& camera, const Frustum& frustum, const DetailSettings& detail, float farClip,
              Stats& stats);

    int meshCount() const { return static_cast<int>(m_meshes.size()); }
    int missingMeshes() const { return m_missing; }

private:
    // One MM1 mesh on the GPU, with its texture names as stored (MM1's) and
    // the materials made from them for the current look.
    struct Mesh {
        const GpuModel* model = nullptr;
        std::vector<std::string> textures;
        std::vector<asset::PkgMaterial> materials;
        Vec3 centre;
        float radius = 0.0f;
    };
    struct Cell {
        int index = 0;
        std::array<Mesh*, 8> groups{}; // city::kMm1CellGroups' order; null = none
        Vec3 centre;
        float radius = 0.0f;
    };
    struct Facade {
        Mat34 transform;
        Mat44 world;
        // The groups to draw (FACADE/BLDG, GRND, TOP, LEFT, RIGHT, BACK), each
        // with its levels of detail H, M, L, VL (null where missing).
        std::vector<std::array<Mesh*, 4>> groups;
        Vec3 centre;
        float radius = 0.0f;
    };

    // The mesh at `path` (cached; null when missing or broken).
    Mesh* mesh(const std::string& path);
    // Remakes every mesh's materials for the look (night alternates).
    void resolveMaterials(bool night);
    void drawMesh(Mesh& m, const Mat44& world, Stats& stats);

    render::Device& m_device;
    TextureLibrary& m_textures;
    ModelLibrary& m_models;
    const vfs::Vfs& m_vfs;
    const city::Mm1City& m_city;
    std::optional<asset::Mm1TexSheet> m_sheet;
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_meshes; // by path; null = missing
    std::vector<Cell> m_cells;
    std::vector<Facade> m_facades;
    int m_missing = 0;
    std::optional<bool> m_night; // the look the materials were made for
};

} // namespace mm2::game
