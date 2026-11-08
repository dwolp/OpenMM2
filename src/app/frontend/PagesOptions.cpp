// Options: the options menu (opt_bk), graphics (gfx_bk), audio (aud_bk),
// controls (ctrl_bk), customize controls (cuss_bk) and about (about_bk).
//
// Widget order and positions follow MM2's menus (OptionsMenu, GraphicsOptions,
// AudioOptions, ControlSetup, ControlCustom, AboutMenu) and tune/widget.csv;
// see docs/frontend.md. MM2 keeps these settings per driver (<driver>.cfg,
// mmPlayerConfig); OpenMM2 keeps them in openmm2.ini so that they apply
// before a driver is chosen.
#include "app/Controls.h"
#include "app/GameInput.h"
#include "app/Tweaks.h"
#include "app/frontend/Frontend.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "platform/Window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>

namespace mm2::app::frontend {
namespace {

using ui::Box;
using ui::SpriteSheet;
using namespace layout;

// Dialogs of the customize page that menu_id does not name.
constexpr int kControlWarningDialog = 21; // ctrl_dlg: "Duplicate key or button"
constexpr int kRefusedDialog = 32;        // xasn_dlg: "You cannot assign this function"

float iniFloat(Context& ctx, const char* section, const char* key, float def, float lo, float hi) {
    return std::clamp(static_cast<float>(ctx.settings.ini.getDouble(section, key, def)), lo, hi);
}
int iniInt(Context& ctx, const char* section, const char* key, int def, int lo, int hi) {
    return static_cast<int>(std::clamp<long long>(ctx.settings.ini.getInt(section, key, def), lo, hi));
}
bool iniBool(Context& ctx, const char* section, const char* key, bool def) {
    return ctx.settings.ini.getBool(section, key, def);
}

// An options sub-page (MM2 `OptionsBase`): widgets 0-2 are DEFAULTS, CANCEL
// and DONE. DEFAULTS asks first (odef_dlg) and applies the defaults without
// saving them; CANCEL and Escape restore the settings the page was entered
// with; DONE keeps and saves them. The strip's OPTIONS runs only the page's
// CancelAction (OptionsBase::IsAnOptionMenu, mmInterface::Update strip id
// 100), then shows the options menu.
class SettingsPage : public Page {
public:
    SettingsPage(Frontend& fe, const char* background, int id)
        : m_savedSettings(fe.ctx.settings), m_savedDisplay(fe.ctx.display), m_savedAutomatic(fe.config.automatic) {
        menuId = id;
        menu.background = background;
        // AudioOptions / GraphicsOptions / ControlSetup::PreSetup end by
        // turning the description label off.
        helpOffOnEntry = id != menu_id::kControlCustom;
        const Vec2 def = fe.layout.position(id, 0, {347, 379});
        const Vec2 can = fe.layout.position(id, 1, kBack);
        const Vec2 dn = fe.layout.position(id, 2, kNext);
        menu.add<ui::SpriteButton>(SpriteSheet{"texture/opt_def.tga", 4}, def.x, def.y, [this, &fe] { askDefaults(fe); })
            .sound = "Selectionmade";
        menu.add<ui::SpriteButton>(SpriteSheet{"texture/opt_can.tga", 4}, can.x, can.y, [this, &fe] { cancel(fe); })
            .sound = "Selectionmade";
        menu.add<ui::SpriteButton>(SpriteSheet{"texture/opt_done.tga", 4}, dn.x, dn.y, [this, &fe] { done(fe); })
            .sound = "Selectionmade";
        menu.onBack = [this, &fe] { cancel(fe); };
    }

protected:
    // Call after the page's own widgets: the strip's OPTIONS button goes back
    // to the options menu through stripOptions; option sub-pages have no PREV.
    void finish(Frontend& fe) { addNavStrip(fe, *this, NavOptions::Cancel, [this, &fe] { stripOptions(fe); }); }

    virtual void resetDefaults(Frontend& fe) = 0;
    // CANCEL (id 500) and Escape (mmInterface::Update): the settings the page
    // was entered with again.
    virtual void cancel(Frontend& fe) {
        restore(fe);
        fe.pop();
    }
    // The page's CancelAction (Audio and Control restore from their
    // snapshot), then the options menu.
    virtual void stripOptions(Frontend& fe) { cancel(fe); }
    void restore(Frontend& fe) {
        fe.ctx.settings = m_savedSettings;
        fe.config.automatic = m_savedAutomatic;
        fe.ctx.applyAudioSettings();
    }
    // DONE (id 0x1f5, mmInterface::Update, after the page's DoneAction).
    virtual void done(Frontend& fe) {
        if (fe.profile && fe.profile->automatic != fe.config.automatic) {
            fe.profile->automatic = fe.config.automatic;
            fe.saveProfile();
        }
        fe.ctx.saveSettings();
        fe.pop();
    }

    Settings m_savedSettings;
    render::DisplaySettings m_savedDisplay;
    bool m_savedAutomatic;

private:
    void askDefaults(Frontend& fe) {
        // DEFAULTS (id 0x1f6, mmInterface::Update): odef_dlg, whose OK runs
        // the page's ResetDefaultAction.
        // odef_dlg: "Are you sure you want to restore the original settings?"
        fe.dialog("jpg/odef_dlg.jpg", menu_id::kDefaults,
                  {{"texture/dlg_ok.tga", {180, 176}, [this, &fe] { resetDefaults(fe); }},
                   {"texture/dlg_can.tga", {18, 176}, {}}});
    }
};

// --- Tweaks (OpenMM2) -----------------------------------------------------------------

// The TWEAKS page's look: flat dark panels, one accent colour, no pictures.
namespace tweaks_ui {
inline std::uint32_t rgba(int r, int g, int b, int a = 255) {
    return render::packColor(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b),
                             static_cast<std::uint8_t>(a));
}
const std::uint32_t kPanel = rgba(13, 15, 24); // opaque: the background's labels must not show
const std::uint32_t kPanelEdge = rgba(255, 255, 255, 26);
const std::uint32_t kAccent = rgba(255, 168, 38);
const std::uint32_t kAccentSoft = rgba(255, 168, 38, 70);
const std::uint32_t kText = rgba(234, 238, 246);
const std::uint32_t kTextDim = rgba(150, 158, 176);
const std::uint32_t kTextOff = rgba(86, 92, 108);
const std::uint32_t kRowCurrent = rgba(255, 255, 255, 24);
const std::uint32_t kRowHover = rgba(255, 255, 255, 11);
const std::uint32_t kPill = rgba(255, 255, 255, 16);
const std::uint32_t kPillHover = rgba(255, 255, 255, 34);
const std::uint32_t kPillText = rgba(20, 20, 26);
const std::uint32_t kTrack = rgba(255, 255, 255, 30);
ui::FontSpec font(int size) { return {"Arial Bold", size, size, 0, 400}; }
} // namespace tweaks_ui

// Every tweak in one scrolling list (app/Tweaks.h): section headers, then
// each choice as a row of pills and its sliders under it. Up/Down move
// between rows (past the first or last, or with Tab, the focus leaves for
// the page's buttons), Left/Right change the current row, Enter steps a
// choice; the mouse clicks pills, drags sliders and scrolls with the wheel.
class TweakList final : public ui::Widget {
public:
    TweakList(Context& ctx, Box b) : m_ctx(ctx) {
        box = b;
        std::string_view section;
        for (const Tweak& t : kTweaks) {
            const std::string_view s = t.kind == TweakKind::Choice ? t.section : section;
            if (s != section) {
                m_rows.push_back({nullptr, s});
                section = s;
            }
            m_rows.push_back({&t, s});
        }
        m_current = nextItem(-1, 1);
    }

    bool modal() const override { return m_engaged; }
    void focusChanged(bool focused) override {
        m_engaged = focused;
        m_dragging = -1;
    }

    // The current row's description, for the page's footer.
    std::string_view help() const {
        const int row = m_hover >= 0 ? m_hover : m_current;
        return row >= 0 && m_rows[static_cast<std::size_t>(row)].tweak ? m_rows[static_cast<std::size_t>(row)].tweak->help
                                                                       : std::string_view{};
    }

