#pragma once

// OpenMM2 Enhanced: Midtown Madness 1's room component lists and
// aiMap::MapComponent, ported from Open1560's game.asm (GPL-3.0; aiMap::ReadBinary
// 59281 builds the lists, MapComponent 61132 reads them). The racers' driver
// finds the road or intersection under a car through them.

#include "city/Mm1AiMap.h"

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

private:
    const Mm1AiMap& m_map;
    std::function<int(const Vec3&)> m_findRoom;
    std::unordered_map<int, std::vector<Mm1Component>> m_rooms;
};

} // namespace mm2::city
