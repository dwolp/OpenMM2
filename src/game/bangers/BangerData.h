/*
    OpenMM2 - breakable/knock-over props ("bangers"): their tuning data
    (MM2's dgBangerData; field structure first taken from Open1560's
    mmBangerData, Copyright (C) 2020 Brick, GPL-3.0-or-later).
*/
#pragma once

#include "game/fx/BirthRule.h"
#include "vfs/Vfs.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::game::bangers {

// tune/banger/<model>.dgBangerData. Breakable parts are separate entries
// named "<model>_break01", "<model>_break02" ... Defaults are the
// dgBangerData constructor's.
struct BangerData {
    std::string name;
    Vec3 size{0.2f, 0.5f, 0.2f}; // collision/inertia box
    Vec3 cg;             // centre of gravity above the model's ground origin; meshes are centred on it
    std::vector<Vec3> glowOffsets; // lamp glows in the CG frame (NumGlows, 1 when absent / GlowOffset)
    float mass = 50.0f;
    float elasticity = 0.5f;
    float friction = 0.9f;
    // A hit whose impulse (the one that would stop the car against an
    // immovable prop) squared is at most this leaves the prop standing like
    // a wall; a harder one breaks it loose (dgImpact::CalcImpact).
    float impulseLimit2 = 0.0f;
    float yRadius = 0.0f; // capsule/sphere radius, and the unhit prop's touch radius
    int spinAxis = 0;    // read, never used
    int flash = 0;       // read, never used
    int numParts = 0;
    int texNumber = 0;   // particle sheet texture/fxpt<N> for the debris, 0 = none
    // 0x10 no shadow pass, 0x40 (lamps; no reader), 0x80 unlit with alpha
    // reference 140, 0x100 glass, 0x200 tree (also set for "_tree" names).
    int billFlags = 0;
    // The id the prop's colliders carry (the instance data's), which
    // AudImpact plays for impacts against it.
    int colliderId = 0;
    // dgBangerData::InitBound: 0 the "<name>_bound" geometry (else a box),
    // 1 box of Size, 2 capsule of YRadius and length Size.y, 3 sphere of YRadius.
    int collisionPrim = 0;
    // dgBangerActiveManager::Update: how a knocked-over prop is simulated:
    // 0x2 without collisions, 0x40 / 0x10 colliding with everything, 0x4
    // with the city only, otherwise not at all.
    int collisionType = 0x10;
    int audioId = 0; // read, never used
    std::optional<fx::BirthRule> birthRule;

    static constexpr int kUnlit = 0x80;
    static constexpr int kTree = 0x200;
};

std::optional<BangerData> parseBangerData(std::string_view name, std::string_view text, std::string* error = nullptr);

// OpenMM2 Enhanced: Midtown Madness 1's tune/<model>.mmbangerdata, the
// format MM2's grew from. Its object headers ("mmBangerData :05e1f360 {",
// "BirthRule asBirthRule :05e1f3ec {") lose their names and addresses and its
// PartNames list is dropped (MM2 names the parts "<model>_breakNN"), then
// parseBangerData reads the rest: the fields are MM2's.
std::optional<BangerData> parseMm1BangerData(std::string_view name, std::string_view text,
                                             std::string* error = nullptr);

// Every tune/banger entry, loaded on demand.
class BangerDataLibrary {
public:
    explicit BangerDataLibrary(const vfs::Vfs& vfs);

    // Null when the model has no banger data.
    const BangerData* find(std::string_view model) const;
    // Part i (0-based) of a breakable banger: "<model>_break%02d".
    const BangerData* part(std::string_view model, int i) const;
    bool has(std::string_view model) const;
    // Multiplies an entry's Mass and ImpulseLimit2 (mmMultiCircuit::Init's
    // barricades, x 26).
    void scaleMass(std::string_view model, float factor);
    // OpenMM2 Enhanced: an entry that has no tune/banger file (MM1's props).
    void add(std::string_view model, BangerData data);
    std::size_t available() const { return m_names.size(); }
    std::vector<std::string> names() const;
    // The game data the entries come from (dgBangerData::InitBound loads the
    // "<name>_bound" geometry from it).
    const vfs::Vfs& vfs() const { return m_vfs; }

private:
    const vfs::Vfs& m_vfs;
    std::map<std::string, std::string, std::less<>> m_names; // lower-case name -> path
    mutable std::map<std::string, std::optional<BangerData>, std::less<>> m_cache;
};

} // namespace mm2::game::bangers
