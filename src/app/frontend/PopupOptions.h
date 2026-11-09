#pragma once

// The in-race popup's OPTIONS pages (MM2 mmPopup menus 5-8: PUOptions,
// PUAudioOptions, PUControl and PUGraphics), the PUMenuBase layout every
// popup menu shares (card, title, text buttons), the popup's menu sounds and
// OpenMM2's popup automation (OPENMM2_POPUP_SCRIPT). The race (RaceScreen)
// owns the popup's state machine (mmPopup::Update) and applies what these
// pages change through PopupOptionsHost.
//
// Positions are fractions of the popup's card (UIMenu::ScaleWidget), turned
// into 640x480 pixels here. See docs/frontend.md, "In-race popup".

#include "app/Context.h"
#include "app/Controls.h"
#include "audio/SoundBank.h"
#include "audio/game/SoundSlot.h"
#include "platform/Input.h"
#include "ui/Widgets.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mm2::app::frontend {

// The OPTIONS pages, the host's quit menu and the key map, by their mmPopup
// menu ids.
enum class PopupPage : std::uint8_t {
    Quit = 2,     // PUQuit (the host of a network race)
    Options = 5,  // PUOptions
    Audio = 6,    // PUAudioOptions
    Control = 7,  // PUControl
    Graphics = 8, // PUGraphics
    Roster = 10,  // PURoster (F6 in a network race)
    KeyMap = 11,  // PUKey (F1)
};

namespace popup {

// mmPopup's card: mmGame::Init creates mmPopup(game, 0.2, 0.1, 0.6, 0.8), so
// MenuManager's popup card covers x 0.2-0.8 and y 0.1-0.9 of the screen.
inline constexpr ui::Box kCard{128.0f, 48.0f, 384.0f, 384.0f};
// PUControl and PUGraphics ask for a 0.9 x 0.8 card, which
// PUMenuBase::PUMenuBase centres: x 0.05-0.95, y 0.1-0.9.
inline constexpr ui::Box kWideCard{32.0f, 48.0f, 576.0f, 384.0f};
// PUKey asks for 0.9 x 0.9 (on screens 512 pixels wide or more): x 0.05-0.95,
// y 0.05-0.95.
inline constexpr ui::Box kKeyCard{32.0f, 24.0f, 576.0f, 432.0f};

// The card behind the popup menus (MenuManager::Init's Card2D, drawn by
// Card2D::Cull): dark blue (16, 31, 93) at alpha 0x80 (Card2D::Init's 127,
// which MenuManager::Init overwrites).
std::uint32_t cardColor();
// The card of a page (MenuManager::AdjustPopupCard: Card2D::SetDimensions
// with the menu's UIMenu::GetDimensions).
ui::Box cardFor(PopupPage page);
// A box in fractions of `card` (UIMenu::ScaleWidget with the card as
// MenuManager::GetScale), in 640x480 pixels.
ui::Box at(const ui::Box& card, float x, float y, float w, float h);

// PUMenuBase's fields: the button height 0.1, the widget height 0.075, the
// exit / previous button at (0.5, 0.9) 0.5 x 0.1, and CreateTitle's title
// line (0, 0, 1, 0.1) after which the widgets start at 0.11.
inline constexpr float kButtonHeight = 0.1f;
inline constexpr float kWidgetHeight = 0.075f;
inline constexpr float kTitleBottom = 0.11f;

// A PUMenuBase text button (UIMenu::AddButton with PUMenuBase's font, GetFont
// 24) of UIButton type `type` at card fractions (x, y) w wide and h high.
ui::TextButton& addButton(ui::Menu& menu, const ui::Box& card, float x, float y, float w, float h,
                          std::string label, int type, std::function<void()> onClick);
// The card (MenuManager::AdjustPopupCard, Card2D::Cull).
void drawCard(render::Overlay2D& overlay, const ui::Box& card);
// PUMenuBase::CreateTitle(1): the menu's name in GetFont 32, white, at the
// card's left, centred in the title line.
void drawTitle(ui::UiFrame& f, const ui::Box& card, std::string_view title);

// PUGraphics' LIGHTING QUALITY slider callback (the function PUGraphics'
// constructor gives that slider): a raised slider becomes the whole part of
// its value + 1, a lowered one its whole part, kept within 1..3 (the popup's
// slider starts at 1, the options page's at 0). Returns the new quality, or
// nothing when the slider equals the current quality.
std::optional<int> lightQualityFromSlider(float slider, int current);

// Which of PUControl's sliders can be used (enabled; the others are greyed).
struct ControlStates {
    bool sensitivity = false, deadZone = false, collision = false, roadForce = false;
};
// PUControl::SetRWStates: everything off, then for a joystick or a wheel the
// sensitivity and the dead zone, and the two force-feedback intensities
// while force feedback runs (mmInput::DoingFF); for a mouse the sensitivity.
void setRWStates(ControlStates& s, controls::Controller c, bool doingFF);
// ControlBase::InitSensitivity: the sensitivity and the dead zone on, then
// both off again for the keyboard and the game pad.
void initSensitivity(ControlStates& s, controls::Controller c);

// The action slots PUKey lists for a controller, in order. mmInput::Init
// turns off Steering and Camera Pan for the keyboard, Steer Left, Steer
// Right and Camera Pan for the other devices; PUKey::PreSetup then walks the
// first 33 slots, or all 34 when fewer than 32 are on (so Enter Chat Msg is
// listed for every device but the keyboard).
std::vector<int> keyMapSlots(controls::Controller c);

} // namespace popup

