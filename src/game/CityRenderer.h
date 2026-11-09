#pragma once

#include "city/CityData.h"
#include "city/CityMesh.h"
#include "city/SdlDraw.h"
#include "game/Camera.h"
#include "game/MeshDraw.h"
#include "game/ModelLibrary.h"
#include "game/RaceConfig.h"
#include "city/RoomLocator.h"
#include "game/RoomVisibility.h"
#include "game/TextureLibrary.h"
#include "render/Device.h"

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::game {

// Graphics options that shape the environment.
struct EnvironmentOptions {
    // gxLightQuality 0-3: how many of the key, fill1 and fill2 lights are on
    // and which ambient level is used (cityLevel::SetupLighting).
    int lightQuality = 3;
    // The Far Clip option in metres (MM2: 100-1000): the camera's far plane,
    // clamping the fog.
    float farClip = 1000.0f;
    // The Cloud Shadows option: 0 none, 1 low, 2 high (gfxCloudShadows).
    int cloudShadows = 2;
    // The Textured Sky option (gfxEnableSky): mmGame::SetLevelGraphics
    // passes it to cityLevel::EnableSky; without it lvlSky::Draw draws
    // nothing and the fog colour the frame is cleared to shows instead.
    bool texturedSky = true;
};

// Lighting, fog and sky for one time of day and weather.
struct Environment {
    render::FrameConstants frame; // lights, ambient and fog (view/proj filled per frame)
    int skyPaintjob = 0;
    float farClip = 1000.0f;
    float fogEnd = 1000.0f;
    Vec4 clearColor{0.5f, 0.6f, 0.7f, 1.0f};
    // The 64 wall shades of sdlCommon::UpdateLighting (0xAARRGGBB): ambient
    // plus the lights falling on a vertical wall facing angle
    // i * pi / 32 - pi / 2.
    std::array<std::uint32_t, 64> wallShades{};
    // Cloud shadows: the texture (shadmap_day in the morning and at noon,
    // shadmap_nite in the evening and at night) and the .tex flag bit a
    // texture needs to receive them (vglCloudMapEnable: 4 for the low
    // setting, 2 for high, 0 none).
    std::string cloudMap;
    std::uint32_t cloudMask = 0;
    bool sky = true; // EnvironmentOptions::texturedSky
};

// Builds the environment from the city's .ltNN and _fog.csv tables.
// Snow (an OpenMM2 option; MM2's weather is 0-3) uses the rain tables.
Environment makeEnvironment(const city::CityData& city, TimeOfDay time, Weather weather,
                            const EnvironmentOptions& options = {});

// What lvlLevel::LoadInstances makes of a city/<map>.inst or _ai.inst record,
// by its flags (the record header's high 16 bits; the low byte is the
// variant, see staticVariant).
enum class StaticKind {
    Fixed,     // lvlFixedMatrix: instance flags 0x600 (visible, static)
    Landmark,  // 0x100: lvlLandmark (terrain-local bound), instance flags
               // 0x730 (0x710 with record flag 0x400)
    MultiRoom, // 0x2000: a collidable lvlFixedMatrix (instance flags 0x730)
               // placed by lvlMultiRoomInstance::Create
    Banger,    // 0x200: dgUnhitBangerInstance::RequestBanger (BangerSet)
};
StaticKind staticKind(std::uint16_t flags);

// lvlLevel::LoadInstances calls SetVariant with the flags' low byte;
// lvlFixedAny::SetVariant keeps it modulo the geometry's number of shader
// sets, the paint job lvlFixedAny::Draw uses (GpuModel::materials applies
// the modulo).
inline int staticVariant(std::uint16_t flags) { return flags & 0xFF; }

// lvlFixedMatrix::IsVisible: in physics mode (cityLevel::Load sets it) an
// object without instance flag 0x100 (a Fixed record: landmarks and
// collidable objects have it) is not drawn while the camera is behind the
// vertical plane through its origin facing along its Z axis:
// (eye.x - x) * z.x + (eye.z - z) * z.z < 0. (lvlFixedRotY::IsVisible has
// the same test, but only landmarks are stored that way and they are exempt.)
bool fixedObjectFacesAway(const Mat34& transform, const Vec3& eye);

// cityLevel::Draw lists at most this many rooms (its cityRoomRec array);
// later rooms are not drawn.
inline constexpr std::size_t kCityMaxDrawnRooms = 512;

// Level of detail for city objects.
struct DetailSettings {
    ObjectDetail objects;  // the Object Detail option (cityLevel::SetObjectDetail)
    bool usePvs = true;    // cull rooms with the city's precomputed visibility
    // OpenMM2 grass (not in MM2): the distance the grass's shell layers
    // are drawn to around the camera (0: none).
    float grassDistance = 0.0f;
};

// Draws the static city: PSDL street geometry, instances and the sky.
class CityRenderer {
public:
    // `isDynamic` marks instances drawn elsewhere (bangers), skipped here.
    CityRenderer(render::Device& device, TextureLibrary& textures, ModelLibrary& models, const city::CityData& city,
                 const std::function<bool(std::string_view)>& isDynamic = {});
    ~CityRenderer();
    CityRenderer(const CityRenderer&) = delete;
    CityRenderer& operator=(const CityRenderer&) = delete;

    // Room containing `position` (0 = none).
    int roomAt(const Vec3& position) const { return m_locator.find(position); }

    // Shades the street geometry for an environment (the walls' light table).
    void setEnvironment(const Environment& env);
    // lvlSky::Update: turns the sky.
    void update(float dt);

    // Call inside a scene pass after setFrameConstants().
    void draw(const Camera& camera, const Frustum& frustum, const Environment& env, const DetailSettings& detail);

    struct Stats {
        int cameraRoom = 0;
        int roomsDrawn = 0;
        int instancesDrawn = 0;
        int drawCalls = 0;
    };
    const Stats& stats() const { return m_stats; }

    // The rooms the last draw() listed (cityLevel::Draw), from which the
    // dynamic objects of the same view are drawn.
    const RoomVisibility& rooms() const { return m_visibility; }

    // Draws one model mesh with its materials (shared with vehicles).
    void drawMesh(const GpuMesh& mesh, const std::vector<asset::PkgMaterial>& materials, const Mat44& world,
                  bool fog = true, bool lighting = true);

private:
    // One sdlPage16::Draw primitive: a range of m_streetIndices drawn with
    // texture slot `slot` (0 = untextured).
    struct Prim {
        std::uint32_t first = 0, count = 0;
        std::uint32_t slot = 0;
        bool wall = false;        // skipped while the camera is behind wall0 -> wall1
        bool belowCamera = false; // skipped while `height` is above the camera
        float height = 0.0f;
        Vec3 wall0, wall1;
    };
    struct Room {
        std::array<std::vector<Prim>, 4> lods; // sdlPage16::Draw's levels of detail
        Vec3 centre;                           // cityLevel's room sphere
        float radius = 0.0f;
        std::vector<std::size_t> instances; // indices into m_instances
    };
    struct InstanceDraw {
        std::string model;
        const GpuModel* gpu = nullptr;
        bool resolved = false;
        Mat44 world;
        Mat34 transform;
        Aabb worldBounds; // valid once resolved
        float radius = 0.0f;
        // A collidable (.inst flag 0x2000) object, drawn through
        // lvlMultiRoomInstance stand-ins: once per frame from whichever of
        // its rooms is drawn first.
        bool multiRoom = false;
        std::uint32_t drawnFrame = 0;
        int variant = 0;         // staticVariant
        bool cullBehind = false; // fixedObjectFacesAway applies (StaticKind::Fixed)
    };

    void drawSky(const Camera& camera, const Environment& env);
    void gatherStreets(const Room& room, int lod, const Vec3& eye, bool nearGrass);
    void drawStreets(bool alphaPass);
    void drawGrass();
    void drawInstance(InstanceDraw& inst, const Frustum& frustum, const Mat34& camera, const DetailSettings& detail);
    void drawModel(const GpuModel& model, const Mat34& transform, asset::Lod lod, int variant);
    void drawCloudShadow(const GpuMesh& mesh, const std::vector<asset::PkgMaterial>& materials,
                         const Mat44& world);
    void resolve(InstanceDraw& inst);

    render::Device& m_device;
    TextureLibrary& m_textures;
    ModelLibrary& m_models;
    const city::CityData& m_city;
    city::RoomLocator m_locator;
    render::BufferHandle m_vertices;
    // CPU copy of the street vertices and how each is shaded (sdlPage16::Draw's
    // vglCurrentColor), recoloured per environment.
    std::vector<render::Vertex3D> m_streetVertices;
    std::vector<city::SdlShade> m_vertexShade;
    std::vector<std::uint8_t> m_vertexLight; // wall light table index
    std::vector<std::uint32_t> m_streetIndices;
    // Texture slots (name per slot; 0 is untextured) and the indices gathered
    // for each this frame (vglBeginBatch / vglEndBatch).
    std::vector<std::string> m_slotNames;
    std::vector<std::vector<std::uint32_t>> m_buckets;
    // OpenMM2 grass: the slots whose texture is grass, and their geometry in
    // the rooms near the camera (drawn again as shell layers).
    std::vector<std::uint8_t> m_slotGrass;
    std::vector<std::vector<std::uint32_t>> m_grassNear;
    static constexpr int kGrassLayers = 16;
    int m_instanceGrassLayers = 0; // while an instance near enough draws (drawInstance)
    std::vector<Room> m_rooms;
    std::vector<InstanceDraw> m_instances;
    std::vector<std::uint8_t> m_roomMarks;
    std::vector<int> m_drawnRooms; // this frame's cityLevel::Draw room list
    RoomVisibility m_visibility;
    const GpuModel* m_sky = nullptr;
    float m_skyAngle = 0.0f;
    int m_lastRoom = 0; // cityLevel's sm_LastPvsRoom
    // This frame's cloud shadow texture and flag mask (Environment).
    const WorldTexture* m_cloud = nullptr;
    std::uint32_t m_cloudMask = 0;
    std::uint32_t m_frame = 0;
    Stats m_stats;
};

// lvlInstance::GetGeomSet: a missing level of detail takes the next less
// detailed one (VL -> L -> M -> H); a missing VL draws nothing.
const GpuMesh* findFilledLod(const GpuModel& model, std::string_view part, asset::Lod lod);

// lvlInstance::GetGeomSet's radius of a part: the largest distance of a
// vertex from the model origin over its levels of detail (modGetStatic).
float geomRadius(const GpuModel& model, std::string_view part);

// lvlSDL::LoadBinary: the name a PSDL texture is looked up by (a movie
// frame name "<base>-0NNN" becomes "<base>").
std::string sdlTextureName(std::string_view name);

} // namespace mm2::game
