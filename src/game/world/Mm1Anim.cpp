#include "game/world/Mm1Anim.h"

#include "asset/Dlp.h"
#include "core/Log.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::game::world {
namespace {

std::string_view textOf(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// A basis (rows: the cubic's a, b, c, d) times four values.
Vec4 basisTimes(const std::array<float, 16>& m, float v0, float v1, float v2, float v3) {
    auto row = [&](int r) {
        const auto i = static_cast<std::size_t>(r * 4);
        return ((m[i] * v0 + m[i + 1] * v1) + m[i + 2] * v2) + m[i + 3] * v3;
    };
    return {row(0), row(1), row(2), row(3)};
}

// TrainHermite (hb): P0, P1, T0, T1.
constexpr std::array<float, 16> kHermite{2, -2, 1, 1, -3, 3, -2, -1, 0, 0, 1, 0, 1, 0, 0, 0};
// CatmullBasis (cb): the uniform cubic B-spline.
constexpr std::array<float, 16> kBSpline{-1.0f / 6.0f, 0.5f, -0.5f, 1.0f / 6.0f, 0.5f, -1.0f, 0.5f, 0.0f,
                                         -0.5f,        0.0f, 0.5f,  0.0f,        1.0f / 6.0f, 2.0f / 3.0f,
                                         1.0f / 6.0f,  0.0f};

Mm1Cubic cubicOf(const std::array<float, 16>& basis, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
    Mm1Cubic out;
    out.axes[0] = basisTimes(basis, a.x, b.x, c.x, d.x);
    out.axes[1] = basisTimes(basis, a.y, b.y, c.y, d.y);
    out.axes[2] = basisTimes(basis, a.z, b.z, c.z, d.z);
    return out;
}

Vec3 unitOr(const Vec3& v, const Vec3& fallback) { return v.mag2() > 1e-12f ? v.normalized() : fallback; }

// mmAnimTrain::GetStart: the segment and progress `offset` metres along the
// loop (a negative offset counts back from the end).
void startAt(const Mm1Anim::Train& t, float offset, int& segment, float& progress) {
    if (offset < 0.0f)
        offset = t.total + offset;
    float acc = 0.0f;
    for (std::size_t i = 0; i < t.lengths.size(); ++i) {
        acc += t.lengths[i];
        if (acc > offset) {
            segment = static_cast<int>(i);
            progress = (offset - (acc - t.lengths[i])) / t.lengths[i];
            return;
        }
    }
}

// The DLP H group's box length along z (mmAnimTrainCar::Init's BoundBox).
std::optional<float> dlpLength(const vfs::Vfs& vfs, const std::string& model) {
    auto bytes = vfs.readAll(std::format("mm1/dlp/{}.dlp", model));
    if (!bytes)
        return std::nullopt;
    auto dlp = asset::parseDlp(*bytes);
    if (!dlp)
        return std::nullopt;
    Aabb box;
    for (const auto& g : dlp->groups) {
        if (!str::iequals(g.name, "H"))
            continue;
        for (auto v : g.vertices)
            if (v < dlp->vertices.size())
                box.expand(dlp->vertices[v]);
        for (auto p : g.patches)
            if (p < dlp->patches.size())
                for (auto v : dlp->patches[p])
                    if (v < dlp->vertices.size())
                        box.expand(dlp->vertices[v]);
    }
    if (!box.valid())
        return std::nullopt;
    return std::abs(box.max.z - box.min.z);
}

} // namespace

std::vector<Vec3> parseMm1AnimPath(std::string_view text) {
    std::vector<Vec3> out;
    for (std::string_view line : str::split(text, '\n')) {
        const auto f = str::split(str::trim(line), ',');
        if (f.size() < 3)
            continue;
        auto num = [&](std::size_t i) { return str::parseDouble(str::trim(f[i])); };
        const auto x = num(0), y = num(1), z = num(2);
        if (x && y && z)
            out.push_back({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)});
    }
    return out;
}

Vec3 Mm1Cubic::position(float t) const {
    auto f = [t](const Vec4& c) { return ((c.x * t + c.y) * t + c.z) * t + c.w; };
    return {f(axes[0]), f(axes[1]), f(axes[2])};
}

Vec3 Mm1Cubic::direction(float t) const {
    // mmAnimTrain::SolveXZCurve: c - (-2 b - 3 a t) t.
    auto f = [t](const Vec4& c) { return c.z - (-2.0f * c.y - 3.0f * (c.x * t)) * t; };
    return {f(axes[0]), f(axes[1]), f(axes[2])};
}

