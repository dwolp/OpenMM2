#include "asset/Mm1Bound.h"

#include "asset/Reader.h"

#include <bit>
#include <cstring>
#include <format>

namespace mm2::asset {
namespace {

constexpr std::uint32_t kMagic = 0x424E4432u; // "2DNB"
constexpr std::size_t kHeaderSize = 0x70;
constexpr std::size_t kPolygonSize = 0x4C;
constexpr std::size_t kPhysicsRecord = 88;

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

} // namespace

std::optional<Mm1Bound> parseMm1Bound(std::span<const std::byte> data, std::string* error) {
    detail::Reader r(data);
    if (r.u32() != kMagic) {
        setError(error, "not an MM1 bound (no 2DNB magic)");
        return std::nullopt;
    }
    // mmBoundTemplate::Load: an offset vector, the grid's dimensions, then
    // Center, Radius, RadiusSqr, BBMin, BBMax, NumVerts, NumPolys,
    // NumHotVerts1, NumHotVerts2, NumEdges, XScale, ZScale, a count,
    // HeightScale and another count (the grid tables' sizes).
    Mm1Bound b;
    b.offset = r.vec3();
    for (auto& g : b.grid)
        g = static_cast<int>(r.u32());
    b.centre = r.vec3();
    b.radius = r.f32();
    r.skip(4); // RadiusSqr
    b.box.min = r.vec3();
    b.box.max = r.vec3();
    const std::uint32_t vertexCount = r.u32();
    const std::uint32_t polygonCount = r.u32();
    r.seek(kHeaderSize);
    if (!r.ok() || vertexCount > (1u << 20) || polygonCount > (1u << 20) ||
        kHeaderSize + std::size_t{vertexCount} * 12 + std::size_t{polygonCount} * kPolygonSize > data.size()) {
        setError(error, std::format("corrupt bound header ({} vertices, {} polygons)", vertexCount, polygonCount));
        return std::nullopt;
    }
    b.vertices.resize(vertexCount);
    for (auto& v : b.vertices)
        v = r.vec3();
    b.polygons.resize(polygonCount);
    for (auto& p : b.polygons) {
        const std::size_t start = r.pos();
        p.room = r.u16();
        p.material = r.u8();
        p.flags = r.u8();
        for (auto& v : p.vertices)
            v = static_cast<std::int16_t>(r.u16());
        r.skip(48); // PlaneEdges
        p.normal = r.vec3();
        p.d = r.f32();
        r.seek(start + kPolygonSize);
        const int n = p.quad() ? 4 : 3;
        for (int i = 0; i < n; ++i)
            if (p.vertices[static_cast<std::size_t>(i)] < 0 ||
                static_cast<std::uint32_t>(p.vertices[static_cast<std::size_t>(i)]) >= vertexCount) {
                setError(error, std::format("polygon refers to vertex {} of {}", p.vertices[static_cast<std::size_t>(i)],
                                            vertexCount));
                return std::nullopt;
            }
    }
    if (!r.ok()) {
        setError(error, "truncated bound");
        return std::nullopt;
    }
    return b;
}

std::optional<std::vector<Mm1PhysicsMaterial>> parseMm1PhysicsDb(std::span<const std::byte> data,
                                                                 std::string* error) {
    // Big-endian throughout: a u32 count, then the records.
    auto be32 = [&](std::size_t off) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | std::to_integer<std::uint32_t>(data[off + static_cast<std::size_t>(i)]);
        return v;
    };
    if (data.size() < 4) {
        setError(error, "file too small");
        return std::nullopt;
    }
    const std::uint32_t count = be32(0);
    if (count > 4096 || 4 + std::size_t{count} * kPhysicsRecord > data.size()) {
        setError(error, std::format("bad material count {}", count));
        return std::nullopt;
    }
    std::vector<Mm1PhysicsMaterial> out(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t base = 4 + std::size_t{i} * kPhysicsRecord;
        const char* name = reinterpret_cast<const char*>(data.data() + base);
        out[i].name.assign(name, strnlen(name, 32));
        for (std::size_t k = 0; k < out[i].values.size(); ++k)
            out[i].values[k] = std::bit_cast<float>(be32(base + 32 + 4 * k));
    }
    return out;
}

} // namespace mm2::asset
