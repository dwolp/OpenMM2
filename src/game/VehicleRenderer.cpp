#include "game/VehicleRenderer.h"

#include "core/StringUtil.h"
#include "game/CityRenderer.h"

#include <cmath>
#include <format>

namespace mm2::game {
namespace {

// ltLight defaults as cars use them (vehCarModel::Init, vehSiren::Init,
// aiVehicleManager::Init): intensity 25, spot exponent 3.
constexpr float kLightIntensity = 25.0f;
constexpr float kSpotExponent = 3.0f;
// vehSiren::Update: the beams turn 2.5 pi rad/s; vehCarModel::DrawHeadlights
// sweeps the headlights at +-42.411503 rad/s while the siren is on.
constexpr float kHeadlightSweep = 42.411503f;
// vehCarModel+0x2c (0.2 in its constructor): WHL4/WHL5 sit (0.2 + 2) wheel
// radii behind WHL2/WHL3 (vehCarModel::Draw).
constexpr float kExtraWheelSpacing = 0.2f + 2.0f;

// ltLight::DrawGlow's size and colour scales, globals in MM2: every vehSiren
// constructor (one per vehCar::Init) sets 0.2 and 0.6 and
// aiVehicleManager::Init sets 0.2 and 0.95. A single-player race initialises
// the AI map after the cars (mmGame::Init), so 0.95 is what is drawn with.
float s_glowSize = 0.2f;
float s_glowColor = 0.95f;
// The view the siren flares are queued for (VehicleRenderer::setLensFlareTarget).
const Mat44* s_flareViewProj = nullptr;
float s_flareAspect = 4.0f / 3.0f;
std::vector<fx::LensFlareQuad>* s_flareOut = nullptr;
// ltFlare::Random draws on MM2's global generator; a car whose set-up does
// not reproduce MM2's order (setSirenFlares) draws its flares from this one.
fx::Rand s_flareRand{0x5A1E};
// vehSiren::Draw: ltLight::ComputeIntensity's threshold for the flares.
constexpr float kFlareThreshold = 0.05f;

std::uint32_t argb(const Vec3& c) {
    auto b = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (b(c.x) << 16) | (b(c.y) << 8) | b(c.z);
}

// Rotates `m` so that its up row points along `normal` (Matrix34::RotateTo).
Mat34 rotateUpTo(const Mat34& m, const Vec3& from, const Vec3& normal) {
    const Vec3 axis = from.cross(normal);
    const float s = axis.mag();
    const float c = std::clamp(from.dot(normal), -1.0f, 1.0f);
    if (s < 1e-6f)
        return m;
    const Mat34 r = Mat34::rotationAxis(axis * (1.0f / s), std::atan2(s, c));
    Mat34 out = m;
    out.m0 = r.transformDir(m.m0);
    out.m1 = r.transformDir(m.m1);
    out.m2 = r.transformDir(m.m2);
    return out;
}

} // namespace

VehicleRenderer::VehicleRenderer(render::Device& device, TextureLibrary& textures, ModelLibrary& models,
                                 const asset::VehicleModel& model, int paintjob, std::string bodyPart,
                                 std::string wheelPrefix)
    : m_device(device), m_textures(textures), m_model(model), m_paintjob(paintjob), m_bodyPart(std::move(bodyPart)),
      m_wheelPrefix(std::move(wheelPrefix)) {
    m_trailer = str::iequals(m_bodyPart, "TRAILER");
    m_gpu = models.add(model.baseName, model.pkg);
    setPaintjob(paintjob);
    if (!m_gpu)
        return;
    // lvlInstance::GetRadius: the body set's radius (the farthest body vertex
    // from the model origin over the levels of detail).
    m_radius = geomRadius(*m_gpu, m_bodyPart);
    // A part's light colour is its first material's diffuse colour
    // (vehCarModel::GetSurfaceColor).
    auto surface = [&](std::string_view part) -> std::optional<Vec3> {
        const GpuMesh* mesh = findFilledLod(*m_gpu, part, asset::Lod::High);
        if (!mesh || mesh->draws.empty() || !str::iequals(mesh->part, part))
            return std::nullopt;
        const auto& mats = m_gpu->materials(m_paintjob);
        const std::uint32_t s = mesh->draws.front().shader;
        const Vec4 d = s < mats.size() ? mats[s].diffuse : Vec4{1, 1, 1, 1};
        return Vec3{d.x, d.y, d.z};
    };
    // Headlight beams: two ltLights at the headlight0/1 pivots, coloured by
    // the headlight0/1 parts, created only when the car has a headlight0 part.
    if (surface("HEADLIGHT0")) {
        for (int i = 0; i < 2; ++i) {
            const auto name = std::format("headlight{}", i);
            const auto* pivot = model.pivot(name);
            const auto color = surface(str::upper(name));
            if (pivot)
                m_headlights[static_cast<std::size_t>(i)] = Light{pivot->origin, color.value_or(Vec3{1, 1, 1})};
        }
    }
    // Siren beams: one ltLight per SRN0-3 part (vehCarModel::InitSirenLight).
    for (int i = 0; i < 4; ++i) {
        const auto name = std::format("srn{}", i);
        if (const auto color = surface(str::upper(name)))
            if (const auto* pivot = model.pivot(name))
                m_sirens.push_back({pivot->origin, *color});
    }
    // vehSiren::Init: twenty lens flares for the siren lights.
    if (!m_sirens.empty())
        m_flare.emplace(kSirenFlares, s_flareRand);
    // Fenders follow the front wheels at their pivot's offset from wheel 0
    // (lifted 2.5 cm; FNDR1 mirrors it).
    if (findFilledLod(*m_gpu, "FNDR0", asset::Lod::High) && model.pivot("fndr0") && model.wheel(0))
        m_fenderOffset = model.pivot("fndr0")->origin - model.wheel(0)->position + Vec3{0, 0.025f, 0};
}

void VehicleRenderer::setPaintjob(int paintjob) {
    m_paintjob = paintjob;
    m_paint = m_gpu ? m_gpu->materials(paintjob) : std::vector<asset::PkgMaterial>{};
    // fxTexelDamage: at the high and medium LODs each material of the high
    // LOD body draws a copy of its clean texture ("<name>", or "<name>" for a
    // "<name>_dmg" material) into which impacts copy patches of
    // "<name>_dmg"; every other material is drawn as stored. The low LODs
    // draw the paint job's materials as stored.
    m_live = m_paint;
    m_texelDamage.reset();
    // aiVehicleInstance and vehTrailerInstance have no fxTexelDamage.
    if (m_traffic || m_trailer)
        return;
    for (const auto& mesh : m_model.pkg.meshes)
        if (str::iequals(mesh.part, m_bodyPart) && mesh.lod == asset::Lod::High) {
            static int serial = 0;
            m_texelDamage = std::make_unique<TexelDamage>(m_device, m_textures, mesh, m_live,
                                                          std::format("{}{}", m_model.baseName, ++serial));
            break;
        }
}

void VehicleRenderer::applyDamage(const Vec3& modelPoint, float radius, std::optional<std::uint32_t> seed) {
    if (m_texelDamage)
        m_texelDamage->apply(modelPoint, radius, seed);
}

std::optional<VehicleRenderer::Breakable> VehicleRenderer::attachedPart(const std::string& part) const {
    const std::string name = str::upper(part);
    const auto* pivot = m_model.pivot(str::lower(part));
    if (!pivot || !m_gpu || !findFilledLod(*m_gpu, name, asset::Lod::High) || m_detached.contains(name))
        return std::nullopt;
    return Breakable{name, pivot->origin};
}

void VehicleRenderer::resetDamage() {
    if (m_texelDamage)
        m_texelDamage->reset();
    reattachAll();
}

std::optional<VehicleRenderer::Breakable> VehicleRenderer::nearestBreakable(const Vec3& modelPoint) const {
    std::optional<Breakable> best;
    float bestD2 = 100000.0f;
    auto consider = [&](const std::string& name) {
        const auto* pivot = m_model.pivot(name);
        const std::string part = str::upper(name);
        if (!pivot || !m_gpu || !findFilledLod(*m_gpu, part, asset::Lod::High) || m_detached.contains(part))
            return;
        const float d2 = pivot->origin.dist2(modelPoint);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = Breakable{part, pivot->origin};
        }
    };
    for (const char* name : {"break0", "break1", "break2", "break3", "break01", "break12", "break23", "break03"})
        consider(name);
    consider(std::format("variant{}", m_paintjob));
    return best;
}

