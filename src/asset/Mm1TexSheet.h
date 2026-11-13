#pragma once

// Midtown Madness 1's texture sheet (mtl/global.tsh): every texture's
// properties, its night alternate and its sibling (the next variation).
// MM2 never reads it; OpenMM2 reads it for the Chicago port. See
// docs/formats/mm1city.md.
//
// After Open1560 (GPL-3.0): agiTexProp (agiworld/texsheet.h), and
// agiTexSheet::Load / Lookup / RemapName / GetVariationCount and
// GetPackedTexture (game.asm, agiworld/texsort.cpp).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::asset {

// agiTexProp::Flags, from the sheet's flag letters.
namespace Mm1TexFlag {
inline constexpr std::uint32_t Snowable = 0x1;            // w
inline constexpr std::uint32_t AlphaGlow = 0x2;           // g
inline constexpr std::uint32_t Lightmap = 0x4;            // l
inline constexpr std::uint32_t Shadow = 0x8;              // s
inline constexpr std::uint32_t Transparent = 0x10;        // t
inline constexpr std::uint32_t Chromakey = 0x20;          // k
inline constexpr std::uint32_t NotLit = 0x40;             // n
inline constexpr std::uint32_t DullOrDamaged = 0x80;      // d
inline constexpr std::uint32_t ClampUOrBoth = 0x100;      // u
inline constexpr std::uint32_t ClampVOrBoth = 0x200;      // v
inline constexpr std::uint32_t ClampBoth = 0x400;         // c
inline constexpr std::uint32_t ClampUOrNeither = 0x800;   // U
inline constexpr std::uint32_t ClampVOrNeither = 0x1000;  // V
inline constexpr std::uint32_t RoadFloorCeiling = 0x2000; // e
inline constexpr std::uint32_t AlwaysModulate = 0x4000;   // m
inline constexpr std::uint32_t AlwaysPerspCorrect = 0x8000; // p
} // namespace Mm1TexFlag

// MM1's weather (mmWeather) as it picks textures: GetPackedTexture appends
// "_FALL" in rain and "_WIN" in snow when the sheet has such an entry
// (mmCullCity::Init sets TextureSuffix).
enum class Mm1TexWeather { Clear, Rain, Snow };

struct Mm1TexProp {
    std::string name;      // upper case, as agiTexSheet::Load stores it
    std::string neighborhood;
    std::uint8_t high = 0, medium = 0, low = 0; // pack shifts per texture quality
    std::uint32_t flags = 0;                    // Mm1TexFlag
    std::string alternate;                      // night texture, upper case; empty = none
    std::string sibling;                        // next variation, upper case; empty = none
    std::uint16_t width = 0, height = 0;
    std::uint32_t dayColor = 0, nightColor = 0; // ARGB
};

class Mm1TexSheet {
public:
    // Parses global.tsh (CSV with a header line). Unknown flag letters are
    // reported in `warnings` and ignored, as MM1's Errorf does.
    static std::optional<Mm1TexSheet> parse(std::string_view text, std::string* error = nullptr);

    // agiTexSheet::Lookup(name, variation): the entry for `name`, then
    // `variation` steps along the sibling chain, stopping early at its end.
    // Null when `name`, or a sibling on the way, is not in the sheet.
    const Mm1TexProp* lookup(std::string_view name, int variation = 0) const;
    // agiTexSheet::GetVariationCount: the length of the sibling chain from `name`.
    int variationCount(std::string_view name) const;
    // agiTexSheet::RemapName: the looked-up entry's name, or its alternate
    // when `alternate` (MM1: sunset and night) and it has one; `name`
    // itself when not in the sheet.
    std::string remapName(std::string_view name, int variation, bool alternate) const;
    // GetPackedTexture's name: `name` with the weather suffix when the sheet
    // has that entry, then remapped. Empty when neither is in the sheet
    // (MM1 quits there).
    std::string resolve(std::string_view name, int variation, bool night, Mm1TexWeather weather) const;

    const std::vector<Mm1TexProp>& entries() const { return m_entries; }
    const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    std::vector<Mm1TexProp> m_entries; // sorted by name (Load sorts with qsort; Lookup bsearches)
    std::vector<std::string> m_warnings;
};

// The texture folders MM1's hardware renderer searches, in order (Open1560
// agigl/glpipe.cpp TexSearchPath): "tex16a", "tex16o", "tex16".
inline constexpr std::string_view kMm1TexSearchPath[] = {"tex16a", "tex16o", "tex16"};

} // namespace mm2::asset
