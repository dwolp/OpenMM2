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

    // mmSky: the dome (bms/mmsky.bms) around the camera with the sky texture
    // of the look, sky_<time><weather> (mmEnvSetup: times m, n, s, d;
    // weathers c, f, r, s). `skyIndex` is Environment::skyPaintjob (time * 4
    // + MM2's weather; cloudy takes clear; `snow` takes MM1's s). Unlit,
    // unfogged, no depth.
    void drawSky(const Camera& camera, int skyIndex, bool snow, int& drawCalls);

    // OpenMM2 Enhanced: where a drawbridge part (city.bridges[set].parts[part])
    // draws now (game::world::Mm1Bridges moves them; until then each part's
    // frame from the gizmo file).
    // `visible` false hides it (a crossing gate knocked loose: the props draw it).
    void setBridgePart(std::size_t set, std::size_t part, const Mat34& transform, bool visible = true);
    // OpenMM2 Enhanced: a model that moves (MM1's El train cars, the plane;
    // game::world::Mm1Anim), drawn as facades are: its plain H/M/L/VL
    // meshes. Returns its index (-1 without meshes) for setMovingModel.
    int addMovingModel(const std::string& model);
    void setMovingModel(int index, const Mat34& transform);

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
        Vec3 localCentre; // the meshes' centre in the facade's frame (moving ones)
        bool hidden = false;
    };

    // A facade of `model`'s plain meshes (and a drawbridge leaf's RAIL
    // group) at `transform`; its index, -1 without meshes.
    int addPartFacade(const std::string& model, const Mat34& transform, bool rail);
    // The mesh at `path` (cached; null when missing or broken).
    // With `clipBelow`, a copy cut off below that height in the mesh's own
    // frame (a facade's side quad hidden by its neighbour); null when nothing
    // is left above it.
    Mesh* mesh(const std::string& path, std::optional<float> clipBelow = std::nullopt);
    // Remakes every mesh's materials for the look (night alternates).
    void resolveMaterials(bool night);
    void drawMesh(Mesh& m, const Mat44& world, Stats& stats, bool cull = false);

    render::Device& m_device;
    TextureLibrary& m_textures;
    ModelLibrary& m_models;
    const vfs::Vfs& m_vfs;
    const city::Mm1City& m_city;
    std::optional<asset::Mm1TexSheet> m_sheet;
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_meshes; // by path; null = missing
    std::vector<Cell> m_cells;
    std::vector<Facade> m_facades;
    // Each bridge set's parts' facade indices (-1: nothing drawn).
    std::vector<std::vector<int>> m_bridgeFacades;
    int m_missing = 0;
    std::optional<bool> m_night; // the look the materials were made for
    asset::Mm1TexWeather m_weather = asset::Mm1TexWeather::Clear;
    Mesh* m_sky = nullptr;
};

} // namespace mm2::game
