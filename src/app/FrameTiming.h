#pragma once

// OpenMM2 debugging aid (not in MM2): CPU time per frame stage, averaged and
// logged once a second while OPENMM2_DEBUG_TIMING is set. A stage is timed
// by a timing::Scope; stages may nest (the log lists them in the order
// they were first seen, each with its own total, not its children's).

#include <chrono>
#include <string_view>

namespace mm2::app::timing {

// Whether OPENMM2_DEBUG_TIMING is set (read once).
bool enabled();
// Adds `ms` to the stage's total this second.
void add(std::string_view stage, double ms);
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
