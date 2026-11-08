#pragma once

// Menu widgets for the original 640x480 frontend.
//
// The original menus are full-screen JPEG backgrounds with the labels and
// empty boxes painted in; interactive elements are TGA sprite sheets with
// the states stacked vertically (see docs/frontend.md). Widgets here are
// positioned in that 640x480 space and drawn through render::Overlay2D, and
// behave like MM2's (UIBMButton, UITextDropdown/mmDropDown, UITextRoller2,
// UISlider/mmSlider, UITextField, UIMenu, MenuManager).

#include "core/Math.h"
#include "platform/Input.h"
#include "render/Overlay2D.h"
#include "ui/Font.h"
#include "ui/Text.h"
#include "ui/TextureCache.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mm2::ui {

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
};

// Navigation intents, merged from keyboard, gamepad and mouse.
struct NavInput {
    bool up = false, down = false, left = false, right = false;
    bool accept = false; // Enter (activates and navigates)
    bool space = false;  // Space (flips toggles only, like MM2)
    bool back = false;   // Escape
    bool enter = false;  // Return/keypad Enter only (text entry commits)
    bool tabNext = false;
    bool home = false, end = false;
    bool pageUp = false, pageDown = false;
    Vec2 mouse{-1e9f, -1e9f}; // virtual coordinates
    bool mouseMoved = false;
    bool mousePressed = false;  // left button went down this frame
    bool mouseReleased = false; // left button went up this frame
    bool mouseDown = false;
    float wheel = 0.0f;
    std::string text;     // typed text this frame (text entry widgets)
    bool backspace = false;
};

// Turns raw input into NavInput with key/stick auto-repeat (the platform
// layer reports only the initial key press).
class NavReader {
public:
    NavInput read(const platform::Input& input, const render::UiLayout& layout, double dt);

private:
    struct Repeat {
        bool held = false;
        double timer = 0.0;
    };
    bool repeat(Repeat& r, bool down, double dt);
    Repeat m_up, m_down, m_left, m_right, m_backspace;
    Vec2 m_lastMouse{-1e9f, -1e9f};
    bool m_wasDown = false;
};

// Plays a frontend sound by MM2 name ("Switch", "Selectionmade", ...) at an
// Angel volume.
using SoundFn = std::function<void(std::string_view name, float volume)>;

// Shared per-frame drawing environment.
struct UiFrame {
    render::Overlay2D& overlay;
    TextureCache& textures;
    TextRenderer& text;
    const NavInput& nav;
    double time = 0.0;
    const SoundFn* sound = nullptr;

    void play(std::string_view name, float volume) const {
        if (sound && *sound && !name.empty())
            (*sound)(name, volume);
    }
};

// Sprite sheet: `frames` equally tall states stacked vertically.
struct SpriteSheet {
    std::string path; // "texture/main_sp.tga"
    int frames = 4;
    bool colorKey = false; // black is transparent (opaque RGB sprites such as lock.tga)
};

// Draws one state of a sprite sheet with its top-left at (x, y).
void drawSpriteFrame(UiFrame& f, const SpriteSheet& sheet, int frame, float x, float y,
                     std::uint32_t color = 0xFFFFFFFFu);
// Size of one frame of a sheet (0x0 if the texture is missing).
Vec2 spriteFrameSize(UiFrame& f, const SpriteSheet& sheet);

