#pragma once

#include "asset/Image.h"
#include "render/Device.h"
#include "vfs/Vfs.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mm2::game {

// A world texture on the GPU. Game images keep their file row order (bottom
// row first), which is what the game's texture coordinates expect, so no
// flipping happens anywhere in 3D rendering.
struct WorldTexture {
    render::TextureHandle handle;
    // The address modes come from the .tex flag word (0x1 clamps U, 0x10000
    // clamps V, as gfxRenderState::DoFlush applies the texture environment).
    render::SamplerDesc sampler;
    std::uint32_t width = 0, height = 0;
    std::uint32_t flags = 0;    // .tex flag word (MM2's texture environment)
    bool translucent = false;   // some texels have alpha < 255
    // The image format carries alpha, which is what makes MM2 draw a texture
    // alpha blended and tested (gfxTexture::Create's alpha bit).
    bool alphaFormat = false;
    // OpenMM2 grass: the name says grass (s_grass, s_grassndirt, ...).
    bool grass = false;
    // Loaded as the night variant ("<name>_ni"; render::DrawFlag::NightLit).
    bool nightLit = false;
};

// Loads textures by base name ("cw_apt_brk"), looking for texture/<name>.tex
// and then .tga (gfxLoadImageAll's order for the formats the 3D world uses).
// A missing texture with frames texture/<name>-0001... is a texture movie
// (gfxGetTextureMovie), cycling at the rate tune/<name>.movie gives.
class TextureLibrary {
public:
    TextureLibrary(render::Device& device, const vfs::Vfs& vfs);
    ~TextureLibrary();
    TextureLibrary(const TextureLibrary&) = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;

    // gfxGetTexture(name, mipmaps). Null when the texture does not exist
    // (logged once). Without `mipmaps` the texture has its top level only
    // (asParticles::SetTexture by name asks for none).
    const WorldTexture* get(std::string_view name, bool mipmaps = true);

    // MM2's texture variant handler (InstallTextureVariantHandler), active
    // in game: in rain a texture loads as "<name>_fa" when that exists (wet
    // roads and decals); at night as "<name>_ni" (lit windows, shop fronts,
    // lamps), and every other texture is darkened (each channel halved on
    // every mip level). Changing the variants reloads the textures.
    void setVariants(bool night, bool rain);
    void setNight(bool night) { setVariants(night, m_rain); }
    bool night() const { return m_night; }
    bool rain() const { return m_rain; }
    // Advances animated textures to `time` seconds.
    void update(double time);

    // The pixels `name` loads as (variants and night darkening applied), for
    // textures that are modified at run time (fxTexelDamage).
    std::optional<asset::Image> image(std::string_view name);
    // Adds a texture made elsewhere under `name` (kept across variant
    // changes); the library destroys it.
    const WorldTexture* adopt(const std::string& name, const WorldTexture& texture);
    void release(const std::string& name);

    std::size_t loadedCount() const { return m_textures.size(); }

    // gfxTexReduceSize: cityLevel::Load sets 32 << Texture Quality (32, 64,
    // 128 or 256 pixels) while the city loads and 0 (no limit) after. A
    // texture named while a limit is set keeps it, also when it is loaded
    // later (OpenMM2 loads on first use) or again (variants): its top mip
    // levels are dropped, or a single level halved, until both sides fit
    // (gfxDefaultPrepareImage).
    void setSizeLimit(int pixels) { m_sizeLimit = pixels; }
    // Names a texture during the city's load: it takes the current limit.
    void declare(std::string_view name);

    // vglSetCloudMap: the cloud shadow texture made from `name` (its colour
    // black, its alpha inverted, no mipmaps). Null when it does not exist.
    const WorldTexture* cloudMap(std::string_view name);

private:
    struct Animation {
        std::vector<WorldTexture> frames;
        float rate = 30.0f;
        WorldTexture current;
    };
    struct LoadedImage {
        asset::Image image;
        std::uint32_t flags = 0;
        bool alphaFormat = false;
        std::size_t generatedLevels = 0; // mip levels to generate below the top
        bool topRowFirst = false;        // stored flipped from a top-first loader
    };
    // gfxLoadImageAll's lookup of one name (no variants, no darkening).
    std::optional<LoadedImage> readImage(const std::string& name, bool mipmaps) const;
    std::optional<WorldTexture> load(const std::string& name, bool darken, bool mipmaps);
    // The variant to load for `name` and whether to darken it.
    std::optional<WorldTexture> loadVariant(const std::string& name, bool mipmaps);
    void clear();

    render::Device& m_device;
    const vfs::Vfs& m_vfs;
    std::unordered_map<std::string, std::optional<WorldTexture>> m_textures;
    std::unordered_map<std::string, Animation> m_animations;
    std::unordered_map<std::string, WorldTexture> m_adopted;
    bool m_night = false;
    bool m_rain = false;
    int m_sizeLimit = 0;                                // while the city loads
    std::unordered_map<std::string, int> m_sizeLimits; // per texture name
    int m_activeLimit = 0;                              // the texture being loaded
};

} // namespace mm2::game
