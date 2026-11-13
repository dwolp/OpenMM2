#pragma once

// Angel "DLP7" templates (dlp/<model>.dlp): the editable source of a model,
// which MM1 still reads at run time for a few named points. OpenMM2 reads
// only what it needs: the groups and the vertex positions.
//
// After Open1560 (GPL-3.0): DLPTemplate::Load, DLPGroup::Load,
// DLPPatch::Load and DLPVertex::Load (agi/dlptmpl.cpp). Everything is big
// endian. Verified on Chicago's drawbridge and crossing gate templates.

#include "core/Math.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::asset {

struct DlpGroup {
    std::string name;
    std::vector<std::uint16_t> vertices; // indices into Dlp::vertices
    std::vector<std::uint16_t> patches;  // indices into Dlp::patches
};

struct Dlp {
    std::vector<DlpGroup> groups;
    // Each patch's vertex ids (indices into `vertices`), SRes * TRes of them.
    std::vector<std::vector<std::uint16_t>> patches;
    std::vector<Vec3> vertices;

    // mmAnimDOF::Init's lookup of a named point ("PIVOT"): the group's only
    // vertex, else the first vertex of its only patch. Nothing otherwise.
    std::optional<Vec3> point(std::string_view group) const;
};

// Inferred (not Open1560's code, which reads a string differently): a
// patch's name is a u32 length and that many bytes; that is how Chicago's
// files are laid out.
std::optional<Dlp> parseDlp(std::span<const std::byte> data, std::string* error = nullptr);

} // namespace mm2::asset
