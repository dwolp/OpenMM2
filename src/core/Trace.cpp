#include "core/Trace.h"

namespace mm2::trace {

namespace {
Sink g_sink = nullptr;
}

void setSink(Sink sink) { g_sink = sink; }
bool active() { return g_sink != nullptr; }
void note(std::string_view what) {
    if (g_sink)
        g_sink(what);
}

} // namespace mm2::trace