    void draw(ui::UiFrame& f, bool focused) override {
        using namespace tweaks_ui;
        const IniFile& ini = m_ctx.settings.ini;
        const float contentH = static_cast<float>(m_rows.size()) * kRowH;
        const Vec4 clip{box.x, box.y, box.w, box.h};
        f.overlay.setClip(&clip);
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            const float y = box.y + static_cast<float>(i) * kRowH - m_scroll;
            if (y + kRowH < box.y || y > box.y + box.h)
                continue;
            const Row& r = m_rows[i];
            if (!r.tweak) {
                // Section header: accent caps and a hairline.
                const float tw = f.text.draw(f.overlay, font(10), r.section, box.x + 10, y + 7, kAccent);
                f.overlay.rect(box.x + 18 + tw, y + 12, box.w - 40 - tw, 1, kPanelEdge);
                continue;
            }
            const Tweak& t = *r.tweak;
            const bool enabled = t.kind == TweakKind::Choice || sliderEnabled(ini, t);
            const bool current = static_cast<int>(i) == m_current && focused;
            if (current) {
                f.overlay.rect(box.x + 2, y + 1, box.w - 12, kRowH - 2, kRowCurrent);
                f.overlay.rect(box.x + 2, y + 1, 2, kRowH - 2, kAccent);
            } else if (static_cast<int>(i) == m_hover) {
                f.overlay.rect(box.x + 2, y + 1, box.w - 12, kRowH - 2, kRowHover);
            }
            const bool child = t.kind == TweakKind::Slider;
            const float lh = f.text.lineHeight(f.overlay, font(child ? 11 : 12));
            f.text.draw(f.overlay, font(child ? 11 : 12), t.label, box.x + (child ? 30 : 14), y + (kRowH - lh) * 0.5f,
                        !enabled ? kTextOff : child ? kTextDim : kText);
            if (t.kind == TweakKind::Choice)
                drawPills(f, t, y, static_cast<int>(i));
            else
                drawSlider(f, t, y, enabled);
        }
        f.overlay.setClip(nullptr);
        // Scroll bar.
        if (contentH > box.h) {
            const float trackH = box.h - 4;
            const float thumbH = std::max(18.0f, trackH * box.h / contentH);
            const float at = (trackH - thumbH) * (m_scroll / (contentH - box.h));
            f.overlay.rect(box.x + box.w - 5, box.y + 2, 3, trackH, kTrack);
            f.overlay.rect(box.x + box.w - 5, box.y + 2 + at, 3, thumbH, kAccentSoft);
        }
    }

    void modalInput(ui::UiFrame& f) override {
        const ui::NavInput& nav = f.nav;
        // Leaving the list: Tab and Escape go on to the page (Escape cancels it).
        if (nav.tabNext || nav.back) {
            m_engaged = false;
            return;
        }
        // The mouse leaving the list hands it to the page's buttons.
        if ((nav.mouseMoved || nav.mousePressed) && !box.contains(nav.mouse) && m_dragging < 0) {
            m_engaged = false;
            m_hover = -1;
            return;
        }
        handleMouse(f);
        if (nav.up)
            step(-1);
        if (nav.down)
            step(1);
        if (nav.left || nav.right)
            adjustRow(f, m_current, nav.right ? 1 : -1, false);
        if (nav.accept)
            adjustRow(f, m_current, 1, true);
    }
    void mouse(ui::UiFrame& f, bool hovered) override {
        if (hovered)
            m_engaged = true; // the pointer is in: the list takes the keys again
        handleMouse(f);
    }

private:
    struct Row {
        const Tweak* tweak = nullptr; // null: a section header
        std::string_view section;
    };
    static constexpr float kRowH = 19.0f;
    float controlX() const { return box.x + box.w * 0.46f; }
    float controlW() const { return box.w * 0.54f - 16.0f; }

    void drawPills(ui::UiFrame& f, const Tweak& t, float y, int row) {
        using namespace tweaks_ui;
        const int value = tweakValue(m_ctx.settings.ini, t.key);
        for (int c = 0; c < t.count; ++c) {
            const Box p = pill(t, y, c);
            const bool on = c == value;
            const bool hover = row == m_hover && p.contains(m_mouse);
            f.overlay.rect(p.x, p.y, p.w, p.h, on ? kAccent : hover ? kPillHover : kPill);
            const std::string_view name = t.choices[static_cast<std::size_t>(c)];
            const float tw = f.text.measure(f.overlay, font(10), name);
            const float lh = f.text.lineHeight(f.overlay, font(10));
            f.text.draw(f.overlay, font(10), name, p.x + (p.w - tw) * 0.5f, p.y + (p.h - lh) * 0.5f,
                        on ? kPillText : kTextDim);
        }
    }
    Box pill(const Tweak& t, float y, int c) const {
        const float gap = 3.0f;
        const float w = (controlW() - gap * static_cast<float>(t.count - 1)) / static_cast<float>(t.count);
        return {controlX() + (w + gap) * static_cast<float>(c), y + 3, w, kRowH - 6};
    }

    void drawSlider(ui::UiFrame& f, const Tweak& t, float y, bool enabled) {
        using namespace tweaks_ui;
        const IniFile& ini = m_ctx.settings.ini;
        const float v = sliderValue(ini, t);
        const Box tr = track(y);
        const float k = (v - t.min) / (t.max - t.min);
        f.overlay.rect(tr.x, tr.y, tr.w, tr.h, kTrack);
        f.overlay.rect(tr.x, tr.y, tr.w * k, tr.h, enabled ? kAccent : kTextOff);
        f.overlay.rect(tr.x + tr.w * k - 3, y + 4, 6, kRowH - 8, enabled ? kText : kTextOff);
        // The value: accent when overridden, dim at its preset.
        const std::string text = sliderText(t, v);
        const float tw = f.text.measure(f.overlay, font(10), text);
        const float lh = f.text.lineHeight(f.overlay, font(10));
        f.text.draw(f.overlay, font(10), text, controlX() + controlW() - tw, y + (kRowH - lh) * 0.5f,
                    !enabled ? kTextOff : sliderOverridden(ini, t) ? kAccent : kTextDim);
    }
    Box track(float y) const { return {controlX() + 4, y + kRowH * 0.5f - 1.5f, controlW() - 58, 3}; }

    int rowAt(Vec2 p) const {
        if (!box.contains(p))
            return -1;
        const int i = static_cast<int>((p.y - box.y + m_scroll) / kRowH);
        return i >= 0 && i < static_cast<int>(m_rows.size()) && m_rows[static_cast<std::size_t>(i)].tweak ? i : -1;
    }
    float rowY(int i) const { return box.y + static_cast<float>(i) * kRowH - m_scroll; }

    void handleMouse(ui::UiFrame& f) {
        const ui::NavInput& nav = f.nav;
        m_mouse = nav.mouse;
        if (nav.wheel != 0.0f && box.contains(nav.mouse))
            scrollBy(-nav.wheel * kRowH * 3.0f);
        if (m_dragging >= 0) {
            if (nav.mouseDown)
                dragTo(m_dragging, nav.mouse.x);
            else
                m_dragging = -1;
            return;
        }
        m_hover = rowAt(nav.mouse);
        if (!nav.mousePressed || m_hover < 0)
            return;
        m_current = m_hover;
        const Tweak& t = *m_rows[static_cast<std::size_t>(m_hover)].tweak;
        const float y = rowY(m_hover);
        if (t.kind == TweakKind::Choice) {
            for (int c = 0; c < t.count; ++c)
                if (pill(t, y, c).contains(nav.mouse) && c != tweakValue(m_ctx.settings.ini, t.key)) {
                    setChoice(m_ctx.settings.ini, t, c);
                    f.play("Selectionmade", 0.75f);
                }
        } else if (sliderEnabled(m_ctx.settings.ini, t)) {
            const Box tr = track(y);
            if (nav.mouse.x >= tr.x - 6 && nav.mouse.x <= tr.x + tr.w + 6) {
                m_dragging = m_hover;
                dragTo(m_hover, nav.mouse.x);
            }
        }
    }
    void dragTo(int row, float x) {
        const Tweak& t = *m_rows[static_cast<std::size_t>(row)].tweak;
        const Box tr = track(rowY(row));
        const float k = std::clamp((x - tr.x) / tr.w, 0.0f, 1.0f);
        float v = t.min + (t.max - t.min) * k;
        v = t.min + std::round((v - t.min) / t.step) * t.step;
        setSlider(m_ctx.settings.ini, t, v);
    }

    void adjustRow(ui::UiFrame& f, int row, int dir, bool wrap) {
        if (row < 0)
            return;
        const Tweak& t = *m_rows[static_cast<std::size_t>(row)].tweak;
        IniFile& ini = m_ctx.settings.ini;
        if (t.kind == TweakKind::Choice) {
            int c = tweakValue(ini, t.key) + dir;
            c = wrap ? (c + t.count) % t.count : std::clamp(c, 0, t.count - 1);
            if (c != tweakValue(ini, t.key)) {
                setChoice(ini, t, c);
                f.play("Selectionmade", 0.75f);
            }
        } else if (sliderEnabled(ini, t) && !wrap) {
            setSlider(ini, t, sliderValue(ini, t) + t.step * static_cast<float>(dir));
        }
    }

    int nextItem(int from, int dir) const {
        for (int i = from + dir; i >= 0 && i < static_cast<int>(m_rows.size()); i += dir)
            if (m_rows[static_cast<std::size_t>(i)].tweak)
                return i;
        return -1;
    }
    void step(int dir) {
        const int next = nextItem(m_current, dir);
        if (next < 0) {
            m_engaged = false; // past the first or last row: the page's buttons
            return;
        }
        m_current = next;
        m_hover = -1;
        // Keep it in view, with its header when it is the first row of a section.
        const float top = static_cast<float>(next - (next > 0 && !m_rows[static_cast<std::size_t>(next - 1)].tweak ? 1 : 0)) * kRowH;
        const float bottom = static_cast<float>(next + 1) * kRowH;
        if (top < m_scroll)
            scrollBy(top - m_scroll);
        else if (bottom > m_scroll + box.h)
            scrollBy(bottom - m_scroll - box.h);
    }
    void scrollBy(float d) {
        const float maxScroll = std::max(0.0f, static_cast<float>(m_rows.size()) * kRowH - box.h);
        m_scroll = std::clamp(m_scroll + d, 0.0f, maxScroll);
    }

