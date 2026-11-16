// Parsers for the in-game sound tables, each following the MM2 loader that
// reads the file; see AudioTables.h.
#include "audio/game/AudioTables.h"

#include "audio/TextFields.h"
#include "core/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mm2::audio::game {
namespace {

using Cells = std::vector<std::string_view>;

// One table as MM2 reads it: fgets lines, strtok cells.
struct Lines {
    std::vector<std::string_view> lines;
    explicit Lines(std::string_view text) : lines(fgetsLines(text)) {}
    std::size_t size() const { return lines.size(); }
    bool has(std::size_t i) const { return i < lines.size(); }
    Cells cells(std::size_t i) const { return has(i) ? strtokFields(lines[i]) : Cells{}; }
};

std::string text(std::string_view cell) { return std::string(cell); }
float num(const Cells& c, std::size_t i) { return crtAtof(field(c, i)); }
int inum(const Cells& c, std::size_t i) { return crtAtoi(field(c, i)); }

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

} // namespace

std::optional<std::string> readText(const vfs::Vfs& vfs, std::string_view path) {
    auto bytes = vfs.readAll(path);
    if (!bytes)
        return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// --- Cars ---------------------------------------------------------------------

std::optional<CarAudioDef> parseCarAudio(std::string_view textIn, std::string* error) {
    const Lines t(textIn);
    // vehCarAudio::Load: line 1 is a header; line 2 "horn, horn volume, flags,
    // num engine samples, clutch, clutch volume".
    CarAudioDef def;
    const Cells h = t.cells(1);
    def.horn = text(field(h, 0));
    def.hornVolume = num(h, 1);
    def.flags = static_cast<unsigned>(inum(h, 2));
    def.clutch = text(field(h, 4));
    def.clutchVolume = num(h, 5);
    // vehEngineAudio::Load: the engine header (fails the load when missing),
    // then every remaining line is an engine sample.
    if (!t.has(2)) {
        setError(error, "no engine header");
        return std::nullopt;
    }
    def.oldEngineLayout = str::iequals(field(t.cells(2), 3), "Volume Divisor");
    for (std::size_t i = 3; i < t.size(); ++i) {
        const Cells e = t.cells(i);
        EngineSampleDef s;
        s.wave = text(field(e, 0));
        if (def.oldEngineLayout) {
            // vehEngineSampleWrapper::ParseCSVBufferOld: name, min volume, max
            // volume, volume divisor, min pitch, max pitch, a value it stores
            // as a reciprocal into the pitch slope (read by nothing, as the
            // pitch range is empty), cut RPM. The pitch range (+0x34 / +0x38)
            // is left as the constructor left it, unset; OpenMM2 takes 0
            // (inferred), so the pitch is the max pitch above 0 RPM.
            s.oldLayout = true;
            s.minVolume = num(e, 1);
            s.maxVolume = num(e, 2);
            s.volumeDivisor = num(e, 3);
            s.minPitch = num(e, 4);
            s.maxPitch = num(e, 5);
            s.cutRpm = num(e, 7);
            def.engine.push_back(std::move(s));
            continue;
        }
        // vehEngineSampleWrapper::ParseCSVBuffer.
        s.minVolume = num(e, 1);
        s.maxVolume = num(e, 2);
        s.fadeInStartRpm = num(e, 3);
        s.fadeInEndRpm = num(e, 4);
        s.fadeOutStartRpm = num(e, 5);
        s.fadeOutEndRpm = num(e, 6);
        s.minPitch = num(e, 7);
        s.maxPitch = num(e, 8);
        s.pitchStartRpm = num(e, 9);
        s.pitchEndRpm = num(e, 10);
        def.engine.push_back(std::move(s));
    }
    return def;
}

// --- Impacts ------------------------------------------------------------------

const BangerSoundDef* ImpactTable::find(int id) const {
    for (const auto& b : bangers)
        if (b.id == id)
            return &b;
    return nullptr;
}

const BangerSoundDef* ImpactTable::byIndex(int index) const {
    if (bangers.empty())
        return nullptr;
    if (index < 0 || static_cast<std::size_t>(index) >= bangers.size())
        index = 0;
    return &bangers[static_cast<std::size_t>(index)];
}

std::optional<ImpactTable> parseImpactTable(std::string_view textIn, std::string* error) {
    const Lines t(textIn);
    ImpactTable table;
    auto discard = [&](const char* why) -> std::optional<ImpactTable> {
        setError(error, std::format("{} before ENDOFDATA (AudImpact::ReadCSV discards the table)", why));
        return std::nullopt;
    };
    // AudImpact::ReadCSV: "***", the banger header, "<name>,<count>,<id>".
    std::size_t i = 0;
    if (!t.has(i))
        return discard("empty file");
    while (true) {
        if (!t.has(++i))
            return discard("missing banger header");
        if (!t.has(++i))
            return discard("missing banger row");
        const Cells b = t.cells(i);
        if (field(b, 0) == "ENDOFDATA")
            break;
        BangerSoundDef banger;
        banger.name = text(field(b, 0));
        const int count = inum(b, 1);
        banger.id = inum(b, 2);
        if (count > 0) {
            // AudImpactData::ReadCSV: the sample header, then <count> rows.
            if (!t.has(++i))
                return discard("missing sample header");
            for (int k = 0; k < count; ++k) {
                if (!t.has(++i))
                    return discard("missing sample row");
                const Cells s = t.cells(i);
                ImpactSampleDef d;
                d.wave = text(field(s, 0));
                d.minVolume = num(s, 1);
                d.maxVolume = num(s, 2);
                d.minForce = num(s, 3);
                d.maxForce = num(s, 4);
                d.frequency = num(s, 5);
                banger.samples.push_back(std::move(d));
            }
        }
        table.bangers.push_back(std::move(banger));
        if (!t.has(++i))
            return discard("end of file");
    }
    return table;
}

// --- Surfaces -----------------------------------------------------------------

// vehSurfaceAudioData::ParseCSVBuffer compares the first eight bytes with
// "NOSOUND\0": exact case.
bool SurfaceSoundDef::hasSurfaceSound() const { return !wave.empty() && wave != "NOSOUND"; }

const SurfaceSoundDef* SurfaceTable::at(int index) const {
    if (surfaces.empty())
        return nullptr;
    if (index < 0 || static_cast<std::size_t>(index) >= surfaces.size())
        index = 0;
    return &surfaces[static_cast<std::size_t>(index)];
}

std::optional<SurfaceTable> parseSurfaceTable(std::string_view textIn, std::string* error) {
    const Lines t(textIn);
    if (!t.has(1)) {
        setError(error, "no tunnel index");
        return std::nullopt;
    }
    SurfaceTable table;
    // vehSurfaceAudio::LoadCSV: header, tunnel sound index.
    table.tunnelIndex = inum(t.cells(1), 0);
    // Then blocks: a header line, the surface row, the skid header and the
    // skid rows. A block cut short keeps what was read (LoadCSV has already
    // added the entry) and ends the table.
    for (std::size_t i = 2; t.has(i);) {
        table.surfaces.emplace_back();
        SurfaceSoundDef& d = table.surfaces.back();
        if (!t.has(++i))
            break;
        const Cells s = t.cells(i);
        d.wave = text(field(s, 0));
        d.maxSpeed = num(s, 1);
        d.minVolume = num(s, 2);
        d.maxVolume = num(s, 3);
        d.minPitch = num(s, 4);
        d.maxPitch = num(s, 5);
        d.minSkidVolume = num(s, 6);
        d.maxSkidVolume = num(s, 7);
        const int skids = inum(s, 8);
        if (!t.has(++i))
            break;
        bool cut = false;
        for (int k = 0; k < skids; ++k) {
            if (!t.has(++i)) {
                cut = true;
                break;
            }
            const Cells sk = t.cells(i);
            d.skids.push_back({text(field(sk, 0)), num(sk, 1), num(sk, 2)});
        }
        if (cut)
            break;
        ++i;
    }
    if (table.surfaces.empty()) {
        setError(error, "no surface entries");
        return std::nullopt;
    }
    return table;
}

// --- Sirens -------------------------------------------------------------------

std::optional<SirenTable> parseSirenTable(std::string_view textIn, std::string* error) {
    const Lines t(textIn);
    SirenTable table;
    // vehPoliceCarAudio::Load: header, explosion sample.
    table.explosion = text(field(t.cells(1), 0));
    // ReadSirenData: "Sample name" switches to sample rows, "play time" to
    // (play time, next) rows for the last sample; both compared exactly.
    int state = -1;
    for (std::size_t i = 2; i < t.size(); ++i) {
        const Cells c = t.cells(i);
        if (c.empty())
            continue;
        if (c[0] == "Sample name") {
            state = 0;
        } else if (c[0] == "play time") {
            state = 1;
        } else if (state == 0) {
            table.samples.push_back({text(c[0]), num(c, 1), {}});
        } else if (state == 1 && !table.samples.empty()) {
            table.samples.back().steps.push_back({num(c, 0), inum(c, 1)});
        }
    }
    if (table.samples.empty()) {
        setError(error, "no siren samples");
        return std::nullopt;
    }
    return table;
}

// --- Small tables -------------------------------------------------------------

std::optional<SuspensionDef> parseSuspension(std::string_view textIn) {
    const Lines t(textIn);
    if (!t.has(1))
        return std::nullopt;
    const Cells c = t.cells(1);
    SuspensionDef d;
    d.wave = text(field(c, 0));
    d.minVelocity = num(c, 1);
    d.maxVelocity = num(c, 2);
    d.minVolume = num(c, 3);
    d.maxVolume = num(c, 4);
    const float divisor = num(c, 5);
    d.volumeScale = divisor == 0.0f ? 0.0f : 1.0f / divisor;
    return d;
}

std::optional<TireWobbleDef> parseTireWobble(std::string_view textIn) {
    const Lines t(textIn);
    if (!t.has(1))
        return std::nullopt;
    const Cells c = t.cells(1);
    TireWobbleDef d;
    d.wave = text(field(c, 0));
    d.minVolume = num(c, 1);
    d.maxVolume = num(c, 2);
    d.minPitch = num(c, 3);
    d.maxPitch = num(c, 4);
    const float divisor = num(c, 5);
    d.pitchScale = divisor == 0.0f ? 0.0f : 1.0f / divisor;
    return d;
}

std::optional<SemiDef> parseSemiData(std::string_view textIn) {
    const Lines t(textIn);
    if (!t.has(1))
        return std::nullopt;
    const Cells c = t.cells(1);
    SemiDef d;
    d.reverse = text(field(c, 0));
    d.airBlow = text(field(c, 1));
    d.reverseVolume = num(c, 2);
    d.airBlowVolume = num(c, 3);
    return d;
}

// IsSemiOrBus / IsPolice compare with strcmp: exact case.
bool VehicleTypes::isFreight(std::string_view car) const {
    return std::find(freight.begin(), freight.end(), car) != freight.end();
}

bool VehicleTypes::isPolice(std::string_view car) const {
    return std::find(police.begin(), police.end(), car) != police.end();
}

VehicleTypes parseVehicleTypes(std::string_view textIn) {
    const Lines t(textIn);
    VehicleTypes types;
    // RegisterSemiNames / RegisterPoliceNames: the names on lines 2 and 4, up
    // to ENDOFDATA; line 6 "TRUE" (any case) for always-nitro.
    auto names = [&](std::size_t line, std::vector<std::string>& out) {
        for (auto cell : t.cells(line)) {
            if (cell == "ENDOFDATA")
                break;
            out.emplace_back(cell);
        }
    };
    names(1, types.freight);
    names(3, types.police);
    types.alwaysNitro = str::iequals(field(t.cells(5), 0), "TRUE");
    return types;
}

// --- Ambient traffic ------------------------------------------------------------

std::optional<AmbientEngineDef> parseAmbientEngine(std::string_view textIn) {
    const Lines t(textIn);
    if (!t.has(1))
        return std::nullopt;
    AmbientEngineDef d;
    // aiEngineAudio::ReadCSV: header; sample and volume; band header; bands.
    const Cells c = t.cells(1);
    d.wave = text(field(c, 0));
    d.volume = num(c, 1);
    for (std::size_t i = 3; i < t.size(); ++i) {
        const Cells b = t.cells(i);
        d.bands.push_back({num(b, 0), num(b, 1), num(b, 2), num(b, 3)});
    }
    return d;
}

std::optional<HornDef> parseHorn(std::string_view textIn) {
    const Lines t(textIn);
    if (!t.has(1))
        return std::nullopt;
    HornDef d;
    // vehHornAudio::ReadCSV: header; sample, volume, pitch, stuck force; then
    // "horn play duration" (any case) starts a pattern and other rows add a
    // (play, pause) beep to it.
    const Cells c = t.cells(1);
    d.wave = text(field(c, 0));
    d.volume = num(c, 1);
    d.pitch = num(c, 2);
    d.stuckImpactForce = num(c, 3);
    for (std::size_t i = 2; i < t.size(); ++i) {
        const Cells r = t.cells(i);
        if (str::iequals(field(r, 0), "horn play duration"))
            d.patterns.emplace_back();
        else if (!d.patterns.empty())
            d.patterns.back().beeps.emplace_back(num(r, 0), num(r, 1));
    }
    return d;
}

// --- City ambience ----------------------------------------------------------------

std::optional<AmbientSoundSet> parseAmbientSoundSet(std::string_view name, std::string_view textIn,
                                                    std::string* error) {
    const Lines t(textIn);
    if (!t.has(1)) {
        setError(error, "missing header");
        return std::nullopt;
    }
    AmbientSoundSet s;
    s.name = str::lower(name);
    // Aud3DAmbientObject::Load: header; min / max distance, priority, area;
    // the sample header.
    const Cells h = t.cells(1);
    s.minDistance = num(h, 0);
    s.maxDistance = num(h, 1);
    s.priority = inum(h, 2);
    s.audibleArea = inum(h, 3);
    std::size_t i = 3;
    // ReadSoundData: sample rows up to "VECTORPOINTS" (exact case).
    bool points = false;
    for (; i < t.size(); ++i) {
        const Cells e = t.cells(i);
        if (e.empty())
            continue;
        if (e[0] == "VECTORPOINTS") {
            points = true;
            break;
        }
        AmbientSampleDef d;
        d.wave = text(e[0]);
        d.volume = num(e, 1);
        d.type = static_cast<AmbientSampleType>(std::clamp(inum(e, 2), 0, 3));
        d.intervalLow = num(e, 3);
        d.intervalHigh = num(e, 4);
        d.active = inum(e, 5) != 0;
        d.minSpeed = num(e, 6);
        d.maxSpeed = num(e, 7);
        d.doppler = inum(e, 8) != 0;
        s.samples.push_back(std::move(d));
    }
    // Aud3DObject::ReadVectorPoints: skips the line after VECTORPOINTS ("x,y,z"),
    // then one point per line.
    if (points) {
        for (i += 2; i < t.size(); ++i) {
            const Cells p = t.cells(i);
            if (p.empty())
                continue;
            s.points.push_back({num(p, 0), num(p, 1), num(p, 2)});
        }
    }
    return s;
}

std::vector<std::string> parseAmbientContainer(std::string_view textIn) {
    // Aud3DAmbObjContainer::Init: a header, then a set name per line.
    const Lines t(textIn);
    std::vector<std::string> names;
    for (std::size_t i = 1; i < t.size(); ++i) {
        const Cells c = t.cells(i);
        if (!c.empty())
            names.push_back(str::lower(c[0]));
    }
    return names;
}

// --- Creature voices --------------------------------------------------------------

std::optional<CreatureVoiceDef> parseCreatureVoice(std::string_view textIn) {
    const Lines t(textIn);
    CreatureVoiceDef d;
    enum class Block { None, Avoid, Impact };
    // AudCreature::ReadCSV: the first line names the first block.
    auto blockOf = [](std::string_view cell) {
        if (str::iequals(cell, "min speed"))
            return Block::Avoid;
        if (str::iequals(cell, "min impact force"))
            return Block::Impact;
        return Block::None;
    };
    Block block = blockOf(field(t.cells(0), 0));
    std::size_t i = 1;
    while (block != Block::None) {
        // AudCreatureAvoid / AudCreatureImpact::ParseCSVBuffer: the values
        // line, the sample header, then sample rows until the next block
        // header or the end of the file.
        const Cells v = t.cells(i);
        VoiceSpeedTrigger trigger;
        if (block == Block::Avoid) {
            trigger.minSpeed = num(v, 0);
            trigger.maxSpeed = num(v, 1);
            trigger.minTimeInRange = num(v, 2);
            trigger.maxTimeOutOfRange = num(v, 3);
        } else {
            d.hasImpact = true;
            d.minImpactForce = num(v, 0);
            d.impactLines.clear();
        }
        Block next = Block::None;
        for (i += 2; i < t.size(); ++i) {
            const Cells r = t.cells(i);
            if (const Block b = blockOf(field(r, 0)); b != Block::None) {
                next = b;
                ++i;
                break;
            }
            if (block == Block::Avoid)
                trigger.lines.push_back({text(field(r, 0)), num(r, 1), 0.0f});
            else
                d.impactLines.push_back({text(field(r, 0)), num(r, 1), num(r, 2)});
        }
        if (block == Block::Avoid)
            d.triggers.push_back(std::move(trigger));
        block = next;
    }
    return d;
}

std::optional<float> parseNumFileChoices(std::string_view textIn) {
    const Lines t(textIn);
    if (!t.has(1))
        return std::nullopt;
    return num(t.cells(1), 0);
}

// --- Announcer --------------------------------------------------------------------

bool SpeechRow::header() const {
    constexpr std::string_view kHeader = "header";
    return name.size() > kHeader.size() &&
           str::iequals(std::string_view(name).substr(name.size() - kHeader.size()), kHeader);
}

bool SpeechRow::headerIs(std::string_view event) const {
    return name.size() >= event.size() && str::iequals(std::string_view(name).substr(0, event.size()), event);
}

std::string SpeechRow::eventName() const {
    // The last space before the "header" suffix ends the name.
    const std::size_t suffix = name.size() - 6;
    std::size_t space = 0;
    for (std::size_t i = 0; i < suffix; ++i)
        if (name[i] == ' ')
            space = i;
    return name.substr(0, space);
}

std::optional<SpeechTable> parseSpeechTable(std::string_view textIn) {
    // mmRaceSpeech / mmCNRSpeech / mmCCSpeech::LoadGroup: a header line, then
    // header and line-set rows "<prefix>,<end>,<add>[,<num used>]".
    const Lines t(textIn);
    if (!t.has(0))
        return std::nullopt;
    SpeechTable table;
    for (std::size_t i = 1; i < t.size(); ++i) {
        const Cells c = t.cells(i);
        if (c.empty())
            continue;
        table.rows.push_back({text(c[0]), num(c, 1), num(c, 2), num(c, 3)});
    }
    return table;
}

std::optional<AnnouncerList> parseAnnouncerList(std::string_view textIn) {
    // mmRaceSpeech::LoadCityInfo: header, count, header, prefix.
    const Lines t(textIn);
    if (!t.has(3))
        return std::nullopt;
    AnnouncerList a;
    a.count = num(t.cells(1), 0);
    a.prefix = str::lower(field(t.cells(3), 0));
    if (a.prefix.empty())
        return std::nullopt;
    return a;
}

} // namespace mm2::audio::game


