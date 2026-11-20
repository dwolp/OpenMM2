#include "asset/Image.h"

#include "core/File.h"
#include "core/StringUtil.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#define STBI_FAILURE_USERMSG
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace mm2::asset {
namespace {

bool fail(std::string* error, std::string msg) {
    if (error)
        *error = std::move(msg);
    return false;
}

std::uint8_t u8at(std::span<const std::byte> d, std::size_t i) { return std::to_integer<std::uint8_t>(d[i]); }

constexpr std::size_t kTexHeaderSize = 14;

std::size_t paletteEntries(TexFormat f) {
    switch (f) {
    case TexFormat::P8:
    case TexFormat::P8A8:
    case TexFormat::PA8: return 256;
    case TexFormat::P4:
    case TexFormat::PA4: return 16;
    default: return 0;
    }
}

// Bytes of one level as gfxLoadTexImage reads them. The 4-bit formats read
// w*h/2 bytes (rounded down), so a 1x1 level has no data.
std::size_t levelBytes(TexFormat f, std::uint32_t w, std::uint32_t h) {
    const std::size_t texels = std::size_t{w} * h;
    switch (f) {
    case TexFormat::P8:
    case TexFormat::PA8: return texels;
    case TexFormat::P8A8:
    case TexFormat::ARGB1555: return texels * 2;
    case TexFormat::P4:
    case TexFormat::PA4: return texels / 2;
    case TexFormat::RGB888: return texels * 3;
    case TexFormat::RGBA8888: return texels * 4;
    }
    return 0;
}

bool knownFormat(std::uint16_t f) {
    switch (static_cast<TexFormat>(f)) {
    case TexFormat::P8:
    case TexFormat::P8A8:
    case TexFormat::ARGB1555:
    case TexFormat::PA8:
    case TexFormat::P4:
    case TexFormat::PA4:
    case TexFormat::RGB888:
    case TexFormat::RGBA8888: return true;
    }
    return false;
}

// gfxLoadTexImage builds an RGB888 gfxImage for P8 and P4 (no alpha) and an
// RGBA8888 or ARGB1555 one for the rest.
bool hasAlpha(TexFormat f) { return f != TexFormat::P8 && f != TexFormat::P4 && f != TexFormat::RGB888; }

// texImage_CheckRes: a side must be a power of two.
bool powerOfTwo(std::uint32_t v) { return (v & (0u - v)) == v; }

// Expands a 5-bit channel to 8 bits by bit replication (inferred: the
// conversion happens in the Direct3D driver, which MM2 does not control).
std::uint8_t expand5(std::uint32_t v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }

} // namespace

bool Image::hasTranslucency() const {
    if (levels.empty())
        return false;
    const auto& px = levels[0].rgba;
    for (std::size_t i = 3; i < px.size(); i += 4)
        if (px[i] != 255)
            return true;
    return false;
}

std::optional<TexHeader> parseTexHeader(std::span<const std::byte> data, std::string* error) {
    if (data.size() < kTexHeaderSize) {
        fail(error, "file too small for a texture header");
        return std::nullopt;
    }
    TexHeader h;
    h.width = loadLE<std::uint16_t>(data.data() + 0);
    h.height = loadLE<std::uint16_t>(data.data() + 2);
    const auto format = loadLE<std::uint16_t>(data.data() + 4);
    h.mipCount = loadLE<std::uint16_t>(data.data() + 6);
    h.reserved = loadLE<std::uint16_t>(data.data() + 8);
    h.flags = loadLE<std::uint32_t>(data.data() + 10);
    if (!knownFormat(format)) {
        fail(error, std::format("unknown texture format {}", format));
        return std::nullopt;
    }
    h.format = static_cast<TexFormat>(format);
    // texImage_CheckRes accepts any power of two (and, as an artefact of its
    // bit test, zero); a zero-sized texture is rejected here.
    if (h.width == 0 || h.height == 0 || !powerOfTwo(h.width) || !powerOfTwo(h.height)) {
        fail(error, std::format("bad resolution {} x {}", h.width, h.height));
        return std::nullopt;
    }
    return h;
}

std::optional<Texture> parseTex(std::span<const std::byte> data, std::string* error) {
    auto header = parseTexHeader(data, error);
    if (!header)
        return std::nullopt;
    Texture tex;
    tex.header = *header;
    const TexFormat fmt = header->format;
    tex.image.alphaFormat = hasAlpha(fmt);

    std::size_t pos = kTexHeaderSize;
    const std::size_t palCount = paletteEntries(fmt);
    if (palCount) {
        if (data.size() < pos + palCount * 4) {
            fail(error, "truncated palette");
            return std::nullopt;
        }
        tex.palette.resize(palCount * 4);
        for (std::size_t i = 0; i < palCount; ++i) {
            const std::byte* e = data.data() + pos + i * 4;
            tex.palette[i * 4 + 0] = std::to_integer<std::uint8_t>(e[2]);
            tex.palette[i * 4 + 1] = std::to_integer<std::uint8_t>(e[1]);
            tex.palette[i * 4 + 2] = std::to_integer<std::uint8_t>(e[0]);
            tex.palette[i * 4 + 3] = std::to_integer<std::uint8_t>(e[3]);
        }
        pos += palCount * 4;
    }
    // P8 and P4 become RGB888 images in MM2: the palette's alpha is dropped.
    const bool paletteAlpha = hasAlpha(fmt);
    auto paletteTexel = [&](std::uint8_t* d, std::size_t index) {
        std::memcpy(d, tex.palette.data() + index * 4, 4);
        if (!paletteAlpha)
            d[3] = 255;
    };

    // gfxImage::Create adds a mip level only while both sides are above 1;
    // gfxLoadTexImage reads that many levels at most (a mip count of 0 reads
    // the whole chain).
    std::uint32_t w = header->width, h = header->height;
    for (std::uint32_t level = 0; header->mipCount == 0 || level < header->mipCount; ++level) {
        if (level > 0) {
            if (w <= 1 || h <= 1)
                break;
            w /= 2;
            h /= 2;
        }
        const std::size_t bytes = levelBytes(fmt, w, h);
        if (data.size() < pos + bytes) {
            fail(error, std::format("truncated mip level {} ({}x{})", level, w, h));
            return std::nullopt;
        }
        const std::byte* src = data.data() + pos;
        Image::Level out;
        out.width = w;
        out.height = h;
        out.rgba.resize(std::size_t{w} * h * 4);
        std::uint8_t* dst = out.rgba.data();
        const std::size_t texels = std::size_t{w} * h;
        switch (fmt) {
        case TexFormat::P8:
        case TexFormat::PA8:
            for (std::size_t i = 0; i < texels; ++i)
                paletteTexel(dst + i * 4, std::to_integer<std::size_t>(src[i]));
            break;
        case TexFormat::P8A8:
            // Palette index, then the texel's own alpha.
            for (std::size_t i = 0; i < texels; ++i) {
                paletteTexel(dst + i * 4, std::to_integer<std::size_t>(src[i * 2]));
                dst[i * 4 + 3] = std::to_integer<std::uint8_t>(src[i * 2 + 1]);
            }
            break;
        case TexFormat::P4:
        case TexFormat::PA4:
            // Two texels per byte, low nibble first. A level with an odd texel
            // count (only 1x1) has no byte for its last texel in MM2, which
            // leaves it uninitialised; it takes palette entry 0 here
            // (inferred).
            for (std::size_t i = 0; i < texels; ++i) {
                const std::size_t b = i / 2 < bytes ? std::to_integer<std::size_t>(src[i / 2]) : 0;
                paletteTexel(dst + i * 4, (i & 1) ? (b >> 4) : (b & 0xF));
            }
            break;
        case TexFormat::ARGB1555:
            for (std::size_t i = 0; i < texels; ++i) {
                const std::uint32_t v = loadLE<std::uint16_t>(src + i * 2);
                dst[i * 4 + 0] = expand5((v >> 10) & 31);
                dst[i * 4 + 1] = expand5((v >> 5) & 31);
                dst[i * 4 + 2] = expand5(v & 31);
                dst[i * 4 + 3] = (v & 0x8000) ? 255 : 0;
            }
            break;
        case TexFormat::RGB888:
            for (std::size_t i = 0; i < texels; ++i) {
                std::memcpy(dst + i * 4, src + i * 3, 3);
                dst[i * 4 + 3] = 255;
            }
            break;
        case TexFormat::RGBA8888: std::memcpy(dst, src, texels * 4); break;
        }
        tex.image.levels.push_back(std::move(out));
        pos += bytes;
    }
    return tex;
}

std::optional<Image> decodeTga(std::span<const std::byte> data, std::string* error) {
    if (data.size() < 18) {
        fail(error, "file too small for a TGA header");
        return std::nullopt;
    }
    const std::uint8_t idLength = u8at(data, 0);
    const std::uint8_t cmapType = u8at(data, 1);
    const std::uint8_t imageType = u8at(data, 2);
    const auto cmapFirst = loadLE<std::uint16_t>(data.data() + 3);
    const auto cmapLength = loadLE<std::uint16_t>(data.data() + 5);
    const std::uint8_t cmapBits = u8at(data, 7);
    const auto width = loadLE<std::uint16_t>(data.data() + 12);
    const auto height = loadLE<std::uint16_t>(data.data() + 14);
    const std::uint8_t bpp = u8at(data, 16);
    const std::uint8_t descriptor = u8at(data, 17);

    const bool rle = imageType >= 9;
    const std::uint8_t baseType = rle ? imageType - 8 : imageType;
    if (baseType < 1 || baseType > 3 || width == 0 || height == 0) {
        fail(error, std::format("unsupported TGA image type {}", imageType));
        return std::nullopt;
    }
    if ((baseType == 1 && (bpp != 8 || cmapType != 1)) || (baseType == 2 && bpp != 15 && bpp != 16 && bpp != 24 &&
                                                            bpp != 32) ||
        (baseType == 3 && bpp != 8)) {
        fail(error, std::format("unsupported TGA pixel depth {} for type {}", bpp, imageType));
        return std::nullopt;
    }

    std::size_t pos = 18 + idLength;
    // Colour map entries.
    std::vector<std::uint8_t> cmap;
    if (cmapType == 1) {
        const std::size_t entryBytes = (cmapBits + 7) / 8;
        if (data.size() < pos + entryBytes * cmapLength) {
            fail(error, "truncated TGA colour map");
            return std::nullopt;
        }
        cmap.resize(std::size_t{cmapLength} * 4);
        for (std::size_t i = 0; i < cmapLength; ++i) {
            const std::byte* e = data.data() + pos + i * entryBytes;
            std::uint8_t* c = cmap.data() + i * 4;
            if (entryBytes >= 3) {
                c[0] = std::to_integer<std::uint8_t>(e[2]);
                c[1] = std::to_integer<std::uint8_t>(e[1]);
                c[2] = std::to_integer<std::uint8_t>(e[0]);
                c[3] = entryBytes == 4 ? std::to_integer<std::uint8_t>(e[3]) : 255;
            } else {
                const auto v = loadLE<std::uint16_t>(e);
                c[0] = static_cast<std::uint8_t>(((v >> 10) & 31) * 255 / 31);
                c[1] = static_cast<std::uint8_t>(((v >> 5) & 31) * 255 / 31);
                c[2] = static_cast<std::uint8_t>((v & 31) * 255 / 31);
                c[3] = 255;
            }
        }
        pos += entryBytes * cmapLength;
    }

    const std::size_t pixelBytes = (bpp + 7) / 8;
    const std::size_t texels = std::size_t{width} * height;
    std::vector<std::uint8_t> raw(texels * pixelBytes);
    if (!rle) {
        if (data.size() < pos + raw.size()) {
            fail(error, "truncated TGA pixel data");
            return std::nullopt;
        }
        std::memcpy(raw.data(), data.data() + pos, raw.size());
    } else {
        std::size_t out = 0;
        while (out < raw.size()) {
            if (pos >= data.size()) {
                fail(error, "truncated TGA RLE data");
                return std::nullopt;
            }
            const std::uint8_t packet = u8at(data, pos++);
            const std::size_t count = (packet & 0x7F) + 1u;
            const std::size_t bytes = count * pixelBytes;
            if (out + bytes > raw.size()) {
                fail(error, "TGA RLE packet overruns image");
                return std::nullopt;
            }
            if (packet & 0x80) {
                if (pos + pixelBytes > data.size()) {
                    fail(error, "truncated TGA RLE data");
                    return std::nullopt;
                }
                for (std::size_t i = 0; i < count; ++i)
                    std::memcpy(raw.data() + out + i * pixelBytes, data.data() + pos, pixelBytes);
                pos += pixelBytes;
            } else {
                if (pos + bytes > data.size()) {
                    fail(error, "truncated TGA RLE data");
                    return std::nullopt;
                }
                std::memcpy(raw.data() + out, data.data() + pos, bytes);
                pos += bytes;
            }
            out += bytes;
        }
    }

    // Output is bottom-up, which is the TGA default (origin bit 5 clear).
    // MM2's gfxLoadTargaImage stores the picture's top row first instead;
    // asset::Image keeps the bottom row first for every non-.tex image, and
    // its users draw it accordingly (see docs/formats/images.md).
    const bool topDown = (descriptor & 0x20) != 0;
    const bool rightToLeft = (descriptor & 0x10) != 0;
    Image img;
    // gfxLoadTargaImage makes a 32-bit TGA an RGBA8888 image, anything else RGB888.
    img.alphaFormat = pixelBytes == 4 || (pixelBytes == 2 && bpp == 16 && (descriptor & 0x0F)) ||
                      (baseType == 1 && cmapBits == 32);
    Image::Level level;
    level.width = width;
    level.height = height;
    level.rgba.resize(texels * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t srcRow = topDown ? height - 1 - y : y;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t srcCol = rightToLeft ? width - 1 - x : x;
            const std::uint8_t* s = raw.data() + (std::size_t{srcRow} * width + srcCol) * pixelBytes;
            std::uint8_t* d = level.rgba.data() + (std::size_t{y} * width + x) * 4;
            if (baseType == 1) {
                const std::size_t idx = s[0] >= cmapFirst ? s[0] - cmapFirst : 0;
                if (idx < cmapLength) {
                    std::memcpy(d, cmap.data() + idx * 4, 4);
                } else {
                    d[0] = d[1] = d[2] = 0;
                    d[3] = 255;
                }
            } else if (baseType == 3) {
                d[0] = d[1] = d[2] = s[0];
                d[3] = 255;
            } else if (pixelBytes == 2) {
                const std::uint16_t v = static_cast<std::uint16_t>(s[0] | (s[1] << 8));
                d[0] = static_cast<std::uint8_t>(((v >> 10) & 31) * 255 / 31);
                d[1] = static_cast<std::uint8_t>(((v >> 5) & 31) * 255 / 31);
                d[2] = static_cast<std::uint8_t>((v & 31) * 255 / 31);
                d[3] = (bpp == 16 && (descriptor & 0x0F) && !(v & 0x8000)) ? 0 : 255;
            } else {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = pixelBytes == 4 ? s[3] : 255;
            }
        }
    }
    img.levels.push_back(std::move(level));
    return img;
}