    Context& m_ctx;
    std::vector<Row> m_rows;
    int m_current = -1;
    int m_hover = -1;
    int m_dragging = -1;
    float m_scroll = 0.0f;
    bool m_engaged = false;
    Vec2 m_mouse{-1e9f, -1e9f};
};

// Graphics options -> TWEAKS: OpenMM2's optional visual enhancements
// (app/Tweaks.h), not in MM2, in one panel over the graphics background.
// DEFAULTS turns every tweak back to the original look and clears the
// overrides, CANCEL restores what the page was entered with, DONE keeps and
// saves (then `onDone`, which updates the graphics page's snapshot).
class TweaksPage final : public SettingsPage {
public:
    TweaksPage(Frontend& fe, std::function<void()> onDone)
        : SettingsPage(fe, "jpg/gfx_bk.jpg", menu_id::kGraphics), m_onDone(std::move(onDone)) {
        m_list = &menu.add<TweakList>(fe.ctx, Box{kPanelX + 6, kPanelY + 36, kPanelW - 12, kPanelH - 64});
        finish(fe);
        menu.setInitialFocus(m_list);
    }

    void drawBelow(Frontend&, ui::UiFrame& f) override {
        using namespace tweaks_ui;
        // The panel: opaque (the background's own labels must not show
        // through), ending above DEFAULTS (347, 379).
        f.overlay.rect(kPanelX, kPanelY, kPanelW, kPanelH, kPanel);
        f.overlay.rect(kPanelX, kPanelY, kPanelW, 2, kAccent);
        f.text.draw(f.overlay, font(16), "TWEAKS", kPanelX + 14, kPanelY + 12, kText);
        const std::string_view note = "OpenMM2 extras  -  off is the original look";
        const float nw = f.text.measure(f.overlay, font(10), note);
        f.text.draw(f.overlay, font(10), note, kPanelX + kPanelW - 14 - nw, kPanelY + 16, kTextDim);
        f.overlay.rect(kPanelX + 10, kPanelY + 33, kPanelW - 20, 1, kPanelEdge);
        // The footer: what the current row does.
        f.overlay.rect(kPanelX + 10, kPanelY + kPanelH - 26, kPanelW - 20, 1, kPanelEdge);
        f.text.draw(f.overlay, font(10), m_list->help(), kPanelX + 14, kPanelY + kPanelH - 19, kTextDim);
        const std::string_view keys = "Up/Down  move     Left/Right  change";
        const float kw = f.text.measure(f.overlay, font(9), keys);
        f.text.draw(f.overlay, font(9), keys, kPanelX + kPanelW - 14 - kw, kPanelY + kPanelH - 18, kTextOff);
    }

protected:
    void resetDefaults(Frontend& fe) override {
        for (const Tweak& t : kTweaks)
            fe.ctx.settings.ini.remove(kTweaksSection, t.key);
    }
    void done(Frontend& fe) override {
        if (m_onDone)
            m_onDone();
        SettingsPage::done(fe);
    }

private:
    static constexpr float kPanelX = 36, kPanelY = 54, kPanelW = 580, kPanelH = 318;

    TweakList* m_list = nullptr;
    std::function<void()> m_onDone;
};

std::unique_ptr<Page> makeTweaksPage(Frontend& fe, std::function<void()> onDone) {
    return std::make_unique<TweaksPage>(fe, std::move(onDone));
}

// --- Options --------------------------------------------------------------------------

// OptionsMenu (menu 2): ABOUT, AUDIO, CONTROLS, GRAPHICS, then PREV.
class OptionsPage final : public Page {
public:
    explicit OptionsPage(Frontend& fe) {
        menuId = menu_id::kOptions;
        menu.background = "jpg/opt_bk.jpg";
        helpOffOnEntry = true; // OptionsMenu::PreSetup turns the description label off
        auto add = [&](int index, const char* sprite, Vec2 code, const char* help, std::function<void()> fn) {
            const Vec2 p = fe.layout.position(menu_id::kOptions, index, code);
            auto& b = menu.add<ui::SpriteButton>(SpriteSheet{sprite, 4}, p.x, p.y, std::move(fn));
            b.help = help;
            return &b;
        };
        auto* about = add(0, "texture/opt_abt.tga", {25, 328}, "jpg/opt_tabt.jpg", [&fe] { fe.push(makeAboutPage(fe)); });
        add(1, "texture/opt_aud.tga", {kColumnX, kRow58}, "jpg/opt_taud.jpg", [&fe] { fe.push(makeAudioPage(fe)); });
        add(2, "texture/opt_ctl.tga", {kColumnX, kRow56}, "jpg/opt_tctl.jpg", [&fe] { fe.push(makeControlPage(fe)); });
        add(3, "texture/opt_gfx.tga", {kColumnX, kRow65}, "jpg/opt_tgfx.jpg", [&fe] { fe.push(makeGraphicsPage(fe)); });
        // Widget 4 is the hidden "mnav_prev" hotspot that places the strip's PREV.
        addBack(fe, *this);
        addNavStrip(fe, *this, NavOptions::Lit);
        menu.setInitialFocus(about); // UIMenu::Enable: widget 0
    }
};

// --- Graphics -------------------------------------------------------------------------

struct Resolution {
    int w, h;
    float hz;
};

// [Graphics] keys and their MM2 defaults: the top tier of
// mmGfxCFG::AutoDetect (a fast machine with a 3D card).
constexpr float kDefaultFarClip = 1000.0f; // camera far plane, 100..1000 m
constexpr int kDefaultLighting = 3;        // cityLevel::sm_LightQuality 0..3
constexpr int kDefaultTexture = 2;         // AutoDetect never picks Very High
constexpr int kDefaultObjectDetail = 3;
constexpr int kDefaultCloudShadows = 2;

class GraphicsPage final : public SettingsPage {
public:
    explicit GraphicsPage(Frontend& fe)
        : SettingsPage(fe, "jpg/gfx_bk.jpg", menu_id::kGraphics), m_pending(fe.ctx.display) {
        Context& ctx = fe.ctx;
        const auto& s = ctx.game->strings;
        const int id = menu_id::kGraphics;
        m_displays = platform::enumerateDisplays();

        // 3-6: toggles (silent). SMART RENDERING (gfx_port) is created and
        // turned off at once in MM2, so it is never shown; the setting stays on.
        addToggle(fe, 3, "texture/gfx_sky.tga", 62, "TexturedSky", "jpg/gfx_tsky.jpg");
        addToggle(fe, 4, "texture/gfx_rflx.tga", 89, "VehicleReflections", "jpg/gfx_tfl.jpg");
        addToggle(fe, 5, "texture/gfx_peds.tga", 116, "ShowPedestrians", "jpg/gfx_tped.jpg");
        addToggle(fe, 6, "texture/gfx_port.tga", 143, "SmartRendering", "").visible = false;

        // 7-9: DISPLAY, RENDERER, RESOLUTION. MM2 lists display adapters,
        // software/hardware renderers and "W x H (N bit color)" modes here;
        // OpenMM2 uses the slots for the window mode, Vulkan/OpenGL and the
        // window size or exclusive mode. MM2's DISPLAY help picture
        // (gfx_td...) is missing from the data.
        menu.add<ui::ValueBox>(
            fe.layout.widget(id, 7, {kBoxX, 62, kBoxWide, kBoxH}),
            [] { return std::vector<std::string>{"Window", "Full Screen (Desktop)", "Full Screen (Exclusive)"}; },
            [this] { return static_cast<int>(m_pending.windowMode); },
            [this](int i) {
                m_pending.windowMode = static_cast<platform::WindowMode>(i);
                m_resolutions = resolutions();
            });
        menu.add<ui::ValueBox>(
                fe.layout.widget(id, 8, {kBoxX, 100, kBoxWide, kBoxH}),
                [&ctx] {
                    const std::string active = render::backendName(ctx.device().backend());
                    return std::vector<std::string>{"Automatic (" + active + ")", "Vulkan", "OpenGL"};
                },
                [this] { return static_cast<int>(m_pending.backend); },
                [this](int i) { m_pending.backend = static_cast<render::Backend>(i); })
            .help = "jpg/gfx_tren.jpg";
        m_resolutions = resolutions();
        auto& resolution = menu.add<ui::ValueBox>(
            fe.layout.widget(id, 9, {kBoxX, 135, kBoxWide, kBoxH}),
            [this] {
                std::vector<std::string> v;
                for (const auto& r : m_resolutions)
                    v.push_back(r.w == 0 ? std::string("Desktop")
                                         : (r.hz > 0 ? std::format("{} x {} ({:.0f} Hz)", r.w, r.h, r.hz)
                                                     : std::format("{} x {}", r.w, r.h)));
                return v;
            },
            [this] { return currentResolution(); }, [this](int i) { setResolution(i); });
        resolution.help = "jpg/gfx_tres.jpg";

        // 10-11: FAR CLIP (VISIBILITY) in metres and LIGHTING QUALITY 0-3.
        // Lighting snaps to whole steps (GraphicsOptions::SetLightQuality):
        // a raised value becomes the whole part of value + 1, a lowered one
        // its whole part, kept within 0..3.
        menu.add<ui::Slider>(
                fe.layout.widget(id, 10, {450, 179, 184, 29}),
                [&ctx] { return iniFloat(ctx, "Graphics", "FarClip", kDefaultFarClip, 100.0f, 1000.0f); },
                [&ctx](float v) { ctx.settings.ini.setDouble("Graphics", "FarClip", v); }, 100.0f, 1000.0f)
            .help = "jpg/gfx_tfp.jpg";
        menu.add<ui::Slider>(
                fe.layout.widget(id, 11, {450, 213, 184, 29}),
                [&ctx] {
                    return static_cast<float>(iniInt(ctx, "Graphics", "LightingQuality", kDefaultLighting, 0, 3));
                },
                [&ctx](float v) {
                    const int old = iniInt(ctx, "Graphics", "LightingQuality", kDefaultLighting, 0, 3);
                    if (v == static_cast<float>(old))
                        return;
                    const int snapped = static_cast<int>(v > static_cast<float>(old) ? v + 1.0f : v);
                    ctx.settings.ini.setInt("Graphics", "LightingQuality", std::clamp(snapped, 0, 3));
                },
                0.0f, 3.0f)
            .help = "jpg/gfx_tlq.jpg";

        // 12-14: TEXTURE QUALITY (390-393, " - Recommended" (389) after the
        // level AutoDetect picks), OBJECT DETAIL (574-577), CLOUD SHADOWS
        // (660, 661, 576).
        std::vector<std::string> texture = {s.get(390, "Low"), s.get(391, "Medium"), s.get(392, "High"),
                                            s.get(393, "Very High [AGP]")};
        texture[kDefaultTexture] += s.get(389, " - Recommended");
        addChoice(fe, 12, Box{kBoxX, 261, kBoxWide, kBoxH}, "TextureQuality", texture, kDefaultTexture,
                  "jpg/gfx_ttq.jpg");
        addChoice(fe, 13, Box{kBoxX, 298, kBoxMid, kBoxH}, "ObjectDetail",
                  {s.get(574, "Low"), s.get(575, "Medium"), s.get(576, "High"), s.get(577, "Very High")},
                  kDefaultObjectDetail, "jpg/gfx_tobj.jpg");
        addChoice(fe, 14, Box{kBoxX, 333, kBoxMid, kBoxH}, "CloudShadows",
                  {s.get(660, "None"), s.get(661, "Low"), s.get(576, "High")}, kDefaultCloudShadows,
                  "jpg/gfx_shd.jpg");

        addAdvanced(fe);
        finish(fe);
        // GraphicsOptions::GraphicsOptions makes RESOLUTION the initial focus
        // (SetFocusWidget right after creating it).
        menu.setInitialFocus(&resolution);
    }