std::vector<VehicleRenderer::Breakable> VehicleRenderer::wreckParts(float mph, fx::Rand& rng) {
    std::vector<Breakable> out;
    if (m_wreckEjected)
        return out;
    m_wreckEjected = true;
    std::set<std::string> taken; // a pair may come up twice
    auto take = [&](const std::string& part) {
        // vehCarModel::InitBreakable adds a part to the one-shot manager when
        // its high LOD mesh exists (vpvw_dune has WHL2 / WHL3 pivots but no
        // such meshes, so its back wheels never fly off). The pivot gives the
        // part's place (vehBreakableMgr::Create's GetPivot; a mesh without a
        // pivot would leave it undefined in MM2, and no retail model has one).
        const auto* pivot = m_model.pivot(str::lower(part));
        if (!pivot || !m_gpu || !findFilledLod(*m_gpu, part, asset::Lod::High) || m_detached.contains(part) ||
            !taken.insert(part).second)
            return;
        out.push_back({part, pivot->origin});
    };
    auto pair = [&] {
        const int k = rng.irand() & 3;
        take(std::format("WHL{}", k));
        take(std::format("HUB{}", k));
    };
    if (mph > 100.0f) {
        for (int k = 0; k < 4; ++k) {
            take(std::format("WHL{}", k));
            take(std::format("HUB{}", k));
        }
        take("FNDR0");
        take("FNDR1");
        take("ENGINE");
    } else if (mph > 75.0f) {
        pair();
        pair();
        take(std::format("FNDR{}", rng.irand() & 1));
    } else if (mph > 50.0f) {
        pair();
    }
    return out;
}

