#include "game/bangers/BangerData.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "data/DatFile.h"

#include <format>

namespace mm2::game::bangers {

std::optional<BangerData> parseBangerData(std::string_view name, std::string_view text, std::string* error) {
    auto file = data::parseDat(text, error);
    if (!file || !file->top()) {
        if (error && error->empty())
            *error = "empty file";
        return std::nullopt;
    }
    const data::DatNode& b = *file->top();
    BangerData d;
    d.name = str::lower(name);
    b.read("Size", d.size);
    b.read("CG", d.cg);
    b.read("Mass", d.mass);
    b.read("Elasticity", d.elasticity);
    b.read("Friction", d.friction);
    b.read("ImpulseLimit2", d.impulseLimit2);
    b.read("YRadius", d.yRadius);
    b.read("SpinAxis", d.spinAxis);
    b.read("Flash", d.flash);
    b.read("NumParts", d.numParts);
    b.read("TexNumber", d.texNumber);
    b.read("BillFlags", d.billFlags);
    b.read("ColliderId", d.colliderId);
    b.read("CollisionPrim", d.collisionPrim);
    b.read("CollisionType", d.collisionType);
    b.read("AudioId", d.audioId);
    for (const auto& c : b.children)
        if (c.name == "GlowOffset" && c.numbers.size() >= 3)
            d.glowOffsets.push_back(
                {static_cast<float>(c.numbers[0]), static_cast<float>(c.numbers[1]), static_cast<float>(c.numbers[2])});
    // dgBangerData::Load: NumGlows is optional and 1 without it.
    int numGlows = 1;
    b.read("NumGlows", numGlows);
    d.glowOffsets.resize(static_cast<std::size_t>(std::max(0, std::min<int>(numGlows, static_cast<int>(d.glowOffsets.size())))));
    if (d.name.find("_tree") != std::string::npos)
        d.billFlags |= BangerData::kTree;
    // asBirthRule::Load skips the block's name, its brace and the first
    // field's name before it reads by position, so the block counts
    // whatever it is called (sp_tree1_s_break06 calls it "asBirthRule").
    const data::DatNode* rule = nullptr;
    for (const auto& c : b.children)
        if (!c.children.empty()) {
            rule = &c;
            break;
        }
    if (rule) {
        fx::BirthRule r;
        if (fx::loadBirthRule(*rule, r)) {
            // asBirthRule::Load reads only the 24 fields dgBangerData::Save
            // writes (by position); LifeVar, Damp, DampVar, Height, Intensity
            // and Color keep the asBirthRule constructor's values. No retail
            // file has them.
            const fx::BirthRule defaults;
            r.lifeVar = defaults.lifeVar;
            r.damp = defaults.damp;
            r.dampVar = defaults.dampVar;
            r.height = defaults.height;
            r.intensity = defaults.intensity;
            r.color = defaults.color;
            d.birthRule = r;
        }
    }
    if (d.mass <= 0.0f)
        d.mass = 1.0f;
    return d;
}

// tune/banger/<name>.dgBangerData (dgBangerData::GetDirName,
// dgBangerData::GetClassName).
std::optional<BangerData> parseMm1BangerData(std::string_view name, std::string_view text, std::string* error) {
    std::string out;
    out.reserve(text.size());
    bool inList = false;
    for (std::string_view rest = text; !rest.empty();) {
        const auto nl = rest.find('\n');
        std::string line(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (inList) {
            if (line.find(']') != std::string::npos)
                inList = false;
            continue;
        }
        if (line.find("PartNames") != std::string::npos && line.find('[') != std::string::npos) {
            inList = line.find(']') == std::string::npos;
            continue;
        }
        // "<Name> [<Class>] :<address> {" -> "<Name> {".
        if (const auto colon = line.find(" :"); colon != std::string::npos && line.find('{') != std::string::npos) {
            std::string head = line.substr(0, colon);
            const auto lastSpace = head.find_last_of(' ');
            const auto firstWord = head.find_first_not_of(" \t");
            if (lastSpace != std::string::npos && lastSpace > firstWord)
                head = head.substr(0, lastSpace); // drop the class name ("asBirthRule")
            line = head + " {";
        }
        out += line;
        out += '\n';
    }
    auto d = parseBangerData(name, out, error);
    if (!d)
        return d;
    d->mm1 = true;
    // MM1's CollisionType (mmBangerActiveManager::Update) as MM2's flags:
    // 1 updated by the active itself and 2 (glass) a mover without terrain
    // or mover collisions, both without collisions here (0x2); 6 (walls) a
    // permanent mover (0x40); 4 (props) and the rest a temporary mover that
    // collides with the terrain and the movers (0x10). MM2's own 0x4 would
    // keep a knocked MM1 prop from hitting the cars.
    const int mm1Type = d->collisionType;
    d->collisionType = mm1Type == 1 || mm1Type == 2 ? 0x2 : mm1Type == 6 ? 0x40 : 0x10;
    return d;
}

BangerDataLibrary::BangerDataLibrary(const vfs::Vfs& vfs) : m_vfs(vfs) {
    constexpr std::string_view prefix = "tune/banger/", suffix = ".dgbangerdata";
    for (const auto& e : vfs.listFiles()) {
        if (!e.path.starts_with(prefix) || !e.path.ends_with(suffix))
            continue;
        const std::string stem = e.path.substr(prefix.size(), e.path.size() - prefix.size() - suffix.size());
        if (stem.empty() || stem[0] == '.' || stem.find('/') != std::string::npos)
            continue; // CVS leftovers ("tune/banger/.#giz_...")
        m_names.emplace(stem, e.path);
    }
}

bool BangerDataLibrary::has(std::string_view model) const { return m_names.contains(str::lower(model)); }

std::vector<std::string> BangerDataLibrary::names() const {
    std::vector<std::string> out;
    for (const auto& [n, p] : m_names)
        out.push_back(n);
    return out;
}

const BangerData* BangerDataLibrary::find(std::string_view modelIn) const {
    const std::string model = str::lower(modelIn);
    if (auto it = m_cache.find(model); it != m_cache.end())
        return it->second ? &*it->second : nullptr;
    std::optional<BangerData> data;
    if (auto it = m_names.find(model); it != m_names.end()) {
        if (auto bytes = m_vfs.readAll(it->second)) {
            std::string error;
            data = parseBangerData(model, std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
                                   &error);
            if (!data)
                log::warn("bangers: {}: {}", it->second, error);
        }
    }
    auto& slot = m_cache[model] = std::move(data);
    return slot ? &*slot : nullptr;
}

void BangerDataLibrary::scaleMass(std::string_view model, float factor) {
    // dgBangerDataManager::AddBangerDataEntry, then Mass (+0x48) and
    // ImpulseLimit2 (+0x54) multiplied in place.
    if (find(model)) {
        auto& data = *m_cache[str::lower(model)];
        data.mass = data.mass * factor;
        data.impulseLimit2 = data.impulseLimit2 * factor;
    }
}

void BangerDataLibrary::add(std::string_view model, BangerData data) {
    const std::string key = str::lower(model);
    m_names.emplace(key, std::string());
    m_cache[key] = std::move(data);
}

const BangerData* BangerDataLibrary::part(std::string_view model, int i) const {
    return find(std::format("{}_break{:02}", str::lower(model), i + 1));
}

} // namespace mm2::game::bangers
