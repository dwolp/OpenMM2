#pragma once

#include "ai/World.h"
#include "asset/Ped.h"
#include "asset/VehicleModel.h"
#include "game/Camera.h"
#include "game/Interpolation.h"
#include "game/RaceConfig.h"
#include "game/ModelLibrary.h"
#include "game/RoomVisibility.h"
#include "game/TextureLibrary.h"
#include "game/VehicleRenderer.h"
#include "render/Device.h"
#include "vfs/Vfs.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace mm2::game {

// Draws the AI layer: ambient traffic (PKG cars posed from the rail state),
// pedestrians (skinned on the CPU from the posed skeleton) and the traffic
// signal glows.
class AiRenderer {
public:
    AiRenderer(render::Device& device, TextureLibrary& textures, ModelLibrary& models, const vfs::Vfs& vfs);

    // A traffic car the physics simulation has taken over, or has just let
    // go of (TrafficBodies): its matrix (model origin) and, while it has a
    // body (aiVehicleActive), its wheels' matrices.
    struct PhysicalCar {
        Mat34 transform;
        bool active = false;
        std::array<Mat34, 6> wheels{};
        std::array<bool, 6> wheelValid{};
    };
    using PhysicalCarQuery = std::function<std::optional<PhysicalCar>(int carId)>;

    // `physicalCar` answers for the cars that the physics simulation has
    // taken over (nullopt for cars on their rails). `lights` is
    // mmGame::InitWeather's light flag (evening, night or fog: the cars'
    // headlights and tail lights); the signals switch to their night glows
    // from the evening on (aiTrafficLightInstance::DrawGlow: time of day > 1).
    // `detail`: the Object Detail thresholds (lvlInstance::IsVisible).
    void draw(const ai::World& world, const Camera& camera, const Frustum& frustum, TimeOfDay time, bool lights,
              const ObjectDetail& detail, const PhysicalCarQuery& physicalCar = {});

    // The ground under the cars for their shadows (aiVehicleInstance::
    // DrawShadow's lvlInstance::DrawPhysics).
    void setGroundProbe(VehicleRenderer::GroundProbe probe) { m_probe = std::move(probe); }
    // OpenMM2 extra, off by default: the traffic cars' enhanced lighting
    // (VehicleRenderer::setEnhancedLighting).
    void setEnhancedCarLighting(bool on) { m_enhancedCarLighting = on; }
    void setCarShine(float shine) { m_carShine = shine; } // the traffic's (Other cars' shine)

    // The traffic lights' aiTrafficLightInstances as props (BangerSet):
    // signal i's GetMatrix while it stands, nullopt once it broke loose
    // (then neither its body nor its glows are drawn). Without one each
    // signal stands at Signal::frame.
    using SignalFrameQuery = std::function<std::optional<Mat34>(int signalIndex)>;
    void setSignalFrames(SignalFrameQuery query) { m_signalFrame = std::move(query); }

    // The rooms the city listed for the view (CityRenderer::rooms()): cars,
    // pedestrians and signals are then drawn from the rooms MM2 keeps them
    // in (RoomVisibility).
    void setRooms(const RoomVisibility* rooms) { m_rooms = rooms; }

    // OpenMM2 extra (a network client of the shared cruise traffic): draws
    // these cars instead of the world's traffic; null draws the world's.
    void setCars(const std::vector<ai::AmbientCar>* cars) { m_carsOverride = cars; }
    // OpenMM2 presentation: the world's cars on their rails and its
    // pedestrians are drawn between its last two steps (the history its step
    // observer keeps, recordAiStep); null draws them where they are.
    void setInterpolation(const StepHistory* history) { m_history = history; }
    // The paint jobs of a traffic model (its package's), 1 without any.
    int paintJobs(const std::string& model);

    struct Stats {
        int cars = 0;
        int peds = 0;
        int signals = 0;
    };
    const Stats& stats() const { return m_stats; }

private:
    struct CarModel {
        std::unique_ptr<asset::VehicleModel> model;
        std::map<int, std::unique_ptr<VehicleRenderer>> renderers; // by paint job
        int paintjobs = 1;
    };
    CarModel* carModel(const std::string& name);
    const asset::PedType* pedType(const std::string& name);
    // `transform`: where the pedestrian is drawn.
    void drawPed(const ai::Pedestrian& ped, const Mat34& transform, const asset::PedType& type,
                 const Camera& camera);
    void drawSkeleton(const ai::Pedestrian& ped, const Mat34& transform, const asset::PedType& type,
                      const Camera& camera);
    void drawSignal(const ai::Signal& signal, const Mat34& frame, const Camera& camera, bool nightGlows,
                    const RoomVisibility::Passes* passes);
    // lvlLevel::MoveToRoom's room of an object, found from its last one
    // (cityLevel::FindRoomId).
    RoomVisibility::Passes roomPasses(std::unordered_map<int, int>& rooms, int id, const Vec3& position);

    render::Device& m_device;
    TextureLibrary& m_textures;
    ModelLibrary& m_models;
    const vfs::Vfs& m_vfs;
    std::map<std::string, std::unique_ptr<CarModel>> m_cars;
    std::map<std::string, std::optional<asset::PedType>> m_pedTypes;
    std::vector<Mat34> m_bones;
    std::vector<render::Vertex3D> m_skinned;
    ObjectDetail m_detail;
    Stats m_stats;
    const RoomVisibility* m_rooms = nullptr;
    VehicleRenderer::GroundProbe m_probe;
    bool m_enhancedCarLighting = false;
    float m_carShine = 1.0f;
    SignalFrameQuery m_signalFrame;
    // The rooms of the traffic cars (aiVehicleAmbient's update after its
    // spline, aiVehicleActive::Update), the pedestrians (aiPedestrian::Update)
    // and the signals (aiTrafficLightSet::SetFourWay), by id or index.
    std::unordered_map<int, int> m_carRooms, m_pedRooms, m_signalRooms;
    const std::vector<ai::AmbientCar>* m_carsOverride = nullptr;
    const StepHistory* m_history = nullptr;
};

// aiTrafficLightInstance::DrawGlow's matrix for the glow and WALK meshes,
// which are modelled from the pole's base: the instance's GetMatrix
// (`frame`, at the CG) less R * CG.
Mat34 signalGlowFrame(const Mat34& frame, const Vec3& cg);

// The direction pedAnimation::DrawSkeleton widens a pedestrian's stick
// figure along, in world space, for a pedestrian placed at `ped` seen from
// a camera placed at `camera`.
Vec3 skeletonWidthAxis(const Mat34& ped, const Mat34& camera);

} // namespace mm2::game
