#pragma once

// Race and AI configuration text files in race/<dir>/ and tune/*.cinfo.
// Format notes: docs/formats/race.md.

#include "core/Math.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::city {

// tune/<city>.cinfo
struct CityInfo {
    std::string localizedName; // "London"
    std::string mapName;       // city/<mapName>.psdl etc.
    std::string raceDir;       // race/<raceDir>/
    int blitzCount = 0, circuitCount = 0, checkpointCount = 0;
    std::vector<std::string> blitzNames, circuitNames, checkpointNames;
    // OpenMM2 Enhanced: a Midtown Madness 1 city (from mm1/city/<map>.cells),
    // with no races yet. Not in MM2.
    bool mm1 = false;
};
CityInfo parseCityInfo(std::string_view text);
// The city's race folder in the VFS: "race/<raceDir>/", or for an MM1 city
// (OpenMM2 Enhanced) "mm1/race/<raceDir>/".
std::string raceFolder(const CityInfo& info);

// <race>waypoints.csv and other "x,y,z,a,..." point lists (crash course
// events, cop-chase points). Column 5 is the checkpoint radius in race files;
// the remaining columns are editor profiling fields and are kept verbatim.
struct Waypoint {
    Vec3 position;
    float heading = 0; // degrees
    float radius = 0;
    std::vector<std::string> extra;
};
std::optional<std::vector<Waypoint>> parseWaypoints(std::string_view text, std::string* error = nullptr);

// *.opp: opponent driving line.
struct OpponentPoint {
    Vec3 position;
    float brake = 0;
    float forwardOffset = 0;
    float sideOffset = 0;
    float targetSpeed = 0;
    float speedStart = 0;
    float sideStart = 0;
};
std::optional<std::vector<OpponentPoint>> parseOpponentPath(std::string_view text,
                                                            std::string* error = nullptr);

// *.aimap / *.aimap_p (amateur / professional): traffic, police and
// opponent setup, as "[Section]" blocks. List sections start with an entry
// count; value sections hold the value lines directly.
struct AiRoadException {
    int road = 0;
    float density = 0;
    float speedLimit = 0;
};
struct AiOpponentInit {
    std::string car;      // vehicle base name, e.g. "vpcoop"
    std::string pathFile; // .opp file in the same race directory
    std::vector<float> params;
};
struct AiPoliceInit {
    std::string car; // "vpcop"
    Vec3 position;
    float heading = 0;         // degrees
    std::vector<float> params; // remaining numbers (start mode, lane, ...)
};
struct AiAmbientType {
    std::string model;    // e.g. "va_compact_s"
    float cumulative = 0; // cumulative spawn probability (0..1)
    float param = 0;
};
struct AiMapSection {
    std::string name;
    bool isList = false;
    std::vector<std::string> lines; // entries (list) or value lines
};
struct AiMapConfig {
    std::optional<float> speedLimit;
    std::optional<float> density;
    std::optional<float> copChaseDistance;
    std::optional<int> ambientLaneChanges;
    std::optional<int> driveOnLeft; // [Ambients Drive On The Left]
    std::optional<int> pedPool;     // [Ped Pool] (aiCityData)
    std::vector<AiRoadException> exceptions;
    std::vector<AiPoliceInit> police;
    std::vector<AiOpponentInit> opponents;
    std::vector<AiAmbientType> ambientTypes;
    std::vector<std::pair<std::string, std::string>> pedNames; // good / bad weather models
    std::vector<std::string> trafficLights;
    std::vector<AiMapSection> sections; // every section verbatim, in file order
};
std::optional<AiMapConfig> parseAiMapConfig(std::string_view text, std::string* error = nullptr);

// mm{blitz,circuit,race,crash}data.csv: one row per race with amateur and
// professional settings.
struct RaceSettings {
    int carType = 0;
    int timeOfDay = 0; // 0 morning, 1 noon, 2 evening, 3 night
    int weather = 0;   // 0 clear, 1 cloudy, 2 foggy, 3 rainy
    int opponents = 0;
    int cops = 0;
    float ambientDensity = 0;
    float pedDensity = 0;
    int numLaps = 0;
    float timeLimit = 0;
    float difficulty = 0;
};
struct RaceTableEntry {
    std::string description;
    RaceSettings amateur, professional;
};
std::optional<std::vector<RaceTableEntry>> parseRaceTable(std::string_view text,
                                                          std::string* error = nullptr);

// crash<N>data.csv / crash<N>data_p.csv: crash course events.
struct CrashEvent {
    std::string file; // event waypoint list, race/<dir>/<file>.csv
    int event = 0;
    int checkpoints = 0;
    float timeLimit = 0;
    float ambientDensity = 0;
    std::vector<float> extra;
};
std::optional<std::vector<CrashEvent>> parseCrashEvents(std::string_view text, std::string* error = nullptr);

// <city>_rewards.csv
struct RaceReward {
    std::string raceType; // blitz, circuit, checkpoint, ...
    std::string raceNum;  // "half", "all" or a number
    std::string car;
    int variant = 0;
    std::string message;
};
std::vector<RaceReward> parseRewards(std::string_view text);

enum class RaceMode { Blitz, Circuit, Checkpoint, CrashCourse };
const char* raceModeName(RaceMode m);
// File prefix in race/<dir>/: blitz, circuit, race, crash.
const char* raceModePrefix(RaceMode m);

} // namespace mm2::city