// MM2's menu pointer (sfPointer), drawn last over a popup when not in a
// window (MenuPointer.cpp).
void drawMenuPointer(Context& ctx, ui::UiFrame& f);
// The left stick moves the menu pointer (MenuPointer.cpp); call before
// reading the menus' input.
void stickPointer(Context& ctx, double dt);

// What the race does when the OPTIONS pages change something.
struct PopupOptionsHost {
    // mmGame's PUGraphics callbacks (FarClipCB and SetLevelGraphics) and
    // lvlLevel::SetObjectDetail: re-read [Graphics] and apply it.
    std::function<void()> graphicsChanged;
    // mmInput::Init and ControlBase::SetSensitivity: re-read [Controls].
    std::function<void()> controlsChanged;
    // MenuManager::Switch to another popup page (nothing: back to PUMain).
    std::function<void(std::optional<PopupPage>)> show;
    // mmPopup::DisablePU: close the popup (the key map's Resume Driving).
    std::function<void()> close;
    // PUQuit's Quit to Lobby (mmGameMulti::BeDone(1)) and End Session
    // (BeDone(0) / (2)).
    std::function<void()> quitToLobby;
    std::function<void()> endSession;
    // PURoster's rows (mmGameMulti::InitRoster: the local player first,
    // then the others) and the host's boot (mmGameMulti::BootPlayerCB).
    struct RosterEntry {
        std::uint8_t id = 0;
        std::string name;
        bool host = false;
    };
    std::function<std::vector<RosterEntry>()> roster;
    std::function<void(std::uint8_t)> boot;
    // PUKey: the control the race reads for an action slot
    // (mmIO::GetDescription).
    std::function<std::string(int)> keyText;
};

// The OPTIONS pages of the in-race popup.
class PopupOptions {
public:
    explicit PopupOptions(Context& ctx) : m_ctx(ctx) {}

    // Builds `page`. Like UIMenu::Enable, this runs the page's PreSetup: the
    // audio and control pages take the copy of the settings their CANCEL
    // puts back (mmPlayerConfig::GetAudio / GetControls), the graphics page
    // shows the current values.
    std::unique_ptr<ui::Menu> build(PopupPage page, const PopupOptionsHost& host);
    // The page's card and title (drawn before its widgets).
    void draw(PopupPage page, ui::UiFrame& f) const;

private:
    void buildOptions(ui::Menu& menu, const PopupOptionsHost& host);
    void buildAudio(ui::Menu& menu, const PopupOptionsHost& host);
    void buildControl(ui::Menu& menu, const PopupOptionsHost& host);
    void buildGraphics(ui::Menu& menu, const PopupOptionsHost& host);
    void buildKeyMap(ui::Menu& menu, const PopupOptionsHost& host);
    void buildQuit(ui::Menu& menu, const PopupOptionsHost& host);
    void buildRoster(ui::Menu& menu, const PopupOptionsHost& host);
    void drawKeyMap(ui::UiFrame& f) const;
    void addOkCancel(ui::Menu& menu, const ui::Box& card, const PopupOptionsHost& host,
                     std::function<void()> cancel);
    void applyControlStates();
    controls::Controller controller() const;
    bool doingForceFeedback() const;

    Context& m_ctx;
    Settings m_saved; // the PreSetup copy CANCEL restores
    popup::ControlStates m_controlStates;
    ui::Slider* m_sensitivity = nullptr;
    ui::Slider* m_deadZone = nullptr;
    ui::Slider* m_collision = nullptr;
    ui::Slider* m_roadForce = nullptr;
    std::function<std::string(int)> m_keyText; // PUKey's descriptions (PopupOptionsHost::keyText)
};

// MenuManager's popup sounds (MenuManager::InitCommonStuff loads
// Moveselector, Selectionmade and Switch; MenuManager::PlaySound plays
// them), for the popup's ui::UiFrame.
class PopupSounds {
public:
    explicit PopupSounds(Context& ctx);
    const ui::SoundFn* fn() const { return &m_fn; }

private:
    void play(std::string_view name, float volume);
    Context& m_ctx;
    std::unique_ptr<audio::SoundBank> m_bank;
    std::map<std::string, audio::game::SoundSlot, std::less<>> m_slots;
    ui::SoundFn m_fn;
};

// OPENMM2_POPUP_SCRIPT drives the in-race popup for automated screenshots,
// e.g. "wait:60;open:graphics;wait:2;nav:down": open:<main|options|audio|
// control|graphics|keymap|quit|roster|exit> opens the popup on that page, nav:<up|down|left|
// right|accept|back> presses a key, wait:<frames> waits.
class PopupScript {
public:
    static std::optional<PopupScript> fromEnvironment();
    explicit PopupScript(std::string_view text);

    struct Step {
        std::string open;                            // the page to open, if any
        platform::Key key = platform::Key::Unknown; // a key to press
    };
    // This frame's step (at most one command that does something).
    Step step();
    bool active() const { return m_next < m_commands.size() || m_wait > 0; }

private:
    std::vector<std::string> m_commands;
    std::size_t m_next = 0;
    int m_wait = 0;
};

// Presses and (on the next frame) releases `key` through SDL, so that it
// reaches the game like a real key (the menus' automation).
void injectKey(Context& ctx, platform::Key key);

} // namespace mm2::app::frontend
