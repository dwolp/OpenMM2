#include "app/FrameTiming.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace mm2::app::timing {

namespace {
struct Stage {
    std::string name;
    double ms = 0.0;      // this second
    double frameMs = 0.0; // this frame
};
std::vector<Stage> g_stages;
int g_frames = 0;
std::chrono::steady_clock::time_point g_since = std::chrono::steady_clock::now();

// The hitch log.
struct Note {
    std::string kind, first;
    int count = 0;
};
std::vector<Note> g_notes; // this frame's
std::chrono::steady_clock::time_point g_frameStart = std::chrono::steady_clock::now();
double g_typical = 0.0; // a running typical frame time (ms)
long long g_frameIndex = 0;
int g_hitches = 0;
double g_worst = 0.0;
std::chrono::steady_clock::time_point g_summarySince = std::chrono::steady_clock::now();

bool timingOn() {
    static const bool on = std::getenv("OPENMM2_DEBUG_TIMING") != nullptr;
    return on;
}
bool hitchesOn() {
    static const bool on = std::getenv("OPENMM2_DEBUG_HITCHES") != nullptr;
    return on;
}
// The fixed threshold (ms), 0 for the relative one.
double hitchThreshold() {
    static const double ms = [] {
        const char* v = std::getenv("OPENMM2_DEBUG_HITCHES");
        const double ms = v && *v ? std::atof(v) : 0.0;
        return ms >= 2.0 ? ms : 0.0; // "1" (or anything else) only turns it on
    }();
    return ms;
}

void endHitchFrame() {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - g_frameStart).count();
    g_frameStart = now;
    ++g_frameIndex;
    // The first frames (loading, the first uploads) set no standard.
    if (g_frameIndex > 30) {
        const double fixed = hitchThreshold();
        const bool hitch = fixed > 0.0 ? ms > fixed : (g_typical > 0.0 && ms > 2.0 * g_typical + 4.0);
        if (hitch) {
            ++g_hitches;
            g_worst = std::max(g_worst, ms);
            std::string line = std::format("hitch: frame {} {:.1f} ms (typical {:.1f}):", g_frameIndex, ms, g_typical);
            for (const Stage& s : g_stages)
                if (s.frameMs >= 0.5)
                    line += std::format(" {} {:.1f}", s.name, s.frameMs);
            for (const Note& n : g_notes)
                line += n.count > 1 ? std::format(" | {} x{} ({}...)", n.kind, n.count, n.first)
                                    : std::format(" | {}", n.first);
            log::info("{}", line);
        }
        // Slow to follow a long frame up, quick to follow a short one down:
        // the typical frame, not the average with its hitches.
        if (!hitch)
            g_typical = g_typical <= 0.0 ? ms : g_typical + (ms - g_typical) * (ms < g_typical ? 0.1 : 0.02);
    } else if (g_frameIndex > 10) {
        g_typical = g_typical <= 0.0 ? ms : g_typical + (ms - g_typical) * 0.2;
    }
    const double since = std::chrono::duration<double>(now - g_summarySince).count();
    if (since >= 10.0) {
        log::info("hitches: {} in the last {:.0f} s, worst {:.1f} ms, typical {:.1f} ms", g_hitches, since, g_worst,
                  g_typical);
        g_hitches = 0;
        g_worst = 0.0;
        g_summarySince = now;
    }
    g_notes.clear();
}
} // namespace

bool enabled() { return timingOn() || hitchesOn(); }

void add(std::string_view stage, double ms) {
    for (Stage& s : g_stages)
        if (s.name == stage) {
            s.ms += ms;
            s.frameMs += ms;
            return;
        }
    g_stages.push_back({std::string(stage), ms, ms});
}

void note(std::string_view what) {
    if (!hitchesOn())
        return;
    const std::string_view kind = what.substr(0, what.find(' '));
    for (Note& n : g_notes)
        if (n.kind == kind) {
            ++n.count;
            return;
        }
    if (g_notes.size() < 16)
        g_notes.push_back({std::string(kind), std::string(what), 1});
}

void endFrame() {
    if (!enabled())
        return;
    if (hitchesOn())
        endHitchFrame();
    for (Stage& s : g_stages)
        s.frameMs = 0.0;
    if (!timingOn())
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
