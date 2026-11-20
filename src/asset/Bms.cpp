#include "asset/Bms.h"

#include "asset/Reader.h"

#include <algorithm>
#include <array>
#include <format>

namespace mm2::asset {
namespace {

constexpr std::uint32_t kMagic = 0x4D534833u; // "3HSM"
constexpr std::size_t kTexParamsSize = 0x30;   // agiTexParameters
// Generous limits for corrupt files; MM1's largest city meshes are a few
// thousand vertices.
constexpr std::uint32_t kMaxCount = 1u << 20;

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

// UnpackNormal from Open1560 code/midtown/agiworld/packnorm.cpp
// (Copyright (C) 2020 Brick, GPL-3.0), unchanged.
// clang-format off
constexpr std::array<Vec3, 198> kNormals{{
    { 0.0000f,  0.0000f,  1.0000f }, { 1.0000f,  0.0000f,  0.0000f }, { 0.0000f,  1.0000f,  0.0000f }, {-1.0000f,  0.0000f,  0.0000f }, { 0.0000f, -1.0000f,  0.0000f }, { 0.0000f,  0.0000f, -1.0000f },
    { 0.2225f,  0.0000f,  0.9749f }, { 0.4339f,  0.0000f,  0.9010f }, { 0.6235f,  0.0000f,  0.7818f }, { 0.7818f,  0.0000f,  0.6235f }, { 0.9010f,  0.0000f,  0.4339f }, { 0.9749f,  0.0000f,  0.2225f },
    { 0.0000f,  0.2225f,  0.9749f }, { 0.0000f,  0.4339f,  0.9010f }, { 0.0000f,  0.6235f,  0.7818f }, { 0.0000f,  0.7818f,  0.6235f }, { 0.0000f,  0.9010f,  0.4339f }, { 0.0000f,  0.9749f,  0.2225f },
    {-0.2225f,  0.0000f,  0.9749f }, {-0.4339f,  0.0000f,  0.9010f }, {-0.6235f,  0.0000f,  0.7818f }, {-0.7818f,  0.0000f,  0.6235f }, {-0.9010f,  0.0000f,  0.4339f }, {-0.9749f,  0.0000f,  0.2225f },
    { 0.0000f, -0.2225f,  0.9749f }, { 0.0000f, -0.4339f,  0.9010f }, { 0.0000f, -0.6235f,  0.7818f }, { 0.0000f, -0.7818f,  0.6235f }, { 0.0000f, -0.9010f,  0.4339f }, { 0.0000f, -0.9749f,  0.2225f },
    { 0.2225f,  0.0000f, -0.9749f }, { 0.4339f,  0.0000f, -0.9010f }, { 0.6235f,  0.0000f, -0.7818f }, { 0.7818f,  0.0000f, -0.6235f }, { 0.9010f,  0.0000f, -0.4339f }, { 0.9749f,  0.0000f, -0.2225f },
    { 0.0000f,  0.2225f, -0.9749f }, { 0.0000f,  0.4339f, -0.9010f }, { 0.0000f,  0.6235f, -0.7818f }, { 0.0000f,  0.7818f, -0.6235f }, { 0.0000f,  0.9010f, -0.4339f }, { 0.0000f,  0.9749f, -0.2225f },
    {-0.2225f,  0.0000f, -0.9749f }, {-0.4339f,  0.0000f, -0.9010f }, {-0.6235f,  0.0000f, -0.7818f }, {-0.7818f,  0.0000f, -0.6235f }, {-0.9010f,  0.0000f, -0.4339f }, {-0.9749f,  0.0000f, -0.2225f },
    { 0.0000f, -0.2225f, -0.9749f }, { 0.0000f, -0.4339f, -0.9010f }, { 0.0000f, -0.6235f, -0.7818f }, { 0.0000f, -0.7818f, -0.6235f }, { 0.0000f, -0.9010f, -0.4339f }, { 0.0000f, -0.9749f, -0.2225f },
    { 0.9749f,  0.2225f,  0.0000f }, { 0.9010f,  0.4339f,  0.0000f }, { 0.7818f,  0.6235f,  0.0000f }, { 0.6235f,  0.7818f,  0.0000f }, { 0.4339f,  0.9010f,  0.0000f }, { 0.2225f,  0.9749f,  0.0000f },
    {-0.2225f,  0.9749f,  0.0000f }, {-0.4339f,  0.9010f,  0.0000f }, {-0.6235f,  0.7818f,  0.0000f }, {-0.7818f,  0.6235f,  0.0000f }, {-0.9010f,  0.4339f,  0.0000f }, {-0.9749f,  0.2225f,  0.0000f },
    {-0.9749f, -0.2225f,  0.0000f }, {-0.9010f, -0.4339f,  0.0000f }, {-0.7818f, -0.6235f,  0.0000f }, {-0.6235f, -0.7818f,  0.0000f }, {-0.4339f, -0.9010f,  0.0000f }, {-0.2225f, -0.9749f,  0.0000f },
    { 0.2225f, -0.9749f,  0.0000f }, { 0.4339f, -0.9010f,  0.0000f }, { 0.6235f, -0.7818f,  0.0000f }, { 0.7818f, -0.6235f,  0.0000f }, { 0.9010f, -0.4339f,  0.0000f }, { 0.9749f, -0.2225f,  0.0000f },
    { 0.2279f,  0.2279f,  0.9466f }, { 0.4505f,  0.2361f,  0.8610f }, { 0.2361f,  0.4505f,  0.8610f }, { 0.6533f,  0.2450f,  0.7164f }, { 0.4691f,  0.4691f,  0.7482f }, { 0.2450f,  0.6533f,  0.7164f },
    { 0.8197f,  0.2502f,  0.5153f }, { 0.6762f,  0.4815f,  0.5575f }, { 0.4815f,  0.6762f,  0.5575f }, { 0.2502f,  0.8197f,  0.5153f }, { 0.9316f,  0.2448f,  0.2685f }, { 0.8288f,  0.4740f,  0.2974f },
    { 0.6729f,  0.6729f,  0.3072f }, { 0.4740f,  0.8288f,  0.2974f }, { 0.2448f,  0.9316f,  0.2685f }, {-0.2279f,  0.2279f,  0.9466f }, {-0.2361f,  0.4505f,  0.8610f }, {-0.4505f,  0.2361f,  0.8610f },
    {-0.2450f,  0.6533f,  0.7164f }, {-0.4691f,  0.4691f,  0.7482f }, {-0.6533f,  0.2450f,  0.7164f }, {-0.2502f,  0.8197f,  0.5153f }, {-0.4815f,  0.6762f,  0.5575f }, {-0.6762f,  0.4815f,  0.5575f },
    {-0.8197f,  0.2502f,  0.5153f }, {-0.2448f,  0.9316f,  0.2685f }, {-0.4740f,  0.8288f,  0.2974f }, {-0.6729f,  0.6729f,  0.3072f }, {-0.8288f,  0.4740f,  0.2974f }, {-0.9316f,  0.2448f,  0.2685f },
    {-0.2279f, -0.2279f,  0.9466f }, {-0.4505f, -0.2361f,  0.8610f }, {-0.2361f, -0.4505f,  0.8610f }, {-0.6533f, -0.2450f,  0.7164f }, {-0.4691f, -0.4691f,  0.7482f }, {-0.2450f, -0.6533f,  0.7164f },
    {-0.8197f, -0.2502f,  0.5153f }, {-0.6762f, -0.4815f,  0.5575f }, {-0.4815f, -0.6762f,  0.5575f }, {-0.2502f, -0.8197f,  0.5153f }, {-0.9316f, -0.2448f,  0.2685f }, {-0.8288f, -0.4740f,  0.2974f },
    {-0.6729f, -0.6729f,  0.3072f }, {-0.4740f, -0.8288f,  0.2974f }, {-0.2448f, -0.9316f,  0.2685f }, { 0.2279f, -0.2279f,  0.9466f }, { 0.2361f, -0.4505f,  0.8610f }, { 0.4505f, -0.2361f,  0.8610f },
    { 0.2450f, -0.6533f,  0.7164f }, { 0.4691f, -0.4691f,  0.7482f }, { 0.6533f, -0.2450f,  0.7164f }, { 0.2502f, -0.8197f,  0.5153f }, { 0.4815f, -0.6762f,  0.5575f }, { 0.6762f, -0.4815f,  0.5575f },
    { 0.8197f, -0.2502f,  0.5153f }, { 0.2448f, -0.9316f,  0.2685f }, { 0.4740f, -0.8288f,  0.2974f }, { 0.6729f, -0.6729f,  0.3072f }, { 0.8288f, -0.4740f,  0.2974f }, { 0.9316f, -0.2448f,  0.2685f },
    { 0.2279f,  0.2279f, -0.9466f }, { 0.2361f,  0.4505f, -0.8610f }, { 0.4505f,  0.2361f, -0.8610f }, { 0.2450f,  0.6533f, -0.7164f }, { 0.4691f,  0.4691f, -0.7482f }, { 0.6533f,  0.2450f, -0.7164f },
    { 0.2502f,  0.8197f, -0.5153f }, { 0.4815f,  0.6762f, -0.5575f }, { 0.6762f,  0.4815f, -0.5575f }, { 0.8197f,  0.2502f, -0.5153f }, { 0.2448f,  0.9316f, -0.2685f }, { 0.4740f,  0.8288f, -0.2974f },
    { 0.6729f,  0.6729f, -0.3072f }, { 0.8288f,  0.4740f, -0.2974f }, { 0.9316f,  0.2448f, -0.2685f }, {-0.2279f,  0.2279f, -0.9466f }, {-0.4505f,  0.2361f, -0.8610f }, {-0.2361f,  0.4505f, -0.8610f },
    {-0.6533f,  0.2450f, -0.7164f }, {-0.4691f,  0.4691f, -0.7482f }, {-0.2450f,  0.6533f, -0.7164f }, {-0.8197f,  0.2502f, -0.5153f }, {-0.6762f,  0.4815f, -0.5575f }, {-0.4815f,  0.6762f, -0.5575f },
    {-0.2502f,  0.8197f, -0.5153f }, {-0.9316f,  0.2448f, -0.2685f }, {-0.8288f,  0.4740f, -0.2974f }, {-0.6729f,  0.6729f, -0.3072f }, {-0.4740f,  0.8288f, -0.2974f }, {-0.2448f,  0.9316f, -0.2685f },
    {-0.2279f, -0.2279f, -0.9466f }, {-0.2361f, -0.4505f, -0.8610f }, {-0.4505f, -0.2361f, -0.8610f }, {-0.2450f, -0.6533f, -0.7164f }, {-0.4691f, -0.4691f, -0.7482f }, {-0.6533f, -0.2450f, -0.7164f },
    {-0.2502f, -0.8197f, -0.5153f }, {-0.4815f, -0.6762f, -0.5575f }, {-0.6762f, -0.4815f, -0.5575f }, {-0.8197f, -0.2502f, -0.5153f }, {-0.2448f, -0.9316f, -0.2685f }, {-0.4740f, -0.8288f, -0.2974f },
    {-0.6729f, -0.6729f, -0.3072f }, {-0.8288f, -0.4740f, -0.2974f }, {-0.9316f, -0.2448f, -0.2685f }, { 0.2279f, -0.2279f, -0.9466f }, { 0.4505f, -0.2361f, -0.8610f }, { 0.2361f, -0.4505f, -0.8610f },
    { 0.6533f, -0.2450f, -0.7164f }, { 0.4691f, -0.4691f, -0.7482f }, { 0.2450f, -0.6533f, -0.7164f }, { 0.8197f, -0.2502f, -0.5153f }, { 0.6762f, -0.4815f, -0.5575f }, { 0.4815f, -0.6762f, -0.5575f },
    { 0.2502f, -0.8197f, -0.5153f }, { 0.9316f, -0.2448f, -0.2685f }, { 0.8288f, -0.4740f, -0.2974f }, { 0.6729f, -0.6729f, -0.3072f }, { 0.4740f, -0.8288f, -0.2974f }, { 0.2448f, -0.9316f, -0.2685f },
}};
// clang-format on

} // namespace

Vec3 unpackBmsNormal(std::uint8_t index) { return index < kNormals.size() ? kNormals[index] : Vec3{0, 1, 0}; }

std::size_t BmsMesh::triangleCount() const {
    std::size_t n = 0;
    for (const auto& s : surfaces)
        n += s.quad() ? 2 : 1;
    return n;
}

Aabb BmsMesh::bounds() const {
    Aabb b;
    for (const auto& p : positions)
        b.expand(p);
    return b;
}

std::optional<BmsMesh> parseBms(std::span<const std::byte> data, std::string* error) {
    detail::Reader r(data);
    // MM1 reads the magic and 12 more header bytes that a plain load does
    // not use.
    if (r.u32() != kMagic) {
        setError(error, "not an MM1 mesh (no 3HSM magic)");
        return std::nullopt;
    }
    r.skip(12);

    // agiMeshSet::BinaryLoad.
    BmsMesh m;
    const std::uint32_t vertexCount = r.u32();
    const std::uint32_t adjunctCount = r.u32();
    const std::uint32_t surfaceCount = r.u32();
    const std::uint32_t indexCount = r.u32();
    m.radius = r.f32();
    m.radiusSqr = r.f32();
    m.boxRadius = r.f32();
    const std::uint8_t textureCount = r.u8();
    m.flags = r.u8();
    r.skip(2); // padding
    r.skip(4); // cache size, recomputed by MM1
    if (!r.ok() || vertexCount > kMaxCount || adjunctCount > 0x10000u || surfaceCount > kMaxCount ||
        indexCount != surfaceCount * 4u) {
        setError(error, std::format("corrupt mesh header (vertices {}, adjuncts {}, surfaces {}, indices {})",
                                    vertexCount, adjunctCount, surfaceCount, indexCount));
        return std::nullopt;
    }

    m.textures.resize(textureCount);
    for (auto& t : m.textures) {
        const std::size_t start = r.pos();
        t.name = r.fixedString(32);
        t.flags = r.u8();
        r.skip(3); // LOD, MaxLOD, padding
        t.props = r.u32();
        t.snow = r.f32();
        t.color = r.u32();
        r.seek(start + kTexParamsSize);
    }

    m.positions.resize(vertexCount);
    for (auto& p : m.positions)
        p = r.vec3();
    if (vertexCount >= 16) {
        m.boundingBox.resize(8);
        for (auto& p : m.boundingBox)
            p = r.vec3();
    }
    if (m.flags & BmsFlag::Normal) {
        m.adjunctNormal.resize(adjunctCount);
        for (auto& n : m.adjunctNormal)
            n = r.u8();
    }
    if (m.flags & BmsFlag::Uv) {
        m.adjunctUv.resize(adjunctCount);
        for (auto& uv : m.adjunctUv)
            uv = r.vec2();
    }
    if (m.flags & BmsFlag::Color) {
        m.adjunctColor.resize(adjunctCount);
        for (auto& c : m.adjunctColor)
            c = r.u32();
    }
    m.adjunctPosition.resize(adjunctCount);
    for (auto& i : m.adjunctPosition)
        i = r.u16();
    if (m.flags & BmsFlag::Planes) {
        m.planes.resize(surfaceCount);
        for (auto& p : m.planes)
            p = r.vec4();
    }
    m.surfaces.resize(surfaceCount);
    for (auto& s : m.surfaces)
        s.texture = r.u8();
    for (auto& s : m.surfaces)
        for (auto& c : s.corners)
            c = r.u16();
    if (!r.ok()) {
        setError(error, "truncated mesh");
        return std::nullopt;
    }

    // Index checks, so consumers can trust the mesh.
    for (auto i : m.adjunctPosition)
        if (i >= vertexCount) {
            setError(error, std::format("adjunct refers to vertex {} of {}", i, vertexCount));
            return std::nullopt;
        }
    for (const auto& s : m.surfaces) {
        for (std::size_t c = 0; c < (s.quad() ? 4u : 3u); ++c)
            if (s.corners[c] >= adjunctCount) {
                setError(error, std::format("surface refers to adjunct {} of {}", s.corners[c], adjunctCount));
                return std::nullopt;
            }
        if (s.texture > textureCount) {
            setError(error, std::format("surface uses texture {} of {}", s.texture, textureCount));
            return std::nullopt;
        }
    }
    return m;
}

BmsConverted bmsToPkgMesh(const BmsMesh& bms) {
    BmsConverted out;
    out.mesh.fvf = Fvf::Xyz | Fvf::Normal | Fvf::Diffuse | (1u << Fvf::TexCountShift);
    for (const auto& t : bms.textures) {
        PkgMaterial mat;
        mat.texture = t.name;
        out.materials.push_back(std::move(mat));
    }
    const bool anyUntextured = std::ranges::any_of(bms.surfaces, [](const BmsSurface& s) { return s.texture == 0; });
    if (anyUntextured)
        out.materials.push_back(PkgMaterial{});

    // One section per texture slot, each a single packet with its own copy
    // of the adjuncts it uses.
    const std::size_t slots = bms.textures.size() + 1;
    std::vector<PkgPacket> packets(slots);
    std::vector<std::vector<std::int32_t>> remap(slots);
    auto corner = [&](std::size_t slot, std::uint16_t adj) -> std::uint16_t {
        auto& map = remap[slot];
        if (map.empty())
            map.assign(bms.adjunctCount(), -1);
        if (map[adj] < 0) {
            PkgVertex v;
            v.position = bms.positions[bms.adjunctPosition[adj]];
            if (!bms.adjunctNormal.empty())
                v.normal = unpackBmsNormal(bms.adjunctNormal[adj]);
            if (!bms.adjunctUv.empty())
                v.uv = bms.adjunctUv[adj];
            // Inferred: MM1 colours are D3DCOLOR (ARGB), the order PkgVertex keeps.
            if (!bms.adjunctColor.empty())
                v.color = bms.adjunctColor[adj];
            map[adj] = static_cast<std::int32_t>(packets[slot].vertices.size());
            packets[slot].vertices.push_back(v);
        }
        return static_cast<std::uint16_t>(map[adj]);
    };
    for (const auto& s : bms.surfaces) {
        auto& idx = packets[s.texture].indices;
        const auto& c = s.corners;
        if (s.quad()) {
            // agiMeshSet's quad split (meshrend.cpp, ClipTri calls).
            for (std::uint16_t a : {c[1], c[2], c[3], c[1], c[3], c[0]})
                idx.push_back(corner(s.texture, a));
        } else {
            for (std::uint16_t a : {c[0], c[1], c[2]})
                idx.push_back(corner(s.texture, a));
        }
    }
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (packets[slot].indices.empty())
            continue;
        PkgSection sec;
        sec.shaderIndex = slot == 0 ? static_cast<std::uint32_t>(out.materials.size() - 1)
                                    : static_cast<std::uint32_t>(slot - 1);
        sec.packets.push_back(std::move(packets[slot]));
        out.mesh.sections.push_back(std::move(sec));
    }
    return out;
}

} // namespace mm2::asset
