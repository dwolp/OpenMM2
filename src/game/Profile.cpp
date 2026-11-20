#include "game/Profile.h"

#include "city/CityData.h"
#include "city/Race.h"
#include "core/Ini.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/StringUtil.h"
#include "game/Catalog.h"

#include <algorithm>
#include <bit>
#include <format>

namespace mm2::game {
namespace {

template <class E>
E clampEnum(long long v, E max) {
    return static_cast<E>(std::clamp<long long>(v, 0, static_cast<long long>(max)));
}

std::string_view asText(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

// MM2 keeps at most 32 races per mode (one bit each in the passed masks).
constexpr int kMaxRaces = 32;

RaceMask bit(int i) { return i >= 0 && i < kMaxRaces ? RaceMask{1} << i : RaceMask{0}; }

// Driver names are kept as typed (mmInterface::PlayerCreate), and IniFile
// trims values but strips one pair of surrounding quotes, so names and the
// values that start with one are written quoted.
std::string quotedValue(std::string_view v) { return std::format("\"{}\"", v); }

// MM2 names its two cities in the progress code: mmInterface::CitySetupCB
// gates races only in "sf" and "london", mmInterface::PlayerResolveCars locks
// the rewards of those two, and mmInterface::PlayerFillStats adds up their
// scores. Other cities behave as if they had no progress rules.
constexpr std::string_view kProgressCities[] = {"sf", "london"};

bool isProgressCity(std::string_view name) {
    for (const auto c : kProgressCities)
        if (str::iequals(c, name))
            return true;
    return false;
}

// Merges a finish into a record (MM2 `mmPlayerCityRecord::NewRecord`): a
// record without a time (none yet) takes the result as it is; afterwards the
// lower time is kept with the car that set it (an equal time keeps the old
// car), the higher score, and the passed flag once set. `passed` also stands
// for MM2's per-city passed mask, which NewRecord only ever sets, so it
// survives the overwrite too.
void merge(RaceRecord& r, bool fresh, const RaceRecord& n) {
    if (fresh || r.time == 0.0f) {
        const bool wasPassed = !fresh && r.passed;
        r = n;
        r.passed = r.passed || wasPassed;
        return;
    }
    if (n.time < r.time) {
        r.time = n.time;
        r.vehicle = n.vehicle;
    }
    r.score = std::max(r.score, n.score);
    r.passed = r.passed || n.passed;
}

// dgGameModeNames up to the '%' (mmRewardList::Load compares the reward's
// race type with them, case-insensitively).
bool isGameModeName(std::string_view type) {
    for (const std::string_view name : {"roam", "race", "multicop", "circuit", "blitz", "croam", "crash"})
        if (str::iequals(name, type))
            return true;
    return false;
}

} // namespace

const char* modeKey(GameMode mode) {
    switch (mode) {
    case GameMode::Cruise: return "cruise";
    case GameMode::Blitz: return "blitz";
    case GameMode::Checkpoint: return "race";
    case GameMode::Circuit: return "circuit";
    case GameMode::CrashCourse: return "crash";
    case GameMode::CopsAndRobbers: return "cops";
    }
    return "cruise";
}

// --- Profile ---------------------------------------------------------------------

std::string Profile::raceKey(std::string_view cityName, std::string_view mode, int index) {
    return std::format("{}.{}.{}", str::lower(cityName), mode, index);
}

const RaceRecord* Profile::record(std::string_view cityName, std::string_view modeName, int index) const {
    const auto it = races.find(raceKey(cityName, modeName, index));
    return it == races.end() ? nullptr : &it->second;
}

bool Profile::load(const std::filesystem::path& path) {
    IniFile ini;
    if (!ini.load(path))
        return false;
    *this = Profile{};
    file = path;
    name = ini.getString("Driver", "Name");
    if (name.empty())
        return false;
    netName = ini.getString("Driver", "NetName", name);
    address = ini.getString("Driver", "Address");
    order = static_cast<int>(ini.getInt("Driver", "Order", 0));

    vehicle = ini.getString("Prefs", "Vehicle", vehicle);
    vehicleColor = static_cast<int>(ini.getInt("Prefs", "Color", 0));
    automatic = ini.getBool("Prefs", "Automatic", true);
    difficulty = clampEnum(ini.getInt("Prefs", "Difficulty", 0), Difficulty::Professional);
    city = ini.getString("Prefs", "City", city);
    mode = clampEnum(ini.getInt("Prefs", "Mode", 0), GameMode::CopsAndRobbers);
    raceIndex = static_cast<int>(ini.getInt("Prefs", "Race", 0));
    // mmPlayerConfig::SetViewSettings; mmPlayer::Reset indexes its three car
    // cameras with the camera number.
    camera = static_cast<int>(std::clamp<long long>(ini.getInt("Prefs", "Camera", 0), 0, 2));
    wideAngle = ini.getBool("Prefs", "WideAngle", false);
    dashboard = ini.getBool("Prefs", "Dashboard", false);
    mirror = ini.getBool("Prefs", "Mirror", false);

    for (const auto& key : ini.keys("Races")) {
        const std::string value = ini.getString("Races", key); // split() returns views into it
        const auto parts = str::split(value, ',');
        std::string k = str::lower(key);
        const auto dots = std::ranges::count(k, '.');
        if (dots == 3) {
            // Earlier OpenMM2 files: <difficulty>.<city>.<mode>.<index> =
            // <bestPosition>,<bestTime>,<wins>. A win was a pass.
            k = k.substr(k.find('.') + 1);
            RaceRecord r;
            if (parts.size() > 1)
                r.time = static_cast<float>(str::parseDouble(parts[1]).value_or(0.0));
            r.passed = parts.size() > 2 && str::parseInt(parts[2]).value_or(0) > 0;
            const bool fresh = !races.contains(k);
            merge(races[k], fresh, r);
            continue;
        }
        RaceRecord r;
        if (!parts.empty())
            r.time = static_cast<float>(str::parseDouble(parts[0]).value_or(0.0));
        if (parts.size() > 1)
            r.vehicle = std::string(str::trim(parts[1]));
        if (parts.size() > 2)
            r.score = static_cast<int>(str::parseInt(parts[2]).value_or(0));
        if (parts.size() > 3)
            r.passed = str::parseInt(parts[3]).value_or(0) != 0;
        races[k] = r;
    }
    // Earlier OpenMM2 files: [Crash] <city>.<lesson> = passed | failed.
    for (const auto& key : ini.keys("Crash")) {
        const std::string k = str::lower(key);
        const auto dot = k.find('.');
        if (dot == std::string::npos)
            continue;
        RaceRecord r{1.0f, {}, 1, str::lower(ini.getString("Crash", key)) == "passed"};
        races[std::format("{}.crash.{}", k.substr(0, dot), k.substr(dot + 1))] = r;
    }
    return true;
}

bool Profile::save() const {
    IniFile ini;
    ini.parse("; OpenMM2 driver profile\n");
    ini.set("Driver", "Name", quotedValue(name));
    ini.set("Driver", "NetName", quotedValue(netName));
    if (!address.empty())
        ini.set("Driver", "Address", quotedValue(address));
    ini.setInt("Driver", "Order", order);
    ini.set("Prefs", "Vehicle", vehicle);
    ini.setInt("Prefs", "Color", vehicleColor);
    ini.setBool("Prefs", "Automatic", automatic);
    ini.setInt("Prefs", "Difficulty", static_cast<int>(difficulty));
    ini.set("Prefs", "City", city);
    ini.setInt("Prefs", "Mode", static_cast<int>(mode));
    ini.setInt("Prefs", "Race", raceIndex);
    ini.setInt("Prefs", "Camera", camera);
    ini.setBool("Prefs", "WideAngle", wideAngle);
    ini.setBool("Prefs", "Dashboard", dashboard);
    ini.setBool("Prefs", "Mirror", mirror);
    // The times in full (MM2's mmPlayerRecord keeps the float), so later
    // finishes compare against the time that was driven.
    for (const auto& [key, r] : races)
        ini.set("Races", key, std::format("{},{},{},{}", r.time, r.vehicle, r.score, r.passed ? 1 : 0));
    return ini.save(file);
}

// --- ProfileStore ------------------------------------------------------------------

ProfileStore::ProfileStore(std::filesystem::path dir) : m_dir(std::move(dir)) {}

std::filesystem::path ProfileStore::defaultDir() { return paths::userDataDir() / "players"; }

std::vector<Profile> ProfileStore::list() const {
    std::vector<Profile> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (!str::iequals(str::fromPath(p.extension()), ".ini") || str::iequals(str::fromPath(p.filename()), "players.ini"))
            continue;
        Profile prof;
        if (prof.load(p))
            out.push_back(std::move(prof));
    }
    // Creation order; profiles from earlier versions (order 0) by name.
    std::ranges::sort(out, [](const Profile& a, const Profile& b) {
        if (a.order != b.order)
            return a.order < b.order;
        return str::lower(a.name) < str::lower(b.name);
    });
    return out;
}

std::optional<Profile> ProfileStore::create(std::string_view rawName, CreateError* error) {
    // mmInterface::PlayerCreate takes the name as typed and refuses only an
    // empty one (a name of spaces is a driver); it is stored quoted.
    const std::string name(rawName);
    auto fail = [&](CreateError e) -> std::optional<Profile> {
        if (error)
            *error = e;
        return std::nullopt;
    };
    if (error)
        *error = CreateError::None;
    if (name.empty())
        return fail(CreateError::EmptyName);
    const auto existing = list();
    if (static_cast<int>(existing.size()) >= kMaxDrivers)
        return fail(CreateError::TooMany);
    int order = 0;
    for (const auto& p : existing) {
        if (p.name == name)
            return fail(CreateError::Duplicate);
        order = std::max(order, p.order);
    }
    // File name: the name reduced to safe characters, made unique.
    std::string base;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            base.push_back(str::toLower(c));
        else if (c == ' ' && !base.empty() && base.back() != '_')
            base.push_back('_');
    }
    if (base.empty())
        base = "driver";
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    std::filesystem::path file = m_dir / (base + ".ini");
    for (int n = 2; std::filesystem::exists(file, ec); ++n)
        file = m_dir / std::format("{}_{}.ini", base, n);
    Profile p;
    p.name = name;
    p.netName = name;
    p.file = file;
    p.order = order + 1;
    if (!p.save())
        return fail(CreateError::CannotSave);
    return p;
}

