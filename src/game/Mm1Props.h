#pragma once

// OpenMM2 Enhanced (not in MM2): a Midtown Madness 1 city's props
// (city/<city>.bng: street lights, stop signs, parking meters, mail
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

#include "asset/Mm1TexSheet.h"
#include "asset/VehicleModel.h"
#include "city/Mm1City.h"
#include "game/ModelLibrary.h"
#include "game/bangers/BangerData.h"
#include "game/bangers/PropPlacement.h"
#include "vfs/Vfs.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mm2::game {

struct Mm1PropStats {
    int placed = 0;
    int models = 0;
    int withoutData = 0; // .bng records whose model has no mmbangerdata or meshes
};

// Registers MM1 models on demand: a model's data (tune/<model>.mmbangerdata)
// and meshes, its breakable parts, and a traffic light's six signal glows
// (bms/<model>/glow<colour><day|night>.bms as the GLOW<COLOUR><DAY|NIGHT>
// groups), with textures resolved for the session's look.
class Mm1ModelLoader {
public:
    Mm1ModelLoader(const vfs::Vfs& vfs, ModelLibrary& models, bangers::BangerDataLibrary& data, bool night,
                   asset::Mm1TexWeather weather);
    ~Mm1ModelLoader();
    Mm1ModelLoader(const Mm1ModelLoader&) = delete;
    Mm1ModelLoader& operator=(const Mm1ModelLoader&) = delete;

    // Whether `model` (any case) has data and meshes, loading it the first
    // time it is asked for.
    bool ensure(const std::string& model);
    int loaded() const; // models registered so far
    // OpenMM2 Enhanced: an MM1 traffic car (bms/<model>/: BODY, WHL0-3,
    // SHADOW, TLIGHT, SLIGHT0/1 as MM2's groups) with its wheels' pivots
    // from mm1/tune/<model>.aivehicledata (FLOff .. BROff) and each wheel
    // mesh's box; nothing when the model or its data is missing.
    std::optional<asset::VehicleModel> vehicle(const std::string& model);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Registers the props' data and models and returns their placements (each
// .bng record's position, facing its 'ahead' point: Angel objects face -z).
std::vector<bangers::PlacedProp> loadMm1Props(const city::Mm1City& city, const vfs::Vfs& vfs, ModelLibrary& models,
                                              bangers::BangerDataLibrary& data, bool night,
                                              asset::Mm1TexWeather weather,
                                              Mm1PropStats* stats = nullptr);
std::vector<bangers::PlacedProp> loadMm1Props(const city::Mm1City& city, Mm1ModelLoader& loader,
                                              Mm1PropStats* stats = nullptr);

} // namespace mm2::game