// Colours and fonts of the frontend (MM2 `MenuManager::GetFGColor` and
// `MenuManager::GetFont`, frontend branch).
namespace style {
inline constexpr std::uint32_t kValueText = 0xFF00FFFFu;      // yellow (0xAABBGGRR), colour 0
inline constexpr std::uint32_t kValueTextFocus = 0xFF0000FFu; // red, colour 3 (focused or active)
inline constexpr std::uint32_t kOptionDisabled = 0xFF008080u; // olive (0.5, 0.5, 0): mmDropDown disabled options
inline constexpr std::uint32_t kRecordText = 0xFFFFFFFFu;     // white, colour 2 (record lists)
// OpenMM2-only text (messages the original showed as pictures, status lines).
inline constexpr std::uint32_t kValueTextDisabled = 0xFF8C7C7Cu; // grey-blue
inline constexpr std::uint32_t kHelpText = 0xFF00FFFFu;          // yellow
FontSpec valueFont(); // string 560 "Arial Bold, 16, 16": every frontend text widget
FontSpec smallFont(); // string 559 "Arial Bold, 14, 14"
FontSpec titleFont(); // string 251 "Gill Sans MT, 16, 22": race titles
// In-game popups (MenuManager's second font set, MenuManager::Init with a
// camera: GetFont 12, 14, 16, 20, 24, 32 = strings 566, 567, 568, 569, 570,
// 571).
FontSpec popupFont();       // GetFont 20, string 569 "Arial Bold, 16, 20" (PUExit's question, results text)
FontSpec popupButtonFont(); // GetFont 24, string 570 "Arial Bold, 16, 24": PUMenuBase buttons
FontSpec popupSmallFont();  // GetFont 16, string 568 "Arial Bold, 14, 16": slider and drop-down labels and values
FontSpec popupTitleFont();  // GetFont 32, string 571 "Arial Bold, 20, 32": PUMenuBase::CreateTitle
// MenuManager's popup line height (MenuManager::InitCommonStuff: the height
// of the 16-point label font over 480), in 640x480 pixels: the space a
// UISlider or UITextDropdown label takes above its control.
inline constexpr float kPopupLineHeight = 16.0f;
inline constexpr std::uint32_t kPopupText = 0xFFFFFFFFu;      // white (popup colours 0, 1)
inline constexpr std::uint32_t kPopupFocus = 0xFF21FFEEu;     // (0.933, 1, 0.129)
inline constexpr std::uint32_t kPopupDisabled = 0xFF595959u;  // (0.35, 0.35, 0.35), colour 5
} // namespace style

class Widget {
public:
    virtual ~Widget() = default;

    Box box;
    bool visible = true;   // uiWidget::TurnOn / TurnOff
    bool enabled = true;   // uiWidget::Enable / Disable
    bool readOnly = false; // shown but skipped by focus (uiWidget::SetReadOnly)
    int group = 0;         // 0 = the page, 1 = the navigation strip
    std::string help;      // help picture shown while focused ("jpg/mn_sp.jpg")
    std::function<std::string()> helpFn; // dynamic help picture (overrides `help`)

    virtual bool focusable() const { return visible && enabled && !readOnly; }
    virtual void draw(UiFrame& f, bool focused) = 0;
    // Enter / A / mouse release over the widget. Returns true if handled.
    virtual bool activate(UiFrame&) { return false; }
    // Space: only toggles respond (MM2 sends Space to the widget's own action).
    virtual bool activateSpace(UiFrame&) { return false; }
    // Left/right. Returns true if handled; focus never moves on left/right.
    virtual bool adjust(UiFrame&, int /*dir*/) { return false; }
    // Page Up (-1) / Page Down (1): the record list's pages.
    virtual bool page(UiFrame&, int /*dir*/) { return false; }
    // Mouse handling while the widget is hovered or focused.
    virtual void mouse(UiFrame&, bool /*hovered*/) {}
    // Called when the widget gains or loses keyboard focus.
    virtual void focusChanged(bool /*focused*/) {}
    // True while the widget wants all input (open dropdown, text entry;
    // MenuManager::RegisterWidgetFocus).
    virtual bool modal() const { return false; }
    // Input routed to a modal widget (uiWidget::CaptureAction).
    virtual void modalInput(UiFrame&) {}
    // Drawn after all widgets (drop-down lists; MenuManager::DeclareLastDrawn).
    virtual void drawPopup(UiFrame&) {}

    std::string helpPicture() const { return helpFn ? helpFn() : help; }
};

// Sprite buttons (UIBMButton): frames normal, highlighted, pressed, disabled
// (4), or normal, highlighted, pressed (3), or with a 5th frame (mnav_opt,
// lit while `lit()` is true). The pressed frame shows only while the mouse
// button is held on the button; its sound plays on the press, and the button
// acts when the mouse button is released over it or on Enter.
class SpriteButton : public Widget {
public:
    SpriteButton(SpriteSheet sheet, float x, float y, std::function<void()> onClick);
    void draw(UiFrame& f, bool focused) override;
    bool activate(UiFrame&) override;
    void mouse(UiFrame& f, bool hovered) override;
    SpriteSheet sheet;
    std::function<void()> onClick;
    std::function<bool()> lit; // 5-frame toggle buttons: show the "on" frames
    // Per-button sound (UIBMButton::PlaySound), played on activation.
    std::string sound;
    float soundVolume = 0.86f;

private:
    bool m_pressed = false; // mouse button went down on it
};

