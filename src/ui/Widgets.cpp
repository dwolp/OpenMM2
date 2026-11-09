#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

namespace mm2::ui {
namespace {

constexpr double kRepeatDelay = 0.40;
constexpr double kRepeatRate = 0.075;

std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return render::packColor(r, g, b, a);
}

void outline(render::Overlay2D& o, Box b, std::uint32_t color) {
    o.rect(b.x, b.y, b.w, 1, color);
    o.rect(b.x, b.y + b.h - 1, b.w, 1, color);
    o.rect(b.x, b.y, 1, b.h, color);
    o.rect(b.x + b.w - 1, b.y, 1, b.h, color);
}

// Draws rows [row, row + rows) of an image (rows counted from the picture's
// top) stretched to the given rectangle, the left `fraction` of its width.
void drawRows(UiFrame& f, const UiTexture& t, int row, int rows, float x, float y, float w, float h,
              float uRight = 1.0f) {
    if (!t || w <= 0.0f || h <= 0.0f)
        return;
    const float th = static_cast<float>(t.height);
    const float vTop = 1.0f - static_cast<float>(row) / th;
    const float vBottom = 1.0f - static_cast<float>(row + rows) / th;
    f.overlay.image(t.handle, x, y, w, h, {0.0f, vTop}, {uRight, vBottom});
}

} // namespace

// --- Style -----------------------------------------------------------------------

namespace style {
FontSpec valueFont() { return {"Arial Bold", 16, 16, 0, 400}; }
FontSpec smallFont() { return {"Arial Bold", 14, 14, 0, 400}; }
FontSpec titleFont() { return {"Gill Sans MT", 16, 22, 0, 700}; }
FontSpec popupFont() { return {"Arial Bold", 16, 20, 0, 400}; }
FontSpec popupButtonFont() { return {"Arial Bold", 16, 24, 0, 400}; }
FontSpec popupSmallFont() { return {"Arial Bold", 14, 16, 0, 400}; }
FontSpec popupTitleFont() { return {"Arial Bold", 20, 32, 0, 400}; }
} // namespace style

// --- Input -----------------------------------------------------------------------

bool NavReader::repeat(Repeat& r, bool down, double dt) {
    if (!down) {
        r.held = false;
        return false;
    }
    if (!r.held) {
        r.held = true;
        r.timer = kRepeatDelay;
        return true;
    }
    r.timer -= dt;
    if (r.timer <= 0.0) {
        r.timer += kRepeatRate;
        return true;
    }
    return false;
}

NavInput NavReader::read(const platform::Input& in, const render::UiLayout& layout, double dt) {
    using platform::GamepadAxis;
    using platform::GamepadButton;
    using platform::Key;
    NavInput n;

    bool up = in.keyDown(Key::Up) || in.keyDown(Key::Kp8);
    bool down = in.keyDown(Key::Down) || in.keyDown(Key::Kp2);
    bool left = in.keyDown(Key::Left) || in.keyDown(Key::Kp4);
    bool right = in.keyDown(Key::Right) || in.keyDown(Key::Kp6);
    n.enter = in.keyPressed(Key::Return) || in.keyPressed(Key::KpEnter);
    n.accept = n.enter;
    n.space = in.keyPressed(Key::Space);
    n.back = in.keyPressed(Key::Escape);
    n.tabNext = in.keyPressed(Key::Tab);
    n.home = in.keyPressed(Key::Home);
    n.end = in.keyPressed(Key::End);
    n.pageUp = in.keyPressed(Key::PageUp);
    n.pageDown = in.keyPressed(Key::PageDown);
    // Gamepads are an OpenMM2 addition (MM2's menus read only keyboard and mouse).
    for (const auto& pad : in.gamepads()) {
        auto btn = [&](GamepadButton b) { return pad.buttons.test(static_cast<int>(b)); };
        auto hit = [&](GamepadButton b) { return pad.pressed.test(static_cast<int>(b)); };
        // The left stick moves the pointer instead (app::frontend::stickPointer).
        up |= btn(GamepadButton::DpadUp);
        down |= btn(GamepadButton::DpadDown);
        left |= btn(GamepadButton::DpadLeft);
        right |= btn(GamepadButton::DpadRight);
        n.accept |= hit(GamepadButton::South) || hit(GamepadButton::Start);
        n.back |= hit(GamepadButton::East) || hit(GamepadButton::Back);
        n.tabNext |= hit(GamepadButton::RightShoulder);
    }
    n.up = repeat(m_up, up, dt);
    n.down = repeat(m_down, down, dt);
    n.left = repeat(m_left, left, dt);
    n.right = repeat(m_right, right, dt);
    n.backspace = repeat(m_backspace, in.keyDown(Key::Backspace), dt);

    n.mouse = layout.toVirtual(in.mousePosition());
    // The first reading only records where the pointer is.
    n.mouseMoved = m_lastMouse.x > -1e8f && (n.mouse.x != m_lastMouse.x || n.mouse.y != m_lastMouse.y);
    m_lastMouse = n.mouse;
    n.mousePressed = in.mousePressed(platform::MouseButton::Left);
    n.mouseDown = in.mouseDown(platform::MouseButton::Left);
    n.mouseReleased = m_wasDown && !n.mouseDown;
    m_wasDown = n.mouseDown;
    n.wheel = in.mouseWheel().y;
    n.text = in.text();
    return n;
}

// --- Sprites -----------------------------------------------------------------------

Vec2 spriteFrameSize(UiFrame& f, const SpriteSheet& sheet) {
    const UiTexture& t = sheet.colorKey ? f.textures.getColorKeyed(sheet.path) : f.textures.get(sheet.path);
    if (!t || sheet.frames <= 0)
        return {};
    return {static_cast<float>(t.width), static_cast<float>(t.height) / static_cast<float>(sheet.frames)};
}

void drawSpriteFrame(UiFrame& f, const SpriteSheet& sheet, int frame, float x, float y, std::uint32_t color) {
    const UiTexture& t = sheet.colorKey ? f.textures.getColorKeyed(sheet.path) : f.textures.get(sheet.path);
    if (!t || sheet.frames <= 0)
        return;
    frame = std::clamp(frame, 0, sheet.frames - 1);
    const float fh = static_cast<float>(t.height) / static_cast<float>(sheet.frames);
    // Game images are stored bottom row first: frame 0 (the top one) is at v = 1.
    const float vTop = 1.0f - static_cast<float>(frame) / static_cast<float>(sheet.frames);
    const float vBottom = 1.0f - static_cast<float>(frame + 1) / static_cast<float>(sheet.frames);
    f.overlay.image(t.handle, x, y, static_cast<float>(t.width), fh, {0.0f, vTop}, {1.0f, vBottom}, color);
}

