#pragma once

// OpenMM2 Enhanced: Midtown Madness 1's room component lists and
// aiMap::MapComponent, ported from Open1560's game.asm (GPL-3.0; aiMap::ReadBinary
// 59281 builds the lists, MapComponent 61132 reads them). The racers' driver
// finds the road or intersection under a car through them.

#include "city/Mm1AiMap.h"
#include "city/Mm1Rail.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace mm2::city {

// A room's component: a path (type 1) or an intersection (type 2).
struct Mm1Component {
    int id = 0;
    int type = 0;
};

class Mm1AiNav {
public:
    // `findRoom` is the city's room lookup (lvlLevel::FindRoomId; 0 = none).
    // ReadBinary's room lists: for each path in order, for i = 2..n-2, the
    // room holding the midpoint of centre segment (i-1, i) gets (path id, 1)
    // (no duplicate check); then for each intersection the room holding its
    // position gets (id, 2) unless it already has that intersection.
    Mm1AiNav(const Mm1AiMap& map, std::function<int(const Vec3&)> findRoom);

    struct Result {
        int id = -1;   // the component id, -1 none
        int type = 0;  // 0 none, 1 road, 2 intersection
        int room = 0;  // the room found (0: none was)
    };

    // aiMap::MapComponent(pos, &type, &room, hint): the room under `pos`; its
    // component with id == hint (any type); else its first intersection; else
    // its first road that has sidewalks; else none (type 0, id -1). With no room
    // under `pos` it returns `hint` and, when that is -1 and `typeIn` is 1,
    // type 0 (type is otherwise left as `typeIn`).
    Result mapComponent(const Vec3& pos, int typeIn, int hint) const;

    const std::vector<Mm1Component>& components(int room) const;
    int room(const Vec3& pos) const { return m_findRoom(pos); }

private:
    const Mm1AiMap& m_map;
    std::function<int(const Vec3&)> m_findRoom;
    std::unordered_map<int, std::vector<Mm1Component>> m_rooms;
};

// The rail's state DetermineOppMapComponent reads: the path the car is on and the
// one it turns onto (-1: none; the rail's CurLink / NextLink), and its numbers.
struct Mm1OppRail {
    int cur = -1;
    int next = -1;
    Mm1RailParams params;
};

// aiMap::DetermineOppMapComponent's arguments and results (asm 61731-64232).
struct Mm1OppInput {
    Vec3 pos;      // the car's position
    Vec3 carBack;  // the car matrix's third row
    float speed = 0.0f;
    float distToSide = 0.0f; // the car's DistToSide so far (the intersection's turn arc reads it)
    int type = 0;     // MapComponent's type in (the car's component type)
    int lastType = 0; // the component type of the previous update (a change onto a road re-points the rail)
    int pathId = -1;  // the hint: the car's current path
    bool roomIsEnclosed = false; // mmCullCity::GetRoomFlags(room) & 3 for the room MapComponent finds
};
struct Mm1OppResult {
    int id = -1;          // the asm's return value (a road's or the intersection's id)
    int type = 0;         // the component type found: 0 none, 1 road, 2 intersection
    int vertex = 2;       // vertIdx
    float roadDist = 0.0f;
    bool hasRoadDist = false;
    float distToSide = 0.0f;
    bool hasDistToSide = false; // false: the asm leaves the caller's value alone
    float targetPtOffset = 20.0f;
    int newCur = -1;      // >= 0: the rail's current path becomes this (the car came onto a road)
};
// The roads' DistToSide handling is the asm's: clamped to the road less the car's side, a
// divided road's near side kept 3 m + rSide in, and a car in the sharp-turn arc keeps the
// caller's value.
Mm1OppResult mm1DetermineOppMapComponent(const Mm1AiMap& map, const Mm1AiNav& nav, const Mm1OppRail& rail,
                                         const Mm1OppInput& in);

} // namespace mm2::city