    void drawBelow(Frontend&, ui::UiFrame& f) override {
        // Panel for the options MM2 did not have, on the empty lower-left art.
        f.overlay.rect(30, 192, 242, 188, render::packColor(8, 2, 46, 215));
        const auto font = ui::style::smallFont();
        const char* labels[] = {"VSYNC", "ANTI-ALIASING", "RENDER SCALE", "UI SCALE", "FIELD OF VIEW", "TWEAKS"};
        for (int i = 0; i < 6; ++i)
            f.text.draw(f.overlay, font, labels[i], 38, 200 + 28.0f * static_cast<float>(i) + 4, ui::style::kValueText);
    }

protected:
    // GraphicsOptions::ResetDefaultAction.
    void resetDefaults(Frontend& fe) override {
        m_pending = render::DisplaySettings{};
        m_pending.backend = fe.ctx.display.backend;
        for (const char* k : {"FarClip", "LightingQuality", "TextureQuality", "ObjectDetail", "CloudShadows",
                              "TexturedSky", "VehicleReflections", "ShowPedestrians", "SmartRendering"})
            fe.ctx.settings.ini.remove("Graphics", k);
        m_resolutions = resolutions();
    }

    // GraphicsOptions::CancelAction is empty, and only CANCEL and Escape
    // restore the graphics (mmInterface::Update, mmPlayerConfig::SetGraphics):
    // from the strip's OPTIONS the changed options stay (MM2 writes them into
    // the driver's file at the next race start, mmInterface::BeDone; OpenMM2
    // saves them with the settings). The display mode, like MM2's
    // DoneAction, changes only on DONE.
    void stripOptions(Frontend& fe) override { fe.pop(); }

    // GraphicsOptions::DoneAction: the display mode changes here.
    void done(Frontend& fe) override {
        Context& ctx = fe.ctx;
        m_pending.sanitize();
        const render::DisplaySettings old = ctx.display;
        // Backend, GPU and validation changes need a new renderer; everything
        // else applies live, between frames (outside any render pass).
        const bool restart = old.backend != m_pending.backend || old.gpu != m_pending.gpu ||
                             old.validation != m_pending.validation;
        ctx.afterFrame.push_back([&ctx, old, next = m_pending] { render::applyDisplaySettings(ctx.renderer, old, next); });
        ctx.display = m_pending;
        ctx.saveSettings();
        if (restart)
            fe.message("The new renderer takes effect after restarting OpenMM2.", [&fe] { fe.pop(); });
        else
            fe.pop();
    }

private:
    void addChoice(Frontend& fe, int index, Box code, const char* key, std::vector<std::string> options, int def,
                   const char* help) {
        Context& ctx = fe.ctx;
        menu.add<ui::ValueBox>(
                fe.layout.widget(menu_id::kGraphics, index, code), [options] { return options; },
                [&ctx, key, def, n = static_cast<int>(options.size())] {
                    return iniInt(ctx, "Graphics", key, def, 0, n - 1);
                },
                [&ctx, key](int i) { ctx.settings.ini.setInt("Graphics", key, i); })
            .help = help;
    }

    ui::LampItem& addToggle(Frontend& fe, int index, const char* sprite, float y, const char* key, const char* help) {
        Context& ctx = fe.ctx;
        const Vec2 p = fe.layout.position(menu_id::kGraphics, index, {kLampX, y});
        auto& lamp = menu.add<ui::LampItem>(
            SpriteSheet{sprite, 5}, p.x, p.y, [&ctx, key] { return iniBool(ctx, "Graphics", key, true); },
            [&ctx, key] { ctx.settings.ini.setBool("Graphics", key, !iniBool(ctx, "Graphics", key, true)); });
        lamp.help = help;
        return lamp;
    }

    void addAdvanced(Frontend& fe) {
        const float x = 140, w = 128, h = 23;
        auto row = [](int i) { return 200 + 28.0f * static_cast<float>(i); };
        menu.add<ui::ValueBox>(
            Box{x, row(0), w, h}, [] { return std::vector<std::string>{"Off", "On", "Adaptive", "Fast (Mailbox)"}; },
            [this] { return static_cast<int>(m_pending.vsync); },
            [this](int i) { m_pending.vsync = static_cast<render::VsyncMode>(i); });
        menu.add<ui::ValueBox>(
            Box{x, row(1), w, h},
            [&fe] {
                std::vector<std::string> v;
                for (auto s : fe.ctx.device().info().msaaSamples)
                    if (s <= 8)
                        v.push_back(s == 1 ? "Off" : std::format("{}x MSAA", s));
                return v;
            },
            [this, &fe] {
                const auto& s = fe.ctx.device().info().msaaSamples;
                for (std::size_t i = 0; i < s.size(); ++i)
                    if (s[i] == m_pending.msaa)
                        return static_cast<int>(i);
                return 0;
            },
            [this, &fe](int i) { m_pending.msaa = fe.ctx.device().info().msaaSamples[static_cast<std::size_t>(i)]; });
        static const float scales[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
        menu.add<ui::ValueBox>(
            Box{x, row(2), w, h},
            [] {
                std::vector<std::string> v;
                for (float s : scales)
                    v.push_back(std::format("{:.0f}%", s * 100));
                return v;
            },
            [this] {
                int best = 2;
                for (int i = 0; i < 6; ++i)
                    if (std::abs(scales[i] - m_pending.renderScale) < std::abs(scales[best] - m_pending.renderScale))
                        best = i;
                return best;
            },
            [this](int i) { m_pending.renderScale = scales[i]; });
        menu.add<ui::ValueBox>(
            Box{x, row(3), w, h}, [] { return std::vector<std::string>{"Fit (4:3)", "Stretch", "Pixel Exact"}; },
            [this] { return static_cast<int>(m_pending.uiScale); },
            [this](int i) { m_pending.uiScale = static_cast<render::UiScaleMode>(i); });
        menu.add<ui::ValueBox>(
            Box{x, row(4), w, h}, [] { return std::vector<std::string>{"Hor+ (wide)", "Vert- (4:3)", "Stretched"}; },
            [this] { return static_cast<int>(m_pending.fovMode); },
            [this](int i) { m_pending.fovMode = static_cast<render::FovMode>(i); });
        // OpenMM2 tweaks (app/Tweaks.h) on their own page. DONE there also
        // updates this page's snapshot, so this page's CANCEL keeps them.
        auto& tweaks = menu.add<ui::TextButton>(Box{x, row(5), w, h}, "OPEN", [this, &fe] {
            fe.push(makeTweaksPage(fe, [this, &fe] { copyTweaks(fe.ctx.settings.ini, m_savedSettings.ini); }));
        });
        tweaks.type = 1;
        tweaks.font = ui::style::smallFont();
    }

    std::vector<Resolution> resolutions() const {
        std::vector<Resolution> v;
        const int d = std::clamp(m_pending.display, 0, std::max(0, static_cast<int>(m_displays.size()) - 1));
        switch (m_pending.windowMode) {
        case platform::WindowMode::Borderless: v.push_back({0, 0, 0}); break;
        case platform::WindowMode::Fullscreen:
            v.push_back({0, 0, 0});
            if (!m_displays.empty())
                for (const auto& m : m_displays[static_cast<std::size_t>(d)].modes)
                    v.push_back({m.width, m.height, m.refreshRate});
            break;
        case platform::WindowMode::Windowed: {
            const int maxW = m_displays.empty() ? 7680 : m_displays[static_cast<std::size_t>(d)].width;
            const int maxH = m_displays.empty() ? 4320 : m_displays[static_cast<std::size_t>(d)].height;
            const int sizes[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 960},
                                    {1366, 768},  {1600, 900},  {1600, 1200}, {1920, 1080}, {2560, 1080},
                                    {2560, 1440}, {3440, 1440}, {3840, 2160}};
            for (const auto& s : sizes)
                if (s[0] <= maxW && s[1] <= maxH)
                    v.push_back({s[0], s[1], 0});
            if (std::ranges::none_of(v, [&](const Resolution& r) {
                    return r.w == m_pending.windowWidth && r.h == m_pending.windowHeight;
                }))
                v.push_back({m_pending.windowWidth, m_pending.windowHeight, 0});
            break;
        }
        }
        return v;
    }