Mm1Cubic mm1TrainCubic(const std::vector<Vec3>& p, const std::vector<float>& lengths, std::size_t i) {
    const std::size_t n = p.size();
    const Vec3& prev = p[i == 0 ? n - 1 : i - 1];
    const Vec3& p0 = p[i];
    const Vec3& p1 = p[(i + 1) % n];
    const Vec3& p2 = p[(i + 2) % n];
    const float len = lengths[i];
    const Vec3 t0 = unitOr(p1 - prev, Vec3{0, 0, -1}) * len;
    const Vec3 t1 = unitOr(p2 - p0, Vec3{0, 0, -1}) * len;
    return cubicOf(kHermite, p0, p1, t0, t1);
}

Mm1Cubic mm1PlaneCubic(const std::vector<Vec3>& p, std::size_t i) {
    const std::size_t n = p.size();
    // The point before the first segment is the second to last (the path
    // closes on itself); the one after the next wraps over n - 1.
    const Vec3& prev = p[i == 0 ? n - 2 : i - 1];
    return cubicOf(kBSpline, prev, p[i], p[(i + 1) % n], p[(i + 2) % (n - 1)]);
}

Mat34 mm1AnimFrame(const Vec3& position, const Vec3& direction) {
    const Vec3 z = unitOr(direction, Vec3{0, 0, -1}) * -1.0f;
    Mat34 m;
    m.m2 = z;
    m.m0 = Vec3{0, 1, 0}.cross(z);
    m.m1 = z.cross(m.m0);
    m.m3 = position;
    return m;
}

Mm1Anim::Train Mm1Anim::makeTrain(std::vector<Vec3> points, const std::string& model, int cars, float carSpacing) {
    Train t;
    t.model = model;
    t.points = std::move(points);
    const std::size_t n = t.points.size();
    // The loop's segments (mmAnimTrain::Init), including the last back to
    // the first point.
    for (std::size_t i = 0; i < n; ++i) {
        t.lengths.push_back(t.points[(i + 1) % n].dist(t.points[i]));
        t.total += t.lengths.back();
    }
    t.speed = t.total / kTrainLapSeconds; // SetPathSpeed
    for (std::size_t i = 0; i < n; ++i)
        t.cubics.push_back(mm1TrainCubic(t.points, t.lengths, i));
    // Each car a box length behind the one before.
    float offset = 0.0f;
    for (int k = 0; k < cars; ++k) {
        TrainCar car;
        startAt(t, offset, car.startSegment, car.startT);
        t.cars.push_back(car);
        offset -= carSpacing;
    }
    return t;
}

Mm1Anim::Plane Mm1Anim::makePlane(std::vector<Vec3> points, const std::string& model) {
    Plane p;
    p.model = model;
    p.points = std::move(points);
    const std::size_t n = p.points.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        p.lengths.push_back(p.points[i + 1].dist(p.points[i]));
        p.total += p.lengths.back();
    }
    p.speed = p.total / kPlaneLapSeconds;
    for (std::size_t i = 0; i + 1 < n; ++i)
        p.cubics.push_back(mm1PlaneCubic(p.points, i));
    return p;
}

void Mm1Anim::updateTrain(Train& t, float dt) {
    if (t.lengths.empty())
        return;
    const auto n = static_cast<int>(t.lengths.size());
    for (auto& car : t.cars) {
        // mmAnimTrain::SolvePosition: the progress at the path speed (times
        // the speed factor, 1), into the next non-empty segment past 1.
        car.t = t.speed * dt / t.lengths[static_cast<std::size_t>(car.segment)] + car.t;
        if (car.t > 1.0f) {
            const float over = (car.t - 1.0f) * t.lengths[static_cast<std::size_t>(car.segment)];
            int guard = n;
            do {
                car.segment = (car.segment + 1) % n;
            } while (t.lengths[static_cast<std::size_t>(car.segment)] == 0.0f && --guard > 0);
            car.t = over / t.lengths[static_cast<std::size_t>(car.segment)];
        }
        const auto& c = t.cubics[static_cast<std::size_t>(car.segment)];
        car.matrix = mm1AnimFrame(c.position(car.t), c.direction(car.t));
    }
}

