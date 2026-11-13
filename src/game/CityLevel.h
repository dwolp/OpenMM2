#pragma once

// The city as MM2's collision manager sees it (cityLevel + lvlSDL): rooms,
// the city's collision polygons and the objects in each room. Implements
// phys::Level for the physics World. See docs/physics.md, "Collision".

#include "asset/Bound.h"
#include "city/CityData.h"
#include "city/RoomLocator.h"
#include "game/Mm1Level.h"
#include "phys/Level.h"
#include "phys/Material.h"
#include "phys/PolygonSoup.h"
#include "vfs/Vfs.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mm2::game {

// bound/<name>_bound.bnd (text, as phBoundGeometry::Load reads it) or, with
// `binary`, bound/<name>_bound.bbnd (phBoundGeometry::LoadBinary, the
// geometry of terrain bounds). Null when missing or malformed.
std::optional<asset::BoundGeometry> loadBoundFile(const vfs::Vfs& vfs, std::string_view name, bool binary = false);
// A bound file's geometry for phys::makeGeometryBound / makeTerrainBound.
// `material` resolves the file's materials (null pointers, or no function:
// the bound's default material).
phys::GeometryData toGeometryData(const asset::BoundGeometry& bound,
                                  const std::function<const phys::Material*(const asset::BoundMaterial&)>& material = {});

// cityLevel::GetTouchedNeighbors: the rooms across `room`'s perimeter edges
// that the sphere reaches in the ground plane (instance rooms whatever the
// sphere), each once, at most `max`; returns how many it wrote to `out`.
int cityTouchedNeighbors(const city::Psdl& psdl, int* out, int max, int room, const Vec3& centre,
                         float radius);

// Things that list instances in rooms besides the city's static objects:
// the props (BangerSet) and the traffic cars on their rails (TrafficBodies).
class InstanceSource {
public:
    virtual ~InstanceSource() = default;
    // The source's instances in `room`, appended (lvlLevel's room lists).
    virtual void instancesIn(int room, std::vector<phys::Instance*>& out) const = 0;
};

// A placed city object with a collision bound (lvlFixedMatrix /
// lvlFixedRotY instances the .inst file flags collidable, and the
// terrain-bound ones).
class StaticInstance final : public phys::Instance {
public:
    const phys::Bound* bound(int which) const override;
    const Mat34& matrix() const override { return m_matrix; }
    float radius() const override { return m_radius; }

    std::shared_ptr<const phys::Bound> collisionBound;
    // lvlInstance::GetBound(1..3) of a non-terrain bound: the box around it.
    std::shared_ptr<const phys::Bound> boxBound;
    Mat34 m_matrix;
    float m_radius = 0.0f;
    std::string name;
};

class CityLevel final : public phys::Level {
public:
    // Builds the level for `city`: materials (city/materials.mtl), the
    // texture -> material table of the city's polygons (materials.csv), the
    // collidable instances and their bounds, and the probe geometry the
    // wheels use. `isBanger` names models that are props (BangerSet's).
    CityLevel(const city::CityData& city, const vfs::Vfs& vfs, const std::function<bool(std::string_view)>& isBanger);

    // The materials for the wheels' probe geometry ("_default" first, then
    // the city's and the bounds' materials in load order).
    const phys::MaterialTable& materials() const { return m_materials; }
    phys::MaterialTable takeMaterials() { return m_materials; }
    // The probe geometry (wheels, line of sight, spawning).
    phys::PolygonSoup takeProbeSoup() { return std::move(m_soup); }

    void addSource(const InstanceSource* source) { m_sources.push_back(source); }
    void removeSource(const InstanceSource* source);

    // phys::Level.
    int findRoom(const Vec3& position, int hint) const override;
    int touchedNeighbors(int* out, int max, int room, const Vec3& centre, float radius) const override;
    int neighbors(int* out, int max, int room) const override;
    int roomFlags(int room) const override;
    int roomInfoFlags(int room) const override;
    void collectProbe(int room, const Vec3& centre, float radius, phys::LevelBound& out) const override;
    void collect(const int* rooms, int count, const Vec3& centre, float radius,
                 phys::LevelBound& out) const override;
    void instances(int room, std::vector<phys::Instance*>& out) const override;
    const phys::Material& material(int index) const override;

    // Statistics.
    int staticInstances() const { return static_cast<int>(m_instances.size()); }
    int missingBounds() const { return m_missingBounds; }

private:
    const city::CityData& m_city;
    city::RoomLocator m_locator;
    phys::MaterialTable m_materials;
    std::deque<phys::Material> m_manager; // lvlMaterialMgr's list
    phys::PolygonSoup m_soup;
    std::vector<std::uint8_t> m_textureMaterials;
    std::vector<std::unique_ptr<StaticInstance>> m_instances;
    std::vector<std::vector<StaticInstance*>> m_roomInstances;
    std::vector<const InstanceSource*> m_sources;
    int m_missingBounds = 0;
    // OpenMM2 Enhanced: the city is MM1's (CityData::mm1); this answers the
    // room and collision queries instead of the PSDL.
    std::unique_ptr<Mm1Level> m_mm1;
};

} // namespace mm2::game
