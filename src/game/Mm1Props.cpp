#include "game/Mm1Props.h"

#include "asset/Bms.h"
#include "asset/Mm1TexSheet.h"
#include "ai/VehicleData.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/TextureLibrary.h"
#include "vfs/Mm1Source.h"

#include <array>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace mm2::game {
namespace {

constexpr std::array<std::pair<const char*, asset::Lod>, 4> kLods{
    {{"h", asset::Lod::High}, {"m", asset::Lod::Medium}, {"l", asset::Lod::Low}, {"vl", asset::Lod::VeryLow}}};

std::string_view textOf(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// mmBangerData's PartNames list: the quoted names between "[" and "]".
std::vector<std::string> partNames(std::string_view text) {
    std::vector<std::string> out;
    const auto start = text.find("PartNames");
    if (start == std::string_view::npos)
        return out;
    const auto open = text.find('[', start), close = text.find(']', start);
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return out;
    std::string_view list = text.substr(open + 1, close - open - 1);
    for (std::size_t q = list.find('"'); q != std::string_view::npos; q = list.find('"', q + 1)) {
        const auto end = list.find('"', q + 1);
        if (end == std::string_view::npos)
            break;
        out.push_back(str::lower(list.substr(q + 1, end - q - 1)));
        q = end;
    }
    return out;
}

} // namespace

struct Mm1ModelLoader::Impl {
    const vfs::Vfs& vfs;
    ModelLibrary& models;
    bangers::BangerDataLibrary& data;
    bool night = false;
    asset::Mm1TexWeather weather = asset::Mm1TexWeather::Clear;
    std::optional<asset::Mm1TexSheet> sheet;
    std::map<std::string, bool> ready; // model -> usable
    int loaded = 0;

    std::string textureName(const std::string& raw) const {
        std::string name = sheet ? sheet->resolve(raw, 0, night, weather) : std::string();
        if (name.empty())
            name = raw;
        return std::string(kMm1TexturePrefix) + str::lower(name);
    }

    // One mesh file appended to `pkg` as `name` at `lod` (its materials
    // appended to the single paint job).
    bool addMesh(asset::Pkg& pkg, const std::string& path, const std::string& part, const std::string& name,
                 asset::Lod lod) const {
        auto bytes = vfs.readAll(path);
        if (!bytes)
            return false;
        auto bms = asset::parseBms(*bytes);
        if (!bms)
            return false;
        auto converted = asset::bmsToPkgMesh(*bms);
        const auto base = static_cast<std::uint32_t>(pkg.paintjobs[0].size());
        for (auto& m : converted.materials) {
            if (!m.texture.empty())
                m.texture = textureName(m.texture);
            pkg.paintjobs[0].push_back(std::move(m));
        }
        for (auto& sec : converted.mesh.sections)
            sec.shaderIndex += base;
        converted.mesh.part = part;
        converted.mesh.lod = lod;
        converted.mesh.name = name;
        pkg.meshes.push_back(std::move(converted.mesh));
        return true;
    }

