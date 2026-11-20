#include "audio/Mm1CdMusic.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

namespace mm2::audio {
namespace {

constexpr std::uint64_t kSector = 2352;           // a raw CD sector: 588 stereo 16-bit frames
constexpr std::uint64_t kSilentGap = 75;          // sectors (1 s) of silence between tracks
constexpr std::uint64_t kMinTrack = 75 * 20;      // shorter pieces are not music
constexpr int kSilentLevel = 24;                  // a sector whose samples stay under this is silent
constexpr double kCdRate = 44100.0;

} // namespace

std::shared_ptr<Mm1CdMusic> Mm1CdMusic::open(const std::filesystem::path& image, int sampleRate) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(image, ec);
    if (ec || size < kSector * 100 || size % kSector != 0)
        return nullptr;
    std::ifstream f(image, std::ios::binary);
    if (!f)
        return nullptr;
    // The data track's end: the ISO volume's size (the primary descriptor, sector 16).
    std::vector<unsigned char> sector(kSector);
    f.seekg(static_cast<std::streamoff>(16 * kSector));
    f.read(reinterpret_cast<char*>(sector.data()), static_cast<std::streamsize>(kSector));
    if (!f)
        return nullptr;
    const std::size_t off = sector[15] == 2 ? 24 : 16; // mode 1 or mode 2 form 1
    if (std::memcmp(sector.data() + off + 1, "CD001", 5) != 0)
        return nullptr;
    std::uint32_t volume = 0;
    std::memcpy(&volume, sector.data() + off + 80, 4);
    const std::uint64_t total = size / kSector;
    if (volume == 0 || volume + 75 * 60 > total)
        return nullptr; // no minute of audio after the data
    std::shared_ptr<Mm1CdMusic> m(new Mm1CdMusic());
    m->m_path = image;
    m->m_rate = sampleRate;
    m->m_audioStart = volume;
    m->m_audioSectors = total - volume;
    m->m_scanner = std::jthread([p = m.get()] { p->scan(); });
    return m;
}

Mm1CdMusic::~Mm1CdMusic() {
    m_generation.fetch_add(1);
    // The jthreads join here.
}

int Mm1CdMusic::trackCount() const {
    std::lock_guard lock(m_tracksMutex);
    return static_cast<int>(m_tracks.size());
}

void Mm1CdMusic::scan() {
    std::ifstream f(m_path, std::ios::binary);
    if (!f)
        return;
    std::vector<Track> tracks;
    constexpr std::uint64_t kBlock = 256; // sectors per read
    std::vector<std::int16_t> buf(kBlock * kSector / 2);
    std::uint64_t trackStart = 0, lastSound = 0, silentRun = 0;
    bool inTrack = false;
    f.seekg(static_cast<std::streamoff>(m_audioStart * kSector));
    for (std::uint64_t base = 0; base < m_audioSectors; base += kBlock) {
        const std::uint64_t n = std::min(kBlock, m_audioSectors - base);
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n * kSector));
        if (!f && !f.eof())
            break;
        for (std::uint64_t s = 0; s < n; ++s) {
            const std::int16_t* p = buf.data() + s * (kSector / 2);
            int peak = 0;
            for (std::size_t i = 0; i < kSector / 2; i += 4)
                peak = std::max(peak, std::abs(static_cast<int>(p[i])));
            const std::uint64_t at = base + s;
            if (peak >= kSilentLevel) {
                if (!inTrack) {
                    inTrack = true;
                    trackStart = at;
                }
                lastSound = at;
                silentRun = 0;
            } else if (inTrack && ++silentRun >= kSilentGap) {
                if (lastSound + 1 - trackStart >= kMinTrack)
                    tracks.push_back({m_audioStart + trackStart, lastSound + 1 - trackStart});
                inTrack = false;
            }
        }
    }
    if (inTrack && lastSound + 1 - trackStart >= kMinTrack)
        tracks.push_back({m_audioStart + trackStart, lastSound + 1 - trackStart});
    {
        std::lock_guard lock(m_tracksMutex);
        m_tracks = std::move(tracks);
    }
    log::info("mm1 music: {} CD audio tracks found in {}", trackCount(), m_path.filename().string());
    m_scanned.store(true);
}

