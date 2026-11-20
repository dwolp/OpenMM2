#pragma once

// OpenMM2 Enhanced: Midtown Madness 1's music. MM1 played it from the disc's CD audio tracks; a raw (2352-byte
// sector) disc image keeps them after the data track. This reads them straight from the image (nothing is copied
// out of it) and streams them to the mixer. The tracks are found by their silence (the image has no table of
// contents): runs of at least a second of silence separate them. Which track plays when is inferred (a random one,
// then the next ones), as MM1's own choice is not decoded.

#include "audio/Mixer.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mm2::audio {

class Mm1CdMusic final : public StreamSource {
public:
    // Null when `image` is not a raw disc image with audio after its data track.
    static std::shared_ptr<Mm1CdMusic> open(const std::filesystem::path& image, int sampleRate);
    ~Mm1CdMusic() override;

    // The tracks found so far (the scan runs in the background right after open()).
    int trackCount() const;
    bool scanned() const { return m_scanned.load(); }
    // Starts a track (0-based) or a random one; loads in the background. Does nothing before the scan is done.
    void play(int track);
    void playRandom();
    void stop();
    bool playing() const;
    // Call every frame: starts the next track when one has ended.
    void update();
    void render(float* stereo, int frames) override;

private:
    struct Track {
        std::uint64_t first = 0, count = 0; // sectors of the image
    };
    Mm1CdMusic() = default;
    void scan();
    void load(int generation, Track track);

    std::filesystem::path m_path;
    int m_rate = 48000;
    std::uint64_t m_audioStart = 0, m_audioSectors = 0;
    std::vector<Track> m_tracks;
    mutable std::mutex m_tracksMutex;
    std::atomic<bool> m_scanned{false};
    std::jthread m_scanner;

    std::mutex m_mutex; // the PCM and the position
    std::shared_ptr<std::vector<std::int16_t>> m_pcm;
    double m_pos = 0.0;
    float m_fade = 0.0f;
    std::atomic<bool> m_ended{false};
    std::atomic<int> m_generation{0};
    std::atomic<bool> m_loading{false};
    int m_current = -1;
    std::jthread m_loader;
};

} // namespace mm2::audio
