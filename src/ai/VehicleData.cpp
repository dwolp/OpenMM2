#include "ai/VehicleData.h"

#include "asset/Mtx.h"
#include "core/StringUtil.h"
#include "data/DatFile.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace mm2::ai {

std::optional<VehicleData> parseMm1VehicleData(std::string_view model, std::string_view text) {
    VehicleData d;
    d.model = str::lower(model);
    bool any = false;
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        std::string clean(str::trim(line));
        std::ranges::replace(clean, '\t', ' ');
        std::vector<std::string_view> words;
        for (std::string_view w : str::split(clean, ' '))
            if (!w.empty())
                words.push_back(w);
        std::vector<float> v;
        for (std::size_t i = 1; i < words.size(); ++i)
            if (auto f = str::parseDouble(words[i]))
                v.push_back(static_cast<float>(*f));
        if (words.empty() || v.empty())
            continue;
        const std::string_view key = words[0];
        auto vec = [&] { return v.size() >= 3 ? Vec3{v[0], v[1], v[2]} : Vec3{}; };
        any = true;
        if (key == "Size") d.size = vec();
        else if (key == "Mass") d.mass = v[0];
        else if (key == "Friction") d.friction = v[0];
        else if (key == "Elasticity") d.elasticity = v[0];
        else if (key == "MaxDamage") d.maxDamage = v[0];
        else if (key == "PtxThresh") d.ptxThresh = v[0];
        else if (key == "Spring") d.spring = v[0];
        else if (key == "Damping") d.damping = v[0];
        else if (key == "SuspensionLimit") d.limit = v[0];
        else if (key == "RubberSpring") d.rubberSpring = v[0];
        else if (key == "RubberDamp") d.rubberDamp = v[0];
        else if (key == "MaxAngClamp") d.maxAng = vec();
        else if (key == "TireRadius") d.wheelRadius = v[0];
        else if (key == "FLOff") d.wheels[0] = vec();
        else if (key == "FROff") d.wheels[1] = vec();
        else if (key == "BLOff") d.wheels[2] = vec();
        else if (key == "BROff") d.wheels[3] = vec();
    }
    if (!any)
        return std::nullopt;
    d.wheelCount = 4;
    d.cg = {0.0f, d.size.y * 0.5f, 0.0f};
    return d;
}

std::optional<VehicleData> loadVehicleData(const vfs::Vfs& vfs, std::string_view model, std::string* error) {
    const std::string path = std::format("tune/vehicle/{}.aivehicledata", str::lower(model));
    auto bytes = vfs.readAll(path);
    if (!bytes) {
        // OpenMM2 Enhanced: an MM1 traffic car.
        if (auto mm1 = vfs.readAll(std::format("mm1/tune/{}.aivehicledata", str::lower(model))))
            if (auto d = parseMm1VehicleData(model, std::string_view(reinterpret_cast<const char*>(mm1->data()), mm1->size())))
                return d;
        if (error)
            *error = path + " not found";
        return std::nullopt;
    }
    auto dat =
        data::parseDat(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), error);
    if (!dat || !dat->top())
        return std::nullopt;
    const auto& n = *dat->top();
    VehicleData d;
    d.model = str::lower(model);
    n.read("Mass", d.mass);
    n.read("Size", d.size);
    n.read("MaxAng", d.maxAng);
    n.read("Elasticity", d.elasticity);
    n.read("Friction", d.friction);
    n.read("MaxDamage", d.maxDamage);
    n.read("PtxThresh", d.ptxThresh);
    n.read("Spring", d.spring);
    n.read("Damping", d.damping);
    n.read("Limit", d.limit);
    n.read("RubberSpring", d.rubberSpring);
    n.read("RubberDamp", d.rubberDamp);
    n.read("CG", d.cg);
    for (int w = 0; w < 6; ++w) {
        const auto mtx = vfs.readAll(std::format("geometry/{}_whl{}.mtx", d.model, w));
        const auto m = mtx ? asset::parseMtx(*mtx) : std::nullopt;
        if (!m)
            break;
        d.wheels[static_cast<std::size_t>(w)] = m->origin;
        d.wheelCount = w + 1;
        if (w == 0)
            d.wheelRadius = std::abs(m->max.y - m->min.y) * 0.5f;
    }
    return d;
}

} // namespace mm2::ai