namespace mm2::audio::game {
namespace {

// A line of MM1's tune files: "key value" (a value may be quoted); block lines
// ("Name Class :addr {") and braces carry no value we read.
struct Mm1Tune {
    std::vector<std::pair<std::string, std::string>> lines;
    bool has(const std::string& key) const {
        for (const auto& [k, v] : lines)
            if (k == key)
                return true;
        return false;
    }
    std::string str(const std::string& key) const {
        for (const auto& [k, v] : lines)
            if (k == key)
                return v;
        return {};
    }
    float num(const std::string& key, float def = 0.0f) const {
        const std::string v = str(key);
        if (v.empty())
            return def;
        try {
            return std::stof(v);
        } catch (...) {
            return def;
        }
    }
};

Mm1Tune readMm1Tune(std::string_view text) {
    constexpr std::string_view kBlank = " \t\r";
    Mm1Tune t;
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        std::string line(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        const auto a = line.find_first_not_of(kBlank);
        if (a == std::string::npos)
            continue;
        const auto b = line.find_first_of(kBlank, a);
        if (b == std::string::npos)
            continue;
        std::string key = line.substr(a, b - a);
        const auto v = line.find_first_not_of(kBlank, b);
        if (v == std::string::npos)
            continue;
        std::string value = line.substr(v);
        while (!value.empty() && kBlank.find(value.back()) != std::string_view::npos)
            value.pop_back();
        if (!value.empty() && value.front() == '"') {
            const auto q = value.find('"', 1);
            value = value.substr(1, q == std::string::npos ? std::string::npos : q - 1);
        }
        t.lines.emplace_back(std::move(key), std::move(value));
    }
    return t;
}

std::string mm1Wave(const std::string& name) { return name.empty() ? std::string("NOSOUND") : "mm1:" + str::lower(name); }

} // namespace

std::optional<CarAudioDef> parseMm1PlayerCarAudio(std::string_view text, std::string* error) {
    const Mm1Tune t = readMm1Tune(text);
    if (!t.has("m_pcLowRpmWave") && !t.has("m_pcHighRpmWave")) {
        if (error)
            *error = "no EngineAudio block";
        return std::nullopt;
    }
    CarAudioDef d;
    d.mm1 = true;
    d.horn = mm1Wave(t.str("m_pcHornWaveName"));
    d.hornVolume = t.num("m_fHornVolume");
    d.flags = static_cast<unsigned>(t.num("m_bFlags"));
    d.clutch = mm1Wave(t.str("m_pcClutchWave"));
    d.clutchVolume = t.num("m_fClutchVolume");
    // mmEngineAudio::UpdateRPM: the "hi" parameters drive the sample at +0x78,
    // the "low" ones the sample at +0x7C.
    const auto sample = [&](const std::string& wave, const std::string& idx, bool low) {
        EngineSampleDef e;
        e.wave = mm1Wave(wave);
        e.mm1 = true;
        e.mm1Low = low;
        e.minVolume = t.num("m_fMinVolume[" + idx + "]");
        e.maxVolume = t.num("m_fMaxVolume[" + idx + "]");
        e.mm1VolumeMultiplier = t.num("m_fVolumeMultiplier[" + idx + "]");
        e.minPitch = t.num("m_fMinPitch[" + idx + "]");
        e.maxPitch = t.num("m_fMaxPitch[" + idx + "]");
        e.mm1PitchMultiplier = t.num("m_fPitchMultiplier[" + idx + "]");
        return e;
    };
    if (!t.str("m_pcHighRpmWave").empty())
        d.engine.push_back(sample(t.str("m_pcHighRpmWave"), "kHiRpmIndex", false));
    if (!t.str("m_pcLowRpmWave").empty())
        d.engine.push_back(sample(t.str("m_pcLowRpmWave"), "kLowRpmIndex", true));
    return d;
}

std::optional<CarAudioDef> parseMm1OpponentCarAudio(std::string_view text, std::string* error) {
    const Mm1Tune t = readMm1Tune(text);
    if (!t.has("m_pcEngineWave")) {
        if (error)
            *error = "no EngineAudioOpponent block";
        return std::nullopt;
    }
    CarAudioDef d;
    d.mm1 = true;
    d.hornVolume = t.num("m_fHornVolume");
    d.flags = static_cast<unsigned>(t.num("m_bFlags"));
    EngineSampleDef e;
    e.wave = mm1Wave(t.str("m_pcEngineWave"));
    e.mm1 = true;
    e.minVolume = t.num("m_fMinVolume");
    e.maxVolume = t.num("m_fMaxVolume");
    e.mm1VolumeMultiplier = t.num("m_fVolumeMultiplier");
    e.minPitch = t.num("m_fMinPitch");
    e.maxPitch = t.num("m_fMaxPitch");
    e.mm1PitchMultiplier = t.num("m_fPitchMultiplier");
    d.engine.push_back(e);
    return d;
}

} // namespace mm2::audio::game