void Mm1CdMusic::play(int track) {
    Track t;
    {
        std::lock_guard lock(m_tracksMutex);
        if (m_tracks.empty())
            return;
        track = ((track % static_cast<int>(m_tracks.size())) + static_cast<int>(m_tracks.size())) %
                static_cast<int>(m_tracks.size());
        t = m_tracks[static_cast<std::size_t>(track)];
    }
    m_current = track;
    log::info("mm1 music: track {} ({:.0f} s)", track + 1, static_cast<double>(t.count) / 75.0);
    const int gen = m_generation.fetch_add(1) + 1;
    m_ended.store(false);
    m_loading.store(true);
    if (m_loader.joinable())
        m_loader.join();
    m_loader = std::jthread([this, gen, t] { load(gen, t); });
}

void Mm1CdMusic::playRandom() {
    const int n = trackCount();
    if (n <= 0)
        return;
    static std::mt19937 rng{std::random_device{}()};
    play(static_cast<int>(rng() % static_cast<unsigned>(n)));
}

void Mm1CdMusic::stop() {
    m_generation.fetch_add(1);
    m_current = -1;
    m_ended.store(false);
    std::lock_guard lock(m_mutex);
    m_pcm.reset();
}

bool Mm1CdMusic::playing() const { return m_current >= 0; }

void Mm1CdMusic::update() {
    if (m_current >= 0 && m_ended.exchange(false))
        play(m_current + 1);
}

void Mm1CdMusic::load(int generation, Track track) {
    std::ifstream f(m_path, std::ios::binary);
    if (!f)
        return;
    auto pcm = std::make_shared<std::vector<std::int16_t>>(track.count * kSector / 2);
    f.seekg(static_cast<std::streamoff>(track.first * kSector));
    constexpr std::uint64_t kBlock = 512;
    for (std::uint64_t done = 0; done < track.count; done += kBlock) {
        if (m_generation.load() != generation)
            return; // another track was asked for
        const std::uint64_t n = std::min(kBlock, track.count - done);
        f.read(reinterpret_cast<char*>(pcm->data() + done * kSector / 2), static_cast<std::streamsize>(n * kSector));
        if (!f)
            return;
    }
    if (m_generation.load() != generation)
        return;
    std::lock_guard lock(m_mutex);
    m_pcm = std::move(pcm);
    m_pos = 0.0;
    m_fade = 0.0f;
    m_loading.store(false);
}

void Mm1CdMusic::render(float* stereo, int frames) {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock || !m_pcm) {
        std::fill(stereo, stereo + static_cast<std::size_t>(frames) * 2, 0.0f);
        return;
    }
    const std::vector<std::int16_t>& pcm = *m_pcm;
    const double step = kCdRate / static_cast<double>(m_rate);
    const double frameCount = static_cast<double>(pcm.size() / 2);
    for (int i = 0; i < frames; ++i) {
        if (m_pos + 1.0 >= frameCount) {
            std::fill(stereo + static_cast<std::size_t>(i) * 2, stereo + static_cast<std::size_t>(frames) * 2, 0.0f);
            m_pcm.reset();
            m_ended.store(true);
            return;
        }
        const auto idx = static_cast<std::size_t>(m_pos);
        const float frac = static_cast<float>(m_pos - static_cast<double>(idx));
        const float gain = std::min(1.0f, m_fade);
        for (int c = 0; c < 2; ++c) {
            const float a = static_cast<float>(pcm[idx * 2 + static_cast<std::size_t>(c)]);
            const float b = static_cast<float>(pcm[(idx + 1) * 2 + static_cast<std::size_t>(c)]);
            stereo[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(c)] =
                (a + (b - a) * frac) * (1.0f / 32768.0f) * gain;
        }
        m_pos += step;
        m_fade += 1.0f / (0.5f * static_cast<float>(m_rate)); // a half second fade in
    }
}

} // namespace mm2::audio
