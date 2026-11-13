#include "game/Mm1Props.h"

#include "asset/Bms.h"
#include "asset/Mm1TexSheet.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/TextureLibrary.h"
#include "vfs/Mm1Source.h"

#include <array>
#include <format>
#include <map>
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

std::vector<bangers::PlacedProp> loadMm1Props(const city::Mm1City& city, const vfs::Vfs& vfs, ModelLibrary& models,
                                              bangers::BangerDataLibrary& data, bool night, bool rain,
                                              Mm1PropStats* stats) {
    const std::string prefix(vfs::kMm1Prefix);
    std::optional<asset::Mm1TexSheet> sheet;
    if (auto b = vfs.readAll(prefix + "mtl/global.tsh"))
        sheet = asset::Mm1TexSheet::parse(textOf(*b));
    auto textureName = [&](const std::string& raw) {
        std::string name = sheet ? sheet->resolve(raw, 0, night,
                                                  rain ? asset::Mm1TexWeather::Rain : asset::Mm1TexWeather::Clear)
                                 : std::string();
        if (name.empty())
            name = raw;
        return std::string(kMm1TexturePrefix) + str::lower(name);
    };

    // One mesh group of a model: its levels of detail, appended to `pkg`
    // (their materials appended to the single paint job).
    auto addGroup = [&](asset::Pkg& pkg, const std::string& model, const std::string& file, const std::string& part) {
        bool any = false;
        for (const auto& [suffix, lod] : kLods) {
            const std::string path = std::format("{}bms/{}/{}{}.bms", prefix, model, file.empty() ? "" : file + "_", suffix);
            auto bytes = vfs.readAll(path);
            if (!bytes)
                continue;
            auto bms = asset::parseBms(*bytes);
            if (!bms)
                continue;
            auto converted = asset::bmsToPkgMesh(*bms);
            const auto base = static_cast<std::uint32_t>(pkg.paintjobs[0].size());
            for (auto& m : converted.materials) {
                if (!m.texture.empty())
                    m.texture = textureName(m.texture);
                pkg.paintjobs[0].push_back(std::move(m));
            }
            for (auto& s : converted.mesh.sections)
                s.shaderIndex += base;
            converted.mesh.part = part;
            converted.mesh.lod = lod;
            converted.mesh.name = part.empty() ? std::string(asset::lodSuffix(lod)) : std::format("{}_{}", part, asset::lodSuffix(lod));
            pkg.meshes.push_back(std::move(converted.mesh));
            any = true;
        }
        return any;
    };

    std::map<std::string, bool> ready; // model -> usable
    Mm1PropStats s;
    std::vector<bangers::PlacedProp> out;
    for (const auto& b : city.bangers) {
        const std::string model = str::lower(b.name);
        auto it = ready.find(model);
        if (it == ready.end()) {
            bool ok = false;
            auto bytes = vfs.readAll(std::format("{}tune/{}.mmbangerdata", prefix, model));
            std::string err;
            auto d = bytes ? bangers::parseMm1BangerData(model, textOf(*bytes), &err) : std::nullopt;
            if (bytes && !d)
                log::warn("mm1 props: tune/{}.mmbangerdata: {}", model, err);
            if (d) {
                asset::Pkg pkg;
                pkg.paintjobs.emplace_back();
                if (addGroup(pkg, model, "", "")) {
                    // The breakable parts: MM2's BREAKnn groups and entries.
                    const auto parts = partNames(textOf(*bytes));
                    for (std::size_t k = 0; k < parts.size(); ++k) {
                        const std::string mm2Part = std::format("BREAK{:02}", k + 1);
                        addGroup(pkg, model, parts[k], mm2Part);
                        if (auto pb = vfs.readAll(std::format("{}tune/{}_{}.mmbangerdata", prefix, model, parts[k])))
                            if (auto pd = bangers::parseMm1BangerData(std::format("{}_break{:02}", model, k + 1),
                                                                       textOf(*pb)))
                                data.add(std::format("{}_break{:02}", model, k + 1), std::move(*pd));
                    }
                    d->numParts = std::min<int>(d->numParts, static_cast<int>(parts.size()));
                    if (models.add(model, pkg)) {
                        data.add(model, std::move(*d));
                        ok = true;
                        ++s.models;
                    }
                }
            }
            it = ready.emplace(model, ok).first;
        }
        if (!it->second) {
            ++s.withoutData;
            continue;
        }
        // Facing the 'ahead' point (Angel objects face -z).
        Vec3 forward{b.ahead.x - b.position.x, 0.0f, b.ahead.z - b.position.z};
        const float len = forward.mag();
        forward = len > 0.0f ? forward * (1.0f / len) : Vec3{0, 0, -1};
        bangers::PlacedProp p;
        p.model = model;
        p.transform.m2 = forward * -1.0f;
        p.transform.m1 = {0.0f, 1.0f, 0.0f};
        p.transform.m0 = p.transform.m1.cross(p.transform.m2);
        p.transform.m3 = b.position;
        p.source = bangers::PlacedProp::Source::Instance;
        out.push_back(std::move(p));
        ++s.placed;
    }
    log::info("mm1 props: {} placed from {} models, {} without data or meshes", s.placed, s.models, s.withoutData);
    if (stats)
        *stats = s;
    return out;
}

} // namespace mm2::game
