#pragma once

// OpenMM2 Enhanced (not in MM2): a Midtown Madness 1 city's drawbridges
// (city/<city>.gizmo), raised and lowered as MM1 does it.
//
// After MM1's mmBridgeMgr / mmBridgeSet / mmAnimDOF (Open1560's game.asm;
// GPL-3.0): each set (two leaves and their four crossing gates) waits down
// for DownInterval seconds, switches its parts up, waits up for UpInterval
// seconds, and switches them down again; a triggered set first waits until
// the player comes near (races). Each part runs a progress 0..1 at its
// Delta per second once its delay has passed, eased by (1 - cos(t pi)) / 2
// to an angle between its off and on goals, and turns its hinge frame by it
// (leaves about their X axis, gates about their Z axis). The tuning is
// tune/<mode>.mmbridgemgr (city::parseMm1BridgeTuning) over MM1's defaults
// and, for races, the triggers mmBridgeMgr::Init sets up.
//
// The leaves and gates are MM2 gizmo bodies (GizmoBody, MM2's moving
// bangers) so the cars collide with them where they are; the leaves collide
// with MM1's own bounds (bnd/<model>_bnd.bnd). Each body is listed in every
// cell its deck or arm covers. The gate posts (tpcrossbase) are ordinary
// props (gateBases). The sets sound with MM2's drawbridge sound (MM1's own
// bridge audio is not ported). Not ported: the gates breaking. The traffic
// keeps off the bridges (city::markMm1BridgePaths).

#include "city/Mm1City.h"
#include "game/CityLevel.h"
#include "game/RaceConfig.h"
#include "game/bangers/BangerData.h"
#include "game/bangers/BangerSet.h"
#include "game/bangers/PropPlacement.h"
#include "audio/game/Ambience.h"
#include "game/world/Gizmos.h"
#include "vfs/Vfs.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mm2::game::world {

class Mm1Bridges final : public InstanceSource {
public:
    struct Options {
        GameMode mode = GameMode::Cruise;
        int raceIndex = 0;
        bool multiplayer = false;
    };

    // mmAnimDOF.
    struct Part {
        enum class Kind { Leaf, Gate };
        // +0x8c: 0 off, 1 turning on, 2 on, 3 turning off.
        enum class State { Off = 0, TurningOn = 1, On = 2, TurningOff = 3 };
        Kind kind = Kind::Leaf;
        std::string model;
        Mat34 base;          // +0x18: the hinge's frame (at the DLP's PIVOT)
        Vec3 bodyOffset;     // the banger data's CG less the pivot, in the part's frame
        State state = State::Off;
        float delay = 0.0f;  // +0x84: seconds waited before moving
        float t = 0.0f;      // +0x88: progress, 0 off .. 1 on
        Mat34 matrix;        // +0x48: the frame it draws at
        std::unique_ptr<GizmoBody> body;
        std::vector<int> rooms; // the cells it covers
    };
    // mmBridgeSet.
    struct Set {
        std::string name;
        city::Mm1BridgeSetTuning tuning;
        std::vector<Part> parts;
        int switchOn = 0;     // +0x3f0
        int state = 0;        // +0x3f4: 0 down, 1 up, 2 moving, 3 waiting for its trigger
        float downTimer = 0.0f; // +0x404
        float upTimer = 0.0f;   // +0x408
        Vec3 centre;            // between the leaves' hinges (the sound's place)
        // OpenMM2: MM2's drawbridge sound (MM1's mmBridgeAudMgr is not
        // ported), playing while the set moves.
        std::unique_ptr<audio::game::BridgeAudio> audio;
        bool sounding = false;
    };

    // The tuning file MM1 loads for a session (mmBridgeMgr::Init names the
    // manager "roam", "race<N>", "circuit<N>", "blitz<N>" or
    // "copsnrobbers", with "m" appended in a network game).
    static std::string tuningName(const Options& options);
    // mmBridgeMgr::Init's tuning: the constructor's defaults; a network
    // game's sets up for good, cruise's up first; a race's sets triggered
    // within four times the leaves' distance of their midpoint and opening
    // to 0.1 pi; then tune/<name>.mmbridgemgr over that when it exists.
    static city::Mm1BridgeTuning tuningFor(const city::Mm1City& city, const vfs::Vfs& vfs, const Options& options);

    // The banger data of every part's model must be registered in `data`
    // (game::Mm1ModelLoader) first; parts without it are left out.
    Mm1Bridges(const city::Mm1City& city, const vfs::Vfs& vfs, const bangers::BangerDataLibrary& data,
               const bangers::BangerSet& bounds, const phys::Level* level, const Options& options);
    ~Mm1Bridges() override;

    // mmBridgeSet::Reset for every set.
    void reset();
    // mmBridgeSet::Update for every set; `player` is the trigger's only
    // tracked object (mmBridgeMgr's Inst).
    void update(float dt, const std::optional<Vec3>& player);

    // The sets' sounds (MM2's "drawbridge", as gizBridge's), and their
    // update with the listener after update().
    void loadAudio(const vfs::Vfs& vfs, audio::SoundBank& bank, audio::Mixer& mixer,
                   audio::game::Object3DManager* manager);
    void updateAudio(const Mat34& listener, float dt, bool inTunnel);
    void stopAudio();

    // InstanceSource: the leaves and gates covering `room`.
    void instancesIn(int room, std::vector<phys::Instance*>& out) const override;

    const std::vector<Set>& sets() const { return m_sets; }
    const city::Mm1BridgeTuning& tuning() const { return m_tuning; }
    // The crossing gates' posts (mmAnimDOF::Init's tpcrossbase), as props.
    std::vector<bangers::PlacedProp> gateBases() const;

    // Exposed for tests.
    static void updatePart(Part& p, const city::Mm1BridgeTuning& t, float dt);
    static void setSwitch(Part& p, bool on);
    static void setState(Part& p, bool on);
    static void place(Part& p, const city::Mm1BridgeTuning& t);

private:
    void updateSet(Set& s, float dt, const std::optional<Vec3>& player);

    city::Mm1BridgeTuning m_tuning;
    std::vector<Set> m_sets;
    std::vector<std::vector<GizmoBody*>> m_rooms;
    std::vector<bangers::PlacedProp> m_gateBases;
    float m_time = 0.0f; // seconds since the reset (the triggers' timeout)
};

} // namespace mm2::game::world
