#pragma once

// Midtown Madness 1 data, for the OpenMM2 Enhanced fork's MM1 cities
// (Chicago). MM2 has nothing like it.

#include "vfs/Vfs.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace mm2::vfs {

// Where MM1's files appear in the VFS: "mm1/city/chicago.cells" and so on.
inline constexpr std::string_view kMm1Prefix = "mm1/";

// Mounts MM1's CORE.AR (an AngelRes archive, see AresArchive) under
// kMm1Prefix, below every other mount, and UI.AR beside it when present.
// `path` is an MM1 disc image, a folder holding CORE.AR (a disc copy or an
// installation), or CORE.AR itself.
bool mountMm1Source(Vfs& vfs, const std::filesystem::path& path, std::string* error = nullptr);

} // namespace mm2::vfs