// Rows with a lamp or check box (blitz.tga, aud_fx.tga ...): five frames
// off, off+highlight, on, on+highlight, disabled. Used as radio items or
// toggles; they flip on mouse press, Enter or Space.
class LampItem : public Widget {
public:
    LampItem(SpriteSheet sheet, float x, float y, std::function<bool()> isOn, std::function<void()> onClick);
    void draw(UiFrame& f, bool focused) override;
    bool activate(UiFrame&) override;
    bool activateSpace(UiFrame& f) override { return activate(f); }
    void mouse(UiFrame& f, bool hovered) override;
    void focusChanged(bool focused) override;
    SpriteSheet sheet;
    std::function<bool()> isOn;
    std::function<void()> onClick;
    std::string sound;
    float soundVolume = 0.86f;
    // One of a group of exclusive lamps (UIBMButton::MexOn): a mouse press
    // on the lamp that is already on only flips the lamp's own state
    // (UIBMButton::DoToggle): its sound, no onClick, and it shows unlit
    // (frame 1) until the focus leaves it. Enter and Space still run onClick.
    bool radio = false;

private:
    bool m_shownOff = false;
};

// Value box with a drop-down list (UITextDropdown / mmDropDown): the value
// is drawn 5 px from the left, the drop_arrow at the right; Enter or a click
// opens a list of every option below the box (more columns when it would
// leave the screen); Left/Right do nothing while it is closed. The box is 23
// px tall whatever the layout says. Up/down roller buttons are separate
// SpriteButtons (see stepOption()).
class ValueBox : public Widget {
public:
    ValueBox(Box box, std::function<std::vector<std::string>()> options, std::function<int()> get,
             std::function<void(int)> set);
    void draw(UiFrame& f, bool focused) override;
    bool activate(UiFrame& f) override;
    bool adjust(UiFrame& f, int dir) override;
    void mouse(UiFrame& f, bool hovered) override;
    bool modal() const override { return m_open; }
    void modalInput(UiFrame& f) override;
    void drawPopup(UiFrame& f) override;

    std::function<std::vector<std::string>()> options;
    std::function<int()> get;
    std::function<void(int)> set;
    // Options that cannot be picked (locked races); drawn olive in the list.
    std::function<bool(int)> optionEnabled;
    bool showArrow = true;
    // In MM2's in-game popups (MenuManager in popup mode) the drop-down uses
    // drop_arrow2, draws its value in the popup's label font and colours
    // with a white outline round the box (TextDropWidget::Init's text
    // effects 0x15), and its label `labelHeight` above the box
    // (UITextDropdown::Init with a label).
    bool popup = false;
    std::string label;
    float labelHeight = style::kPopupLineHeight;
    // OpenMM2: the value's (and its list's) font instead of the default.
    std::optional<FontSpec> font;

    bool isOptionEnabled(int i) const { return !optionEnabled || optionEnabled(i); }

private:
    struct Cell {
        Box box;
        int index;
    };
    std::vector<Cell> listCells(std::size_t count) const;
    bool m_open = false;
    int m_hover = 0;
};

// Steps a value box to the next enabled option in `dir` (the separate
// roller_up/roller_down buttons of MM2's menus); `wrap` per page. Returns
// true when the value changed.
bool stepOption(ValueBox& box, int dir, bool wrap);

