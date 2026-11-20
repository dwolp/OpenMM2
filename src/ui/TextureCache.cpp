#include "ui/TextureCache.h"

#include "asset/Image.h"
#include "core/Log.h"
#include "core/StringUtil.h"

namespace mm2::ui {

TextureCache::TextureCache(render::Device& device, const vfs::Vfs& vfs) : m_device(device), m_vfs(vfs) {}

TextureCache::~TextureCache() { clear(); }

void TextureCache::clear() {
    for (auto& [path, t] : m_textures)
        if (t.handle)
            m_device.destroyTexture(t.handle);
    m_textures.clear();
}

const UiTexture& TextureCache::get(std::string_view path) { return load(path, false); }

const UiTexture& TextureCache::getColorKeyed(std::string_view path) { return load(path, true); }

const UiTexture& TextureCache::load(std::string_view path, bool colorKey) {
    const std::string file = str::normalizeVirtualPath(path);
    const std::string key = colorKey ? file + "#key" : file;
    if (auto it = m_textures.find(key); it != m_textures.end())
        return it->second;
    UiTexture tex;
    if (auto bytes = m_vfs.readAll(file)) {
        std::string error;
        if (auto image = asset::decodeImageFile(file, *bytes, &error); image && !image->empty()) {
            auto& level = image->levels[0];
            // OpenMM2 Enhanced: MM1's menu pictures (mm1/bmp16/*.jpg) are
            // stored with their rows the other way up.
            if (file.starts_with("mm1/")) {
                const std::size_t row = std::size_t{level.width} * 4;
                for (std::uint32_t y = 0; y < level.height / 2; ++y)
                    std::swap_ranges(level.rgba.begin() + static_cast<std::ptrdiff_t>(y * row),
                                     level.rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                     level.rgba.begin() + static_cast<std::ptrdiff_t>((level.height - 1 - y) * row));
            }
            if (colorKey) {
                for (std::size_t i = 0; i + 3 < level.rgba.size(); i += 4)
                    if (level.rgba[i] == 0 && level.rgba[i + 1] == 0 && level.rgba[i + 2] == 0)
                        level.rgba[i + 3] = 0;
            }
            render::TextureDesc desc;
            desc.width = level.width;
            desc.height = level.height;
            desc.debugName = key;
            const render::TextureData data{level.rgba.data(), 0};
            tex.handle = m_device.createTexture(desc, std::span(&data, 1));
            tex.width = level.width;
            tex.height = level.height;
        } else {
            log::warn("ui: cannot decode {}: {}", file, error);
        }
    } else {
        log::warn("ui: missing image {}", file);
    }
    return m_textures.emplace(key, tex).first->second;
}

} // namespace mm2::ui
