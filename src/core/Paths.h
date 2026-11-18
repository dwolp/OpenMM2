#pragma once

#include <filesystem>

namespace mm2::paths {

// Directory containing the running executable.
std::filesystem::path executableDir();

// Per-user configuration directory (created on demand).
//   Windows: %APPDATA%\OpenMM2
//   Linux:   $XDG_CONFIG_HOME/openmm2 (default ~/.config/openmm2)
std::filesystem::path userConfigDir();

// Per-user data directory for saves, imported game data and logs (created on demand).
//   Windows: %LOCALAPPDATA%\OpenMM2
//   Linux:   $XDG_DATA_HOME/openmm2 (default ~/.local/share/openmm2)
std::filesystem::path userDataDir();

// When "portable.txt" exists next to the executable, config and data live
// next to the executable instead (USB-stick installs, development builds).
bool isPortable();

// In a portable install, the first start with no settings of its own copies the per-user ones over (openmm2.ini and
// the players folder: driver profiles and the hall of fame), so the move to portable keeps them. The game data and the
// disc images are not copied: the settings hold their paths. Returns whether anything was copied.
bool migrateLegacyUserData();

} // namespace mm2::paths