// MM2's VSWidget, the scroll bar of the record lists (UICompositeScroll)
// and the customize list (UICWArray): scroll_uarr, a trough of segments
// (scroll_inact, the thumb in scroll_act; each a band of the bitmap's half
// height, the second band while the list has the focus) and scroll_darr,
// with 2 px between the arrows and the trough. The arrows (4 frames:
// unfocused, focused, focused and pressed) sit centred over the trough.
// The bar counts in segments: its value is the thumb's first segment, and
// the owner turns it into a row (firstRow, UICompositeScroll::VScrollCB).
class ScrollBar {
public:
    // VSWidget::Init / SetStep / SetHotSpots: the trough's left top and the
    // bar's height (2 to 200 segments fit between the arrows).
    void place(float x, float y, float height);
    // VSWidget::SetTrough: the thumb's share of the trough (rows shown over
    // rows), clamped to 0..1; the thumb spans whole part of segments x share
    // segments, at least 2 and at most all of them.
    void setRatio(float ratio) { m_ratio = std::clamp(ratio, 0.0f, 1.0f); }
    int value() const { return m_value; }
    // The value whose first row (firstRow) is `row`, for lists whose owner
    // scrolls them by other means (the customize list's selection).
    void setRow(UiFrame& f, int row, int count);
    // VSWidget::Inc / Dec: one segment, clamped (`f` gives the sizes).
    bool inc(UiFrame& f);
    bool dec(UiFrame& f);
    // UICompositeScroll::VScrollCB: the first row of `count`, `rows` at a
    // time: value / (bar height / segment - 1) x count, rounded, clamped.
    int firstRow(UiFrame& f, int count, int rows) const;
    // VSWidget::Cull.
    void draw(UiFrame& f, bool focused) const;
    // VSWidget::EvalMouseXY for a press (or a drag while the owner holds
    // the mouse): the up arrow decrements, the down arrow increments, the
    // trough moves the thumb a segment at a time until it covers the
    // pointer; "Switch" on a press. False when the pointer is off the bar.
    bool mouse(UiFrame& f, bool press);
    // Any other mouse event (VSWidget::Action) unpresses the arrows.
    void release() { m_upPressed = m_downPressed = false; }
    // Back to the top (a new list).
    void reset() {
        m_value = 0;
        release();
    }
    bool contains(UiFrame& f, Vec2 p) const;

private:
    struct Metrics {
        float segW = 0, segH = 0, arrowW = 0, arrowH = 0;
        int segments = 2; // SetStep
        int thumb = 1;    // SetTrough: the thumb's last segment past its first
        int base = 1;     // CalcTroughRatio: whole segments in the height, less one
    };
    Metrics metrics(UiFrame& f) const;
    void clamp(const Metrics& m);

    float m_x = 0, m_y = 0, m_height = 0;
    float m_ratio = 0.25f; // VSWidget::SetStep's SetTrough(0.25)
    int m_value = 0;
    bool m_upPressed = false, m_downPressed = false;
};

// Text button of the in-game popups (UIButton in MM2's PUMenuBase menus,
// e.g. the results): white text, yellow-green when focused, grey when
// disabled (MenuManager::GetFGColor, popup branch); acts on Enter or a click.
class TextButton : public Widget {
public:
    TextButton(Box box, std::string label, std::function<void()> onClick);
    void draw(UiFrame& f, bool focused) override;
    bool activate(UiFrame& f) override;
    void mouse(UiFrame& f, bool hovered) override;
    std::string label;
    std::function<void()> onClick;
    // MM2 UIButton::SetType, as mmTextNode::RenderText draws its effects:
    // 1 centres the label in the box and outlines the box with a white
    // rectangle (the popups' OK, Cancel, Previous Menu and Resume Driving),
    // 2 centres the label (PUMain's and PUOptions' rows), 0 centres it
    // vertically only (PUExit's Yes and No). -1 (OpenMM2's results page)
    // draws it from the box's top-left corner.
    int type = -1;
    FontSpec font = style::popupFont();
};

// Text toggle of the in-game popups (UIToggleButton2, UIMenu::AddToggle2):
// the label centred in an outlined box (UIButton type 1) and "ON" or "OFF"
// (strings 607, 606) centred in the `stateWidth` right of that box. Enter or
// a click flips it and plays "Selectionmade"; Space does nothing (the
// widget's action takes only Enter and the mouse).
class TextToggle : public Widget {
public:
    TextToggle(Box box, std::string label, std::function<bool()> isOn, std::function<void()> flip);
    void draw(UiFrame& f, bool focused) override;
    bool activate(UiFrame& f) override;
    void mouse(UiFrame& f, bool hovered) override;
    std::string label;
    std::string onText = "ON", offText = "OFF";
    float stateWidth = 0.0f; // the ON/OFF part, inside the box at its right
    FontSpec font = style::popupSmallFont();
    std::function<bool()> isOn;
    std::function<void()> flip;
};

