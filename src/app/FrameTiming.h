#pragma once

// OpenMM2 debugging aid (not in MM2): CPU time per frame stage, averaged and
// logged once a second while OPENMM2_DEBUG_TIMING is set. A stage is timed
// by a timing::Scope; stages may nest (the log lists them in the order
// they were first seen, each total including the stages inside it).
//
// OPENMM2_DEBUG_HITCHES (=1, or "=<ms>" from 2 up for a fixed bound) logs every frame that takes
// much longer than usual (over twice the running typical frame and 4 ms
// more, or over the given ms): its time, the typical one, its stages over
// 0.5 ms and the events noted in it (timing::note: textures loaded, the ray
// scene's captures, ...), and a summary every 10 seconds.

#include <chrono>
#include <string_view>

namespace mm2::app::timing {

// Whether OPENMM2_DEBUG_TIMING or OPENMM2_DEBUG_HITCHES is set (read once).
bool enabled();
// Adds `ms` to the stage's total this second (and this frame).
void add(std::string_view stage, double ms);
// Notes something that happened this frame, for the hitch log ("texture
// cw_apt_brk 1.2 ms"). Repeated notes of one kind are counted: the kind is
// the text up to its first space.
void note(std::string_view what);
// Ends a frame; once a second logs each stage's mean per frame.
void endFrame();

class Scope {
public:
    explicit Scope(std::string_view stage) : m_stage(stage) {
        if (enabled())
            m_start = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (enabled())
            add(m_stage, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count());
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::string_view m_stage;
    std::chrono::steady_clock::time_point m_start{};
};

} // namespace mm2::app::timing
