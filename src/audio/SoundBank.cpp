#include "audio/SoundBank.h"

#include "core/Log.h"
#include "core/StringUtil.h"

namespace mm2::audio {

SoundBank::SoundBank(const vfs::Vfs& vfs) : m_vfs(vfs) {
    for (const auto& e : vfs.listFiles()) {
        // OpenMM2 Enhanced: MM1's AUDIO.AR (mounted under mm1/) is indexed with an
        // "mm1:" name prefix, so its samples never replace MM2's.
        const bool mm1 = e.path.starts_with("mm1/aud/");
        if ((!mm1 && !e.path.starts_with("aud/")) || !e.path.ends_with(".wav"))
            continue;
        std::string base = e.path.substr(e.path.rfind('/') + 1);
        base.resize(base.size() - 4); // ".wav"
        if (mm1)
            base = "mm1:" + base;
        if (base.ends_with(".11k")) {
            base.resize(base.size() - 4);
            m_index[base].k11 = e.path;
        } else if (base.ends_with(".22k")) {
            base.resize(base.size() - 4);
            m_index[base].k22 = e.path;
        } else {
            m_index[base].other = e.path;
        }
    }
}

void SoundBank::setQuality(Quality q) {
    if (q != m_quality) {
        m_quality = q;
        m_cache.clear();
    }
}

std::string SoundBank::resolve(std::string_view name) const {
    const auto it = m_index.find(str::lower(name));
    if (it == m_index.end())
        return {};
    const Variants& v = it->second;
    const std::string& preferred = m_quality == Quality::High ? v.k22 : v.k11;
    const std::string& fallback = m_quality == Quality::High ? v.k11 : v.k22;
    if (!preferred.empty())
        return preferred;
    if (!fallback.empty())
        return fallback;
    return v.other;
}

std::shared_ptr<const SoundBuffer> SoundBank::get(std::string_view name) {
    const std::string key = str::lower(name);
    if (auto it = m_cache.find(key); it != m_cache.end())
        return it->second;
    const std::string path = resolve(key);
    std::shared_ptr<const SoundBuffer> result;
    if (!path.empty()) {
        if (auto bytes = m_vfs.readAll(path)) {
            std::string error;
            if (auto sound = decodeWav(*bytes, &error))
                result = std::make_shared<const SoundBuffer>(std::move(*sound));
            else
                log::warn("audio: {}: {}", path, error);
        }
    }
    if (!result && !m_reportedMissing[key]) {
        m_reportedMissing[key] = true;
        log::warn("audio: sound '{}' not found", name);
    }
    m_cache[key] = result;
    return result;
}

} // namespace mm2::audio