    int currentResolution() const {
        for (std::size_t i = 0; i < m_resolutions.size(); ++i) {
            const auto& r = m_resolutions[i];
            if (m_pending.windowMode == platform::WindowMode::Windowed) {
                if (r.w == m_pending.windowWidth && r.h == m_pending.windowHeight)
                    return static_cast<int>(i);
            } else if (r.w == m_pending.fullscreenMode.width && r.h == m_pending.fullscreenMode.height &&
                       (r.hz == 0 || std::abs(r.hz - m_pending.fullscreenMode.refreshRate) < 0.5f)) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }

    void setResolution(int i) {
        const auto& r = m_resolutions[static_cast<std::size_t>(i)];
        if (m_pending.windowMode == platform::WindowMode::Windowed) {
            m_pending.windowWidth = r.w;
            m_pending.windowHeight = r.h;
        } else if (r.w == 0) {
            m_pending.fullscreenMode = {};
        } else {
            for (const auto& d : m_displays)
                for (const auto& m : d.modes)
                    if (m.width == r.w && m.height == r.h && m.refreshRate == r.hz)
                        m_pending.fullscreenMode = m;
        }
    }

    render::DisplaySettings m_pending;
    std::vector<platform::DisplayInfo> m_displays;
    std::vector<Resolution> m_resolutions;
};

// --- Audio --------------------------------------------------------------------------------

class AudioPage final : public SettingsPage {
public:
    explicit AudioPage(Frontend& fe) : SettingsPage(fe, "jpg/aud_bk.jpg", menu_id::kAudio) {
        Context& ctx = fe.ctx;
        const auto& s = ctx.game->strings;
        const int id = menu_id::kAudio;
        auto& st = ctx.settings;

        // 3-6: SOUND FX, COMMENTARY, MUSIC, CITY SOUNDS (silent toggles,
        // greyed without a sound device). Music and city sounds exclude each
        // other (AudioOptions::ToggleMusic / ToggleAmbient); both may be off.
        addToggle(fe, 3, "texture/aud_fx.tga", 62, "jpg/aud_tfx.jpg", [&st] { return st.soundEffects; },
                  [&st] { st.soundEffects = !st.soundEffects; });
        addToggle(fe, 4, "texture/aud_com.tga", 91, "jpg/aud_tcom.jpg", [&st] { return st.commentary; },
                  [&st] { st.commentary = !st.commentary; });
        addToggle(fe, 5, "texture/aud_musc.tga", 125, "jpg/aud_tmus.jpg", [&st] { return st.music; }, [&st] {
            st.music = !st.music;
            if (st.music)
                st.citySounds = false;
        });
        addToggle(fe, 6, "texture/aud_amb.tga", 152, "jpg/aud_tcty.jpg", [&st] { return st.citySounds; }, [&st] {
            st.citySounds = !st.citySounds;
            if (st.citySounds)
                st.music = false;
        });

        // 7-9: DEVICE (initial focus: AudioOptions::AudioOptions sets it
        // with SetFocusWidget), STEREO FX (326 Mono / 327 Stereo), SOUND
        // QUALITY (574-576). Quality sets MM2's channel count, 8/16/32
        // (AudioOptions::SetQuality); the sounds are always the 22 kHz ones
        // (InitAudioManager). OpenMM2's mixer has no channel limit to set, so
        // the choice is stored only.
        auto& device = menu.add<ui::ValueBox>(
            fe.layout.widget(id, 7, {kBoxX, 62, kBoxWide, kBoxH}),
            [&ctx] {
                const std::string n = ctx.audioDevice.deviceName();
                return std::vector<std::string>{n.empty() ? std::string("No sound device") : n};
            },
            [] { return 0; }, [](int) {});
        // STEREO FX: Mono, Stereo, and Surround on a 16-bit device (string
        // 329; 328 "Surround (Not Recommended)" on an 8-bit one), as
        // AudioOptions::AudioOptions builds the list; OpenMM2's output is
        // never 8-bit.
        menu.add<ui::ValueBox>(
            fe.layout.widget(id, 8, {kBoxX, 100, kBoxMid, kBoxH}),
            [o = std::vector<std::string>{s.get(326, "Mono"), s.get(327, "Stereo"), s.get(329, "Surround")}] {
                return o;
            },
            [&st] { return st.stereoFx; }, [&st](int i) { st.stereoFx = i; });
        menu.add<ui::ValueBox>(
            fe.layout.widget(id, 9, {kBoxX, 134, kBoxMid, kBoxH}),
            [lo = s.get(574, "Low"), mid = s.get(575, "Medium"), hi = s.get(576, "High")] {
                return std::vector<std::string>{lo, mid, hi};
            },
            [&st] { return st.soundQuality; }, [&st](int i) { st.soundQuality = i; });

        // 10-12: SOUND FX VOLUME (effects, engines, voices), MUSIC/CITY
        // VOLUME (music and ambience), BALANCE -1..1 with the normal arrows.
        // MM2 maps the volumes through a log-200 curve
        // (AudManager::AssignWaveVolume); OpenMM2's mixer gains stay linear.
        menu.add<ui::Slider>(fe.layout.widget(id, 10, {450, 212, 183, 29}), [&st] { return st.effectsVolume; },
                             [&ctx](float v) {
                                 ctx.settings.effectsVolume = ctx.settings.engineVolume = ctx.settings.voiceVolume = v;
                                 ctx.applyAudioSettings();
                             });
        menu.add<ui::Slider>(fe.layout.widget(id, 11, {450, 246, 183, 29}), [&st] { return st.musicVolume; },
                             [&ctx](float v) {
                                 ctx.settings.musicVolume = v;
                                 ctx.applyAudioSettings();
                             });
        menu.add<ui::Slider>(
            fe.layout.widget(id, 12, {450, 280, 183, 29}), [&st] { return st.balance; },
            [&ctx](float v) {
                ctx.settings.balance = v;
                ctx.applyAudioSettings();
            },
            -1.0f, 1.0f);
        finish(fe);
        menu.setInitialFocus(&device);
    }

protected:
    void resetDefaults(Frontend& fe) override {
        // AudioOptions::ResetDefaultAction: volumes 1, 1, balance 0; sound FX
        // and commentary on, music off, city sounds on, stereo, high quality.
        // Applied at once, saved only with DONE.
        auto& st = fe.ctx.settings;
        const Settings d;
        st.effectsVolume = st.engineVolume = st.voiceVolume = 1.0f;
        st.musicVolume = 1.0f;
        st.balance = 0.0f;
        st.soundEffects = d.soundEffects;
        st.commentary = d.commentary;
        st.music = d.music;
        st.citySounds = d.citySounds;
        st.stereoFx = d.stereoFx;
        st.soundQuality = d.soundQuality;
        fe.ctx.applyAudioSettings();
    }

private:
    void addToggle(Frontend& fe, int index, const char* sprite, float y, const char* help, std::function<bool()> on,
                   std::function<void()> flip) {
        Context& ctx = fe.ctx;
        const Vec2 p = fe.layout.position(menu_id::kAudio, index, {kLampX, y});
        auto& lamp = menu.add<ui::LampItem>(SpriteSheet{sprite, 5}, p.x, p.y, std::move(on),
                                            [&ctx, flip = std::move(flip)] {
                                                flip();
                                                ctx.applyAudioSettings();
                                            });
        lamp.help = help;
        lamp.enabled = ctx.audioDevice.isOpen();
    }
};

// --- Controls --------------------------------------------------------------------------------

// MM2's five controller types (mmInput devices 0-4; strings 580-584).
enum class Controller { Mouse, Keyboard, Joystick, GamePad, Wheel };

// [Controls] keys in MM2 units, with mmPlayerConfig::DefaultControls values.
struct ControlDefaults {
    static constexpr float kSensitivity = 1.0f; // 0.5 .. 2.0 (input gain is its inverse)
    static constexpr float kDeadZone = 0.1f;    // 0 .. 0.33
    static constexpr float kCollision = 1.0f;   // force feedback, 0 .. 2
    static constexpr float kRoadForce = 1.0f;   // 0 .. 2
};

Controller controller(Context& ctx) {
    return static_cast<Controller>(iniInt(ctx, "Controls", "Controller", static_cast<int>(Controller::Keyboard), 0, 4));
}

bool joystickConnected(Context& ctx) { return !ctx.input.gamepads().empty() || !ctx.input.joysticks().empty(); }

class ControlPage final : public SettingsPage {
public:
    explicit ControlPage(Frontend& fe) : SettingsPage(fe, "jpg/ctrl_bk.jpg", menu_id::kControl) {
        Context& ctx = fe.ctx;
        const auto& s = ctx.game->strings;
        const int id = menu_id::kControl;

        // 3-5: AUTO REVERSE (on), POV HAT (off), FORCE FEEDBACK (off).
        m_autoReverse = &addToggle(fe, 3, "texture/ctrl_aut.tga", 64, "AutoReverse", true, "jpg/ctl_trev.jpg");
        m_pov = &addToggle(fe, 4, "texture/ctrl_pov.tga", 91, "UsePovHat", false, "jpg/ctl_tpov.jpg");
        m_feedback = &addToggle(fe, 5, "texture/ctrl_fbk.tga", 118, "ForceFeedback", false, "jpg/ctl_tffb.jpg");

        // 6: CONTROLLERS, the five fixed types; the joystick types are greyed
        // without a joystick (ControlSetup::ControlSelect).
        auto& device = menu.add<ui::ValueBox>(
            fe.layout.widget(id, 6, {kBoxX, 71, kBoxWide, kBoxH}),
            [names = std::vector<std::string>{s.get(580, "Mouse"), s.get(581, "Keyboard"), s.get(582, "Joystick"),
                                              s.get(583, "Game Pad"), s.get(584, "Steering Wheel")}] { return names; },
            [&ctx] { return static_cast<int>(controller(ctx)); },
            [&ctx](int i) { ctx.settings.ini.setInt("Controls", "Controller", i); });
        device.optionEnabled = [&ctx](int i) { return i < 2 || joystickConnected(ctx); };
        device.help = "jpg/ctl_tcon.jpg";

        // 7-8: STEERING SENSITIVITY, CONTROLLER DEAD ZONE.
        m_sensitivity = &addSlider(fe, 7, {450, 108, 186, 29}, "Sensitivity", ControlDefaults::kSensitivity, 0.5f,
                                   2.0f, "jpg/ctl_tss.jpg");
        // The dead-zone picture is ctl_dd (ControlSetup::FocusDescription,
        // index 9).
        m_deadZone = &addSlider(fe, 8, {450, 142, 186, 29}, "DeadZone", ControlDefaults::kDeadZone, 0.0f, 0.33f,
                                "jpg/ctl_dd.jpg");

        // 9: CALIBRATE. MM2 opens the Windows game-controller panel; OpenMM2
        // leaves calibration to the operating system.
        const Vec2 cal = fe.layout.position(id, 9, {460, 180});
        m_calibrate = &menu.add<ui::SpriteButton>(SpriteSheet{"texture/ctrl_cal.tga", 4}, cal.x, cal.y, [&fe] {
            fe.message("Calibration is handled by the operating system; use the dead zone setting to adjust.");
        });
        m_calibrate->help = "jpg/ctl_tcal.jpg";

        // 10-11: force-feedback COLLISION and ROAD FORCE intensity.
        m_collision = &addSlider(fe, 10, {450, 246, 186, 29}, "FFCollision", ControlDefaults::kCollision, 0.0f, 2.0f,
                                 "jpg/ctl_titn.jpg");
        m_roadForce = &addSlider(fe, 11, {450, 280, 186, 29}, "FFRoadForce", ControlDefaults::kRoadForce, 0.0f, 2.0f,
                                 "jpg/ctl_trf.jpg");

        // 12: CUSTOMIZE CONTROLS.
        const Vec2 cus = fe.layout.position(id, 12, {348, 315});
        menu.add<ui::SpriteButton>(SpriteSheet{"texture/ctrl_cus.tga", 4}, cus.x, cus.y,
                                   [&fe] { fe.push(makeCustomizeControlsPage(fe)); })
            .help = "jpg/ctl_tcus.jpg";
        // ControlSetup::ControlSetup also creates and initialises an
        // mmMouseSteerBar (mouse_bar at 0.1, 0.85) that ControlSetup::Update
        // feeds with mmInput::GetSteering, but never adds it to the menu's
        // node tree: its Cull never runs and nothing is drawn (only the
        // constructor and destructor touch it, ControlSetup +0x7224).
        finish(fe);
    }

