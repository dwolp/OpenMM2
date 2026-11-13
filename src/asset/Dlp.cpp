#include "asset/Dlp.h"

#include <bit>
#include <cstring>
#include <format>

namespace mm2::asset {
namespace {

class BigReader {
public:
    explicit BigReader(std::span<const std::byte> data) : m_data(data) {}
    bool ok() const { return m_ok; }
    bool skip(std::size_t n) {
        if (!m_ok || m_data.size() - m_pos < n)
            return m_ok = false;
        m_pos += n;
        return true;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(be(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(be(2)); }
    std::uint32_t u32() { return be(4); }
    float f32() { return std::bit_cast<float>(be(4)); }
    std::string bytes(std::size_t n) {
        if (!m_ok || m_data.size() - m_pos < n) {
            m_ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos), n);
        m_pos += n;
        return s;
    }

private:
    std::uint32_t be(std::size_t n) {
        if (!m_ok || m_data.size() - m_pos < n) {
            m_ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < n; ++i)
            v = (v << 8) | static_cast<std::uint32_t>(m_data[m_pos + i]);
        m_pos += n;
        return v;
    }
    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

} // namespace

std::optional<Vec3> Dlp::point(std::string_view name) const {
    for (const auto& g : groups) {
        if (g.name != name)
            continue;
        if (g.vertices.size() == 1 && g.vertices[0] < vertices.size())
            return vertices[g.vertices[0]];
        if (g.patches.size() == 1 && g.patches[0] < patches.size()) {
            const auto& p = patches[g.patches[0]];
            if (!p.empty() && p[0] < vertices.size())
                return vertices[p[0]];
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Dlp> parseDlp(std::span<const std::byte> data, std::string* error) {
    BigReader r(data);
    if (r.bytes(4) != "DLP7") {
        setError(error, "not a DLP7 template");
        return std::nullopt;
    }
    const std::uint32_t groupCount = r.u32(), patchCount = r.u32(), vertexCount = r.u32();
    // Every count is bounded by the bytes left (each item takes at least one).
    if (!r.ok() || groupCount > data.size() || patchCount > data.size() || vertexCount > data.size()) {
        setError(error, "bad DLP7 header");
        return std::nullopt;
    }
    Dlp d;
    d.groups.reserve(groupCount);
    for (std::uint32_t i = 0; i < groupCount && r.ok(); ++i) {
        DlpGroup g;
        std::string name = r.bytes(r.u8());
        g.name = name.substr(0, name.find('\0'));
        const std::uint32_t nv = r.u32(), np = r.u32();
        if (!r.ok() || nv > data.size() || np > data.size())
            break;
        for (std::uint32_t k = 0; k < nv && r.ok(); ++k)
            g.vertices.push_back(r.u16());
        for (std::uint32_t k = 0; k < np && r.ok(); ++k)
            g.patches.push_back(r.u16());
        d.groups.push_back(std::move(g));
    }
    d.patches.reserve(patchCount);
    for (std::uint32_t i = 0; i < patchCount && r.ok(); ++i) {
        // SRes, TRes, Flags, ROpts, MtlIdx, TexIdx, PhysIdx.
        const std::uint32_t s = r.u16(), t = r.u16();
        r.skip(10);
        std::vector<std::uint16_t> ids;
        for (std::uint32_t k = 0; k < s * t && r.ok(); ++k) {
            ids.push_back(r.u16());
            r.skip(12 + 8 + 4); // normal, uv, colour
        }
        r.skip(r.u32()); // the name
        d.patches.push_back(std::move(ids));
    }
    for (std::uint32_t i = 0; i < vertexCount && r.ok(); ++i) {
        const float x = r.f32(), y = r.f32(), z = r.f32();
        d.vertices.push_back({x, y, z});
    }
    if (!r.ok()) {
        setError(error, "DLP7 template ends early");
        return std::nullopt;
    }
    return d;
}

} // namespace mm2::asset
