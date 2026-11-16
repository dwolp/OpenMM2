#include "city/Mm1AiNav.h"

#include <algorithm>

namespace mm2::city {

Mm1AiNav::Mm1AiNav(const Mm1AiMap& map, std::function<int(const Vec3&)> findRoom)
    : m_map(map), m_findRoom(std::move(findRoom)) {
    for (const Mm1AiPath& p : map.paths) {
        const std::size_t n = p.center.size();
        if (n < 4)
            continue; // the asm needs n - 1 > 2
        for (std::size_t i = 2; i + 1 < n; ++i) {
            const Vec3& a = p.center[i - 1];
            const Vec3& b = p.center[i];
            // c[i-1] - (c[i] - c[i-1]) * -0.5
            const Vec3 mid{a.x - (b.x - a.x) * -0.5f, a.y - (b.y - a.y) * -0.5f, a.z - (b.z - a.z) * -0.5f};
            const int room = m_findRoom(mid);
            if (room != 0)
                m_rooms[room].push_back({p.id, 1});
        }
    }
    for (const Mm1AiIntersection& x : map.intersections) {
        const int room = m_findRoom(x.position);
        if (room == 0)
            continue;
        auto& list = m_rooms[room];
        const bool has = std::any_of(list.begin(), list.end(),
                                     [&](const Mm1Component& c) { return c.type == 2 && c.id == x.id; });
        if (!has)
            list.push_back({x.id, 2});
    }
}

const std::vector<Mm1Component>& Mm1AiNav::components(int room) const {
    static const std::vector<Mm1Component> none;
    const auto it = m_rooms.find(room);
    return it == m_rooms.end() ? none : it->second;
}

Mm1AiNav::Result Mm1AiNav::mapComponent(const Vec3& pos, int typeIn, int hint) const {
    Result r;
    r.type = typeIn;
    const int room = m_findRoom(pos);
    if (room == 0) {
        r.id = hint;
        if (hint == -1 && typeIn == 1)
            r.type = 0;
        return r;
    }
    r.room = room;
    const auto& list = components(room);
    for (const Mm1Component& c : list)
        if (c.id == hint) {
            r.id = c.id;
            r.type = c.type;
            return r;
        }
    for (const Mm1Component& c : list)
        if (c.type == 2) {
            r.id = c.id;
            r.type = 2;
            return r;
        }
    for (const Mm1Component& c : list) {
        if (c.id >= 0 && static_cast<std::size_t>(c.id) < m_map.paths.size() &&
            m_map.paths[static_cast<std::size_t>(c.id)].numSidewalks != 0) {
            r.id = c.id;
            r.type = c.type;
            return r;
        }
    }
    r.id = -1;
    r.type = 0;
    return r;
}

} // namespace mm2::city