void VehicleRenderer::detach(const std::string& part, std::optional<std::size_t> banger) {
    m_detached.insert(part);
    if (banger)
        m_ejectedBangers.push_back(*banger);
}

void VehicleRenderer::reattachAll() {
    // vehBreakableMgr::Reset: each part back on, and the hit banger it became
    // detached (lvlInstance vtable +0x28, dgHitBangerInstance::Detach). MM2
    // walks its breakable lists in their order (BREAK parts, then wheels,
    // hubs, fenders, engine); OpenMM2 detaches in ejection order, which only
    // changes the order of dgBangerActiveManager's free list.
    if (m_ejectedPartReset)
        for (const std::size_t i : m_ejectedBangers)
            m_ejectedPartReset(i);
    m_ejectedBangers.clear();
    m_detached.clear();
    m_wreckEjected = false;
}

std::vector<std::string> VehicleRenderer::materialTextures() const {
    std::vector<std::string> out;
    for (const auto& m : m_paint)
        out.push_back(m.texture);
    return out;
}

std::optional<asset::Lod> VehicleRenderer::lodFor(const VehiclePose& pose, const Mat34& camera) const {
    // lvlInstance::IsVisible: the view depth of the car minus its radius
    // against the Object Detail thresholds; dynamic objects end at NoDraw.
    return objectLod(viewDepth(camera, pose.body.m3), m_radius, m_detail, m_detail.noDraw);
}

namespace {
bool g_sunShadowsActive = false; // VehicleRenderer::setSunShadowsActive
} // namespace

void VehicleRenderer::setSunShadowsActive(bool on) { g_sunShadowsActive = on; }

void VehicleRenderer::setLensFlareTarget(const Mat44* viewProj, float aspect,
                                         std::vector<fx::LensFlareQuad>* out) {
    s_flareViewProj = viewProj;
    s_flareAspect = aspect;
    s_flareOut = out;
}

void VehicleRenderer::setSirenFlares(std::uint32_t randomState) {
    if (m_sirens.empty())
        return;
    fx::Rand random(randomState);
    m_flare.emplace(kSirenFlares, random);
}

std::uint32_t takeVehCarInitDraws(fx::Rand& random) {
    const std::uint32_t flares = random.state();
    random.discard(kVehCarInitDraws);
    return flares;
}

void VehicleRenderer::setLightGlowScales(float size, float color) {
    s_glowSize = size;
    s_glowColor = color;
}

