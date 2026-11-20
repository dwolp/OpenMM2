#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::asset {

// Decoded image: RGBA8 texels with rows ordered BOTTOM to TOP: row 0 is the
// bottom edge of the picture and corresponds to texture coordinate v = 0 in
// the game's UVs (verified: the stop sign face in sp_stop_f.pkg has v = 1 at
// its top, and .tex files store the picture's bottom row first). Upload rows
// in order and use the game's UVs unchanged; flip only for display formats
// such as PNG (encodePng does). levels[0] is the full-size image; further
// entries are the mip levels stored in the file, each half the previous size.
struct Image {
    struct Level {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba; // width * height * 4 bytes
    };
    std::vector<Level> levels;
    // True when the source format carries alpha (MM2's gfxImage RGBA8888 or
    // ARGB1555 images: .tex PA8, P8A8, PA4, RGBA8888 and ARGB1555, 32-bit
    // TGA). gfxTexture::Create marks such textures as alpha textures; the
    // flag comes from the format, not from the texel values.
    bool alphaFormat = false;

    std::uint32_t width() const { return levels.empty() ? 0 : levels[0].width; }
    std::uint32_t height() const { return levels.empty() ? 0 : levels[0].height; }
    bool empty() const { return levels.empty(); }
    // True when any texel of the top level has alpha < 255.
    bool hasTranslucency() const;
};

// --- Angel .tex textures (texture/*.tex) -------------------------------------
//
// See docs/formats/tex.md. Read as MM2's gfxLoadTexImage does. Header
// (14 bytes, little-endian):
//   u16 width, u16 height, u16 format, u16 mipCount, u16 (ignored; always 1),
//   u32 flags
// followed by a palette (B,G,R,A bytes, like a Windows RGBQUAD; 256 entries,
// 16 for the 4-bit formats) and then the mip levels, largest first, tightly
// packed, each stored bottom row first.

enum class TexFormat : std::uint16_t {
    P8 = 1,        // 8-bit palette index; drawn opaque (the palette's alpha is ignored)
    P8A8 = 2,      // 8-bit palette index followed by an 8-bit alpha per texel; not in retail files
    ARGB1555 = 6,  // 16-bit A1 R5 G5 B5; not in retail files
    PA8 = 14,      // 8-bit palette with per-entry alpha
    P4 = 15,       // 4-bit palette (16 entries), opaque; not in retail files
    PA4 = 16,      // 4-bit palette with alpha; not in retail files
    RGB888 = 17,   // 24-bit, bytes R,G,B
    RGBA8888 = 18, // 32-bit, bytes R,G,B,A (note: palettes are B,G,R,A)
};

// Texture flags ("TexEnv"): gfxLoadTexImage stores them in the image and
// gfxTexture::Create ORs them into the texture's state. MM2's render-state
// flush (gfxRenderState::DoFlush) reads two of them, the texture address
// modes; everything else repeats. The other bits the exporter wrote (0x2,
// 0x4, 0x8000) are not read by MM2's renderer. See docs/formats/tex.md.
namespace TexFlags {
inline constexpr std::uint32_t ClampU = 0x1;     // D3DTSS_ADDRESSU = CLAMP (else WRAP)
inline constexpr std::uint32_t ClampV = 0x10000; // D3DTSS_ADDRESSV = CLAMP (else WRAP)
} // namespace TexFlags

struct TexHeader {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    TexFormat format = TexFormat::P8;
    // Levels stored in the file. 0 means "the whole chain" (gfxImage::Create
    // keeps halving), as in MM2. The image holds at most as many levels as
    // MM2's chain has: a level is added only while both sides are above 1.
    std::uint16_t mipCount = 0;
    std::uint16_t reserved = 0;
    std::uint32_t flags = 0;
};

struct Texture {
    TexHeader header;
    Image image;
    // Palette converted to R,G,B,A bytes; empty for true-colour formats.
    std::vector<std::uint8_t> palette;
};

// Fails, like gfxLoadTexImage, on an unknown format or a side that is not a
// power of two ("Bad resolution"); MM2 then tries the next image type.
std::optional<Texture> parseTex(std::span<const std::byte> data, std::string* error = nullptr);
// Header only (cheap; used by listings).
std::optional<TexHeader> parseTexHeader(std::span<const std::byte> data, std::string* error = nullptr);

// --- Other image files ----------------------------------------------------------
//
// MM2's own readers are narrower than these (see docs/formats/images.md):
// gfxLoadTargaImage reads only uncompressed 24/32-bit TGAs (it ignores the
// image type, colour map and ID field), gfxLoadBmpImage only uncompressed
// 8/24-bit BMPs, gfxLoadJPEGImage baseline JPEG through IJG libjpeg, and PNG
// is not supported. Every retail image decodes the same in both.

// Truevision TGA: uncompressed and RLE, true-colour (16/24/32-bit),
// colour-mapped and greyscale. Honours the origin bits (output bottom-up).
std::optional<Image> decodeTga(std::span<const std::byte> data, std::string* error = nullptr);

// JPEG, BMP and PNG through stb_image (single level, opaque except PNG/BMP alpha).
std::optional<Image> decodeStb(std::span<const std::byte> data, std::string* error = nullptr);

// DirectDraw surfaces (.dds), as Midtown Madness 1 stores its textures
// (tex16o/, tex16a/, texp/; MM2 never reads them, see docs/formats/mm1city.md).
// Uncompressed RGB(A) with any bit masks (MM1: RGB565 and ARGB4444) and the
// stored mip chain. Rows stay in file order, top of the picture first: MM1
// draws through Direct3D, where v = 0 is the top row, so row 0 is v = 0 as
// for every Image (encodePng with flip = false shows it upright).
// alphaFormat is set when the format has an alpha mask. Not supported:
// block-compressed (FourCC) surfaces, and palettised ones (MM1's texp/
// keep their palettes in separate .lut files).
std::optional<Image> decodeDds(std::span<const std::byte> data, std::string* error = nullptr);

// Decodes by file extension: .tex, .tga, .jpg/.jpeg, .bmp, .png, .dds.
std::optional<Image> decodeImageFile(std::string_view path, std::span<const std::byte> data,
                                     std::string* error = nullptr);

// Encodes one level as a PNG file (RGBA8). `flip` writes a bottom-up level
// (the usual Image row order) upright; pass false for top-down rows (DDS).
std::vector<std::byte> encodePng(const Image::Level& level, bool flip = true);

} // namespace mm2::asset
