#include "asset/Mm1TexSheet.h"

#include "core/StringUtil.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace mm2::asset {
namespace {

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

template <class T>
T number(std::string_view s, int base = 10) {
    T v{};
    s = trim(s);
    std::from_chars(s.data(), s.data() + s.size(), v, base);
    return v;
}

// agiTexProp's flag letters (Open1560 texsheet.h).
std::uint32_t flagFor(char c) {
    switch (c) {
    case 'w': return Mm1TexFlag::Snowable;
    case 'g': return Mm1TexFlag::AlphaGlow;
    case 'l': return Mm1TexFlag::Lightmap;
    case 's': return Mm1TexFlag::Shadow;
    case 't': return Mm1TexFlag::Transparent;
    case 'k': return Mm1TexFlag::Chromakey;
    case 'n': return Mm1TexFlag::NotLit;
    case 'd': return Mm1TexFlag::DullOrDamaged;
    case 'u': return Mm1TexFlag::ClampUOrBoth;
    case 'v': return Mm1TexFlag::ClampVOrBoth;
    case 'c': return Mm1TexFlag::ClampBoth;
    case 'U': return Mm1TexFlag::ClampUOrNeither;
    case 'V': return Mm1TexFlag::ClampVOrNeither;
    case 'e': return Mm1TexFlag::RoadFloorCeiling;
    case 'm': return Mm1TexFlag::AlwaysModulate;
    case 'p': return Mm1TexFlag::AlwaysPerspCorrect;
    default: return 0;
    }
}

} // namespace

std::optional<Mm1TexSheet> Mm1TexSheet::parse(std::string_view text, std::string* error) {
    Mm1TexSheet sheet;
    bool header = true;
    int lineNo = 0;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        line = trim(line);
        if (line.empty())
            continue;
        if (header) { // the column names
            header = false;
            continue;
        }
        std::vector<std::string_view> f;
        for (std::string_view rest = line;;) {
            const auto comma = rest.find(',');
            f.push_back(trim(rest.substr(0, comma)));
            if (comma == std::string_view::npos)
                break;
            rest = rest.substr(comma + 1);
        }
        if (f.size() < 12) {
            setError(error, std::format("line {}: {} fields, expected 12", lineNo, f.size()));
            return std::nullopt;
        }
        Mm1TexProp p;
        p.name = upper(f[0]);
        p.neighborhood = std::string(f[1]);
        p.high = number<std::uint8_t>(f[2]);
        p.medium = number<std::uint8_t>(f[3]);
        p.low = number<std::uint8_t>(f[4]);
        for (char c : f[5]) {
            const std::uint32_t bit = flagFor(c);
            if (bit == 0)
                sheet.m_warnings.push_back(std::format("line {}: unknown flag '{}' on {}", lineNo, c, p.name));
            p.flags |= bit;
        }
        p.alternate = upper(f[6]);
        p.sibling = upper(f[7]);
        p.width = number<std::uint16_t>(f[8]);
        p.height = number<std::uint16_t>(f[9]);
        p.dayColor = number<std::uint32_t>(f[10], 16);
        p.nightColor = number<std::uint32_t>(f[11], 16);
        sheet.m_entries.push_back(std::move(p));
    }
    std::ranges::stable_sort(sheet.m_entries, {}, &Mm1TexProp::name);
    return sheet;
}

const Mm1TexProp* Mm1TexSheet::lookup(std::string_view name, int variation) const {
    std::string key = upper(name);
    for (;;) {
        const auto it = std::ranges::lower_bound(m_entries, key, {}, &Mm1TexProp::name);
        if (it == m_entries.end() || it->name != key)
            return nullptr;
        if (variation <= 0 || it->sibling.empty())
            return &*it;
        --variation;
        key = it->sibling;
    }
}

int Mm1TexSheet::variationCount(std::string_view name) const {
    int n = 0;
    std::string key = upper(name);
    // Bounded, in case a sheet's siblings form a loop.
    while (!key.empty() && n <= static_cast<int>(m_entries.size())) {
        const auto it = std::ranges::lower_bound(m_entries, key, {}, &Mm1TexProp::name);
        if (it == m_entries.end() || it->name != key)
            break;
        ++n;
        key = it->sibling;
    }
    return n;
}

std::string Mm1TexSheet::remapName(std::string_view name, int variation, bool alternate) const {
    const Mm1TexProp* p = lookup(name, variation);
    if (!p)
        return std::string(name);
    if (alternate && !p->alternate.empty())
        return p->alternate;
    return p->name;
}

std::string Mm1TexSheet::resolve(std::string_view name, int variation, bool night, Mm1TexWeather weather) const {
    std::string full = upper(name);
    if (weather == Mm1TexWeather::Rain)
        full += "_FALL";
    else if (weather == Mm1TexWeather::Snow)
        full += "_WIN";
    if (!lookup(full))
        full = upper(name);
    if (!lookup(full))
        return {};
    return remapName(full, variation, night);
}

} // namespace mm2::asset