void VehicleRenderer::setTraffic(bool traffic) {
    m_traffic = traffic;
    if (traffic) {
        // aiVehicleInstance draws its colour's shaders as stored: no texel
        // damage.
        m_texelDamage.reset();
        m_live = m_paint;
    }
}

void VehicleRenderer::drawPart(std::string_view part, asset::Lod lod, const Mat34& transform,
                               const MeshDrawOptions& options, bool live) {
    if (!m_detached.empty() && m_detached.contains(std::string(part)))
        return;
    // The part's geometry set as lvlInstance::GetGeomSet fills it: a missing
    // level takes the next less detailed one, never a more detailed one.
    const GpuMesh* mesh = m_gpu ? findFilledLod(*m_gpu, part, lod) : nullptr;
    if (!mesh)
        return;
    MeshDrawOptions o = options;
    o.pixelLighting = m_enhancedLighting; // only lit draws use it (not shadows or glows)
    drawGpuMesh(m_device, m_textures, *mesh, live ? m_live : m_paint, Mat44::fromMat34(transform), o);
}

void VehicleRenderer::drawReflection(const Mat34& body) {
    // modShader::BeginEnvMap + modStatic::DrawEnvMapped: the high LOD body
    // again with refl_dc (cityLevel::GetEnvMap, intensity 1) mapped from the
    // camera-space normals, lit by an ambient of ftol(power x 255) grey only,
    // added (ONE/ONE). It is drawn inside the dynamic object pass, so it is
    // fogged.
    if (!m_reflections)
        return;
    const WorldTexture* env = m_textures.get("refl_dc");
    const GpuMesh* mesh = env ? findFilledLod(*m_gpu, m_bodyPart, asset::Lod::High) : nullptr;
    if (!mesh)
        return;
    for (const auto& d : mesh->draws) {
        const float power = d.shader < m_live.size() ? m_live[d.shader].shininess : 0.0f;
        const int grey = static_cast<int>(power * 255.0f);
        if (grey == 0)
            continue;
        const float level = static_cast<float>(grey & 0xFF) / 255.0f;
        render::DrawCall call;
        call.vertices = {mesh->vertices, 0};
        call.indices = {mesh->indices, 0};
        call.count = d.indexCount;
        call.first = d.firstIndex;
        call.baseVertex = d.baseVertex;
        call.constants.world = Mat44::fromMat34(body);
        call.constants.color = {level, level, level, 1.0f};
        call.constants.flags = render::DrawFlag::Texture1 | render::DrawFlag::EnvMap1 | render::DrawFlag::Fog;
        if (m_enhancedLighting)
            call.constants.flags |= render::DrawFlag::PixelLighting; // per-pixel map and Fresnel
        call.textures[1] = {env->handle, env->sampler};
        call.state.blend = render::BlendMode::Add;
        call.state.depthWrite = false;
        call.state.depthCompare = render::CompareOp::LessEqual;
        call.state.cull = render::CullMode::Back;
        m_device.draw(call);
    }
}

void VehicleRenderer::draw(const VehiclePose& pose, const Mat34& camera) {
    if (!m_gpu)
        return;
    if (m_rooms && m_rooms->active()) {
        // vehCar::Update moves the car's instance to FindRoomId's room.
        m_room = m_rooms->findRoom(pose.body.m3, m_room);
        draw(pose, camera, m_rooms->passes(m_room));
        return;
    }
    // Without a city view's room list: the car, its shadow and its glows
    // while it is visible.
    const auto visible = lodFor(pose, camera);
    if (!visible)
        return;
    if (m_traffic)
        drawTraffic(pose, *visible);
    else if (m_trailer)
        drawTrailer(pose, *visible);
    else
        drawCar(pose, *visible);
    drawShadow(pose);
    if (!m_trailer) // vehTrailerInstance keeps lvlInstance's empty DrawGlow
        drawGlows(pose, camera);
}

