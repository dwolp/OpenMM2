#include "city/CityData.h"
#include "city/Mm1AiMap.h"

#include "city/Reader.h"
#include "city/RoomInfo.h"
#include "city/SdlCollect.h"
#include "core/StringUtil.h"
#include "vfs/Mm1Source.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <mutex>

namespace mm2::city {
namespace {

std::optional<std::vector<std::byte>> read(const vfs::Vfs& v, const std::string& path) {
    return v.readAll(path);
}

std::string text(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

std::vector<CityInfo> listCities(const vfs::Vfs& v) {
    std::vector<CityInfo> out;
    for (const auto& e : v.listFiles()) {
        if (!e.path.starts_with("tune/") || !e.path.ends_with(".cinfo") ||
            e.path.find('/', 5) != std::string::npos)
            continue;
        if (auto bytes = read(v, e.path)) {
            auto info = parseCityInfo(text(*bytes));
            if (!info.mapName.empty())
                out.push_back(std::move(info));
        }
    }
    // OpenMM2 Enhanced: MM1's cities, when MM1's data is mounted (MM1 has
    // no .cinfo; the name is the map's, capitalised).
    const std::string mm1City = std::string(vfs::kMm1Prefix) + "city/";
    for (const auto& e : v.listFiles()) {
        if (!e.path.starts_with(mm1City) || !e.path.ends_with(".cells") ||
            e.path.find('/', mm1City.size()) != std::string::npos)
            continue;
        const std::string map = e.path.substr(mm1City.size(), e.path.size() - mm1City.size() - 6);
        if (std::ranges::any_of(out, [&](const CityInfo& i) { return str::iequals(i.mapName, map); }))
            continue;
        // MM1's own tune/<map>.cinfo (in UI.AR, MM2's format: the names and
        // race counts), else the map's name.
        CityInfo info;
        if (auto bytes = read(v, std::string(vfs::kMm1Prefix) + "tune/" + map + ".cinfo"))
            info = parseCityInfo(text(*bytes));
        if (info.mapName.empty())
            info.mapName = map;
        if (info.raceDir.empty())
            info.raceDir = map;
        if (info.localizedName.empty()) {
            info.localizedName = map;
            if (!info.localizedName.empty())
                info.localizedName[0] =
                    static_cast<char>(std::toupper(static_cast<unsigned char>(info.localizedName[0])));
        }
        info.mm1 = true;
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, {}, &CityInfo::mapName);
    return out;
}

std::vector<RaceDefinition> listRaces(const vfs::Vfs& v, const CityInfo& info) {
    std::vector<RaceDefinition> out;
    const std::string dir = raceFolder(info);
    auto existing = [&](const std::string& path) { return v.exists(path) ? path : std::string(); };

    struct ModeSpec {
        RaceMode mode;
        int count;
        const std::vector<std::string>* names;
        const char* table;
    };
    // Crash courses have no count or names in the .cinfo; they are numbered
    // from 0 while crash<N>data.csv exists.
    int crashCount = 0;
    while (v.exists(std::format("{}crash{}data.csv", dir, crashCount)))
        ++crashCount;
    const ModeSpec modes[] = {
        {RaceMode::Blitz, info.blitzCount, &info.blitzNames, "mmblitzdata.csv"},
        {RaceMode::Circuit, info.circuitCount, &info.circuitNames, "mmcircuitdata.csv"},
        {RaceMode::Checkpoint, info.checkpointCount, &info.checkpointNames, "mmracedata.csv"},
        {RaceMode::CrashCourse, crashCount, nullptr, "mmcrashdata.csv"},
    };
    for (const auto& m : modes) {
        std::vector<RaceTableEntry> table;
        if (auto bytes = read(v, dir + m.table))
            if (auto t = parseRaceTable(text(*bytes)))
                table = std::move(*t);
        const char* prefix = raceModePrefix(m.mode);
        for (int i = 0; i < m.count; ++i) {
            RaceDefinition r;
            r.mode = m.mode;
            r.index = i;
            if (m.names && i < static_cast<int>(m.names->size()))
                r.name = (*m.names)[static_cast<std::size_t>(i)];
            else if (i < static_cast<int>(table.size()))
                r.name = table[static_cast<std::size_t>(i)].description;
            if (i < static_cast<int>(table.size()))
                r.settings = table[static_cast<std::size_t>(i)];
            const std::string stem = std::format("{}{}{}", dir, prefix, i);
            r.waypoints = existing(stem + "waypoints.csv");
            r.aiMap = existing(stem + ".aimap");
            r.aiMapPro = existing(stem + ".aimap_p");
            r.pathSet = existing(stem + ".pathset");
            if (m.mode == RaceMode::CrashCourse) {
                r.crashEvents = existing(stem + "data.csv");
                r.crashEventsPro = existing(stem + "data_p.csv");
            }
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::optional<CityData> loadCity(const vfs::Vfs& v, std::string_view city, std::string* error) {
    CityData c;
    bool found = false;
    for (auto& info : listCities(v)) {
        if (str::iequals(info.mapName, city) || str::iequals(info.localizedName, city)) {
            c.info = std::move(info);
            found = true;
            break;
        }
    }
    if (!found) {
        // No .cinfo: fall back to treating the name as a map name.
        c.info.mapName = std::string(city);
        c.info.raceDir = std::string(city);
        c.warnings.push_back(std::format("no tune/*.cinfo for '{}'", city));
    }
    const std::string map = str::lower(c.info.mapName);
    const std::string cityDir = "city/" + map;
    const std::string raceDir = raceFolder(c.info);

    auto warn = [&](std::string s) { c.warnings.push_back(std::move(s)); };
    auto bytesOf = [&](const std::string& path, bool required) -> std::optional<std::vector<std::byte>> {
        auto b = read(v, path);
        if (!b && !required)
            warn(std::format("{}: missing", path));
        return b;
    };

    if (!v.exists(cityDir + ".psdl") && hasMm1City(v, map)) {
        // OpenMM2 Enhanced: an MM1 city. Its streets are MM1's cells; the
        // PSDL stays empty but for the bounds and sphere the game reads.
        std::string mm1Error;
        auto mm1 = loadMm1City(v, map, &mm1Error);
        if (!mm1) {
            if (error)
                *error = mm1Error;
            return std::nullopt;
        }
        for (const auto& w : mm1->warnings)
            c.warnings.push_back(w);
        if (mm1->extent) {
            const auto& e = *mm1->extent;
            c.psdl.bounds.expand(Vec3{e[0], -20.0f, e[1]});
            c.psdl.bounds.expand(Vec3{e[2], 200.0f, e[3]});
            c.psdl.sphereCenter = {(e[0] + e[2]) * 0.5f, 0.0f, (e[1] + e[3]) * 0.5f};
            c.psdl.sphereRadius = 0.5f * std::hypot(e[2] - e[0], e[3] - e[1]);
        }
        // MM1's lights and fog as MM2's 16 tables (time * 4 + weather; MM2's
        // weathers are clear, cloudy, fog and rain, MM1's clear, fog, rain and
        // snow: cloudy takes MM1's clear). MM1's fog starts at the camera.
        if (!mm1->lighting.empty()) {
            c.fog.assign(static_cast<std::size_t>(kTimesOfDay * kWeathers), FogDef{});
            for (int t = 0; t < kTimesOfDay; ++t)
                for (int w = 0; w < kWeathers; ++w) {
                    const int mm1Weather = w == 0 || w == 1 ? 0 : (w == 2 ? 1 : 2);
                    const Mm1Lighting* row = nullptr;
                    for (const auto& l : mm1->lighting)
                        if (l.timeOfDay == t && l.weather == mm1Weather)
                            row = &l;
                    if (!row)
                        continue;
                    const auto index = static_cast<std::size_t>(lightingIndex(t, w));
                    LightingDef lt;
                    lt.name = std::format("mm1-{}-{}", t, mm1Weather);
                    // MM1's headings and pitches point toward the light
                    // (pitch up from the horizon: the sun rises at heading
                    // 0, toward +x and the lake, and sets at pi); MM2's are
                    // the way the light travels (pitch negative, down).
                    constexpr float kHalfTurn = 3.14159265f;
                    lt.keyHeading = row->sunHeading + kHalfTurn;
                    lt.keyPitch = -row->sunPitch;
                    lt.keyColor = row->sunColor;
                    lt.fill1Heading = row->fill1Heading + kHalfTurn;
                    lt.fill1Pitch = -row->fill1Pitch;
                    lt.fill1Color = row->fill1Color;
                    lt.fill2Heading = row->fill2Heading + kHalfTurn;
                    lt.fill2Pitch = -row->fill2Pitch;
                    lt.fill2Color = row->fill2Color;
                    auto channel = [](float v) {
                        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    };
                    lt.ambient = 0xFF000000u | channel(row->ambient.x) << 16 | channel(row->ambient.y) << 8 |
                                 channel(row->ambient.z);
                    c.lighting[index] = lt;
                    c.ambientBeforeLoad[index] = lt.ambient;
                    FogDef& fog = c.fog[index];
                    fog.r = row->fogColor[0];
                    fog.g = row->fogColor[1];
                    fog.b = row->fogColor[2];
                    fog.start = 0.0f;
                    fog.end = row->fogEnd;
                    fog.description = lt.name;
                }
        }
        // MM1's road network, converted to MM2's (city/<map>/<map>.bai).
        if (auto b = read(v, std::format("{}city/{}/{}.bai", vfs::kMm1Prefix, map, map))) {
            std::string baiError;
            if (auto bai = parseMm1Bai(*b, &baiError)) {
                c.aiMap = mm1ToAiMap(*bai);
                // Each intersection's cell, from the footprints just above
                // its centre (MM2 keeps the PSDL room there).
                if (mm1->cellLocator)
                    for (auto& n : c.aiMap->intersections)
                        n.room = static_cast<std::uint16_t>(mm1->cellLocator->find(n.center + Vec3{0, 1, 0}));
            }
            else
                c.warnings.push_back(std::format("{}.bai (MM1): {}", map, baiError));
        }
        // MM1's cruise settings (race/<dir>/roam.aimap, MM2's format; its
        // police posts give a position and heading but none of MM2's
        // optional behaviour numbers, which then keep their defaults).
        // Inferred: MM1's [Speed Limit] is in miles per hour (30, where its
        // roads' own limits are 15 m/s and MM2's cities give 15).
        if (auto b = read(v, std::format("{}race/{}/roam.aimap", vfs::kMm1Prefix, map))) {
            std::string cfgError;
            if (auto cfg = parseAiMapConfig(text(*b), &cfgError)) {
                cfg->opponents.clear();
                adaptMm1AiMapConfig(*cfg);
                c.cruise = cfg;
                c.cruisePro = cfg;
            } else {
                c.warnings.push_back(std::format("roam.aimap (MM1): {}", cfgError));
            }
        }
        c.mm1 = std::move(*mm1);
    } else {
        const std::string path = cityDir + ".psdl";
        auto b = read(v, path);
        if (!b) {
            if (error)
                *error = std::format("{} not found", path);
            return std::nullopt;
        }
        std::string err;
        auto p = parsePsdl(*b, &err);
        if (!p) {
            if (error)
                *error = std::format("{}: {}", path, err);
            return std::nullopt;
        }
        c.psdl = std::move(*p);
    }

    std::string err;
    auto loadInst = [&](const std::string& path, std::vector<Instance>& out) {
        if (auto b = bytesOf(path, false)) {
            if (auto i = parseInst(*b, &err))
                out = std::move(*i);
            else
                warn(std::format("{}: {}", path, err));
        }
    };
    loadInst(cityDir + ".inst", c.instances);
    loadInst(cityDir + "_ai.inst", c.aiInstances);

    if (auto b = bytesOf(cityDir + ".cpvs", false)) {
        c.pvs = parseCpvs(*b, &err);
        if (!c.pvs)
            warn(std::format("{}.cpvs: {}", cityDir, err));
        else if (c.pvs->roomCount() != c.psdl.roomCount())
            warn(std::format("{}.cpvs covers {} rooms, PSDL has {}", cityDir, c.pvs->roomCount(),
                             c.psdl.roomCount()));
    }
    if (auto b = bytesOf(cityDir + ".bai", false)) {
        c.aiMap = parseBai(*b, &err);
        if (!c.aiMap)
            warn(std::format("{}.bai: {}", cityDir, err));
    }
    // aiMap::ReadBinary: the shortcut roads of <city>_sup.bai (optional).
    if (c.aiMap) {
        if (auto b = bytesOf(cityDir + "_sup.bai", false)) {
            if (auto shortcuts = parseShortcutBai(*b, &err))
                addShortcuts(*c.aiMap, std::move(*shortcuts), &c.psdl);
            else
                warn(std::format("{}_sup.bai: {}", cityDir, err));
        }
    }

    if (!c.mm1) { // an MM1 city's tables came from its tune/lighting.csv
        // LoadCityTimeWeatherLighting keeps the 16 tables for the whole
        // session and runs ComputeAmbientLightLevels before each .ltNN loads
        // into its table, so the lower light qualities' ambient levels come
        // from what the table held before: the constructor's ambient for the
        // first city of the session, the last loaded city's afterwards.
        // The tables keep whatever they held when a file is missing or
        // lacks a field (datParser::Load): the constructor's values for the
        // first city of the session, the previous city's afterwards.
        static std::mutex historyMutex;
        static std::array<LightingDef, kTimesOfDay * kWeathers> tables = [] {
            std::array<LightingDef, kTimesOfDay * kWeathers> t;
            t.fill(defaultLighting());
            return t;
        }();
        const std::lock_guard lock(historyMutex);
        for (int i = 0; i < kTimesOfDay * kWeathers; ++i) {
            auto& table = tables[static_cast<std::size_t>(i)];
            c.ambientBeforeLoad[static_cast<std::size_t>(i)] = table.ambient;
            const std::string path = std::format("{}.lt{:02}", cityDir, i);
            if (auto b = bytesOf(path, false)) {
                if (auto parsed = parseLighting(text(*b), &err, &table))
                    table = std::move(*parsed);
                else
                    warn(std::format("{}: {}", path, err));
            }
            c.lighting[static_cast<std::size_t>(i)] = table;
        }
    }
    if (auto b = bytesOf(cityDir + "_fog.csv", false)) {
        if (auto f = parseFogTable(text(*b), &err))
            c.fog = std::move(*f);
        else
            warn(std::format("{}_fog.csv: {}", cityDir, err));
    }
    if (auto b = bytesOf(cityDir + ".sky", false))
        c.sky = parseSky(text(*b));
    if (auto b = bytesOf(cityDir + ".water", false))
        c.water = parseWater(text(*b));
    if (auto b = bytesOf(cityDir + ".ext", false))
        c.extent = parseExtent(text(*b));
    if (auto b = bytesOf(cityDir + ".reset", false))
        c.resetPoints = parseResetPoints(text(*b));
    if (auto b = bytesOf(cityDir + ".lmap", false)) {
        c.roomColors = parseLightMap(*b, &err);
        if (!c.roomColors)
            warn(std::format("{}.lmap: {}", cityDir, err));
    }
    if (auto b = bytesOf("city/materials.mtl", false)) {
        if (auto m = parseMaterialLibrary(text(*b), &err))
            c.materials = std::move(*m);
        else
            warn(std::format("city/materials.mtl: {}", err));
    }
    if (auto b = bytesOf("city/materials.csv", false))
        c.textureMaterials = parseTextureMaterials(text(*b));

    // lvlRoomInfo's flags (cityLevel::Load, lvlLevel::LoadInstances).
    {
        const auto materials = sdlTextureMaterials(c.psdl, c.textureMaterials, [&](std::string_view name) {
            return sdlMaterialIndex(c.materials, name);
        });
        c.levelRoomFlags = levelRoomFlags(c.psdl, materials, c.water ? &*c.water : nullptr, c.instances,
                                          c.aiInstances, map);
    }

    auto loadAiConfig = [&](const std::string& path, std::optional<AiMapConfig>& out) {
        if (auto b = bytesOf(path, false)) {
            out = parseAiMapConfig(text(*b), &err);
            if (!out)
                warn(std::format("{}: {}", path, err));
        }
    };
    loadAiConfig(raceDir + "roam.aimap", c.cruise);
    loadAiConfig(raceDir + "roam.aimap_p", c.cruisePro);

    // City-wide gizmo path sets: race/<dir>/<map>_<thing>.pathset.
    for (const auto& e : v.listFiles()) {
        if (!e.path.starts_with(raceDir) || !e.path.ends_with(".pathset"))
            continue;
        const std::string name = e.path.substr(raceDir.size(), e.path.size() - raceDir.size() - 8);
        if (!name.starts_with(map + "_") || name.find('_', map.size() + 1) != std::string::npos)
            continue;
        if (auto b = read(v, e.path)) {
            if (auto ps = parsePathSet(*b, &err)) {
                c.cityPathSets.push_back(std::move(*ps));
                c.cityPathSetNames.push_back(name);
            } else {
                warn(std::format("{}: {}", e.path, err));
            }
        }
    }

    c.races = listRaces(v, c.info);
    if (auto b = read(v, std::format("{}{}_rewards.csv", raceDir, map)))
        c.rewards = parseRewards(text(*b));
    return c;
}

} // namespace mm2::city
