// MM2: aiVehicleInstance::Draw, aiVehicleInstance::DrawPart,
// aiVehicleInstance::DrawShadow, aiVehicleInstance::DrawGlow,
// aiPedestrianInstance::Draw, aiPedestrianInstance::GetMatrix,
// aiPedestrianInstance::GetPosition, aiPedestrianInstance::ComputeLod.
#include "game/AiRenderer.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/CityRenderer.h"
#include "game/MeshDraw.h"

#include <format>

namespace mm2::game {

Vec3 skeletonWidthAxis(const Mat34& ped, const Mat34& camera) {
    // pedAnimation::DrawSkeleton offsets each quad's corners in the
    // pedestrian's own space (the world matrix is the pedestrian's) by the
    // first row of gfxRenderState's modelview matrix: the pedestrian's X axis
    // in view space, the view matrix being the camera's inverse with z
    // negated (sm_FullComposite). For a pedestrian and a camera turned only
    // about Y that is the camera's right axis; a pitched camera tilts it.
    const Vec3& x = ped.m0;
    const Vec3 local{x.dot(camera.m0), x.dot(camera.m1), -x.dot(camera.m2)};
    return ped.transformDir(local);
}

AiRenderer::AiRenderer(render::Device& device, TextureLibrary& textures, ModelLibrary& models, const vfs::Vfs& vfs)
    : m_device(device), m_textures(textures), m_models(models), m_vfs(vfs) {}

AiRenderer::CarModel* AiRenderer::carModel(const std::string& name) {
    if (auto it = m_cars.find(name); it != m_cars.end())
        return it->second.get();
    auto entry = std::make_unique<CarModel>();
    std::string error;
    auto read = [this](std::string_view path) { return m_vfs.readAll(path); };
    if (auto model = asset::loadVehicleModel(name, read, &error)) {
        entry->paintjobs = std::max<int>(1, static_cast<int>(model->pkg.paintjobs.size()));
        entry->model = std::make_unique<asset::VehicleModel>(std::move(*model));
    } else {
        log::warn("ai: traffic model {}: {}", name, error);
    }
    auto* raw = entry.get();
    m_cars[name] = std::move(entry);
    return raw;
}

const asset::PedType* AiRenderer::pedType(const std::string& name) {
    auto it = m_pedTypes.find(name);
    if (it == m_pedTypes.end()) {
        std::string error;
        auto read = [this](std::string_view path) { return m_vfs.readAll(path); };
        auto type = asset::loadPedType(name, read, &error);
        if (type)
            asset::normalizePedRoots(*type); // pedAnimation::Load
        else
            log::warn("ai: pedestrian type {}: {}", name, error);
        it = m_pedTypes.emplace(name, std::move(type)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// The pedestrian's mesh skinned by m_bones and placed at `transform`, into
// m_skinned.
void AiRenderer::skinPed(const asset::PedMesh& mesh, const Mat34& transform) {
    m_skinned.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& v = mesh.vertices[i];
        const Mat34 bone = v.bone < m_bones.size() ? m_bones[v.bone] * transform : transform;
        const Vec3 p = bone.transform(v.position);
        const Vec3 n = bone.transformDir(v.normal).normalized();
        auto& out = m_skinned[i];
        out = {};
        out.position[0] = p.x;
        out.position[1] = p.y;
        out.position[2] = p.z;
        out.normal[0] = n.x;
        out.normal[1] = n.y;
        out.normal[2] = n.z;
        out.color = render::packColor(v.color);
        out.uv0[0] = out.uv1[0] = v.uv.x;
        out.uv0[1] = out.uv1[1] = v.uv.y;
    }
}

void AiRenderer::drawPed(const ai::Pedestrian& ped, const Mat34& transform, const asset::PedType& type,
                         const Camera& camera) {
    const asset::PedAnimation* anim = type.animation(ped.animFile);
    if (!anim)
        anim = type.animation(ped.state);
    // pedAnimationInstance::Draw poses the sequence's current frame with the
    // root translations pedAnimation::Load adjusted (normalizePedRoots): the
    // pose starts and ends at the pedestrian's origin, which carries the
    // motion. aiPedestrianInstance::Draw: the posed model within 35 m of the
    // camera, the stick figure (pedAnimation::DrawSkeleton) beyond.
    if (transform.m3.dist2(camera.position()) >= 1225.0f) {
        asset::posePed(type.skeleton, anim, ped.frame, m_bones);
        drawSkeleton(ped, transform, type, camera);
        return;
    }
    const auto& mesh = type.mesh;
    // OpenMM2: posed and skinned once a frame, by whichever view draws it
    // first (the sun's, the reflections' and the camera's share it).
    auto [cached, fresh] = m_skinnedPeds.try_emplace(ped.id);
    if (fresh) {
        asset::posePed(type.skeleton, anim, ped.frame, m_bones);
        skinPed(mesh, transform);
        cached->second.vertices =
            m_device.uploadTransient(render::BufferKind::Vertex, std::span<const render::Vertex3D>(m_skinned));
        cached->second.indices =
            m_device.uploadTransient(render::BufferKind::Index, std::span<const std::uint32_t>(mesh.indices));
    }
    const render::BufferSlice vb = cached->second.vertices, ib = cached->second.indices;
    for (std::size_t m = 0; m < mesh.materials.size(); ++m) {
        const auto& mat = mesh.materials[m];
        if (!mat.indexCount)
            continue;
        Vec4 color{mat.diffuse.x, mat.diffuse.y, mat.diffuse.z, 1.0f};
        if (const auto* shader = type.shaders.get(static_cast<std::uint32_t>(ped.variant), static_cast<std::uint32_t>(m)))
            color = {shader->diffuse.x, shader->diffuse.y, shader->diffuse.z, 1.0f};
        render::DrawCall call;
        call.vertices = vb;
        call.indices = ib;
        call.indexType = render::IndexType::U32;
        call.count = mat.indexCount;
        call.first = mat.firstIndex;
        call.constants.color = color;
        call.constants.flags = render::DrawFlag::Lighting | render::DrawFlag::Fog;
        // modModel::Draw under the default culling (as for every model).
        call.state.cull = render::CullMode::Back;
        call.state.frontFace = render::FrontFace::CounterClockwise;
        m_device.draw(call);
    }
}

void AiRenderer::drawSkeleton(const ai::Pedestrian& ped, const Mat34& transform, const asset::PedType& type,
                              const Camera& camera) {
    // pedAnimation::DrawSkeleton: for each bone with a width in the .rays
    // file, its position raised by the bone's offset (which its children then
    // use), a quad to its parent's position across skeletonWidthAxis (the
    // start and end half widths), in the colour the variant's row picks from
    // its shaders' diffuse colours; untextured, unlit, both sides.
    if (!type.rays)
        return;
    const auto& rays = *type.rays;
    const std::size_t n = std::min(rays.bones.size(), m_bones.size());
    const std::vector<int>* row =
        ped.variant >= 0 && static_cast<std::size_t>(ped.variant) < rays.variants.size()
            ? &rays.variants[static_cast<std::size_t>(ped.variant)]
            : nullptr;
    const Vec3 right = skeletonWidthAxis(transform, camera.transform);
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint16_t> indices;
    for (std::size_t j = 0; j < n; ++j) {
        const auto& bone = rays.bones[j];
        const float w0 = bone.values.x, w1 = bone.values.y;
        if (w0 == 0.0f)
            continue;
        m_bones[j].m3.y = bone.values.z + m_bones[j].m3.y;
        const std::size_t parent = static_cast<std::size_t>(std::clamp(bone.a, 0, static_cast<int>(n) - 1));
        // The variant's shader colour: 0xFF, then trunc(diffuse x 255).
        std::uint32_t argb = 0xFFFFFFFFu;
        const auto variant = static_cast<std::uint32_t>(ped.variant);
        if (row && j < row->size())
            if (const auto* shader = type.shaders.get(variant, static_cast<std::uint32_t>((*row)[j]))) {
                auto byte = [](float v) {
                    return static_cast<std::uint32_t>(static_cast<int>(v * 255.0f)) & 0xFFu;
                };
                argb = 0xFF000000u | byte(shader->diffuse.x) << 16 | byte(shader->diffuse.y) << 8 |
                       byte(shader->diffuse.z);
            }
        const Vec3 a = transform.transform(m_bones[j].m3);
        const Vec3 b = transform.transform(m_bones[parent].m3);
        const Vec3 da = right * w0, db = right * w1;
        const auto base = static_cast<std::uint16_t>(vertices.size());
        for (const Vec3& p : {a - da, a + da, b - db, b + db}) {
            render::Vertex3D v{};
            v.position[0] = p.x;
            v.position[1] = p.y;
            v.position[2] = p.z;
            v.normal[1] = 1.0f;
            v.color = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
            vertices.push_back(v);
        }
        for (std::uint16_t i : {std::uint16_t(0), std::uint16_t(1), std::uint16_t(2), std::uint16_t(2),
                                std::uint16_t(1), std::uint16_t(3)})
            indices.push_back(static_cast<std::uint16_t>(base + i));
    }
    if (indices.empty())
        return;
    render::DrawCall call;
    call.vertices =
        m_device.uploadTransient(render::BufferKind::Vertex, std::span<const render::Vertex3D>(vertices));
    call.indices =
        m_device.uploadTransient(render::BufferKind::Index, std::span<const std::uint16_t>(indices));
    call.count = static_cast<std::uint32_t>(indices.size());
    call.constants.world = Mat44::identity();
    call.constants.flags = render::DrawFlag::VertexColor | render::DrawFlag::Fog;
    call.state.cull = render::CullMode::None;
    m_device.draw(call);
}

Mat34 signalGlowFrame(const Mat34& frame, const Vec3& cg) {
    // aiTrafficLightInstance::DrawGlow: GetMatrix less R * CG (summed y, z,
    // then x of the CG).
    Mat34 m = frame;
    m.m3 = {m.m3.x - ((frame.m1.x * cg.y + frame.m2.x * cg.z) + frame.m0.x * cg.x),
            m.m3.y - ((frame.m1.y * cg.y + frame.m2.y * cg.z) + frame.m0.y * cg.x),
            m.m3.z - ((frame.m1.z * cg.y + frame.m2.z * cg.z) + frame.m0.z * cg.x)};
    return m;
}

void AiRenderer::drawSignal(const ai::Signal& signal, const Mat34& frame, const Camera& camera,
                            bool nightGlows, const RoomVisibility::Passes* passes) {
    const GpuModel* model = m_models.get(signal.model);
    if (!model)
        return;
    // aiTrafficLightInstance::Draw: the body at GetMatrix, the instance's
    // frame at its CG (the body mesh is centred on it).
    // lvlInstance::IsVisible with the Object Detail thresholds, and
    // aiTrafficLightInstance::Draw's first shader set.
    const auto lod = objectLod(viewDepth(camera.transform, frame.m3), geomRadius(*model, ""), m_detail);
    if (!passes && !lod)
        return;
    if (lod && (!passes || passes->objects))
        if (const GpuMesh* body = findFilledLod(*model, "", *lod))
            drawGpuMesh(m_device, m_textures, *body, model->materials(0), Mat44::fromMat34(frame));
    // aiTrafficLightInstance::DrawGlow: the light's glow together with the
    // pedestrian signal (WALK only in the walk phase), both or neither.
    const char* colour = signal.state == ai::LightState::Green  ? "GREEN"
                       : signal.state == ai::LightState::Amber  ? "YELLOW"
                                                                : "RED";
    // Drawn in cityLevel::DrawRooms' glow pass (rooms nearer than NoDraw,
    // whatever IsVisible says): unlit, no fog, added (ONE/ONE), no depth
    // writes. Without the city's room list the signal's own distance stands
    // in for its room's.
    if (passes ? !passes->shadowsAndGlows : frame.m3.dist2(camera.position()) >= sq(m_detail.noDraw))
        return;
    const char* time = nightGlows ? "NIGHT" : "DAY";
    const GpuMesh* glow = findFilledLod(*model, std::format("{}GLOW{}", colour, time), asset::Lod::High);
    const char* walkName = signal.state == ai::LightState::Walk ? "WALK" : "NOWALK";
    const GpuMesh* walk = findFilledLod(*model, std::format("{}_{}", walkName, time), asset::Lod::High);
    if (glow && walk) {
        // DrawGlow: GetMatrix less R * CG, the base the glow meshes are
        // modelled from.
        const Mat44 world = Mat44::fromMat34(signalGlowFrame(frame, signal.cg));
        MeshDrawOptions opts;
        opts.lighting = false;
        opts.fog = false;
        opts.blend = render::BlendMode::Add;
        opts.depthWrite = false;
        opts.alphaRef = 1.0f / 255.0f; // the default alpha test (alpha not 0)
        drawGpuMesh(m_device, m_textures, *glow, model->materials(0), world, opts);
        drawGpuMesh(m_device, m_textures, *walk, model->materials(0), world, opts);
    }
}

void AiRenderer::draw(const ai::World& world, const Camera& camera, const Frustum& frustum, TimeOfDay time,
                      bool lights, const ObjectDetail& detail, const PhysicalCarQuery& physicalCar) {
    m_stats = {};
    m_detail = detail;
    const Vec3 eye = camera.position();
    const int blinkClock = world.blinkClock();
    const bool rooms = m_rooms && m_rooms->active();
    // The world's own cars on their rails between its last two steps (a
    // shared-traffic client's cars come already placed).
    const StepHistory* history = m_carsOverride ? nullptr : m_history;
    for (const auto& car : m_carsOverride ? *m_carsOverride : world.cars()) {
        const std::optional<PhysicalCar> physical = physicalCar ? physicalCar(car.id) : std::nullopt;
        const std::uint64_t key = drawnKey(Drawn::RailCar, static_cast<std::uint64_t>(car.id));
        const auto generation = static_cast<std::uint32_t>(car.spawns);
        const Mat34 transform = physical ? physical->transform
                                : history ? history->transform(key, car.transform, generation)
                                          : car.transform;
        // cityLevel::DrawRooms: from the car's room; the renderer's
        // lvlInstance::IsVisible ends the car itself at NoDraw.
        RoomVisibility::Passes passes;
        if (rooms)
            passes = roomPasses(m_carRooms, car.id, transform.m3);
        if (!frustum.intersectsSphere(transform.m3, 4.0f)) {
            if (!rooms)
                continue;
            passes.objects = false;
        }
        if (!passes.objects && !passes.shadowsAndGlows)
            continue;
        CarModel* cm = carModel(car.model);
        if (!cm || !cm->model)
            continue;
        // aiVehicleInstance::SetColor: trunc(frand * (paint jobs - 1)), so the
        // last paint job is never chosen.
        const int paint =
            cm->paintjobs > 1 ? static_cast<int>(car.paint * static_cast<float>(cm->paintjobs - 1)) : 0;
        auto& r = cm->renderers[paint];
        if (!r) {
            r = std::make_unique<VehicleRenderer>(m_device, m_textures, m_models, *cm->model, paint);
            r->setTraffic(true);
        }
        r->setDetail(detail);
        r->setGroundProbe(m_probe);
        r->setEnhancedLighting(m_enhancedCarLighting);
        VehiclePose pose;
        pose.body = transform;
        // aiVehicleInstance::Draw: the rail's tyre rotation about each axle,
        // no steering; with a body, the wheels where its vehWheelCheaps put
        // them.
        const float tireRotation =
            history && !physical ? history->turn(key, car.transform, car.tireRotation, ai::kTireRotationWrap,
                                                 generation)
                                 : car.tireRotation;
        for (std::size_t i = 0; i < 6; ++i)
            pose.wheelSpin[i] = -tireRotation; // rolling forward (-Z) spins about -X
        if (physical && physical->active) {
            pose.physical = true;
            pose.hasWheelWorld = true;
            pose.wheelWorld = physical->wheels;
            pose.wheelValid = physical->wheelValid;
        }
        pose.brakeLights = car.braking;
        pose.headlights = lights;
        // aiVehicleInstance::DrawGlow: the indicators the AI set (+0x1a),
        // blinking with the car's own phase.
        if (ai::World::indicatorsOn(car, blinkClock))
            pose.indicators = static_cast<int>(car.signal);
        if (rooms)
            r->draw(pose, camera.transform, passes);
        else
            r->draw(pose, camera.transform);
        ++m_stats.cars;
    }
    // Pedestrians are dynamic objects too: nothing beyond NoDraw (inferred:
    // MM2 draws them through lvlInstance::IsVisible like the cars).
    for (const auto& ped : world.peds()) {
        // Between the AI's last two steps; one put on a road in the last
        // step (aiPedestrian::Reset) is drawn where it is.
        const Mat34 transform =
            m_history ? m_history->transform(drawnKey(Drawn::Pedestrian, static_cast<std::uint64_t>(ped.id)),
                                             ped.transform, ped.placed ? 1u : 0u)
                      : ped.transform;
        if (transform.m3.dist2(eye) > sq(detail.noDraw) || !frustum.intersectsSphere(transform.m3, 2.0f))
            continue;
        if (rooms && !roomPasses(m_pedRooms, ped.id, transform.m3).objects)
            continue;
        if (const asset::PedType* type = pedType(ped.typeName)) {
            drawPed(ped, transform, *type, camera);
            ++m_stats.peds;
        }
    }
    int index = 0;
    for (const auto& signal : world.signals()) {
        const int id = index++;
        // The aiTrafficLightInstance's GetMatrix while it stands; once it
        // broke loose it is neither drawn nor glows (its parts are props).
        Mat34 frame = signal.frame();
        if (m_signalFrame) {
            const auto f = m_signalFrame(id);
            if (!f)
                continue;
            frame = *f;
        }
        if (!frustum.intersectsSphere(frame.m3, 6.0f))
            continue;
        if (rooms) {
            const RoomVisibility::Passes passes = roomPasses(m_signalRooms, id, frame.m3);
            if (!passes.objects && !passes.shadowsAndGlows)
                continue;
            drawSignal(signal, frame, camera, time >= TimeOfDay::Evening, &passes);
        } else {
            drawSignal(signal, frame, camera, time >= TimeOfDay::Evening, nullptr);
        }
        ++m_stats.signals;
    }
}

int AiRenderer::paintJobs(const std::string& model) {
    const CarModel* cm = carModel(model);
    return cm && cm->model ? cm->paintjobs : 1;
}

RoomVisibility::Passes AiRenderer::roomPasses(std::unordered_map<int, RoomHint>& rooms, int id,
                                              const Vec3& position) {
    RoomHint& hint = rooms[id];
    if (!hint.known || hint.at.x != position.x || hint.at.y != position.y || hint.at.z != position.z) {
        hint.room = m_rooms->findRoom(position, hint.room);
        hint.at = position;
        hint.known = true;
    }
    return m_rooms->passes(hint.room);
}

} // namespace mm2::game