void VehicleRenderer::draw(const VehiclePose& pose, const Mat34& camera, const RoomVisibility::Passes& passes) {
    if (!m_gpu)
        return;
    // cityLevel_drawObjects: the car itself through lvlInstance::IsVisible.
    if (passes.objects)
        if (const auto visible = lodFor(pose, camera)) {
            if (m_traffic)
                drawTraffic(pose, *visible);
            else if (m_trailer)
                drawTrailer(pose, *visible);
            else
                drawCar(pose, *visible);
        }
    // cityLevel_drawShadows, cityLevel_drawLights: by the room alone.
    if (passes.shadowsAndGlows) {
        drawShadow(pose);
        if (!m_trailer) // vehTrailerInstance keeps lvlInstance's empty DrawGlow
            drawGlows(pose, camera);
    }
}

void VehicleRenderer::drawCar(const VehiclePose& pose, asset::Lod lod) {
    // vehCarModel::Draw. The very low LOD is the paint job's body alone; the
    // others use the texel damage shaders at the high and medium LODs.
    if (lod == asset::Lod::VeryLow) {
        drawPart(m_bodyPart, lod, pose.body, {}, false);
        return;
    }
    const bool live = lod == asset::Lod::High || lod == asset::Lod::Medium;
    // vppanozgt in paint job 4 draws with alpha reference 0 (alpha test
    // GREATER 0) instead of the dynamic pass's 100.
    MeshDrawOptions part;
    if (str::iequals(m_model.baseName, "vppanozgt") && m_paintjob == 4)
        part.alphaRef = 1.0f / 255.0f;
    drawPart(m_bodyPart, lod, pose.body, part, live);
    MeshDrawOptions decal = part;
    decal.blend = render::BlendMode::Alpha;
    drawPart("DECAL", lod, pose.body, decal, live);
    // vehBreakableMgr::Draw: the attached breakable parts and the paint job's
    // variant part at their pivots.
    for (const char* name : {"break0", "break1", "break2", "break3", "break01", "break12", "break23", "break03"})
        if (const auto* pivot = m_model.pivot(name))
            drawPart(str::upper(name), lod, Mat34::translation(pivot->origin) * pose.body, part, live);
    const auto variant = std::format("variant{}", m_paintjob);
    if (const auto* pivot = m_model.pivot(variant))
        drawPart(str::upper(variant), lod, Mat34::translation(pivot->origin) * pose.body, part, live);

    if (lod == asset::Lod::High) {
        drawReflection(pose.body);
        // Fenders: turned with the front wheels, upright with the body.
        if (m_fenderOffset && pose.hasWheelWorld) {
            for (int i = 0; i < 2; ++i) {
                if (!pose.wheelValid[static_cast<std::size_t>(i)])
                    continue;
                const Mat34& wheel = pose.wheelWorld[static_cast<std::size_t>(i)];
                Mat34 m;
                m.m0 = wheel.m0;
                m.m1 = pose.body.m1;
                m.m2 = m.m0.cross(m.m1);
                Vec3 offset = *m_fenderOffset;
                if (i == 1)
                    offset.x = -offset.x;
                m.m3 = m.transformDir(offset) + wheel.m3;
                drawPart(std::format("FNDR{}", i), asset::Lod::High, m, part, live);
            }
        }
    }
    // Wheels and hubs at the simulation's wheel matrices. WHL4 and WHL5 are
    // WHL2 and WHL3 moved (0.2 + 2) wheel radii back along the body.
    for (std::size_t i = 0; i < 6; ++i) {
        const auto m = wheelMatrix(pose, i);
        if (!m)
            continue;
        drawPart(std::format("{}{}", m_wheelPrefix, i), lod, *m, part, live);
        if (i < 4)
            drawPart(std::format("HUB{}", i), lod, *m, part, live);
    }
}

std::optional<Mat34> VehicleRenderer::wheelMatrix(const VehiclePose& pose, std::size_t i) const {
    if (i >= 4) {
        const auto* lead = m_model.wheel(static_cast<int>(i) - 2);
        const auto m = lead ? wheelMatrix(pose, i - 2) : std::nullopt;
        if (!m || !m_model.wheel(static_cast<int>(i)))
            return std::nullopt;
        Mat34 out = *m;
        out.m3 += pose.body.m2 * (kExtraWheelSpacing * lead->radius);
        return out;
    }
    if (pose.hasWheelWorld) {
        if (!pose.wheelValid[i])
            return std::nullopt;
        return pose.wheelWorld[i];
    }
    const auto* w = m_model.wheel(static_cast<int>(i));
    if (!w)
        return std::nullopt;
    const Vec3 pivot = w->position + Vec3{0, pose.wheelDrop[i], 0};
    return Mat34::rotationX(pose.wheelSpin[i]) * Mat34::rotationY(pose.wheelSteer[i]) * Mat34::translation(pivot) *
           pose.body;
}

