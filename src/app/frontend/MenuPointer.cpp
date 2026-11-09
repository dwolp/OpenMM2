// The menus' own mouse pointer (MM2's sfPointer).

#include "app/frontend/Frontend.h"
#include "app/frontend/PopupOptions.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace mm2::app::frontend {

// sfPointer: MenuManager's pointer node. ResChange loads texture/midcursor.tga
// and limits its corner to the screen less 4 pixels; Update declares it to
// the cull manager only when the game is not in a window (gfxPipeline's
// inWindow, set by -window or -max), where DirectDraw's exclusive mode shows
// no Windows cursor; Cull copies it, colour keyed (CopyClippedBitmap with
// transparency), with its top-left corner at the mouse. In a window Windows'
// own cursor shows instead.
//
// OpenMM2 draws it the same way whenever the window is not a decorated one
// (borderless or exclusive full screen) and hides the system cursor there.
// The picture keeps its size on the 640x480 menu screen, as OpenMM2 scales
// the whole menu; MM2 drew it at its pixel size whatever the resolution.
// MM2 also hides it while an IME composition is open; OpenMM2 does not track
// that.
// The race's popups (mmPopup) are menus of the same MenuManager, so they show
// the pointer too.
void drawMenuPointer(Context& ctx, ui::UiFrame& f) {
    if (ctx.display.windowMode == platform::WindowMode::Windowed)
        return;
    ImGui::SetMouseCursor(ImGuiMouseCursor_None); // hides the system cursor next frame
    const ui::UiTexture& t = f.textures.getColorKeyed("texture/midcursor.tga");
    if (!t)
        return;
    const auto extent = ctx.device().outputExtent();
    Vec2 p = ctx.input.mousePosition();
    p.x = std::clamp(p.x, 0.0f, std::max(0.0f, static_cast<float>(extent.width) - 4.0f));
    p.y = std::clamp(p.y, 0.0f, std::max(0.0f, static_cast<float>(extent.height) - 4.0f));
    const Vec2 v = f.overlay.layout().toVirtual(p);
    ui::drawImage(f.overlay, t, v.x, v.y);
}

void drawMenuPointer(Frontend& fe, ui::UiFrame& f) { drawMenuPointer(fe.ctx, f); }

// OpenMM2 (not in MM2, whose menus read no gamepad): a gamepad's left stick
// moves the pointer, faster the further it is pushed (squared, for fine
// aim), at most 1.2 screen heights a second; the hovered item takes the
// focus as under the mouse, so A presses it.
void stickPointer(Context& ctx, double dt) {
    constexpr float kDeadZone = 0.2f;
    constexpr float kSpeed = 1.2f; // screen heights per second at full tilt
    Vec2 stick{};
    for (const auto& pad : ctx.input.gamepads()) {
        const Vec2 v{pad.axes[static_cast<int>(platform::GamepadAxis::LeftX)],
                     pad.axes[static_cast<int>(platform::GamepadAxis::LeftY)]};
        if (v.x * v.x + v.y * v.y > stick.x * stick.x + stick.y * stick.y)
            stick = v;
    }
    const float mag = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (mag <= kDeadZone)
        return;
    const float t = std::min((mag - kDeadZone) / (1.0f - kDeadZone), 1.0f);
    const float height = static_cast<float>(ctx.device().outputExtent().height);
    const float step = t * t * kSpeed * height * static_cast<float>(dt) / mag;
    ctx.input.moveMouse(ctx.window(), {stick.x * step, stick.y * step});
}

} // namespace mm2::app::frontend