// --- SpriteButton -------------------------------------------------------------------

SpriteButton::SpriteButton(SpriteSheet sheet_, float x, float y, std::function<void()> click)
    : sheet(std::move(sheet_)), onClick(std::move(click)) {
    box = {x, y, 0, 0};
}

void SpriteButton::draw(UiFrame& f, bool focused) {
    // UIBMButton::GetHitArea: one frame's rectangle. The frame follows the
    // button's state (UIBMButton::Switch, drawn by UIBMButton::Cull).
    const Vec2 size = spriteFrameSize(f, sheet);
    box.w = size.x;
    box.h = size.y;
    int frame = 0;
    const bool held = m_pressed && f.nav.mouseDown && box.contains(f.nav.mouse);
    if (!enabled)
        frame = sheet.frames >= 4 ? (sheet.frames == 7 ? 4 : sheet.frames - 1) : 0;
    else if (lit && lit())
        frame = focused ? 3 : 2;
    else if (held && sheet.frames >= 3)
        frame = 2;
    else if (focused)
        frame = 1;
    drawSpriteFrame(f, sheet, frame, box.x, box.y,
                    !enabled && sheet.frames < 4 ? render::packColor(120, 120, 120) : 0xFFFFFFFFu);
}

// UIBMButton::Action on Enter: the button's sound (UIBMButton::PlaySound),
// then its callback.
bool SpriteButton::activate(UiFrame& f) {
    if (!enabled)
        return false;
    f.play(sound, soundVolume);
    if (onClick)
        onClick();
    return true;
}

void SpriteButton::mouse(UiFrame& f, bool hovered) {
    if (!enabled) {
        m_pressed = false;
        return;
    }
    // UIBMButton::Action: the press shows the pressed frame and plays the
    // button's sound; the menu acts on the release over the button
    // (UIMenu::CheckMouseHits with a release event), wherever the press was.
    if (hovered && f.nav.mousePressed) {
        m_pressed = true;
        f.play(sound, soundVolume);
    }
    if (f.nav.mouseReleased) {
        m_pressed = false;
        if (hovered && onClick)
            onClick();
    } else if (!f.nav.mouseDown) {
        m_pressed = false;
    }
}

// --- LampItem --------------------------------------------------------------------------

LampItem::LampItem(SpriteSheet sheet_, float x, float y, std::function<bool()> on, std::function<void()> click)
    : sheet(std::move(sheet_)), isOn(std::move(on)), onClick(std::move(click)) {
    box = {x, y, 0, 0};
}

void LampItem::draw(UiFrame& f, bool focused) {
    // UIBMButton::Update re-reads the lit state every frame; UIBMButton::Switch
    // and UIBMButton::Cull pick and draw the frame.
    const Vec2 size = spriteFrameSize(f, sheet);
    box.w = size.x;
    box.h = size.y;
    const bool selected = isOn && isOn();
    if (!selected)
        m_shownOff = false;
    const bool on = selected && !m_shownOff;
    const int frame = !enabled ? 4 : (on ? (focused ? 3 : 2) : (focused ? 1 : 0));
    drawSpriteFrame(f, sheet, frame, box.x, box.y);
}

// UIBMButton::Action on Enter or Space: the lamp's sound and its callback,
// which flips it (UIBMButton::DoToggle) or selects it (UIBMButton::MexOn).
bool LampItem::activate(UiFrame& f) {
    if (!enabled || readOnly)
        return false;
    f.play(sound, soundVolume);
    if (onClick)
        onClick();
    return true;
}

void LampItem::mouse(UiFrame& f, bool hovered) {
    // UIBMButton::DoToggle: toggles flip on the press.
    if (!hovered || !f.nav.mousePressed)
        return;
    if (radio && enabled && !readOnly && isOn && isOn()) {
        f.play(sound, soundVolume);
        m_shownOff = !m_shownOff;
        return;
    }
    activate(f);
}

void LampItem::focusChanged(bool) { m_shownOff = false; }

// --- ValueBox ---------------------------------------------------------------------------

namespace {
constexpr float kDropHeight = 23.0f; // drop_arrow frame height + 2 (UITextDropdown::Init)
} // namespace

ValueBox::ValueBox(Box b, std::function<std::vector<std::string>()> opts, std::function<int()> g,
                   std::function<void(int)> s)
    : options(std::move(opts)), get(std::move(g)), set(std::move(s)) {
    box = b;
    box.h = kDropHeight;
}

void ValueBox::draw(UiFrame& f, bool focused) {
    const auto opts = options();
    const int cur = m_open ? m_hover : (get ? get() : -1);
    const std::string text = cur >= 0 && cur < static_cast<int>(opts.size()) ? opts[static_cast<std::size_t>(cur)] : "";
    // UITextDropdown::Switch: colour 3 while focused, else colour 0
    // (MenuManager::GetFGColor: red / yellow in the menus, yellow-green /
    // white in the popups).
    const bool hot = focused || m_open;
    const std::uint32_t color = popup ? (hot ? style::kPopupFocus : style::kPopupText)
                                      : (hot ? style::kValueTextFocus : style::kValueText);
    const FontSpec spec = this->font ? *this->font : popup ? style::popupSmallFont() : style::valueFont();
    if (popup && !label.empty())
        f.text.draw(f.overlay, spec, label, box.x, box.y - labelHeight, style::kPopupText);
    const float lh = f.text.lineHeight(f.overlay, spec);
    const Vec4 clip{box.x, box.y, box.w, box.h};
    f.overlay.setClip(&clip);
    f.text.draw(f.overlay, spec, text, box.x + 5, box.y + (box.h - lh) * 0.5f, color);
    f.overlay.setClip(nullptr);
    if (popup)
        outline(f.overlay, box, rgba(255, 255, 255)); // text effect 4: a white-pen rectangle
    // UITextDropdown::Cull copies the arrow's frame whenever the box is
    // drawn, read-only or not.
    if (showArrow && enabled)
        drawSpriteFrame(f, {popup ? "texture/drop_arrow2.tga" : "texture/drop_arrow.tga", 3},
                        m_open ? 2 : (focused ? 1 : 0), box.x + box.w - 21, box.y + 1);
}