void VehicleRenderer::drawTraffic(const VehiclePose& pose, asset::Lod lod) {
    // aiVehicleInstance::Draw: the body at its LOD with the colour's shaders;
    // the breakables always at the high LOD; at the high LOD only, the
    // reflection and the wheels (LAME_WHEELS is off: no wheels below it).
    drawPart(m_bodyPart, lod, pose.body, {}, false);
    for (const char* name : {"break0", "break1", "break2", "break3"})
        if (const auto* pivot = m_model.pivot(name))
            drawPart(str::upper(name), asset::Lod::High, Mat34::translation(pivot->origin) * pose.body, {}, false);
    if (lod != asset::Lod::High)
        return;
    drawReflection(pose.body);
    // The wheels on the rails turn about their axles at their pivots (no
    // steering); WHL4 and WHL5 likewise at their own pivots. A car with a
    // body has its wheels where its vehWheelCheaps put them (pose.wheelWorld).
    for (std::size_t i = 0; i < 6; ++i) {
        std::optional<Mat34> m;
        if (pose.hasWheelWorld) {
            if (pose.wheelValid[i])
                m = pose.wheelWorld[i];
        } else if (i < 4) {
            m = wheelMatrix(pose, i);
        } else if (const auto* w = m_model.wheel(static_cast<int>(i))) {
            m = Mat34::rotationX(pose.wheelSpin[i]) * Mat34::translation(w->position) * pose.body;
        }
        if (m)
            drawPart(std::format("{}{}", m_wheelPrefix, i), asset::Lod::High, *m, {}, false);
    }
}

void VehicleRenderer::drawTrailer(const VehiclePose& pose, asset::Lod lod) {
    // vehTrailerInstance::Draw: the body at its LOD with the paint job's
    // shaders as stored; at the high LOD only, TLIGHT while the tow car
    // brakes harder than 0.1 (an ordinary lit draw in the object pass, not
    // a glow: its black, fully transparent fxltglowred material leaves it
    // invisible under the pass's alpha test, inferred from the material)
    // and TWHL0-3 at the trailer's
    // wheel matrices. None of vehCarModel's other parts: no reflection,
    // decal, breakables, fenders, hubs or TWHL4/5, so TWHL0/1's medium and
    // low meshes (modelled away from their pivots) are never drawn.
    drawPart(m_bodyPart, lod, pose.body, {}, false);
    if (lod != asset::Lod::High)
        return;
    if (pose.brakeLights)
        drawPart("TLIGHT", asset::Lod::High, pose.body, {}, false);
    for (std::size_t i = 0; i < 4; ++i)
        if (pose.hasWheelWorld && pose.wheelValid[i])
            drawPart(std::format("{}{}", m_wheelPrefix, i), asset::Lod::High, pose.wheelWorld[i], {}, false);
}

std::optional<Mat34> groundShadowMatrix(const Mat34& body, const VehicleRenderer::GroundProbe& probe) {
    // lvlInstance::DrawPhysics: the ground 1 m above to 1 m below the car,
    // else to 5 m below; no shadow on slopes steeper than normal.y 0.7. The
    // car's frame is turned onto the ground (upside down: its flipped up axis).
    if (!probe)
        return std::nullopt;
    Vec3 point, normal;
    const Vec3 p = body.m3;
    if (!probe(p + Vec3{0, 1, 0}, p - Vec3{0, 1, 0}, point, normal) &&
        !probe(p + Vec3{0, 1, 0}, p - Vec3{0, 5, 0}, point, normal))
        return std::nullopt;
    if (normal.y < 0.7f)
        return std::nullopt;
    Mat34 m;
    if (body.m1.y <= 0.0f) {
        m = rotateUpTo(body, -body.m1, normal);
        m.m1 = -m.m1;
    } else {
        m = rotateUpTo(body, body.m1, normal);
    }
    m.m3 = point;
    return m;
}

