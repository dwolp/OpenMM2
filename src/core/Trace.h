#pragma once

// OpenMM2 debugging aid (not in MM2): notes of work that can make a frame
// slow (a texture loaded, a ray scene captured), for the app's hitch log
// (app/FrameTiming.h, OPENMM2_DEBUG_HITCHES). Without a sink set, notes cost
// a branch; build the text only when active() says so.

#include <chrono>
#include <string_view>

namespace mm2::trace {

using Sink = void (*)(std::string_view);

// Where the notes go (null: nowhere).
void setSink(Sink sink);
bool active();
void note(std::string_view what);

// Milliseconds since `start`, for the notes.
inline double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace mm2::trace