    // Which widgets are usable (ControlSetup::ActivateDeviceOptions per
    // controller type, then InitCustomControls and SetFFPermissions): AUTO
    // REVERSE always; SENSITIVITY for mouse, joystick, game pad and wheel;
    // DEAD ZONE and CALIBRATE for joystick and wheel; POV HAT for a joystick
    // with a hat; FORCE FEEDBACK whenever a force-feedback device is present,
    // whatever the type; the two intensities while FORCE FEEDBACK is on.
    void update(Frontend& fe, double) override {
        Context& ctx = fe.ctx;
        const Controller c = controller(ctx);
        const bool stick = c == Controller::Joystick || c == Controller::Wheel;
        const auto bound = static_cast<controls::Controller>(static_cast<int>(c));
        const float deadZone = iniFloat(ctx, "Controls", "DeadZone", ControlDefaults::kDeadZone, 0.0f, 0.33f);
        m_autoReverse->enabled = true;
        m_sensitivity->enabled = c != Controller::Keyboard;
        m_deadZone->enabled = stick;
        // ControlSetup::InitCustomControls: POV HAT for the joystick when it
        // has one (mmInput::JoystickHasCoolie).
        m_pov->enabled = c == Controller::Joystick && controls::readJoystick(ctx.input, bound, deadZone).hasPov;
        // ControlSetup::ActivateDeviceOptions / InitCustomControls: FORCE
        // FEEDBACK with the joystick or the wheel controller and a
        // force-feedback joystick (mmInput +0x160; OpenMM2 also counts a
        // pad with rumble motors), the intensities while it is on
        // (SetFFPermissions).
        m_feedback->enabled = stick && ctx.input.forceFeedback(false) != nullptr;
        const bool feedbackOn = m_feedback->enabled && iniBool(ctx, "Controls", "ForceFeedback", false);
        m_collision->enabled = feedbackOn;
        m_roadForce->enabled = feedbackOn;
        m_calibrate->enabled = stick;
    }

protected:
    void cancel(Frontend& fe) override {
        // Bindings kept with the customize page's DONE stay (MM2 restores
        // from the driver's saved configuration, which DONE wrote).
        auto& ini = fe.ctx.settings.ini;
        for (const auto& k : m_savedSettings.ini.keys("Controls"))
            if (k.starts_with("Bind."))
                m_savedSettings.ini.remove("Controls", k);
        for (const auto& k : ini.keys("Controls"))
            if (k.starts_with("Bind."))
                m_savedSettings.ini.set("Controls", k, ini.getString("Controls", k));
        SettingsPage::cancel(fe);
    }

    void resetDefaults(Frontend& fe) override {
        // The defaults, plus an automatic transmission and the keyboard
        // (mmInput::AutoSetup); key bindings are left alone.
        auto& ini = fe.ctx.settings.ini;
        for (const auto& k : ini.keys("Controls"))
            if (!k.starts_with("Bind."))
                ini.remove("Controls", k);
        ini.setInt("Controls", "Controller", static_cast<int>(Controller::Keyboard));
        fe.config.automatic = true;
    }

private:
    ui::LampItem& addToggle(Frontend& fe, int index, const char* sprite, float y, const char* key, bool def,
                            const char* help) {
        Context& ctx = fe.ctx;
        const Vec2 p = fe.layout.position(menu_id::kControl, index, {kLampX, y});
        auto& lamp = menu.add<ui::LampItem>(
            SpriteSheet{sprite, 5}, p.x, p.y, [&ctx, key, def] { return iniBool(ctx, "Controls", key, def); },
            [&ctx, key, def] { ctx.settings.ini.setBool("Controls", key, !iniBool(ctx, "Controls", key, def)); });
        lamp.help = help;
        return lamp;
    }

