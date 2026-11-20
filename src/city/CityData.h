#pragma once

// Everything the game needs about one city, loaded from the mounted game data.

#include "city/AiMap.h"
#include "city/Environment.h"
#include "city/Inst.h"
#include "city/Mm1AiMap.h"
#include "city/Mm1City.h"
#include "city/PathSet.h"
#include "city/Psdl.h"
#include "city/Pvs.h"
#include "city/Race.h"
#include "vfs/Vfs.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::city {

// One race event and where its files live (virtual paths; files that a race
// does not have are left empty).
struct RaceDefinition {
    RaceMode mode = RaceMode::Blitz;
    int index = 0;    // N in blitzN / circuitN / raceN / crashN
    std::string name; // localized name from the .cinfo (blitz/circuit/checkpoint)
    std::optional<RaceTableEntry> settings;
    std::string waypoints;      // race/<dir>/<prefix>Nwaypoints.csv
    std::string aiMap;          // race/<dir>/<prefix>N.aimap
    std::string aiMapPro;       // race/<dir>/<prefix>N.aimap_p
    std::string pathSet;        // race/<dir>/<prefix>N.pathset
    std::string crashEvents;    // race/<dir>/crashNdata.csv
    std::string crashEventsPro; // race/<dir>/crashNdata_p.csv
};

// cityTimeWeatherLighting's constructor: ambient 0xFF101010.
inline constexpr std::uint32_t kDefaultLightingAmbient = 0xFF101010u;
constexpr std::array<std::uint32_t, kTimesOfDay * kWeathers> defaultAmbients() {
    std::array<std::uint32_t, kTimesOfDay * kWeathers> a{};
    for (auto& v : a)
        v = kDefaultLightingAmbient;
    return a;
}

struct CityData {
    CityInfo info;
    // MM2 cities' streets. For an MM1 city (mm1 set) it has no rooms; only
    // its bounds and sphere are filled, from the MM1 city's extent.
    Psdl psdl;
    // OpenMM2 Enhanced: a Midtown Madness 1 city (Chicago), loaded from the
    // MM1 files under vfs::kMm1Prefix when the city has no PSDL. Not in MM2.
    std::optional<Mm1City> mm1;
    std::vector<Instance> instances;   // city/<map>.inst
    std::vector<Instance> aiInstances; // city/<map>_ai.inst (signs, signals)
    std::optional<RoomPvs> pvs;        // city/<map>.cpvs
    std::optional<AiMap> aiMap;        // city/<map>.bai
    // OpenMM2 Enhanced: an MM1 city's own road network (MM1's .bai; converted for the AI, and the collision's
    // elevated-road guard).
    std::shared_ptr<const Mm1AiMap> mm1Ai;
    std::array<std::optional<LightingDef>, kTimesOfDay * kWeathers> lighting;
    // The ambient each lighting table held before this city's .ltNN files
    // loaded (see loadCity): what MM2 derives the lower light qualities'
    // ambient levels from.
    std::array<std::uint32_t, kTimesOfDay * kWeathers> ambientBeforeLoad = defaultAmbients();
    std::vector<FogDef> fog;
    // OpenMM2 Enhanced: an MM1 city's snow (MM1's weather 3, which MM2's 16
    // tables have no place for), one per time of day; empty otherwise.
    std::array<std::optional<LightingDef>, kTimesOfDay> snowLighting;
    std::array<FogDef, kTimesOfDay> snowFog;
    std::optional<SkyDef> sky;
    std::optional<WaterDef> water;
    std::optional<MapExtent> extent;
    std::vector<ResetPoint> resetPoints;
    std::optional<std::vector<std::uint32_t>> roomColors; // city/<map>.lmap
    std::vector<PhysMaterial> materials;                  // city/materials.mtl
    std::vector<TextureMaterial> textureMaterials;        // city/materials.csv
    // The game's own room flags (lvlRoomInfo, see RoomInfo.h), by room id.
    std::vector<std::uint16_t> levelRoomFlags;
    std::optional<AiMapConfig> cruise, cruisePro;         // race/<dir>/roam.aimap(_p)
    std::vector<PathSet> cityPathSets;                    // race/<dir>/<map>_*.pathset (bridges, ferries...)
    std::vector<std::string> cityPathSetNames;
    std::vector<RaceDefinition> races;
    std::vector<RaceReward> rewards;
    // Non-fatal problems (missing optional files, parse errors in extras).
    std::vector<std::string> warnings;
};

// All cities defined by tune/*.cinfo, in name order.
std::vector<CityInfo> listCities(const vfs::Vfs& vfs);

// Race events of a city, with settings from mm<mode>data.csv.
std::vector<RaceDefinition> listRaces(const vfs::Vfs& vfs, const CityInfo& info);

// Loads a city by map name ("london", "sf") or localized name. Missing
// optional files produce warnings; a missing or corrupt PSDL is an error.
std::optional<CityData> loadCity(const vfs::Vfs& vfs, std::string_view city, std::string* error = nullptr);

} // namespace mm2::city