bool ProfileStore::remove(const Profile& p) {
    std::error_code ec;
    return !p.file.empty() && std::filesystem::remove(p.file, ec);
}

std::string ProfileStore::lastUsed() const {
    IniFile ini;
    ini.load(m_dir / "players.ini");
    return ini.getString("Players", "Last");
}

void ProfileStore::setLastUsed(std::string_view name) {
    IniFile ini;
    ini.load(m_dir / "players.ini");
    ini.set("Players", "Last", quotedValue(name));
    ini.save(m_dir / "players.ini");
}

// --- HallOfFame -------------------------------------------------------------------------

std::string HallOfFame::key(Difficulty d, std::string_view cityName, std::string_view mode, int index) {
    return std::format("{}.{}.{}.{}", d == Difficulty::Professional ? "pro" : "amateur", str::lower(cityName), mode,
                       index);
}

void HallOfFame::submit(Difficulty d, std::string_view cityName, std::string_view mode, int index,
                        const HallEntry& e) {
    Table& t = m_tables[key(d, cityName, mode, index)];
    auto insert = [](std::array<HallEntry, kEntries>& list, std::size_t at, const HallEntry& entry) {
        for (std::size_t i = list.size() - 1; i > at; --i)
            list[i] = list[i - 1];
        list[at] = entry;
    };
    for (std::size_t i = 0; i < t.byTime.size(); ++i) {
        if (t.byTime[i].time == 0.0f || t.byTime[i].time > e.time) {
            insert(t.byTime, i, e);
            break;
        }
    }
    for (std::size_t i = 0; i < t.byScore.size(); ++i) {
        if (t.byScore[i].score < e.score) {
            insert(t.byScore, i, e);
            break;
        }
    }
}