std::vector<ValueBox::Cell> ValueBox::listCells(std::size_t count) const {
    // mmDropDown::InitString: rows of the box's size below it; a row that
    // would pass the bottom of the screen starts a further column to the
    // right. When the list is too tall and two columns would not fit right of
    // the box, the list starts as many columns further left as it has extra
    // columns (not before the screen's left edge); otherwise it starts at the
    // box, whether or not the columns fit.
    std::vector<Cell> cells;
    const float top = box.y + box.h;
    const int perColumn = std::max(1, static_cast<int>((480.0f - top) / kDropHeight));
    float x0 = box.x;
    if (top + static_cast<float>(count) * kDropHeight > 480.0f && box.x + 2.0f * box.w > 640.0f) {
        const int n = static_cast<int>(count);
        const int extraColumns = n / perColumn + (n % perColumn != 0 ? 1 : 0) - 1;
        x0 = std::max(0.0f, box.x - static_cast<float>(extraColumns) * box.w);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const int col = static_cast<int>(i) / perColumn, row = static_cast<int>(i) % perColumn;
        cells.push_back({{x0 + static_cast<float>(col) * box.w, top + static_cast<float>(row) * kDropHeight, box.w,
                          kDropHeight},
                         static_cast<int>(i)});
    }
    return cells;
}

bool ValueBox::activate(UiFrame& f) {
    if (!enabled || readOnly || options().empty())
        return false;
    // UITextDropdown::SetSliderFocus: the open list takes all input.
    m_open = true;
    m_hover = std::max(0, get());
    // UITextDropdown::Action: opening with Enter plays
    // MenuManager::PlaySound(1), which only the popups hear.
    if (popup && !f.nav.mousePressed)
        f.play("Selectionmade", 0.75f);
    return true;
}

bool ValueBox::adjust(UiFrame&, int) {
    // UITextDropdown: Left/Right do nothing on a closed box.
    return true;
}

void ValueBox::mouse(UiFrame& f, bool hovered) {
    // UITextDropdown::Action: a press inside the box opens the list.
    if (hovered && f.nav.mousePressed && !m_open)
        activate(f);
}

void ValueBox::modalInput(UiFrame& f) {
    const auto opts = options();
    const int n = static_cast<int>(opts.size());
    if (n == 0) {
        m_open = false;
        return;
    }
    // TextDropWidget::IncDrop / DecDrop step by one and stop at the ends;
    // TextDropWidget::SetValue turns an entry that cannot be picked into the
    // first one that can (mmDropDown::FindFirstEnabled, entry 0 when none).
    auto settle = [&](int i) {
        i = std::clamp(i, 0, n - 1);
        if (isOptionEnabled(i))
            return i;
        for (int k = 0; k < n; ++k)
            if (isOptionEnabled(k))
                return k;
        return 0;
    };
    const NavInput& nav = f.nav;
    // UITextDropdown::CaptureAction: every Home, Up, Left, Down, Right and
    // End in the open list, and a mouse pick, play MenuManager::PlaySound(1)
    // (heard in the popups only).
    const bool step = nav.up || nav.left || nav.down || nav.right || nav.home || nav.end;
    if (popup && step)
        f.play("Selectionmade", 0.75f);
    if (nav.up || nav.left)
        m_hover = settle(m_hover - 1);
    if (nav.down || nav.right)
        m_hover = settle(m_hover + 1);
    if (nav.home)
        m_hover = settle(0);
    if (nav.end)
        m_hover = settle(n - 1);
    const auto cells = listCells(opts.size());
    const Cell* under = nullptr;
    for (const auto& c : cells)
        if (c.box.contains(nav.mouse))
            under = &c;
    // mmDropDown::SetHighlight moves the highlight to enabled entries only.
    if (under && nav.mouseMoved && isOptionEnabled(under->index))
        m_hover = under->index;
    // UITextDropdown::CaptureAction: Enter or a release over an entry picks
    // it (an entry that cannot be picked picks the first one that can).
    if (nav.accept || (nav.mouseReleased && under)) {
        if (popup && !nav.accept)
            f.play("Selectionmade", 0.75f);
        set(settle(nav.accept ? m_hover : under->index));
        m_open = false;
    } else if (nav.back || (nav.mousePressed && !under && !box.contains(nav.mouse))) {
        m_open = false;
    }
}

void ValueBox::drawPopup(UiFrame& f) {
    if (!m_open)
        return;
    const auto opts = options();
    // mmDropDown::Update draws its entries in the TextDropWidget's font (the
    // popups' label font there) and always in yellow, the locked ones olive
    // (mmDropDown::SetDisabledColors), the highlight outlined
    // (mmDropDown::SetHighlight).
    const FontSpec spec = this->font ? *this->font : popup ? style::popupSmallFont() : style::valueFont();
    for (const auto& c : listCells(opts.size())) {
        f.overlay.rect(c.box.x, c.box.y, c.box.w, c.box.h, rgba(0, 0, 0));
        const Vec4 clip{c.box.x, c.box.y, c.box.w, c.box.h};
        f.overlay.setClip(&clip);
        f.text.draw(f.overlay, spec, opts[static_cast<std::size_t>(c.index)], c.box.x, c.box.y,
                    isOptionEnabled(c.index) ? style::kValueText : style::kOptionDisabled);
        f.overlay.setClip(nullptr);
        if (c.index == m_hover)
            outline(f.overlay, c.box, rgba(255, 255, 255));
    }
}

bool stepOption(ValueBox& box, int dir, bool wrap) {
    if (!box.enabled || box.readOnly)
        return false;
    const int n = static_cast<int>(box.options().size());
    if (n == 0)
        return false;
    const int cur = box.get();
    int i = cur;
    for (int tries = 0; tries < n; ++tries) {
        i += dir;
        if (i < 0 || i >= n) {
            if (!wrap)
                return false;
            i = (i + n) % n;
        }
        if (!box.isOptionEnabled(i)) {
            // MM2's arrows do not step past a locked entry unless they wrap.
            if (!wrap)
                return false;
            continue;
        }
        if (i == cur)
            return false;
        box.set(i);
        return true;
    }
    return false;
}

// --- TextButton ----------------------------------------------------------------------------

TextButton::TextButton(Box b, std::string text, std::function<void()> click)
    : label(std::move(text)), onClick(std::move(click)) {
    box = b;
}

void TextButton::draw(UiFrame& f, bool focused) {
    // UIButton::Switch: colour 3 when focused, else 0; colour 5 when it
    // cannot be used (UIButton::SetReadOnly).
    const std::uint32_t color = !enabled ? style::kPopupDisabled : (focused ? style::kPopupFocus : style::kPopupText);
    if (type < 0) {
        f.text.draw(f.overlay, font, label, box.x, box.y, color);
        return;
    }
    // UIButton::SetType's effects as mmTextNode::RenderText draws them:
    // DT_VCENTER for every type, DT_CENTER for types 1 and 2, the white-pen
    // rectangle for type 1.
    const float lh = f.text.lineHeight(f.overlay, font);
    const float y = box.y + (box.h - lh) * 0.5f;
    if (type == 1 || type == 2)
        f.text.draw(f.overlay, font, label, box.x + box.w * 0.5f, y, color, Align::Center);
    else
        f.text.draw(f.overlay, font, label, box.x, y, color);
    if (type == 1)
        outline(f.overlay, box, rgba(255, 255, 255));
}