// Read-only text in a box (driver name, totals).
class TextBox : public Widget {
public:
    TextBox(Box box, std::function<std::string()> text, Align align = Align::Left);
    bool focusable() const override { return false; }
    void draw(UiFrame& f, bool focused) override;
    std::function<std::string()> text;
    Align align;
    FontSpec font = style::valueFont();
    std::uint32_t color = style::kValueText;
};

// Number roller with built-in arrows (UITextRoller2: LAPS, OPPONENTS): the
// value centred in the box left of the arrows, roller_up at the top right
// and roller_down 18 px below; Left/Right and the arrows step it by one and
// play "Switch". The box is 34 px tall.
class Roller : public Widget {
public:
    Roller(Box box, std::function<std::vector<std::string>()> options, std::function<int()> get,
           std::function<void(int)> set);
    void draw(UiFrame& f, bool focused) override;
    bool adjust(UiFrame& f, int dir) override;
    void mouse(UiFrame& f, bool hovered) override;
    void focusChanged(bool focused) override;
    std::function<std::vector<std::string>()> options;
    std::function<int()> get;
    std::function<void(int)> set;
    int maxIndex = -1; // highest selectable index (-1 = last option)

private:
    int m_clicked = 0; // arrow last clicked with the mouse (+1 up, -1 down)
};

// Slider in one row of a grid box (UISlider / mmSlider): slider_larr at the
// left of the layout box, a track of 2 px segments, slider_rarr after it;
// the value bar is drawn from slider_actl/slider_inactl at y + 11. Twenty
// positions, so one step is (max - min) / 19; Left/Right and the arrows step,
// a click on the track sets the value, every change plays "Switch".
// Read-only sliders (the garage's statistics) use slider_roactl and have no
// arrows.
class Slider : public Widget {
public:
    Slider(Box row, std::function<float()> get, std::function<void(float)> set, float min = 0.0f, float max = 1.0f);
    void draw(UiFrame& f, bool focused) override;
    bool adjust(UiFrame& f, int dir) override;
    void mouse(UiFrame& f, bool hovered) override;
    void focusChanged(bool focused) override;
    std::function<float()> get;
    std::function<void(float)> set;
    float min, max;
    // A UISlider with its label above (MM2's popups, UISlider::Init with a
    // negative label mode): the label at the box's top in the popup's label
    // font, the slider row `labelHeight` below it; the box covers both.
    std::string label;
    float labelHeight = 0.0f;
    // mmSlider::LoadBitmap's balance arrows (slider_lbal / slider_rbal),
    // which the popup's BALANCE slider asks for.
    bool balance = false;

    int segments() const;
    float step() const;

private:
    float rowY() const { return box.y + labelHeight; }
    int m_clicked = 0; // arrow last clicked with the mouse (-1 left, +1 right)
};

// Selectable list drawn into a box (OpenMM2's multiplayer session list and
// the control bindings).
class ListBox : public Widget {
public:
    ListBox(Box box, std::function<std::vector<std::string>()> items, std::function<int()> get,
            std::function<void(int)> set);
    void draw(UiFrame& f, bool focused) override;
    bool adjust(UiFrame&, int) override { return true; }
    void mouse(UiFrame& f, bool hovered) override;
    // Up/down move the selection while focused; returns true if consumed.
    bool moveSelection(int dir);
    std::function<std::vector<std::string>()> items;
    std::function<int()> get;
    std::function<void(int)> set;
    std::function<void()> onDoubleClick;
    // A row picked with a click or Enter (MM2's boot list acts on a click).
    std::function<void(int)> onPick;
    bool activate(UiFrame&) override;
    float rowHeight = 18.0f;

private:
    int m_scroll = 0;
    double m_lastClick = -1.0;
    int m_lastClickRow = -1;
};

// Single-line text entry (UITextField): editing whenever it has focus; the
// first key typed replaces the text; printable characters only, up to
// `maxLength`; Backspace removes one character; Enter commits, Tab moves on,
// Escape goes to the page's back handler. Drawn as " text", red on an opaque
// black card while editing, yellow otherwise; no caret, no frame.
class TextEntry : public Widget {
public:
    TextEntry(Box box, std::string* value, std::size_t maxLength = 18);
    void draw(UiFrame& f, bool focused) override;
    bool activate(UiFrame&) override;
    bool modal() const override { return m_editing; }
    void modalInput(UiFrame& f) override;
    void focusChanged(bool focused) override;
    void beginEdit();
    std::string* value;
    std::size_t maxLength;
    std::function<void()> onCommit;
    // In MM2's in-game popups (PUChat): the popup's label font and colours
    // (white, yellow-green while editing) and a white outline round the
    // field (text effects 0x45).
    bool popup = false;

private:
    bool m_editing = false;
    bool m_fresh = true; // the next key replaces the text
};