const HallOfFame::Table* HallOfFame::table(Difficulty d, std::string_view cityName, std::string_view mode,
                                           int index) const {
    const auto it = m_tables.find(key(d, cityName, mode, index));
    return it == m_tables.end() ? nullptr : &it->second;
}

bool HallOfFame::load(const std::filesystem::path& path) {
    m_tables.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return true;
    // Sections are not enumerable through IniFile, so they are found by name.
    IniFile ini;
    if (!ini.load(path))
        return false;
    auto parse = [&](const std::string& section, const std::string& k) {
        HallEntry e;
        const std::string v = ini.getString(section, k);
        const auto parts = str::split(v, '|');
        if (parts.size() >= 4) {
            e.driver = std::string(parts[0]);
            e.vehicle = std::string(parts[1]);
            e.time = static_cast<float>(str::parseDouble(parts[2]).value_or(0.0));
            e.score = static_cast<int>(str::parseInt(parts[3]).value_or(0));
            e.passed = parts.size() >= 5 && str::parseInt(parts[4]).value_or(0) != 0;
        }
        return e;
    };
    for (const auto& k : ini.keys("Index")) {
        const std::string section = str::lower(k);
        Table& t = m_tables[section];
        for (int i = 0; i < kEntries; ++i) {
            t.byTime[static_cast<std::size_t>(i)] = parse(section, std::format("time{}", i));
            t.byScore[static_cast<std::size_t>(i)] = parse(section, std::format("score{}", i));
        }
    }
    return true;
}