std::optional<Image> decodeStb(std::span<const std::byte> data, std::string* error) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()), static_cast<int>(data.size()),
                                        &w, &h, &comp, 4);
    if (!px) {
        fail(error, std::format("image decode failed: {}", stbi_failure_reason()));
        return std::nullopt;
    }
    Image img;
    img.alphaFormat = comp == 4 || comp == 2;
    Image::Level level;
    level.width = static_cast<std::uint32_t>(w);
    level.height = static_cast<std::uint32_t>(h);
    // stb_image returns the top row first; store bottom-up.
    const std::size_t rowBytes = std::size_t(w) * 4;
    level.rgba.resize(rowBytes * h);
    for (int y = 0; y < h; ++y)
        std::memcpy(level.rgba.data() + std::size_t(y) * rowBytes, px + std::size_t(h - 1 - y) * rowBytes, rowBytes);
    stbi_image_free(px);
    img.levels.push_back(std::move(level));
    return img;
}

std::optional<Image> decodeDds(std::span<const std::byte> data, std::string* error) {
    // DDSURFACEDESC2 after the "DDS " magic; the pixel format at +76.
    constexpr std::size_t kHeader = 128;
    constexpr std::uint32_t kPfAlpha = 0x1, kPfFourCC = 0x4, kPfPalette8 = 0x20, kPfRgb = 0x40;
    if (data.size() < kHeader || std::memcmp(data.data(), "DDS ", 4) != 0) {
        fail(error, "not a DDS file");
        return std::nullopt;
    }
    auto u32 = [&](std::size_t off) { return loadLE<std::uint32_t>(data.data() + off); };
    const std::uint32_t height = u32(12), width = u32(16);
    const std::uint32_t mipCount = std::max(1u, u32(28));
    const std::uint32_t pfFlags = u32(80), bits = u32(88);
    const std::array<std::uint32_t, 4> masks{u32(92), u32(96), u32(100), u32(104)}; // R, G, B, A
    if (pfFlags & kPfFourCC) {
        fail(error, "compressed (FourCC) DDS is not supported");
        return std::nullopt;
    }
    if (pfFlags & kPfPalette8) {
        fail(error, "palettised DDS is not supported (MM1 keeps texp/ palettes in .lut files)");
        return std::nullopt;
    }
    if (!(pfFlags & kPfRgb) || (bits != 16 && bits != 24 && bits != 32) || width == 0 || height == 0 ||
        width > 16384 || height > 16384 || mipCount > 16) {
        fail(error, std::format("unsupported DDS ({}x{}, {} bits, pixel format flags 0x{:X})", width, height, bits,
                                pfFlags));
        return std::nullopt;
    }
    const bool hasAlpha = (pfFlags & kPfAlpha) && masks[3] != 0;

    // Per channel: shift to the mask's lowest bit and its largest value.
    struct Channel {
        std::uint32_t mask = 0, shift = 0, max = 0;
    };
    std::array<Channel, 4> ch{};
    for (int c = 0; c < 4; ++c) {
        ch[c].mask = (c == 3 && !hasAlpha) ? 0 : masks[c];
        if (ch[c].mask == 0)
            continue;
        while (!((ch[c].mask >> ch[c].shift) & 1u))
            ++ch[c].shift;
        ch[c].max = ch[c].mask >> ch[c].shift;
    }

    const std::size_t bpp = bits / 8;
    Image img;
    img.alphaFormat = hasAlpha;
    std::size_t pos = kHeader;
    std::uint32_t w = width, h = height;
    for (std::uint32_t level = 0; level < mipCount; ++level) {
        const std::size_t bytes = std::size_t{w} * h * bpp;
        if (pos + bytes > data.size()) {
            // Keep the levels that are complete; a missing top level is an error.
            if (level == 0) {
                fail(error, "truncated DDS");
                return std::nullopt;
            }
            break;
        }
        Image::Level lv;
        lv.width = w;
        lv.height = h;
        lv.rgba.resize(std::size_t{w} * h * 4);
        for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
            std::uint32_t px = 0;
            for (std::size_t b = 0; b < bpp; ++b)
                px |= std::uint32_t{u8at(data, pos + i * bpp + b)} << (8 * b);
            for (int c = 0; c < 4; ++c) {
                std::uint8_t v = 255;
                if (ch[c].mask != 0) {
                    const std::uint32_t raw = (px & ch[c].mask) >> ch[c].shift;
                    v = static_cast<std::uint8_t>((raw * 255u + ch[c].max / 2) / ch[c].max);
                }
                lv.rgba[i * 4 + c] = v;
            }
        }
        img.levels.push_back(std::move(lv));
        pos += bytes;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return img;
}