// Static picture from the archives drawn into a box (map previews).
class Picture : public Widget {
public:
    Picture(Box box, std::function<std::string()> path);
    // A UIIcon added to a menu (the race map) is enabled and writable like
    // any uiWidget, so the menu's focus stops on it while it is shown; it
    // draws no highlight.
    bool focusStop = false;
    bool focusable() const override;
    void draw(UiFrame& f, bool focused) override;
    std::function<std::string()> path;
};

// Free-form drawing hook (performance bars, custom panels).
class Custom : public Widget {
public:
    explicit Custom(std::function<void(UiFrame&)> fn) : m_fn(std::move(fn)) {}
    bool focusable() const override { return false; }
    void draw(UiFrame& f, bool) override { m_fn(f); }

private:
    std::function<void(UiFrame&)> m_fn;
};

// One screen of the frontend: a background picture plus widgets with MM2's
// focus rules (UIMenu, MenuManager::ScanGlobalKeys): Down/Tab and Up step
// through the widgets in creation order, which is the order of tune/widget.csv;
// past the last page widget focus goes to the navigation strip and back; Up
// before the first goes to the other group's first widget (MM2
// `MenuManager::ToggleFocus`). Left/Right never move focus. The mouse
// focuses what it is over; over empty space the highlight and help picture
// disappear until the next key.
class Menu {
public:
    explicit Menu(std::string bg = {}) : background(std::move(bg)) {}

    // Appends a widget; creation order is the focus order (UIMenu::AddWidget).
    template <class W, class... Args>
    W& add(Args&&... args) {
        auto w = std::make_unique<W>(std::forward<Args>(args)...);
        W& ref = *w;
        m_widgets.push_back(std::move(w));
        return ref;
    }

    // Processes navigation for the focused widget.
    void update(UiFrame& f);
    // Background, then drawContent().
    void draw(UiFrame& f);
    // Widgets, the help picture and pop-ups (no background). An inactive menu
    // (under a dialog) shows no focus and no help picture.
    void drawContent(UiFrame& f, bool active = true);

    std::string background;           // full-screen picture ("jpg/main_bk.jpg")
    Vec2 helpPos{40, 396};            // the "desc icons" label (219x69 pictures, drawn 1:1)
    std::string defaultHelp;          // picture when the focused widget has none (pages that keep one there)
    std::function<void()> onBack;     // Escape
    // In-game popups (MenuManager::EnablePU) play "Moveselector" when the
    // focus moves; the frontend menus are silent.
    bool popupSounds = false;

    Widget* focused() const;
    void focus(const Widget* w);
    // The widget focused whenever the page is entered (default: the first one).
    void setInitialFocus(const Widget* w);
    // Back to the initial widget (MM2 resets focus every time a page is entered).
    void resetFocus();
    // Keeps the keyboard position but lights nothing until the next key or
    // the mouse (MenuManager::CloseDialog, the popups' EnablePU).
    void unlight() { m_highlight = false; }
    // No keyboard position and nothing lit: the next Up / Down / Tab lights
    // the initial widget and Enter does nothing (a popup page whose widget 0
    // is its title label).
    void park();
    // The help picture stays off until the focus moves to another widget
    // (the menus whose PreSetup turns the description label off).
    void hideHelpUntilFocusMoves() { m_helpHidden = true; }
    // Moves a widget to the end of the focus order.
    void moveToEnd(const Widget* w);
    std::vector<const Widget*> widgetsInGroup(int group) const;
    bool modalActive() const;

private:
    void setFocus(int index);
    void step(int dir);
    int firstFocusable(int group) const;
    std::vector<std::unique_ptr<Widget>> m_widgets;
    int m_focus = -1;
    int m_initial = -1;
    bool m_highlight = true; // false after the mouse left every widget
    bool m_parked = false;
    bool m_helpHidden = false;
    const Widget* m_pressed = nullptr; // the widget the mouse press hit (sfPointer)
};

} // namespace mm2::ui