    ui::Slider& addSlider(Frontend& fe, int index, Box code, const char* key, float def, float lo, float hi,
                          const char* help) {
        Context& ctx = fe.ctx;
        auto& slider = menu.add<ui::Slider>(
            fe.layout.widget(menu_id::kControl, index, code),
            [&ctx, key, def, lo, hi] { return iniFloat(ctx, "Controls", key, def, lo, hi); },
            [&ctx, key](float v) { ctx.settings.ini.setDouble("Controls", key, v); }, lo, hi);
        slider.help = help;
        return slider;
    }

    ui::LampItem* m_autoReverse = nullptr;
    ui::LampItem* m_pov = nullptr;
    ui::LampItem* m_feedback = nullptr;
    ui::Slider* m_sensitivity = nullptr;
    ui::Slider* m_deadZone = nullptr;
    ui::Slider* m_collision = nullptr;
    ui::Slider* m_roadForce = nullptr;
    ui::SpriteButton* m_calibrate = nullptr;
};

// --- Customize controls ----------------------------------------------------------------------

// MM2's 34 action slots and the five controllers' binding sets
// (mmInput::SetDefaultConfig) are app::controls' tables, which the race
// reads too. Bindings are stored as [Controls] Bind.<string id> (the
// keyboard's set) or Bind.<controller>.<string id> = <key name, "Mouse
// Left", "Joy Button 3", ...>.
using controls::Action;
using controls::kUnbound;
using platform::Key;

controls::Controller boundController(Context& ctx) {
    return static_cast<controls::Controller>(static_cast<int>(controller(ctx)));
}

// The joystick as the race reads it, for the wheel's defaults and capture.
controls::InputFrame inputFrame(Context& ctx, controls::Controller c) {
    const auto size = ctx.window().size();
    const float deadZone = iniFloat(ctx, "Controls", "DeadZone", ControlDefaults::kDeadZone, 0.0f, 0.33f);
    return controls::readFrame(ctx.input, c, deadZone, static_cast<float>(size.width),
                               static_cast<float>(size.height));
}

controls::BindingSet boundSet(Context& ctx, controls::Controller c, const controls::JoystickFrame& joy) {
    return controls::loadBindings(ctx.settings.ini, c, joy.present ? joy.numButtons : -1);
}

// The action list (MM2's "CW Array", UICWArray): two columns, the action in
// white and its control (mmIODev::GetDescription), red while it is focused
// or waiting for a control; 20 px rows, 15 visible, scrolling with the
// arrows. It lists the slots the chosen controller reads and may rebind
// (mmInput::Init; UICWArray::Redraw). Enter or a click waits for a key, a
// mouse button or a joystick control (mmInput::PollStates); Escape cancels.
class BindingList final : public ui::Widget {
public:
    static constexpr int kRows = 15;
    static constexpr float kRowH = 20.0f;

    BindingList(Frontend& fe, Box b) : m_fe(fe) { box = {b.x, b.y, b.w, kRowH * kRows}; }

    // Opens the dialogs for a refused control (xasn_dlg) or one another
    // listed action uses (ctrl_dlg; `force` binds it anyway).
    std::function<void()> onRefused;
    std::function<void(std::function<void()> force)> onDuplicate;
    std::function<void()> onEscape;

    bool modal() const override { return m_active || m_capturing || m_swallow; }
    void focusChanged(bool focused) override {
        m_active = focused;
        if (!focused)
            m_capturing = false;
    }

    void draw(ui::UiFrame& f, bool focused) override {
        Context& ctx = m_fe.ctx;
        listActions();
        const auto font = ui::style::valueFont();
        const controls::Controller c = boundController(ctx);
        const controls::BindingSet set = boundSet(ctx, c, inputFrame(ctx, c).joy);
        const auto strings = [&ctx](std::uint32_t id, const char* fallback) {
            return ctx.game->strings.get(id, fallback);
        };
        m_scroll = std::clamp(m_scroll, 0, std::max(0, static_cast<int>(m_actions.size()) - kRows));
        for (int i = 0; i < kRows; ++i) {
            const int idx = m_scroll + i;
            if (idx >= static_cast<int>(m_actions.size()))
                break;
            const Action a = m_actions[static_cast<std::size_t>(idx)];
            const float y = box.y + kRowH * static_cast<float>(i) + 2;
            const bool selected = idx == m_selected && (focused || m_capturing);
            f.text.draw(f.overlay, font, ctx.game->strings.get(controls::info(a).stringId), box.x, y,
                        ui::style::kRecordText);
            const std::string text = controls::describe(set[static_cast<std::size_t>(a)], strings);
            // UIControlWidget::Init: the binding centred in the row's right
            // half (text effects 3); red while capturing
            // (UIControlWidget::EnableField; OpenMM2 also while selected).
            f.text.draw(f.overlay, font, text, box.x + box.w * 0.75f, y,
                        selected ? ui::style::kValueTextFocus : ui::style::kRecordText, ui::Align::Center);
        }
        // The VSWidget (VSWidget::Cull) 10 px right of the list, as tall as
        // the rows, hidden while every row fits (UICWArray::SetVScrollVals);
        // the list's focus lights its band (UICWArray::Switch).
        if (scrolls()) {
            placeBar(f);
            m_bar.draw(f, focused || m_active);
        }
    }

    bool activate(ui::UiFrame&) override {
        m_active = true;
        startCapture();
        return true;
    }

    // Called every frame while the list has the focus but not the keyboard
    // (the mouse went elsewhere): take the keyboard back on a click, the
    // mouse returning, or an arrow key. A press on a row starts its capture
    // (UICWArray::Action).
    void mouse(ui::UiFrame& f, bool hovered) override {
        const ui::NavInput& nav = f.nav;
        if (hovered && nav.mousePressed) {
            m_active = true;
            select(rowAt(nav.mouse.y));
            startCapture();
        } else if (nav.mousePressed && onBar(f)) {
            m_active = true;
            barMouse(f, true);
        } else if (hovered && nav.mouseMoved) {
            m_active = true;
        } else if (nav.up || nav.down) {
            m_active = true;
            select(m_selected + (nav.up ? -1 : 1));
        } else if (nav.accept) {
            m_active = true;
            startCapture();
        }
    }

    void modalInput(ui::UiFrame& f) override {
        const ui::NavInput& nav = f.nav;
        if (m_swallow) {
            // The control that ended a capture must not also act on the page.
            m_swallow = false;
            return;
        }
        if (m_capturing) {
            capture();
            return;
        }
        // The scroll bar (VSWidget::Action, UICWArray::CaptureAction: a
        // press, then the drag until the release) and row clicks while the
        // list has the keyboard.
        if (nav.mouseReleased) {
            m_dragging = false;
            m_bar.release();
        }
        if (m_dragging && nav.mouseDown && nav.mouseMoved) {
            barMouse(f, false);
            return;
        }
        if (nav.mousePressed) {
            if (onBar(f)) {
                barMouse(f, true);
            } else if (box.contains(nav.mouse)) {
                select(rowAt(nav.mouse.y));
                startCapture();
            }
            return;
        }
        if (nav.wheel != 0.0f)
            m_scroll -= static_cast<int>(nav.wheel);
        if (nav.up)
            select(m_selected - 1);
        if (nav.down)
            select(m_selected + 1);
        if (nav.accept)
            startCapture();
        if (nav.tabNext)
            m_active = false; // the menu moves the focus on
        if (nav.back && onEscape)
            onEscape();
        // The mouse leaving the list hands it back to the page's widgets.
        if (nav.mouseMoved && !m_dragging && !box.contains(nav.mouse) && !onBar(f))
            m_active = false;
    }

private:
    void listActions() {
        // UICWArray::Redraw: the slots the controller reads and does not fix.
        const controls::Controller c = boundController(m_fe.ctx);
        m_actions.clear();
        for (std::size_t i = 0; i < controls::kActionCount; ++i)
            if (controls::slotListed(c, static_cast<Action>(i)))
                m_actions.push_back(static_cast<Action>(i));
        if (!m_actions.empty())
            m_selected = std::clamp(m_selected, 0, static_cast<int>(m_actions.size()) - 1);
    }

    int rowAt(float y) const { return m_scroll + static_cast<int>((y - box.y) / kRowH); }

    bool scrolls() const { return static_cast<int>(m_actions.size()) > kRows; }

    // The bar follows the list's first row (the selection scrolls it too).
    void placeBar(ui::UiFrame& f) {
        const int count = static_cast<int>(m_actions.size());
        m_bar.place(box.x + box.w + 10, box.y, box.h);
        m_bar.setRatio(count > 0 ? static_cast<float>(kRows) / static_cast<float>(count) : 1.0f);
        m_bar.setRow(f, m_scroll, count);
    }

    bool onBar(ui::UiFrame& f) {
        if (!scrolls())
            return false;
        placeBar(f);
        return m_bar.contains(f, f.nav.mouse);
    }

