#include "app/FrameTiming.h"

#include "core/Log.h"

#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace mm2::app::timing {

namespace {
struct Stage {
    std::string name;
    double ms = 0.0;
};
std::vector<Stage> g_stages;
int g_frames = 0;
std::chrono::steady_clock::time_point g_since = std::chrono::steady_clock::now();
} // namespace

bool enabled() {
    static const bool on = std::getenv("OPENMM2_DEBUG_TIMING") != nullptr;
    return on;
}

void add(std::string_view stage, double ms) {
    for (Stage& s : g_stages)
        if (s.name == stage) {
            s.ms += ms;
            return;
        }
    g_stages.push_back({std::string(stage), ms});
}

void endFrame() {
    if (!enabled())
        return;
    ++g_frames;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - g_since).count();
    if (seconds < 1.0)
        return;
    std::string line = std::format("timing: {:.0f} fps, ms per frame:", g_frames / seconds);
    for (Stage& s : g_stages) {
        line += std::format(" {} {:.2f}", s.name, s.ms / g_frames);
        s.ms = 0.0;
    }
    log::info("{}", line);
    g_frames = 0;
    g_since = now;
}

} // namespace mm2::app::timing