    // One mesh group of a model: its levels of detail.
    bool addGroup(asset::Pkg& pkg, const std::string& model, const std::string& file, const std::string& part) const {
        const std::string prefix(vfs::kMm1Prefix);
        bool any = false;
        for (const auto& [suffix, lod] : kLods) {
            const std::string path =
                std::format("{}bms/{}/{}{}.bms", prefix, model, file.empty() ? "" : file + "_", suffix);
            const std::string name =
                part.empty() ? std::string(asset::lodSuffix(lod)) : std::format("{}_{}", part, asset::lodSuffix(lod));
            any = addMesh(pkg, path, part, name, lod) || any;
        }
        return any;
    }
};

Mm1ModelLoader::Mm1ModelLoader(const vfs::Vfs& vfs, ModelLibrary& models, bangers::BangerDataLibrary& data,
                               bool night, asset::Mm1TexWeather weather)
    : m_impl(std::make_unique<Impl>(Impl{vfs, models, data, night, weather, std::nullopt, {}, 0})) {
    if (auto b = vfs.readAll(std::string(vfs::kMm1Prefix) + "mtl/global.tsh"))
        m_impl->sheet = asset::Mm1TexSheet::parse(textOf(*b));
}

Mm1ModelLoader::~Mm1ModelLoader() = default;

int Mm1ModelLoader::loaded() const { return m_impl->loaded; }

bool Mm1ModelLoader::ensure(const std::string& rawModel) {
    Impl& im = *m_impl;
    const std::string model = str::lower(rawModel);
    if (auto it = im.ready.find(model); it != im.ready.end())
        return it->second;
    const std::string prefix(vfs::kMm1Prefix);
    bool ok = false;
    auto bytes = im.vfs.readAll(std::format("{}tune/{}.mmbangerdata", prefix, model));
    std::string err;
    auto d = bytes ? bangers::parseMm1BangerData(model, textOf(*bytes), &err) : std::nullopt;
    if (bytes && !d)
        log::warn("mm1 props: tune/{}.mmbangerdata: {}", model, err);
    if (d) {
        // A lamp's glow (mmBangerInstance::DrawGlow: a card at GlowOffset in
        // the CG frame) only where mmUnhitBangerInstance::Init adds the GLOW
        // meshes: an instance flagged GLOW (0x40) or data with BillFlags 0x20
        // (BANGER_BILL_FLAG_LOWER_HALF_GLOW). Every MM1 data file has a
        // GlowOffset, so the rest must not glow. Inferred per model: in
        // Chicago every instance of a model with GLOW meshes (opstlite,
        // tpltst, tpsawhrslt) is flagged, and no other is.
        bool glows = (d->billFlags & 0x20) != 0;
        for (const char* g : {"glow", "glow_h", "glow_m", "glow_l", "glow_vl"})
            glows = glows || im.vfs.exists(std::format("{}bms/{}/{}.bms", prefix, model, g));
        if (!glows)
            d->glowOffsets.clear();
        asset::Pkg pkg;
        pkg.paintjobs.emplace_back();
        if (im.addGroup(pkg, model, "", "")) {
            // The breakable parts: MM2's BREAKnn groups and entries.
            const auto parts = partNames(textOf(*bytes));
            for (std::size_t k = 0; k < parts.size(); ++k) {
                const std::string mm2Part = std::format("BREAK{:02}", k + 1);
                im.addGroup(pkg, model, parts[k], mm2Part);
                if (auto pb = im.vfs.readAll(std::format("{}tune/{}_{}.mmbangerdata", prefix, model, parts[k])))
                    if (auto pd = bangers::parseMm1BangerData(std::format("{}_break{:02}", model, k + 1),
                                                               textOf(*pb)))
                        im.data.add(std::format("{}_break{:02}", model, k + 1), std::move(*pd));
            }
            // A traffic light's signal glows (aiTrafficLightInstance::Init:
            // GLOWREDDAY .. GLOWGREENNIGHT), single meshes drawn as the high
            // level of detail.
            for (const char* colour : {"RED", "YELLOW", "GREEN"})
                for (const char* time : {"DAY", "NIGHT"}) {
                    const std::string group = std::format("GLOW{}{}", colour, time);
                    im.addMesh(pkg, std::format("{}bms/{}/{}.bms", prefix, model, str::lower(group)), group,
                               std::format("{}_{}", group, asset::lodSuffix(asset::Lod::High)), asset::Lod::High);
                }
            d->numParts = std::min<int>(d->numParts, static_cast<int>(parts.size()));
            if (im.models.add(model, pkg)) {
                im.data.add(model, std::move(*d));
                ok = true;
                ++im.loaded;
            }
        }
    }
    im.ready.emplace(model, ok);
    return ok;
}

std::optional<asset::VehicleModel> Mm1ModelLoader::vehicle(const std::string& rawModel) {
    Impl& im = *m_impl;
    const std::string model = str::lower(rawModel);
    const std::string prefix(vfs::kMm1Prefix);
    auto tune = im.vfs.readAll(std::format("{}tune/{}.aivehicledata", prefix, model));
    auto data = tune ? ai::parseMm1VehicleData(model, textOf(*tune)) : std::nullopt;
    if (!data)
        return std::nullopt;
    asset::VehicleModel v;
    v.baseName = model;
    v.pkg.paintjobs.emplace_back();
    if (!im.addGroup(v.pkg, model, "body", "BODY"))
        return std::nullopt;
    im.addGroup(v.pkg, model, "shadow", "SHADOW");
    // Single meshes (no levels of detail): the lights, drawn as the high one.
    for (const char* part : {"tlight", "slight0", "slight1"}) {
        const std::string group = str::upper(part);
        im.addMesh(v.pkg, std::format("{}bms/{}/{}.bms", prefix, model, part), group,
                   std::format("{}_{}", group, asset::lodSuffix(asset::Lod::High)), asset::Lod::High);
    }
    for (int i = 0; i < 4; ++i) {
        const std::string file = std::format("whl{}", i);
        if (!im.addGroup(v.pkg, model, file, std::format("WHL{}", i)))
            continue;
        // The wheel's box about its centre (the mesh is modelled there),
        // placed at its offset: what geometry/<model>_whlN.mtx gives MM2.
        Aabb box;
        if (auto b = im.vfs.readAll(std::format("{}bms/{}/{}_h.bms", prefix, model, file)))
            if (auto bms = asset::parseBms(*b))
                box = bms->bounds();
        if (!box.valid())
            continue;
        const Vec3 at = data->wheels[static_cast<std::size_t>(i)];
        asset::Mtx m;
        m.origin = at;
        m.min = at + box.min;
        m.max = at + box.max;
        m.center = (m.min + m.max) * 0.5f;
        v.pivots.emplace(file, m);
        v.wheels.push_back({i, at, std::abs((box.max.y - box.min.y) * 0.5f), box.max.x - box.min.x});
    }
    return v;
}

std::vector<bangers::PlacedProp> loadMm1Props(const city::Mm1City& city, const vfs::Vfs& vfs, ModelLibrary& models,
                                              bangers::BangerDataLibrary& data, bool night,
                                              asset::Mm1TexWeather weather,
                                              Mm1PropStats* stats) {
    Mm1ModelLoader loader(vfs, models, data, night, weather);
    return loadMm1Props(city, loader, stats);
}

std::vector<bangers::PlacedProp> loadMm1Props(const city::Mm1City& city, Mm1ModelLoader& loader,
                                              Mm1PropStats* stats) {
    Mm1PropStats s;
    std::vector<bangers::PlacedProp> out;
    for (const auto& b : city.bangers) {
        const std::string model = str::lower(b.name);
        if (!loader.ensure(model)) {
            ++s.withoutData;
            continue;
        }
        // mmUnhitBangerInstance::Init: the X axis points horizontally toward
        // the 'ahead' point (its height and distance are free: 85 of
        // Chicago's records are not 1 m away), Y up, Z = (-x.z, 0, x.x). Only
        // the yaw is kept (FromMatrix: Angle = (m0.x, m0.z)).
        Vec3 along{b.ahead.x - b.position.x, 0.0f, b.ahead.z - b.position.z};
        const float len = along.mag();
        along = len > 0.0f ? along * (1.0f / len) : Vec3{1, 0, 0}; // Vector3::operator~
        bangers::PlacedProp p;
        p.model = model;
        p.transform.m0 = along;
        p.transform.m1 = {0.0f, 1.0f, 0.0f};
        p.transform.m2 = {-along.z, 0.0f, along.x};
        p.transform.m3 = b.position;
        p.source = bangers::PlacedProp::Source::Instance;
        out.push_back(std::move(p));
        ++s.placed;
    }
    s.models = loader.loaded();
    log::info("mm1 props: {} placed from {} models, {} without data or meshes", s.placed, s.models, s.withoutData);
    if (stats)
        *stats = s;
    return out;
}

} // namespace mm2::game