bool HallOfFame::save(const std::filesystem::path& path) const {
    IniFile ini;
    ini.parse("; OpenMM2 race records (hall of fame)\n");
    // Times are written in full (MM2 stores the float), so a reloaded record
    // compares as the original did.
    auto format = [](const HallEntry& e) {
        return quotedValue(std::format("{}|{}|{}|{}|{}", e.driver, e.vehicle, e.time, e.score, e.passed ? 1 : 0));
    };
    for (const auto& [section, t] : m_tables) {
        ini.set("Index", section, "1");
        for (int i = 0; i < kEntries; ++i) {
            const auto& te = t.byTime[static_cast<std::size_t>(i)];
            const auto& se = t.byScore[static_cast<std::size_t>(i)];
            if (te.time > 0.0f)
                ini.set(section, std::format("time{}", i), format(te));
            if (se.score > 0)
                ini.set(section, std::format("score{}", i), format(se));
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return ini.save(path);
}

// --- Progress --------------------------------------------------------------------------

Progress::Progress(std::vector<CityProgressInfo> cities, std::vector<Reward> rewards)
    : m_cities(std::move(cities)), m_rewards(std::move(rewards)) {}

Progress Progress::load(const vfs::Vfs& vfs) {
    const Catalog vehicles = Catalog::load(vfs);
    std::vector<CityProgressInfo> cities;
    std::vector<Reward> rewards;
    for (const auto& info : city::listCities(vfs)) {
        CityProgressInfo c;
        c.name = str::lower(info.mapName);
        c.blitzCount = info.blitzCount;
        c.circuitCount = info.circuitCount;
        c.checkpointCount = info.checkpointCount;
        const std::string dir = city::raceFolder(info);
        // mmRewardList::Load: a row is kept only when its race type is one of
        // dgGameModeNames (up to the '%') and its car is in the vehicle list
        // (mmVehList::GetVehicleID); at most 32 kept rows; the message ends
        // at the next comma.
        if (auto bytes = vfs.readAll(std::format("{}{}_rewards.csv", dir, c.name))) {
            int rows = 0;
            for (const auto& r : city::parseRewards(asText(*bytes))) {
                if (!isGameModeName(r.raceType) || !vehicles.vehicle(r.car)) {
                    log::warn("progress: {} reward row for {} {} skipped", c.name, r.raceType, r.car);
                    continue;
                }
                if (++rows > 32)
                    break;
                const std::string message = r.message.substr(0, r.message.find(','));
                rewards.push_back({c.name, str::lower(r.raceType), str::lower(r.raceNum), str::lower(r.car), r.variant,
                                   std::string(str::trim(message))});
            }
        }
        log::info("progress: {}: {} blitz, {} circuit, {} checkpoint races, {} lessons", c.name, c.blitzCount, c.circuitCount,
                  c.checkpointCount, c.crashCount);
        cities.push_back(std::move(c));
    }
    log::info("progress: {} cities, {} rewards", cities.size(), rewards.size());
    return Progress(std::move(cities), std::move(rewards));
}

const CityProgressInfo* Progress::city(std::string_view name) const {
    for (const auto& c : m_cities)
        if (str::iequals(c.name, name))
            return &c;
    return nullptr;
}

int Progress::raceCount(const CityProgressInfo& c, std::string_view mode) const {
    if (mode == "blitz")
        return c.blitzCount;
    if (mode == "circuit")
        return c.circuitCount;
    if (mode == "race")
        return c.checkpointCount;
    if (mode == "crash")
        return c.crashCount;
    return 0;
}

RaceMask Progress::passedMask(const Profile& p, std::string_view cityName, std::string_view mode) const {
    RaceMask m = 0;
    for (int i = 0; i < kMaxRaces; ++i)
        if (const RaceRecord* r = p.record(cityName, mode, i); r && r->passed)
            m |= bit(i);
    return m;
}

int Progress::passedCount(const Profile& p, std::string_view cityName, std::string_view mode) const {
    return std::popcount(passedMask(p, cityName, mode));
}

RaceMask Progress::openMask(const Profile* p, std::string_view cityName, std::string_view mode) const {
    // mmInterface::CitySetupCB: without a driver, or in a city other than
    // San Francisco and London, every mask is all ones.
    if (!p || !city(cityName) || !isProgressCity(cityName))
        return ~RaceMask{0};
    const RaceMask passed = passedMask(*p, cityName, mode);
    auto all = [&](RaceMask bits) { return (passed & bits) == bits; };
    if (mode == "race") {
        // mmPlayerData::ResolveCheckpointProgress: groups of three (the
        // menu's mask, RaceMenu::SetProgressMask).
        RaceMask open = 0x7;
        if (all(0x7))
            open |= 0x38;
        if (all(0x38))
            open |= 0x1c0;
        if (all(0x1c0))
            open |= 0xe00;
        return open;
    }
    if (mode == "crash") {
        // mmPlayerData::ResolveCrashProgress: lessons 0-2, 4-6 and 8-10 are
        // open; midterms 3, 7 and 11 follow their lessons; the final (12)
        // needs everything before it.
        if (all(0xfff))
            return ~RaceMask{0};
        RaceMask open = 0x777;
        if (all(0x7))
            open |= 0x8;
        if (all(0x70))
            open |= 0x80;
        if (all(0x700))
            open |= 0x800;
        return open;
    }
    // Blitz and circuit races are never locked: RaceMenuBase::RaceMenuBase
    // sets their masks to all ones.
    return ~RaceMask{0};
}

bool Progress::raceOpen(const Profile* p, std::string_view cityName, std::string_view mode, int index) const {
    return (openMask(p, cityName, mode) & bit(index)) != 0;
}

bool Progress::rewardMet(const Profile& p, const Reward& r) const {
    const CityProgressInfo* c = city(r.city);
    if (!c)
        return false;
    const RaceMask passed = passedMask(p, r.city, r.raceType);
    const int count = raceCount(*c, r.raceType);
    if (r.raceNum == "half")
        return std::popcount(passed) >= count / 2;
    if (r.raceNum == "all")
        return std::popcount(passed) >= count;
    const auto index = str::parseInt(r.raceNum);
    return index && (passed & bit(static_cast<int>(*index))) != 0;
}

bool Progress::vehicleUnlocked(const Profile& p, std::string_view vehicle) const {
    return variantUnlocked(p, vehicle, 0);
}

bool Progress::variantUnlocked(const Profile& p, std::string_view vehicle, int variant) const {
    // mmInterface::PlayerResolveCars clears every lock, then
    // mmRewardList::UnlockPlayerRewards locks what the rows of San Francisco
    // and London still ask for.
    for (const auto& r : m_rewards)
        if (r.variant == variant && str::iequals(r.vehicle, vehicle) && isProgressCity(r.city) &&
            !rewardMet(p, r))
            return false;
    return true;
}

bool Progress::recordable(const RaceConfig& played, const RaceConfig& defaults) {
    if (played.multiplayer || played.raceIndex < 0)
        return false;
    // The checks of the modes' RegisterFinish: time of day, weather, cops and
    // traffic must be the race's defaults; circuits also check laps and
    // opponents.
    const bool environment = played.timeOfDay == defaults.timeOfDay && played.weather == defaults.weather &&
                             played.copDensity == defaults.copDensity &&
                             played.trafficDensity == defaults.trafficDensity;
    switch (played.mode) {
    case GameMode::CrashCourse: return played.raceIndex < 13;
    case GameMode::Blitz: return played.raceIndex < 12 && environment;
    case GameMode::Checkpoint: return environment;
    case GameMode::Circuit:
        return environment && played.laps == defaults.laps && played.opponents == defaults.opponents;
    case GameMode::Cruise:
    case GameMode::CopsAndRobbers: return false;
    }
    return false;
}

std::optional<Reward> Progress::record(Profile& p, const RaceResult& result) const {
    const RaceConfig& cfg = result.config;
    const bool crash = cfg.mode == GameMode::CrashCourse;
    const bool race = cfg.mode == GameMode::Blitz || cfg.mode == GameMode::Circuit || cfg.mode == GameMode::Checkpoint;
    // Races are recorded at the finish line (also when lost); a lesson when it
    // is passed or failed. Quitting records nothing.
    if (cfg.multiplayer || cfg.raceIndex < 0 || cfg.raceIndex >= kMaxRaces || !(crash ? result.ended : race && result.finished))
        return std::nullopt;

    const std::string mode = modeKey(cfg.mode);
    std::vector<const Reward*> lockedBefore;
    for (const auto& r : m_rewards)
        if (r.raceType == mode && str::iequals(r.city, cfg.city) && !variantUnlocked(p, r.vehicle, r.variant))
            lockedBefore.push_back(&r);

    RaceRecord n;
    if (crash) {
        n.time = 1.0f;
        n.score = 1;
    } else {
        n.time = cfg.mode == GameMode::Circuit && result.bestLapSeconds > 0.0f ? result.bestLapSeconds
                                                                               : result.timeSeconds;
        n.score = std::max(0, result.score);
    }
    n.vehicle = cfg.vehicle;
    n.passed = result.won;
    const std::string key = Profile::raceKey(cfg.city, mode, cfg.raceIndex);
    const bool fresh = !p.races.contains(key);
    merge(p.races[key], fresh, n);

    // mmRewardList::CheckReward: the first row of this mode, in table order,
    // that is now met and whose vehicle or paint job was locked.
    for (const Reward* r : lockedBefore)
        if (rewardMet(p, *r))
            return *r;
    return std::nullopt;
}

int Progress::totalScore(const Profile& p, std::string_view cityName) const {
    // mmPlayerData::GetTotalScore: the records of the city's checkpoint
    // races, blitzes and circuits, up to each mode's race count.
    const CityProgressInfo* c = city(cityName);
    if (!c)
        return 0;
    int total = 0;
    for (const char* mode : {"race", "blitz", "circuit"}) {
        const int count = std::min(raceCount(*c, mode), kMaxRaces);
        for (int i = 0; i < count; ++i)
            if (const RaceRecord* r = p.record(cityName, mode, i))
                total += r->score;
    }
    return total;
}

int Progress::totalScore(const Profile& p) const {
    // mmInterface::PlayerFillStats: London plus San Francisco.
    int total = 0;
    for (const auto c : kProgressCities)
        total += totalScore(p, c);
    return total;
}

} // namespace mm2::game