// UIButton::Action: Enter, or a click (TextButton::mouse).
bool TextButton::activate(UiFrame& f) {
    if (!enabled)
        return false;
    f.play("Selectionmade", 0.75f); // MenuManager::PlaySound(1), heard in the popups
    if (onClick)
        onClick();
    return true;
}

void TextButton::mouse(UiFrame& f, bool hovered) {
    if (hovered && f.nav.mouseReleased)
        activate(f);
}

// --- TextToggle ----------------------------------------------------------------------------

TextToggle::TextToggle(Box b, std::string text, std::function<bool()> on, std::function<void()> f)
    : label(std::move(text)), isOn(std::move(on)), flip(std::move(f)) {
    box = b;
}

void TextToggle::draw(UiFrame& f, bool focused) {
    // UIButton::Switch colours the label (3 focused, else 0); the ON/OFF
    // text node `stateWidth` wide (UIToggleButton2::Init) keeps the default
    // white and shows the state (UIToggleButton2::DrawOn / DrawOff).
    const std::uint32_t color = !enabled ? style::kPopupDisabled : (focused ? style::kPopupFocus : style::kPopupText);
    const float lh = f.text.lineHeight(f.overlay, font);
    const float y = box.y + (box.h - lh) * 0.5f;
    const Box labelBox{box.x, box.y, box.w - stateWidth, box.h};
    f.text.draw(f.overlay, font, label, labelBox.x + labelBox.w * 0.5f, y, color, Align::Center);
    outline(f.overlay, labelBox, rgba(255, 255, 255));
    const bool on = isOn && isOn();
    f.text.draw(f.overlay, font, on ? onText : offText, labelBox.x + labelBox.w + stateWidth * 0.5f, y,
                style::kPopupText, Align::Center);
}

bool TextToggle::activate(UiFrame& f) {
    // UIToggleButton2::Action: Enter toggles (DoToggle), then UIButton::Action
    // plays MenuManager::PlaySound(1) and calls the callback.
    if (!enabled || readOnly)
        return false;
    if (flip)
        flip();
    f.play("Selectionmade", 0.75f);
    return true;
}

void TextToggle::mouse(UiFrame& f, bool hovered) {
    if (hovered && f.nav.mouseReleased)
        activate(f);
}

// --- TextBox -------------------------------------------------------------------------------

TextBox::TextBox(Box b, std::function<std::string()> t, Align a) : text(std::move(t)), align(a) { box = b; }

// Drawn like a UILabel (UILabel::Init: its font, colour and alignment).
void TextBox::draw(UiFrame& f, bool) {
    const std::string s = text ? text() : std::string();
    const float lh = f.text.lineHeight(f.overlay, font);
    const float x = align == Align::Left ? box.x + 5 : (align == Align::Center ? box.x + box.w * 0.5f : box.x + box.w - 5);
    f.text.draw(f.overlay, font, s, x, box.y + (box.h - lh) * 0.5f, color, align);
}

// --- Roller -----------------------------------------------------------------------------------

namespace {
constexpr float kRollerArrowW = 29.0f, kRollerArrowH = 17.0f;
} // namespace

Roller::Roller(Box b, std::function<std::vector<std::string>()> opts, std::function<int()> g,
               std::function<void(int)> s)
    : options(std::move(opts)), get(std::move(g)), set(std::move(s)) {
    box = b;
    box.h = 2 * kRollerArrowH;
}

void Roller::draw(UiFrame& f, bool focused) {
    // UITextRoller2::Switch: the value in colour 3 when focused, else 0.
    // UITextRoller2::Cull: the arrows only while writable.
    const auto opts = options();
    const int cur = get ? get() : -1;
    const std::string text = cur >= 0 && cur < static_cast<int>(opts.size()) ? opts[static_cast<std::size_t>(cur)] : "";
    const FontSpec font = style::valueFont();
    const float lh = f.text.lineHeight(f.overlay, font);
    f.text.draw(f.overlay, font, text, box.x + (box.w - kRollerArrowW) * 0.5f, box.y + (box.h - lh) * 0.5f,
                focused ? style::kValueTextFocus : style::kValueText, Align::Center);
    if (readOnly || !enabled)
        return;
    const float ax = box.x + box.w - kRollerArrowW;
    drawSpriteFrame(f, {"texture/roller_up.tga", 3}, m_clicked > 0 ? 2 : (focused ? 1 : 0), ax, box.y);
    drawSpriteFrame(f, {"texture/roller_down.tga", 3}, m_clicked < 0 ? 2 : (focused ? 1 : 0), ax, box.y + 18);
}

bool Roller::adjust(UiFrame& f, int dir) {
    m_clicked = 0; // UITextRoller2::Action: Left / Right clear the pressed arrow
    if (!enabled || readOnly)
        return true;
    const int n = static_cast<int>(options().size());
    const int last = maxIndex >= 0 ? std::min(maxIndex, n - 1) : n - 1;
    const int next = std::clamp(get() + dir, 0, std::max(0, last));
    // UITextRoller2::Inc / Dec: "Switch" on every step, also at a limit.
    f.play("Switch", 0.85f);
    if (next != get())
        set(next);
    return true;
}

void Roller::mouse(UiFrame& f, bool hovered) {
    // UITextRoller2::Action: the pressed arrow shows until the button is
    // released. UITextRoller2::EvalMouseXY: a press on the up arrow steps
    // up, on the down arrow down.
    if (f.nav.mouseReleased)
        m_clicked = 0;
    if (!hovered || !f.nav.mousePressed || readOnly)
        return;
    const float ax = box.x + box.w - kRollerArrowW;
    const Box upBox{ax, box.y, kRollerArrowW, kRollerArrowH};
    const Box downBox{ax, box.y + 18, kRollerArrowW, kRollerArrowH};
    if (upBox.contains(f.nav.mouse)) {
        adjust(f, 1);
        m_clicked = 1;
    } else if (downBox.contains(f.nav.mouse)) {
        adjust(f, -1);
        m_clicked = -1;
    }
}

void Roller::focusChanged(bool) { m_clicked = 0; }

// --- Slider ---------------------------------------------------------------------------------

namespace {
constexpr float kArrowW = 25.0f; // slider_larr / slider_rarr: 25x29, 5 frames
constexpr float kBarTop = 11.0f; // the value bar starts 11 px below the row's top
} // namespace

Slider::Slider(Box row, std::function<float()> g, std::function<void(float)> s, float lo, float hi)
    : get(std::move(g)), set(std::move(s)), min(lo), max(hi) {
    box = row;
}