Mat34 trafficShadowMatrix(const Mat34& body, bool physical, const VehicleRenderer::GroundProbe& probe) {
    // aiVehicleInstance::DrawShadow: a car with a body (an aiVehicleActive)
    // lays its shadow on the ground under the active's matrix
    // (lvlInstance::DrawPhysics); otherwise, or with no ground there, the
    // shadow sits at GetMatrix while the car is upright (m1.y >= 0) and on
    // the ground under it when upside down and there is ground.
    if (physical)
        if (auto m = groundShadowMatrix(body, probe))
            return *m;
    if (!(body.m1.y >= 0.0f))
        if (auto m = groundShadowMatrix(body, probe))
            return *m;
    return body;
}

std::optional<Mat34> VehicleRenderer::shadowMatrix(const Mat34& body) const {
    return groundShadowMatrix(body, m_probe);
}

void VehicleRenderer::drawShadow(const VehiclePose& pose) {
    if (g_sunShadowsActive)
        return; // OpenMM2 sun shadows replace the blob
    // vehCarModel::DrawShadow: the high LOD shadow mesh on the ground,
    // alpha blended, depth tested without writes and pulled forward (MM2
    // narrows the depth range to 0.001-0.999).
    std::optional<Mat34> m;
    if (m_traffic)
        m = trafficShadowMatrix(pose.body, pose.physical, m_probe);
    else if (m_probe)
        m = shadowMatrix(pose.body);
    else
        m = pose.body; // OpenMM2: without a ground probe the shadow sits at the body
    if (!m)
        return;
    MeshDrawOptions shadow;
    shadow.lighting = false;
    shadow.depthWrite = false;
    shadow.depthBias = true;
    shadow.blend = render::BlendMode::Alpha;
    // The shadow pass comes after cityLevel::DrawRooms has put the alpha
    // test back to its default (alpha not 0), not the GREATER 100 of the
    // object passes.
    shadow.alphaRef = 1.0f / 255.0f;
    drawPart("SHADOW", asset::Lod::High, *m, shadow, false);
}

void VehicleRenderer::addLightGlow(fx::ParticleRenderer& cards, const Vec3& position, const Vec3& direction,
                                   const Vec3& color, const Mat34& camera) {
    // ltLight::ComputeIntensity: I = 25 cos^3(theta) / d^2 towards the eye,
    // 0 behind the light; ltLight::DrawGlow: a camera-facing card of half
    // size s sqrt(I d^2) with c x the light colour (s, c: setLightGlowScales).
    const Vec3 toEye = camera.m3 - position;
    const float d2 = toEye.mag2();
    if (d2 <= 0.0f)
        return;
    const float c = toEye.dot(direction) / std::sqrt(d2);
    if (c < 0.0f)
        return;
    const float intensity = kLightIntensity / d2 * std::pow(c, kSpotExponent);
    fx::SparkPos card;
    card.position = position;
    card.radius = std::sqrt(d2 * intensity) * s_glowSize;
    card.color = argb(color * s_glowColor);
    cards.add(card, 1, 1, camera);
}

