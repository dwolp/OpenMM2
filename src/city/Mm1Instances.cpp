#include "city/Mm1Instances.h"

#include "core/File.h"

#include <cmath>
#include <cstring>
#include <format>

namespace mm2::city {
namespace {

constexpr std::size_t kBngRecord = 0x1C;
constexpr std::size_t kFcdRecord = 0x2C;
// MM1 reads names into a 64-byte stack buffer (LoadBangers, ebp-60h).
constexpr std::size_t kMaxName = 64;
constexpr std::uint32_t kMaxRecords = 1u << 20;

void setError(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
}

Vec3 vec3At(const std::byte* p) { return {loadLE<float>(p), loadLE<float>(p + 4), loadLE<float>(p + 8)}; }

// The shared shape of both files: count, then records each followed by a
// NUL-terminated name. `fill` decodes one fixed record.
template <class T, class Fill>
std::optional<std::vector<T>> parseRecords(std::span<const std::byte> data, std::size_t recordSize, Fill fill,
                                           std::string* error) {
    if (data.size() < 4) {
        setError(error, "file too small for a record count");
        return std::nullopt;
    }
    const auto count = loadLE<std::uint32_t>(data.data());
    if (count > kMaxRecords) {
        setError(error, std::format("implausible record count {}", count));
        return std::nullopt;
    }
    std::vector<T> out;
    out.reserve(count);
    std::size_t pos = 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + recordSize > data.size()) {
            setError(error, std::format("record {} truncated", i));
            return std::nullopt;
        }
        T rec;
        fill(rec, data.data() + pos);
        pos += recordSize;
        const auto* name = reinterpret_cast<const char*>(data.data() + pos);
        const std::size_t len = strnlen(name, std::min(kMaxName, data.size() - pos));
        if (len == kMaxName || pos + len >= data.size()) {
            setError(error, std::format("record {} has an unterminated name", i));
            return std::nullopt;
        }
        rec.name.assign(name, len);
        pos += len + 1;
        out.push_back(std::move(rec));
    }
    if (pos != data.size()) {
        setError(error, std::format("{} bytes after the last record", data.size() - pos));
        return std::nullopt;
    }
    return out;
}

} // namespace

std::optional<std::vector<Mm1Banger>> parseMm1Bng(std::span<const std::byte> data, std::string* error) {
    return parseRecords<Mm1Banger>(
        data, kBngRecord,
        [](Mm1Banger& r, const std::byte* p) {
            r.cell = loadLE<std::uint16_t>(p);
            r.flags = loadLE<std::uint16_t>(p + 2);
            r.position = vec3At(p + 4);
            r.ahead = vec3At(p + 16);
        },
        error);
}

std::optional<std::vector<Mm1Facade>> parseMm1Fcd(std::span<const std::byte> data, std::string* error) {
    return parseRecords<Mm1Facade>(
        data, kFcdRecord,
        [](Mm1Facade& r, const std::byte* p) {
            r.cell = loadLE<std::uint16_t>(p);
            r.flags = loadLE<std::uint16_t>(p + 2);
            r.start = vec3At(p + 4);
            r.end = vec3At(p + 16);
            r.sides = vec3At(p + 28);
            r.width = loadLE<float>(p + 40);
        },
        error);
}

Mat34 mm1MatrixFromPoints(const Vec3& start, const Vec3& end, float width) {
    // MM1's order: the scale from the difference's length, then the unit
    // direction from Vector3::InvMag (taken here as 1 / length; whether MM1's
    // rounds the same is unverified).
    const Vec3 d{end.x - start.x, end.y - start.y, end.z - start.z};
    const float scale = d.mag() / width;
    const float inv = 1.0f / d.mag();
    Mat34 m;
    m.m0 = {inv * d.x, inv * d.y, inv * d.z};
    m.m1 = {0.0f, 1.0f, 0.0f};
    m.m2 = m.m0.cross(m.m1);
    m.m0 = {m.m0.x * scale, m.m0.y * scale, m.m0.z * scale};
    m.m3 = start;
    if (m.m3.y > end.y)
        m.m3.y = end.y;
    return m;
}

Mat34 mm1ShearMatrix(const Vec3& start, const Vec3& end, float width) {
    // The scale from the difference's length (Mag / width), then the unit
    // direction (InvMag) times the scale for x; z from the unit direction.
    const Vec3 d{end.x - start.x, end.y - start.y, end.z - start.z};
    const float scale = d.mag() / width;
    const float inv = 1.0f / d.mag();
    const Vec3 u{inv * d.x, inv * d.y, inv * d.z};
    Mat34 m;
    m.m0 = {scale * u.x, scale * u.y, scale * u.z};
    m.m1 = {0.0f, 1.0f, 0.0f};
    m.m2 = {0.0f - u.z, 0.0f, u.x - 0.0f};
    m.m3 = start;
    return m;
}

Mat34 mm1BuildingMatrix(const Vec3& a, const Vec3& b, const Vec3& c) {
    Mat34 m;
    m.m0 = {b.x - a.x, b.y - a.y, b.z - a.z};
    m.m1 = {0.0f, 1.0f, 0.0f}; // YAXIS
    m.m2 = {c.x - a.x, c.y - a.y, c.z - a.z};
    m.m3 = a;
    return m;
}

} // namespace mm2::city
