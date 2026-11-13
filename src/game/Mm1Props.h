#pragma once

// OpenMM2 Enhanced (not in MM2): a Midtown Madness 1 city's props
// (city/<city>.bng: street lights, traffic lights, parking meters, mail
// boxes, trees...) as MM2 bangers, standing and knocked over by MM2's prop
// physics (bangers::BangerSet).
//
// For every model the .bng names: its data from MM1's
// tune/<model>.mmbangerdata (bangers::parseMm1BangerData), its meshes
// bms/<model>/{h,m,l,vl}.bms as a model of that name in the ModelLibrary,
// and its breakable parts (mmBangerData's PartNames: meshes
// bms/<model>/<part>_<lod>.bms, data tune/<model>_<part>.mmbangerdata) as
// MM2's "BREAKnn" mesh groups and "<model>_breakNN" entries. Textures are
// resolved through MM1's texture sheet for the session's look.

#include "city/Mm1City.h"
#include "game/ModelLibrary.h"
#include "game/bangers/BangerData.h"
#include "game/bangers/PropPlacement.h"
#include "vfs/Vfs.h"

#include <vector>

namespace mm2::game {

struct Mm1PropStats {
    int placed = 0;
    int models = 0;
    int withoutData = 0; // .bng records whose model has no mmbangerdata or meshes
};

// Registers the props' data and models and returns their placements (each
// .bng record's position, facing its 'ahead' point: Angel objects face -z).
std::vector<bangers::PlacedProp> loadMm1Props(const city::Mm1City& city, const vfs::Vfs& vfs, ModelLibrary& models,
                                              bangers::BangerDataLibrary& data, bool night, bool rain,
                                              Mm1PropStats* stats = nullptr);

} // namespace mm2::game