    // VSWidget::EvalMouseXY, then the list's first row from the bar
    // (UICWArray::VScrollCB).
    void barMouse(ui::UiFrame& f, bool press) {
        placeBar(f);
        if (m_bar.mouse(f, press) && press)
            m_dragging = true;
        m_scroll = m_bar.firstRow(f, static_cast<int>(m_actions.size()), kRows);
    }

    void select(int i) {
        listActions();
        m_selected = std::clamp(i, 0, std::max(0, static_cast<int>(m_actions.size()) - 1));
        if (m_selected < m_scroll)
            m_scroll = m_selected;
        if (m_selected >= m_scroll + kRows)
            m_scroll = m_selected - kRows + 1;
    }

    // UICWArray::EnterCapture / mmInput::CaptureState(1).
    void startCapture() {
        m_capturing = true;
        const controls::Controller c = boundController(m_fe.ctx);
        m_reader.begin(inputFrame(m_fe.ctx, c));
    }

    // UICWArray::CheckCapture, then UICWArray::AcceptCapture (or
    // ResetCapture for Escape).
    void capture() {
        Context& ctx = m_fe.ctx;
        listActions();
        if (m_actions.empty()) {
            m_capturing = false;
            return;
        }
        const controls::Controller c = boundController(ctx);
        const controls::InputFrame frame = inputFrame(ctx, c);
        const controls::Captured what = m_reader.poll(frame, c);
        if (what.kind == controls::Captured::Kind::None)
            return;
        m_capturing = false;
        m_swallow = true;
        if (what.kind == controls::Captured::Kind::Key) {
            const auto k = static_cast<Key>(what.value);
            if (k == Key::Escape)
                return; // ResetCapture
            // F1-F10 are reserved (xasn_dlg).
            if (k >= Key::F1 && k <= Key::F10) {
                if (onRefused)
                    onRefused();
                return;
            }
        }
        const Action slot = m_actions[static_cast<std::size_t>(m_selected)];
        const controls::BindingSet before = boundSet(ctx, c, frame.joy);
        auto set = std::make_shared<controls::BindingSet>(before);
        auto rebinder = std::make_shared<controls::Rebinder>(*set, c);
        // Writes the slots the capture changed (mmInput::AssignIO also
        // unbinds the others that used the control).
        auto store = [&ctx, c, before, set] {
            for (std::size_t i = 0; i < controls::kActionCount; ++i)
                if ((*set)[i] != before[i])
                    controls::storeBinding(ctx.settings.ini, c, static_cast<Action>(i), (*set)[i]);
        };
        switch (rebinder->capture(slot, what)) {
        case controls::CaptureResult::Assigned: store(); break;
        case controls::CaptureResult::Duplicate:
            if (onDuplicate)
                onDuplicate([rebinder, slot, store] {
                    rebinder->forceAssign(slot); // UICWArray::ForceCapture
                    store();
                });
            break;
        case controls::CaptureResult::Rejected:
            if (onRefused)
                onRefused();
            break;
        }
    }

    Frontend& m_fe;
    std::vector<Action> m_actions;
    controls::CaptureReader m_reader;
    int m_selected = 0;
    int m_scroll = 0;
    ui::ScrollBar m_bar;
    bool m_dragging = false;
    bool m_active = false;
    bool m_capturing = false;
    bool m_swallow = false;
};

class CustomizePage final : public SettingsPage {
public:
    explicit CustomizePage(Frontend& fe) : SettingsPage(fe, "jpg/cuss_bk.jpg", menu_id::kControlCustom) {
        // Widget 3: the action list at 50,62, 250 wide (UICWArray::Init).
        auto& list =
            menu.add<BindingList>(fe, fe.layout.widget(menu_id::kControlCustom, 3, {50, 62, 250, 20}));
        list.onEscape = [this, &fe] { cancel(fe); };
        // xasn_dlg (dialog 0x20, which mmInterface::Update opens after
        // ControlCustom::BadAssignCB): Dialog_Message::Init(100, "dlg_ok") in
        // mmInterface::mmInterface, one OK button.
        list.onRefused = [&fe] {
            fe.dialog("jpg/xasn_dlg.jpg", kRefusedDialog, {{"texture/dlg_ok.tga", {296, 38}, {}}});
        };
        list.onDuplicate = [&fe](std::function<void()> force) {
            // ctrl_dlg (Dialog_ControlAssign, dialog 0x15): OK assigns the
            // control anyway and leaves the other action unbound
            // (ControlCustom::VerifyBadAssignment); CANCEL keeps the old
            // binding (ControlCustom::CancelBadAssignment).
            fe.dialog("jpg/ctrl_dlg.jpg", kControlWarningDialog,
                      {{"texture/dlg_ok.tga", {180, 176}, std::move(force)},
                       {"texture/dlg_can.tga", {18, 176}, {}}});
        };
        // ControlCustom::ControlCustom sets no focus widget: the page opens
        // on DEFAULTS (widget 0).
        finish(fe);
    }

protected:
    // ControlCustom::CancelAction, then the options menu itself, not the
    // control page (only CANCEL and Escape go back there, mmInterface::Update
    // case 0x29). The control page's changes stay, as in MM2.
    void stripOptions(Frontend& fe) override {
        restore(fe);
        fe.popTo(fe.depth() - 2);
    }

    void resetDefaults(Frontend& fe) override {
        // UICWArray::DefaultCFG: every controller's set back to its defaults.
        auto& ini = fe.ctx.settings.ini;
        for (const auto& k : ini.keys("Controls"))
            if (k.starts_with("Bind."))
                ini.remove("Controls", k);
    }
};

// --- About ------------------------------------------------------------------------------------

// AboutMenu (menu 34): the credits picture (credits.jpg, 215x4582) scrolls
// in the box at 39,203 (215x173): it starts at the top, holds for 1.5 s and
// then scrolls at 50 px/s in whole pixels, wrapping without a gap
// (AboutMenu::Update, AboutMenu::Cull). The product ID label shows string
// 325 "UNKNOWN", MM2's text when the registry has no product ID: OpenMM2
// never reads CD keys. No navigation strip.
class AboutPage final : public Page {
public:
    explicit AboutPage(Frontend& fe) {
        menuId = menu_id::kAbout;
        menu.background = "jpg/about_bk.jpg";
        m_credits = fe.layout.widget(menu_id::kAbout, 0, {39, 203, 215, 173});
        menu.add<ui::Custom>([this, &fe](ui::UiFrame& f) { drawCredits(fe, f); });
        const Vec2 done = fe.layout.position(menu_id::kAbout, 1, kNext);
        auto& d = menu.add<ui::SpriteButton>(SpriteSheet{"texture/opt_done.tga", 4}, done.x, done.y, [&fe] { fe.pop(); });
        d.sound = "Selectionmade";
        m_pid = fe.layout.widget(menu_id::kAbout, 2, {130, 180, 300, 18});
        menu.add<ui::TextBox>(m_pid, [&fe] { return fe.ctx.game->strings.get(325, "UNKNOWN"); }).font =
            ui::FontSpec{"Arial Bold", 18, 18, 0, 400}; // GetFont(20): string 561
        menu.onBack = [&fe] { fe.pop(); };
        menu.setInitialFocus(&d);
    }

    // The credits' clock starts when the page is entered (AboutMenu::PreSetup).
    void update(Frontend&, double dt) override { m_time += dt; }

private:
    void drawCredits(Frontend& fe, ui::UiFrame& f) const {
        const ui::UiTexture& t = fe.textures.get("jpg/credits.jpg");
        if (!t)
            return;
        constexpr double kHold = 1.5, kSpeed = 50.0;
        const float h = static_cast<float>(t.height);
        const int pixels = m_time > kHold ? static_cast<int>((m_time - kHold) * kSpeed + 0.5) : 0;
        const float offset = static_cast<float>(pixels % static_cast<int>(t.height));
        const Vec4 clip{m_credits.x, m_credits.y, m_credits.w, m_credits.h};
        f.overlay.setClip(&clip);
        ui::drawImage(f.overlay, t, m_credits.x, m_credits.y - offset);
        if (h - offset < m_credits.h) // the top follows the bottom
            ui::drawImage(f.overlay, t, m_credits.x, m_credits.y - offset + h);
        f.overlay.setClip(nullptr);
    }

    Box m_credits, m_pid;
    double m_time = 0.0;
};

} // namespace

std::unique_ptr<Page> makeOptionsPage(Frontend& fe) { return std::make_unique<OptionsPage>(fe); }
std::unique_ptr<Page> makeGraphicsPage(Frontend& fe) { return std::make_unique<GraphicsPage>(fe); }
std::unique_ptr<Page> makeTweaksOptionsPage(Frontend& fe) { return makeTweaksPage(fe, {}); }
std::unique_ptr<Page> makeAudioPage(Frontend& fe) { return std::make_unique<AudioPage>(fe); }
std::unique_ptr<Page> makeControlPage(Frontend& fe) { return std::make_unique<ControlPage>(fe); }
std::unique_ptr<Page> makeCustomizeControlsPage(Frontend& fe) { return std::make_unique<CustomizePage>(fe); }
std::unique_ptr<Page> makeAboutPage(Frontend& fe) { return std::make_unique<AboutPage>(fe); }

} // namespace mm2::app::frontend