std::optional<Image> decodeImageFile(std::string_view path, std::span<const std::byte> data, std::string* error) {
    if (str::iendsWith(path, ".tex")) {
        auto tex = parseTex(data, error);
        if (!tex)
            return std::nullopt;
        return std::move(tex->image);
    }
    if (str::iendsWith(path, ".tga"))
        return decodeTga(data, error);
    if (str::iendsWith(path, ".dds"))
        return decodeDds(data, error);
    if (str::iendsWith(path, ".jpg") || str::iendsWith(path, ".jpeg") || str::iendsWith(path, ".bmp") ||
        str::iendsWith(path, ".png"))
        return decodeStb(data, error);
    fail(error, std::format("unknown image type '{}'", path));
    return std::nullopt;
}

std::vector<std::byte> encodePng(const Image::Level& level, bool flip) {
    std::size_t size = 0;
    // The last argument flips rows, so a bottom-up image is written upright.
    void* png = tdefl_write_image_to_png_file_in_memory_ex(level.rgba.data(), static_cast<int>(level.width),
                                                           static_cast<int>(level.height), 4, &size, 6,
                                                           flip ? MZ_TRUE : MZ_FALSE);
    if (!png)
        return {};
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), png, size);
    mz_free(png);
    return out;
}

} // namespace mm2::asset