void VehicleRenderer::drawGlows(const VehiclePose& pose, const Mat34& camera) {
    // vehCarModel::DrawGlow / aiVehicleInstance::DrawGlow (between
    // ltLight::DrawGlowBegin and ltLight::DrawGlowEnd): lighting off, no
    // fog, no depth writes, added (ONE/ONE), the high LOD light parts.
    MeshDrawOptions glow;
    glow.lighting = false;
    glow.fog = false;
    glow.blend = render::BlendMode::Add;
    glow.depthWrite = false;
    // The glow pass runs with the default alpha test (alpha not 0).
    glow.alphaRef = 1.0f / 255.0f;
    if (m_traffic) {
        // Tail lights while braking or stopped, and again with the light flag.
        if (pose.brakeLights)
            drawPart("TLIGHT", asset::Lod::High, pose.body, glow, false);
    } else if (pose.brakeLights) {
        drawPart("TLIGHT", asset::Lod::High, pose.body, glow, false);
        drawPart("BLIGHT", asset::Lod::High, pose.body, glow, false);
    }
    if (pose.headlights)
        drawPart("TLIGHT", asset::Lod::High, pose.body, glow, false);
    if (m_traffic) {
        // aiVehicleInstance::DrawGlow: the indicators after the tail lights.
        if (pose.indicators & 1)
            drawPart("SLIGHT0", asset::Lod::High, pose.body, glow, false);
        if (pose.indicators & 2)
            drawPart("SLIGHT1", asset::Lod::High, pose.body, glow, false);
    }
    if (!m_traffic && pose.reverseLights)
        drawPart("RLIGHT", asset::Lod::High, pose.body, glow, false);

    m_cards.begin();
    if (m_traffic) {
        // aiVehicleInstance::DrawGlow: one shared white spot light (the
        // manager's) at the headlight0 pivot, and its mirror image when the
        // car has a HEADLIGHT1 part, pointing forward and pulled s metres
        // towards the camera.
        if (pose.headlights && m_headlights[0]) {
            Vec3 pull = pose.body.m3 - camera.m3;
            const float len2 = pull.mag2();
            pull = len2 > 0.0f ? pull * (-s_glowSize / std::sqrt(len2)) : Vec3{};
            const Vec3 local = m_headlights[0]->position;
            addLightGlow(m_cards, pose.body.transform(local) + pull, -pose.body.m2, {1, 1, 1}, camera);
            if (findFilledLod(*m_gpu, "HEADLIGHT1", asset::Lod::High))
                addLightGlow(m_cards, pose.body.transform({-local.x, local.y, local.z}) + pull, -pose.body.m2,
                             {1, 1, 1}, camera);
        }
    } else {
        // Headlight beams (vehCarModel::DrawHeadlights) with the light flag,
        // or sweeping while the siren is on: the two ltLights keep their
        // world-space directions, set to the car's forward axis without the
        // siren and turned about Y by +-42.411503 rad/s of frame time from
        // wherever they point while it is on (Vector3::RotateY), so the
        // sweep does not follow the car's turning.
        const bool sweeping = pose.siren && !m_sirens.empty();
        if ((pose.headlights || sweeping) && (m_headlights[0] || m_headlights[1])) {
            if (sweeping) {
                // The frame's time from the siren's angle (2.5 pi rad/s,
                // kept within a turn).
                float turned = pose.sirenAngle - m_beamSirenAngle;
                if (turned < 0.0f)
                    turned = turned + 6.2831855f;
                const float sweep = turned / (2.5f * 3.1415927f) * kHeadlightSweep;
                m_beamDirection[0] = Mat34::rotationY(sweep).transformDir(m_beamDirection[0]);
                m_beamDirection[1] = Mat34::rotationY(-sweep).transformDir(m_beamDirection[1]);
            } else {
                m_beamDirection[0] = m_beamDirection[1] = -pose.body.m2;
            }
            for (std::size_t i = 0; i < 2; ++i)
                if (m_headlights[i])
                    addLightGlow(m_cards, pose.body.transform(m_headlights[i]->position), m_beamDirection[i],
                                 m_headlights[i]->color, camera);
        }
        m_beamSirenAngle = pose.sirenAngle;
        // Siren beams (vehSiren::Draw): world-space directions turning about
        // Y, a quarter turn apart.
        if (pose.siren)
            for (std::size_t i = 0; i < m_sirens.size(); ++i) {
                const float a = static_cast<float>(i) * 1.5707964f + pose.sirenAngle;
                const Vec3 direction = Mat34::rotationY(a).transformDir({1, 0, 0});
                const Vec3 position = pose.body.transform(m_sirens[i].position);
                addLightGlow(m_cards, position, direction, m_sirens[i].color, camera);
                // ltLensFlare::Draw with ltLight::ComputeIntensity(eye, 0.05).
                if (m_flare && s_flareOut && s_flareViewProj)
                    m_flare->draw(position, m_sirens[i].color,
                                  fx::spotIntensity(position, direction, camera.m3, kFlareThreshold),
                                  *s_flareViewProj, s_flareAspect, *s_flareOut);
            }
    }
    m_cards.flush(m_device, m_textures.get("lt_glow"), {render::BlendMode::Add, false, {1, 1, 1, 1}});
}

} // namespace mm2::game