int Slider::segments() const {
    // mmSlider: the track is cut into 2 px segments.
    const float w = std::round(box.w);
    return std::clamp(static_cast<int>(std::round((w - 2.0f * kArrowW) / 2.0f)) - 1, 2, 300);
}

float Slider::step() const {
    // mmSlider::SetStep: twenty positions on a track of more than 20 segments.
    const int positions = segments() > 20 ? 20 : 5;
    return (max - min) / static_cast<float>(positions - 1);
}

void Slider::draw(UiFrame& f, bool focused) {
    // UISlider's label (colour 0, white in the popups) does not change with
    // the focus; the bar's band does.
    if (!label.empty())
        f.text.draw(f.overlay, style::popupSmallFont(), label, box.x, box.y, style::kPopupText);
    const float trackX = box.x + kArrowW;
    const float trackW = 2.0f * static_cast<float>(segments());
    // mmSlider::UpdatePosition: the bar fills the whole part of the fraction
    // x (segments + 1) segments.
    const float frac = max > min ? std::clamp((get() - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    const float filled =
        std::min(trackW, 2.0f * std::floor(frac * static_cast<float>(segments() + 1)));
    const float barY = rowY() + kBarTop;
    if (readOnly) {
        // Read-only sliders (the garage's statistics): slider_roactl, 11 px,
        // the rest a 1 px slider_roinactl line (its height is inferred).
        const UiTexture& on = f.textures.get("texture/slider_roactl.tga");
        const UiTexture& off = f.textures.get("texture/slider_roinactl.tga");
        if (on)
            drawRows(f, on, 0, static_cast<int>(on.height), trackX, barY, filled, static_cast<float>(on.height),
                     filled / static_cast<float>(on.width));
        if (off)
            drawRows(f, off, 0, 1, trackX + filled, barY, trackW - filled, 1.0f,
                     (trackW - filled) / static_cast<float>(off.width));
        return;
    }
    // mmSlider::LoadBitmap, mmSlider::Cull: the bitmaps are read in 6-row
    // bands: 0 unfocused, 1 focused, 2 disabled; black is transparent. The
    // empty part is the band's first row of slider_inactl (the band reading
    // is inferred from mmSlider).
    const int band = !enabled ? 2 : (focused ? 1 : 0);
    const UiTexture& on = f.textures.getColorKeyed("texture/slider_actl.tga");
    const UiTexture& off = f.textures.getColorKeyed("texture/slider_inactl.tga");
    if (on)
        drawRows(f, on, band * 6, 6, trackX, barY, filled, 6.0f, filled / static_cast<float>(on.width));
    if (off)
        drawRows(f, off, band * 6, 1, trackX + filled, barY, trackW - filled, 1.0f,
                 (trackW - filled) / static_cast<float>(off.width));
    const int base = !enabled ? 4 : (focused ? 1 : 0);
    const char* left = balance ? "texture/slider_lbal.tga" : "texture/slider_larr.tga";
    const char* right = balance ? "texture/slider_rbal.tga" : "texture/slider_rarr.tga";
    drawSpriteFrame(f, {left, 5}, enabled && m_clicked < 0 ? 2 : base, box.x, rowY());
    drawSpriteFrame(f, {right, 5}, enabled && m_clicked > 0 ? 2 : base, trackX + trackW, rowY());
}

bool Slider::adjust(UiFrame& f, int dir) {
    if (!enabled || readOnly)
        return true;
    // UISlider::Action: Left mmSlider::Dec, Right mmSlider::Inc, then
    // "Switch", also at a limit. The step starts from the slider's own value,
    // which mmSlider::SetValue clamped when UISlider::Update took the bound
    // variable (a race's cop count of 3 is 1 on the slider, so Left gives
    // 1 - step).
    set(std::clamp(std::clamp(get(), min, max) + step() * static_cast<float>(dir), min, max));
    f.play("Switch", 0.85f);
    m_clicked = 0;
    return true;
}

void Slider::mouse(UiFrame& f, bool hovered) {
    // UISlider::Action: the pressed arrow shows until the button is released.
    if (f.nav.mouseReleased)
        m_clicked = 0;
    if (!enabled || readOnly || !hovered || !f.nav.mousePressed)
        return;
    // UISlider::EvalMouseXY: only the arrows' row counts (not a popup
    // slider's label above it).
    const float rowH = spriteFrameSize(f, {balance ? "texture/slider_lbal.tga" : "texture/slider_larr.tga", 5}).y;
    if (f.nav.mouse.y < rowY() || (rowH > 0.0f && f.nav.mouse.y >= rowY() + rowH))
        return;
    const float trackX = box.x + kArrowW;
    const float trackW = 2.0f * static_cast<float>(segments());
    const float mx = f.nav.mouse.x;
    if (mx < trackX) {
        adjust(f, -1);
        m_clicked = -1;
    } else if (mx >= trackX + trackW) {
        adjust(f, 1);
        m_clicked = 1;
    } else {
        // A click on the track sets the value where it lands (no dragging;
        // mmSlider::SetValue).
        set(std::clamp(min + (max - min) * (mx - trackX) / trackW, min, max));
        f.play("Switch", 0.85f);
        m_clicked = 0;
    }
}

void Slider::focusChanged(bool) { m_clicked = 0; }

// --- ListBox --------------------------------------------------------------------------------

ListBox::ListBox(Box b, std::function<std::vector<std::string>()> it, std::function<int()> g, std::function<void(int)> s)
    : items(std::move(it)), get(std::move(g)), set(std::move(s)) {
    box = b;
}

bool ListBox::moveSelection(int dir) {
    const int n = static_cast<int>(items().size());
    if (n == 0)
        return false;
    const int cur = get();
    const int next = std::clamp(cur + dir, 0, n - 1);
    if (next == cur)
        return false;
    set(next);
    return true;
}

void ListBox::draw(UiFrame& f, bool focused) {
    const auto list = items();
    const int rows = std::max(1, static_cast<int>((box.h - 4) / rowHeight));
    const int sel = get();
    if (sel >= 0) {
        if (sel < m_scroll)
            m_scroll = sel;
        if (sel >= m_scroll + rows)
            m_scroll = sel - rows + 1;
    }
    m_scroll = std::clamp(m_scroll, 0, std::max(0, static_cast<int>(list.size()) - rows));
    const FontSpec font = style::valueFont();
    const Vec4 clip{box.x, box.y, box.w, box.h};
    for (int i = 0; i < rows; ++i) {
        const int idx = m_scroll + i;
        if (idx >= static_cast<int>(list.size()))
            break;
        const float y = box.y + 2 + static_cast<float>(i) * rowHeight;
        if (idx == sel)
            outline(f.overlay, {box.x + 2, y, box.w - 4, rowHeight}, rgba(255, 255, 255));
        f.overlay.setClip(&clip);
        f.text.draw(f.overlay, font, list[static_cast<std::size_t>(idx)], box.x + 5, y + 1,
                    idx == sel && focused ? style::kValueTextFocus : style::kValueText);
        f.overlay.setClip(nullptr);
    }
    if (static_cast<int>(list.size()) > rows) {
        drawSpriteFrame(f, {"texture/scroll_uarr.tga", 4}, m_scroll > 0 ? 0 : 3, box.x + box.w - 21, box.y + 1);
        drawSpriteFrame(f, {"texture/scroll_darr.tga", 4},
                        m_scroll + rows < static_cast<int>(list.size()) ? 0 : 3, box.x + box.w - 21,
                        box.y + box.h - 22);
    }
}

void ListBox::mouse(UiFrame& f, bool hovered) {
    if (!hovered)
        return;
    const auto list = items();
    if (f.nav.wheel != 0.0f)
        m_scroll -= static_cast<int>(f.nav.wheel);
    if (!f.nav.mousePressed)
        return;
    const int rows = std::max(1, static_cast<int>((box.h - 4) / rowHeight));
    if (static_cast<int>(list.size()) > rows && f.nav.mouse.x >= box.x + box.w - 22) {
        m_scroll += f.nav.mouse.y < box.y + box.h * 0.5f ? -1 : 1;
        return;
    }
    const int row = m_scroll + static_cast<int>((f.nav.mouse.y - box.y - 2) / rowHeight);
    if (row >= 0 && row < static_cast<int>(list.size())) {
        const bool dbl = row == m_lastClickRow && f.time - m_lastClick < 0.4;
        set(row);
        m_lastClick = f.time;
        m_lastClickRow = row;
        if (onPick)
            onPick(row);
        if (dbl && onDoubleClick)
            onDoubleClick();
    }
}

bool ListBox::activate(UiFrame&) {
    const int sel = get();
    if (!onPick || sel < 0 || sel >= static_cast<int>(items().size()))
        return false;
    onPick(sel);
    return true;
}

// --- TextEntry ------------------------------------------------------------------------------

TextEntry::TextEntry(Box b, std::string* v, std::size_t maxLen) : value(v), maxLength(maxLen) { box = b; }

void TextEntry::draw(UiFrame& f, bool focused) {
    // UITextField::ToggleField: an opaque black card (Card2D::Cull) behind
    // red text while editing, nothing behind yellow text otherwise (the frame
    // around the field is painted on the backgrounds).
    const FontSpec font = popup ? style::popupSmallFont() : style::valueFont();
    const float lh = f.text.lineHeight(f.overlay, font);
    const bool active = focused && m_editing;
    if (active)
        f.overlay.rect(box.x, box.y, box.w, box.h, rgba(0, 0, 0));
    const Vec4 clip{box.x, box.y, box.w, box.h};
    f.overlay.setClip(&clip);
    const std::uint32_t color = popup ? (active ? style::kPopupFocus : style::kPopupText)
                                      : (active ? style::kValueTextFocus : style::kValueText);
    f.text.draw(f.overlay, font, " " + *value, box.x, box.y + (box.h - lh) * 0.5f, color);
    f.overlay.setClip(nullptr);
    if (popup)
        outline(f.overlay, box, rgba(255, 255, 255));
}

bool TextEntry::activate(UiFrame&) {
    if (!m_editing) {
        m_editing = true;
        m_fresh = false;
    }
    return true;
}

void TextEntry::beginEdit() {
    m_editing = true;
    m_fresh = true;
}

void TextEntry::focusChanged(bool focused) {
    // UITextField::Switch / ToggleField: focus is editing; the first key
    // replaces the text.
    m_editing = focused;
    m_fresh = focused;
}

void TextEntry::modalInput(UiFrame& f) {
    for (char c : f.nav.text) {
        // No control characters (UITextField::IsValidChar). Printable ASCII
        // and UTF-8 continuation bytes; the fonts cover Latin-1.
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            continue;
        if (m_fresh) {
            value->clear();
            m_fresh = false;
        }
        if (value->size() < maxLength) {
            value->push_back(c);
            // UITextField::WmCharHandler: MenuManager::PlaySound(0) for every
            // accepted character, heard in the popups only (PUChat).
            if (popup)
                f.play("Moveselector", 0.75f);
        }
    }
    if (f.nav.backspace && !value->empty()) {
        m_fresh = false;
        // Remove one UTF-8 code point.
        std::size_t n = value->size() - 1;
        while (n > 0 && (static_cast<unsigned char>((*value)[n]) & 0xC0) == 0x80)
            --n;
        value->resize(n);
    }
    // Enter commits; Tab and Escape end editing and are handled by the menu.
    if (f.nav.enter) {
        m_editing = false;
        if (popup)
            f.play("Moveselector", 0.75f); // UITextField::KeyAction
        if (onCommit)
            onCommit();
    } else if (f.nav.tabNext || f.nav.back) {
        m_editing = false;
    }
}

// --- ScrollBar (VSWidget) ---------------------------------------------------------------------

namespace {
const SpriteSheet kScrollUp{"texture/scroll_uarr.tga", 4, true};
const SpriteSheet kScrollDown{"texture/scroll_darr.tga", 4, true};
const SpriteSheet kScrollOn{"texture/scroll_act.tga", 2, true};
const SpriteSheet kScrollOff{"texture/scroll_inact.tga", 2, true};
constexpr float kScrollGap = 2.0f; // VSWidget::VSWidget (+0xac, +0xb0)
} // namespace

void ScrollBar::place(float x, float y, float height) {
    m_x = x;
    m_y = y;
    m_height = height;
}

ScrollBar::Metrics ScrollBar::metrics(UiFrame& f) const {
    Metrics m;
    const Vec2 seg = spriteFrameSize(f, kScrollOn);
    const Vec2 arrow = spriteFrameSize(f, kScrollDown);
    m.segW = seg.x;
    m.segH = std::floor(seg.y);
    m.arrowW = arrow.x;
    m.arrowH = std::floor(arrow.y);
    if (m.segH <= 0.0f)
        return m;
    const int height = static_cast<int>(m_height);
    m.segments = std::clamp((height - 2 * static_cast<int>(m.arrowH)) / static_cast<int>(m.segH), 2, 200);
    m.base = std::max(1, height / static_cast<int>(m.segH) - 1);
    const int thumb = static_cast<int>(static_cast<float>(m.segments) * m_ratio) - 1;
    m.thumb = thumb > 0 ? std::min(thumb, m.segments - 1) : 1;
    return m;
}

void ScrollBar::clamp(const Metrics& m) { m_value = std::clamp(m_value, 0, std::max(0, m.segments - 1 - m.thumb)); }

bool ScrollBar::inc(UiFrame& f) {
    const Metrics m = metrics(f);
    m_downPressed = true;
    const int before = m_value;
    ++m_value;
    clamp(m);
    return m_value != before;
}

bool ScrollBar::dec(UiFrame& f) {
    const Metrics m = metrics(f);
    m_upPressed = true;
    const int before = m_value;
    --m_value;
    clamp(m);
    return m_value != before;
}

void ScrollBar::setRow(UiFrame& f, int row, int count) {
    const Metrics m = metrics(f);
    m_value = count > 0 ? static_cast<int>(static_cast<double>(row) * m.base / count + 0.5) : 0;
    clamp(m);
}

int ScrollBar::firstRow(UiFrame& f, int count, int rows) const {
    const Metrics m = metrics(f);
    const int row = static_cast<int>(static_cast<double>(m_value) / m.base * count + 0.5);
    return std::clamp(row, 0, std::max(0, count - rows));
}

bool ScrollBar::contains(UiFrame& f, Vec2 p) const {
    const Metrics m = metrics(f);
    // SetHotSpots: the arrows' width from the trough's left, the arrows and
    // the trough without the gaps.
    const float bottom = m_y + m.arrowH + m.segH * static_cast<float>(m.segments) + m.arrowH;
    return p.x >= m_x && p.x <= m_x + m.arrowW && p.y >= m_y && p.y <= bottom;
}

void ScrollBar::draw(UiFrame& f, bool focused) const {
    Metrics m = metrics(f);
    const int top = std::clamp(m_value, 0, std::max(0, m.segments - 1 - m.thumb));
    const float arrowX = m_x - std::floor((m.arrowW - m.segW) * 0.5f);
    const int band = focused ? 1 : 0;
    drawSpriteFrame(f, kScrollUp, focused ? (m_upPressed ? 2 : 1) : 0, arrowX, m_y);
    float y = m_y + kScrollGap + m.arrowH;
    for (int i = 0; i < m.segments; ++i) {
        const bool thumb = i >= top && i <= top + m.thumb;
        drawSpriteFrame(f, thumb ? kScrollOn : kScrollOff, band, m_x, y);
        y += m.segH;
    }
    drawSpriteFrame(f, kScrollDown, focused ? (m_downPressed ? 2 : 1) : 0, arrowX, y + kScrollGap);
}

bool ScrollBar::mouse(UiFrame& f, bool press) {
    const Metrics m = metrics(f);
    const Vec2 p = f.nav.mouse;
    if (!contains(f, p)) {
        release();
        return false;
    }
    const float troughTop = m_y + m.arrowH;
    const float troughBottom = troughTop + m.segH * static_cast<float>(m.segments);
    if (p.y <= troughTop) {
        dec(f);
    } else if (p.y > troughBottom) {
        inc(f);
    } else {
        // The thumb walks towards the pointer until it covers it.
        while (p.y < troughTop + m.segH * static_cast<float>(m_value) && dec(f)) {
        }
        while (p.y > troughTop + m.segH * static_cast<float>(m_value + m.thumb + 1) && inc(f)) {
        }
        release();
    }
    if (press)
        f.play("Switch", 0.85f);
    return true;
}

// --- Picture -----------------------------------------------------------------------------------

Picture::Picture(Box b, std::function<std::string()> p) : path(std::move(p)) { box = b; }

// UIIcon::Switch only records the focus: a focus stop with no highlight.
bool Picture::focusable() const {
    return focusStop && Widget::focusable() && path && !path().empty();
}

// UIIcon::Cull; the picture can change at any time (UIIcon::LoadBitchmap).
void Picture::draw(UiFrame& f, bool) {
    const std::string p = path ? path() : std::string();
    if (p.empty())
        return;
    drawImage(f.overlay, f.textures.get(p), box.x, box.y, box.w, box.h);
}

// --- Menu ----------------------------------------------------------------------------------------

Widget* Menu::focused() const {
    return m_focus >= 0 && m_focus < static_cast<int>(m_widgets.size()) ? m_widgets[static_cast<std::size_t>(m_focus)].get()
                                                                          : nullptr;
}

void Menu::park() {
    setFocus(-1);
    m_parked = true;
    m_highlight = false;
}

// Moves the keyboard position, also between the page and the strip
// (MenuManager::SwitchFocus): the old widget is unlit (UIMenu::ClearSelected),
// the new one lit (UIMenu::SetSelected).
void Menu::setFocus(int index) {
    if (index >= 0)
        m_parked = false;
    if (index == m_focus)
        return;
    // Only a move from one widget to another brings the help picture back.
    if (m_focus >= 0 && index >= 0)
        m_helpHidden = false;
    if (Widget* old = focused())
        old->focusChanged(false);
    m_focus = index;
    if (Widget* w = focused())
        w->focusChanged(true);
}

void Menu::focus(const Widget* w) {
    for (std::size_t i = 0; i < m_widgets.size(); ++i) {
        if (m_widgets[i].get() == w) {
            // The first explicit focus is the page's initial one.
            if (m_initial < 0)
                m_initial = static_cast<int>(i);
            setFocus(static_cast<int>(i));
        }
    }
}

// UIMenu::SetFocusWidget.
void Menu::setInitialFocus(const Widget* w) {
    for (std::size_t i = 0; i < m_widgets.size(); ++i)
        if (m_widgets[i].get() == w)
            m_initial = static_cast<int>(i);
    focus(w);
}

void Menu::resetFocus() {
    m_highlight = true;
    if (m_initial >= 0 && m_widgets[static_cast<std::size_t>(m_initial)]->focusable())
        setFocus(m_initial);
    else
        setFocus(firstFocusable(0) >= 0 ? firstFocusable(0) : firstFocusable(1));
}

void Menu::moveToEnd(const Widget* w) {
    const Widget* focusedWidget = focused();
    const Widget* initial = m_initial >= 0 ? m_widgets[static_cast<std::size_t>(m_initial)].get() : nullptr;
    const auto it = std::ranges::find_if(m_widgets, [w](const auto& p) { return p.get() == w; });
    if (it == m_widgets.end())
        return;
    auto moved = std::move(*it);
    m_widgets.erase(it);
    m_widgets.push_back(std::move(moved));
    m_focus = m_initial = -1;
    for (std::size_t i = 0; i < m_widgets.size(); ++i) {
        if (m_widgets[i].get() == focusedWidget)
            m_focus = static_cast<int>(i);
        if (m_widgets[i].get() == initial)
            m_initial = static_cast<int>(i);
    }
}

std::vector<const Widget*> Menu::widgetsInGroup(int g) const {
    std::vector<const Widget*> out;
    for (const auto& w : m_widgets)
        if (w->group == g)
            out.push_back(w.get());
    return out;
}

bool Menu::modalActive() const {
    const Widget* w = focused();
    return w && w->modal();
}

// UIMenu::FindTheFirstFocusWidget, within one group.
int Menu::firstFocusable(int group) const {
    for (std::size_t i = 0; i < m_widgets.size(); ++i)
        if (m_widgets[i]->group == group && m_widgets[i]->focusable())
            return static_cast<int>(i);
    return -1;
}

void Menu::step(int dir) {
    // UIMenu::Increment / Decrement within the focused group, then
    // MenuManager::ToggleFocus: the other group's first widget.
    const Widget* cur = focused();
    if (!cur) {
        resetFocus();
        return;
    }
    const int group = cur->group;
    const int n = static_cast<int>(m_widgets.size());
    for (int i = m_focus + dir; i >= 0 && i < n; i += dir) {
        const Widget& w = *m_widgets[static_cast<std::size_t>(i)];
        if (w.group == group && w.focusable()) {
            setFocus(i);
            return;
        }
    }
    const int other = firstFocusable(group == 0 ? 1 : 0);
    if (other >= 0) {
        setFocus(other);
        return;
    }
    // A single group (dialogs) wraps.
    for (int k = 0; k < n; ++k) {
        const int i = dir > 0 ? k : n - 1 - k;
        const Widget& w = *m_widgets[static_cast<std::size_t>(i)];
        if (w.group == group && w.focusable()) {
            setFocus(i);
            return;
        }
    }
}

// One frame of the menu's input (MenuManager::CheckInput, UIMenu::CheckInput):
// a capturing widget first, then the mouse (MenuManager::MouseAction,
// UIMenu::MouseHitCheck, MenuManager::NotifyMouseSelect), then the keys
// (UIMenu::ScanInput, UIMenu::KeyboardAction).
void Menu::update(UiFrame& f) {
    const NavInput& nav = f.nav;
    if ((!focused() || !focused()->focusable()) && !m_parked)
        resetFocus();
    Widget* cur = focused();
    Widget* hovered = nullptr;
    for (auto& w : m_widgets)
        if (w->focusable() && w->box.contains(nav.mouse))
            hovered = w.get();
    if (cur && cur->modal()) {
        // A click on another widget takes the focus from a text entry.
        const bool clickAway = nav.mousePressed && hovered && hovered != cur && dynamic_cast<TextEntry*>(cur);
        if (!clickAway) {
            cur->modalInput(f);
            // A text entry hands Tab and Escape on to the menu.
            if (cur->modal() || !(nav.tabNext || nav.back))
                return;
        }
    }

    // Mouse: the widget under the pointer takes the focus; over empty space
    // the highlight disappears (MenuManager::ClearAllWidgets).
    if (nav.mouseMoved || nav.mousePressed) {
        if (hovered) {
            if (hovered != cur)
                for (std::size_t i = 0; i < m_widgets.size(); ++i)
                    if (m_widgets[i].get() == hovered)
                        setFocus(static_cast<int>(i));
            cur = hovered;
            m_highlight = true;
        } else if (nav.mouseMoved) {
            m_highlight = false;
        }
    }
    // sfPointer::Update: a release counts only over the widget that took the
    // press; released anywhere else (or after a press on nothing) it does
    // nothing, so dragging onto a button does not click it.
    if (nav.mousePressed)
        m_pressed = hovered;
    if (cur) {
        if (nav.mouseReleased && hovered != m_pressed) {
            NavInput unreleased = nav;
            unreleased.mouseReleased = false;
            UiFrame g{f.overlay, f.textures, f.text, unreleased, f.time, f.sound};
            cur->mouse(g, cur == hovered);
        } else {
            cur->mouse(f, cur == hovered);
        }
    }
    if (nav.mouseReleased)
        m_pressed = nullptr;
    if (cur && cur->modal())
        return;

    // MenuManager::ScanGlobalKeys in the popups: Escape and Enter play
    // Selectionmade before the widget acts, Up, Down and Tab Moveselector,
    // whatever the focus then does (MenuManager::PlaySound, heard only while
    // a popup is up).
    if (popupSounds && (nav.back || nav.accept))
        f.play("Selectionmade", 0.75f);
    if (popupSounds && (nav.up || nav.down || nav.tabNext))
        f.play("Moveselector", 0.75f);
    if (nav.back) {
        // MenuManager::ScanGlobalKeys: Escape on the navigation strip moves
        // the focus back to the page and then backs the page up as well.
        if (cur && cur->group != 0 && firstFocusable(0) >= 0)
            setFocus(firstFocusable(0));
        if (onBack)
            onBack();
        return;
    }
    if (!cur)
        return;
    if (nav.up || nav.down || nav.left || nav.right || nav.tabNext || nav.accept || nav.space)
        m_highlight = true;
    if (nav.up)
        step(-1);
    if (nav.down || nav.tabNext)
        step(1);
    cur = focused();
    if (!cur)
        return;
    if (nav.left)
        cur->adjust(f, -1);
    if (nav.right)
        cur->adjust(f, 1);
    if (nav.pageUp)
        cur->page(f, -1);
    if (nav.pageDown)
        cur->page(f, 1);
    if (nav.accept)
        cur->activate(f);
    else if (nav.space)
        cur->activateSpace(f);
}

void Menu::draw(UiFrame& f) {
    if (!background.empty())
        drawImage(f.overlay, f.textures.get(background), 0, 0, 640, 480);
    drawContent(f);
}

// UIMenu::Update; an inactive menu lights nothing (UIMenu::ClearWidgets).
void Menu::drawContent(UiFrame& f, bool active) {
    const Widget* cur = active && m_highlight ? focused() : nullptr;
    for (auto& w : m_widgets)
        if (w->visible)
            w->draw(f, w.get() == cur);
    // The description picture follows the focus (MM2 FocusDescription
    // callbacks, UIBMLabel::SetBitmapName; drawn by UIBMLabel::Update):
    // nothing when the focused widget has none.
    if (active && !m_helpHidden) {
        std::string helpPic = cur ? cur->helpPicture() : std::string();
        if (helpPic.empty())
            helpPic = defaultHelp;
        if (!helpPic.empty())
            drawImage(f.overlay, f.textures.get(helpPic), helpPos.x, helpPos.y);
    }
    for (auto& w : m_widgets)
        if (w->visible)
            w->drawPopup(f);
}

} // namespace mm2::ui