void Mm1Anim::updatePlane(Plane& p, float dt) {
    if (p.lengths.empty())
        return;
    const auto segs = static_cast<int>(p.lengths.size());
    // mmAnimPlane::SolvePosition (closed: segments wrap over count - 1).
    p.t = p.speed * dt / p.lengths[static_cast<std::size_t>(p.segment)] + p.t;
    if (p.t > 1.0f) {
        const float over = (p.t - 1.0f) * p.lengths[static_cast<std::size_t>(p.segment)];
        p.segment = (p.segment + 1) % segs;
        p.t = over / p.lengths[static_cast<std::size_t>(p.segment)];
    }
    // SolveVector clamps the progress; UpdatePathPosition adds PlaneFudge.
    const float t = std::clamp(p.t, 0.0f, 1.0f);
    const auto& c = p.cubics[static_cast<std::size_t>(p.segment)];
    p.matrix = mm1AnimFrame(c.position(t) + Vec3{0.0f, 10.0f, 0.0f}, c.direction(t));
}

std::unique_ptr<Mm1Anim> Mm1Anim::load(const city::Mm1City& city, const vfs::Vfs& vfs) {
    const std::string dir = std::format("mm1/city/{}/", str::lower(city.name));
    auto list = vfs.readAll(dir + "anim.csv");
    if (!list)
        return nullptr;
    auto anim = std::make_unique<Mm1Anim>();
    for (std::string_view line : str::split(textOf(*list), '\n')) {
        const std::string name = str::lower(str::trim(line));
        if (name.empty())
            continue;
        auto bytes = vfs.readAll(dir + name + ".csv");
        if (!bytes)
            continue;
        auto points = parseMm1AnimPath(textOf(*bytes));
        // mmAnimSpline::Init: by the file's name.
        if (name == "anim_eltrain" && points.size() > 3) {
            // mmAnimTrainCar::Init: a car's length is its DLP's H box along z
            // plus 1 m (10 m without the DLP); mmAnimTrain::Init spaces the
            // cars by that less 1 m.
            const float spacing = dlpLength(vfs, "r_l_train").value_or(9.0f);
            anim->m_train = makeTrain(std::move(points), "r_l_train", kTrainCars, spacing);
        } else if (name == "anim_plane" && points.size() > 3) {
            anim->m_plane = makePlane(std::move(points), "vaboeing");
        }
    }
    anim->reset();
    log::info("mm1 anim: {}{}", anim->m_train ? "El train " : "", anim->m_plane ? "plane" : "");
    return anim;
}

void Mm1Anim::reset() {
    // mmAnimTrain::Reset (mmAnimTrainCar::Reset: back to the start) and
    // mmAnimPlane::Reset (phase 0: the first segment's start).
    if (m_train) {
        for (auto& c : m_train->cars) {
            c.segment = c.startSegment;
            c.t = c.startT;
        }
        updateTrain(*m_train, 0.0f);
    }
    if (m_plane) {
        m_plane->segment = 0;
        m_plane->t = 0.0f;
        updatePlane(*m_plane, 0.0f);
    }
    if (m_trainAudio)
        m_trainAudio->reset();
}

void Mm1Anim::update(float dt) {
    if (m_train)
        updateTrain(*m_train, dt);
    if (m_plane)
        updatePlane(*m_plane, dt);
}

Mm1Anim::~Mm1Anim() { stopAudio(); }

void Mm1Anim::loadAudio(const vfs::Vfs& vfs, audio::SoundBank& bank, audio::Mixer& mixer,
                        audio::game::Object3DManager* manager) {
    if (!m_train)
        return;
    m_trainAudio = std::make_unique<audio::game::SubwayAudio>();
    if (!m_trainAudio->load(vfs, bank, mixer, "subwaycar", manager))
        m_trainAudio.reset();
}

void Mm1Anim::updateAudio(const Mat34& listener, float dt, bool inTunnel) {
    if (!m_trainAudio || !m_train || m_train->cars.size() <= static_cast<std::size_t>(kTrainSoundCar))
        return;
    m_trainAudio->setPosition(m_train->cars[static_cast<std::size_t>(kTrainSoundCar)].matrix.m3);
    m_trainAudio->update(listener, m_train->speed, dt, inTunnel); // always running
}

void Mm1Anim::stopAudio() {
    if (m_trainAudio)
        m_trainAudio->stop();
}

} // namespace mm2::game::world
