#pragma once

// Internal interfaces of the frontend (menus). See docs/frontend.md for the
// screen flow and where the layout comes from.

#include "app/Context.h"
#include "audio/SoundBank.h"
#include "audio/game/SoundSlot.h"
#include "city/CityData.h"
#include "game/Profile.h"
#include "game/RaceConfig.h"
#include "ui/MenuLayout.h"
#include "ui/Text.h"
#include "ui/TextureCache.h"
#include "ui/Widgets.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mm2::app::frontend {

class Frontend;
class Showroom;

// One screen or dialog of the frontend.
class Page {
public:
    virtual ~Page() = default;
    ui::Menu menu;
    // Dialogs draw on top of the page below them and offset their widgets by
    // `origin` (the dialog picture's position on the 640x480 screen).
    bool dialog = false;
    Vec2 origin{0, 0};
    std::string dialogPicture; // jpg/..._dlg.jpg
    // MM2 menu id (menu_id::...): entering a page with a new id plays that
    // menu's switch sound (MM2 `MenuManager::PlayMenuSwitchSound`).
    int menuId = -1;

    virtual void update(Frontend&, double /*dt*/) {}
    virtual void drawBelow(Frontend&, ui::UiFrame&) {} // after background, before widgets
    virtual void drawAbove(Frontend&, ui::UiFrame&) {} // after widgets
    virtual void onEnter(Frontend&) {}                 // when it becomes the top page again
    // The menu's PreSetup turns the description label off: no help picture
    // on entering it until the focus moves (MainMenu, OptionsMenu,
    // AudioOptions, GraphicsOptions, ControlSetup).
    bool helpOffOnEntry = false;
    // A page with 3D content (the garage's car): its background is drawn in
    // the scene pass under the 3D, as MM2's camera underlay is, and
    // drawScene() adds the 3D after it.
    virtual bool drawsScene() const { return false; }
    virtual void drawScene(Frontend&) {}
};

// State shared by all pages and the page stack.
class Frontend {
public:
    Frontend(Context& ctx);
    ~Frontend();

    Context& ctx;
    ui::TextureCache textures;
    ui::TextRenderer text;
    ui::NavReader navReader;
    ui::MenuLayout layout; // tune/widget.csv + tune/menu.csv
    game::ProfileStore store;
    game::Progress progress;
    game::HallOfFame hallOfFame; // <players dir>/records.ini
    std::optional<game::Profile> profile;
    std::vector<city::CityInfo> cities;
    std::vector<std::vector<city::RaceDefinition>> races; // per city, parallel to `cities`
    game::RaceConfig config;                                // being assembled by the menus
    // Dialog_TCPIP's address field: the driver's last one on selection
    // (mmInterface::PlayerSetState), saved with the event (BeDone).
    std::string tcpAddress;
    double time = 0.0;

    // Page stack.
    void push(std::unique_ptr<Page> page);
    void pop();
    void replace(std::unique_ptr<Page> page); // pops the top page, pushes `page`
    void popTo(std::size_t depth);            // keeps the bottom `depth` pages
    std::size_t depth() const { return m_pages.size(); }
    Page* top() const { return m_pages.empty() ? nullptr : m_pages.back().get(); }

    // OpenMM2's own messages (MM2 shows only pictures): msg_dlg with the
    // text and an OK button.
    void message(std::string text, std::function<void()> then = {});
    // OpenMM2's own questions: msg_dlg with the text, OK and Cancel.
    void question(std::string text, std::function<void()> yes);

    // One of MM2's picture dialogs (Dialog_Message): the picture centred on
    // the screen (PUMenuBase), its buttons at their tune/widget.csv
    // positions (dialog `menuId`, button index), focus on the first; Escape
    // runs the last button.
    struct DialogButton {
        std::string sprite;         // "texture/dlg_ok.tga"
        Vec2 position;              // relative to the dialog, when widget.csv has no row
        std::function<void()> then; // after the dialog closes
    };
    void dialog(std::string picture, int menuId, std::vector<DialogButton> buttons);
    // The quit question (quit_dlg, 27): OK quits.
    void askQuit();
    // A picture dialog with only OK at +296,+38 (400x76: lock_dlg, lstp_dlg,
    // plim_dlg) or +180,+176 (300x225: dupp_dlg).
    void notice(std::string picture, int menuId, std::function<void()> then = {});

    // Frontend sounds by MM2 name (aud/aud22/<name>.22k.wav or the 11 kHz
    // file, per the sound quality option), as 2D effects.
    void playSound(std::string_view name, float volume);
    bool soundPlaying(std::string_view name) const;
    // While set, page changes play no menu switch sound (building the page
    // stack after a race).
    bool quietSwitches = false;

    // Profile helpers.
    void selectProfile(const std::string& name);
    void saveProfile();

    // City/race lookups.
    int cityIndex(std::string_view name) const;
    const city::CityInfo* currentCity() const;
    // Races of the current mode in the current city (blitz/circuit/checkpoint/crash).
    std::vector<const city::RaceDefinition*> racesFor(game::GameMode mode, std::string_view city) const;

    // Fills `config` from the profile's last choices.
    void configFromProfile();
    // Applies the selected race's default environment (MM2
    // `RaceMenuBase::SetStateRace`) for `cfg.difficulty`.
    void applyRaceDefaults(game::RaceConfig& cfg) const;
    // Name of the race `cfg` describes ("Cruise" etc. for the modes without races).
    std::string raceName(const game::RaceConfig& cfg) const;
    // Records a finished race in the driver's profile and the race records;
    // returns the reward it announces.
    std::optional<game::Reward> recordResult(const game::RaceResult& result);
    // Starts the session described by `config`.
    void startRace();
    // In a multiplayer session: the garage's car and paint job become the
    // local player's (mmInterface::ChangePlayerData) and the driver's.
    void applyLobbyCar();
    // Entering multiplayer: a locked car becomes vpbug with paint job 0, a
    // locked paint job paint job 0 (mmInterface::GetUnlockedCar /
    // GetUnlockedColor).
    void unlockedNetCar();
    // A network race starting (mmInterface::MultiStartGame calls BeDone):
    // the lobby's car and the session's event become the driver's last.
    void saveNetEvent();

    void update(double dt);
    void draw();
    // The topmost full-screen page when it draws 3D (dialogs over it keep
    // it), and its scene pass.
    Page* scenePage() const;
    void drawScene();
    // The garage's 3D car, kept for the process like MM2's menus (the
    // camera distance and each car's angle carry over).
    Showroom& showroom();

private:
    void drawPage(Page& p, ui::UiFrame& f, bool active);
    void topChanged();
    std::vector<std::unique_ptr<Page>> m_pages;
    std::vector<std::unique_ptr<Page>> m_graveyard; // popped this frame; freed after drawing
    int m_menuId = -1; // menu id of the topmost full page
    std::unique_ptr<Showroom> m_showroom;
    std::unique_ptr<audio::SoundBank> m_soundBank;
    std::map<std::string, audio::game::SoundSlot, std::less<>> m_sounds;
    ui::SoundFn m_soundFn;
};

// Page factories.
std::unique_ptr<Page> makeTitlePage(Frontend& fe);
std::unique_ptr<Page> makeDriverPage(Frontend& fe);
std::unique_ptr<Page> makeNewDriverDialog(Frontend& fe);
std::unique_ptr<Page> makeDriverStatsDialog(Frontend& fe);
std::unique_ptr<Page> makeRaceRecordsDialog(Frontend& fe);
std::unique_ptr<Page> makeRacesPage(Frontend& fe);
std::unique_ptr<Page> makeVehiclePage(Frontend& fe);
std::unique_ptr<Page> makeShowcasePage(Frontend& fe, std::string vehicle);
std::unique_ptr<Page> makeOptionsPage(Frontend& fe);
std::unique_ptr<Page> makeGraphicsPage(Frontend& fe);
std::unique_ptr<Page> makeAudioPage(Frontend& fe);
std::unique_ptr<Page> makeControlPage(Frontend& fe);
std::unique_ptr<Page> makeCustomizeControlsPage(Frontend& fe);
std::unique_ptr<Page> makeAboutPage(Frontend& fe);
std::unique_ptr<Page> makeCrashIntroPage(Frontend& fe);
std::unique_ptr<Page> makeCrashCoursePage(Frontend& fe, std::string city);
std::unique_ptr<Page> makeResultsPage(Frontend& fe, const game::RaceResult& result, std::optional<game::Reward> reward);
std::unique_ptr<Page> makeSessionsPage(Frontend& fe);
// Multiplayer (PagesMulti.cpp).
std::unique_ptr<Page> makeHostOptionsDialog(Frontend& fe);
std::unique_ptr<Page> makeAddressDialog(Frontend& fe);
std::unique_ptr<Page> makeLobbyPage(Frontend& fe);
std::unique_ptr<Page> makeHostSettingsPage(Frontend& fe);
std::unique_ptr<Page> makeEjectDialog(Frontend& fe);
// MM2's own menu pointer, drawn last when not in a window (MenuPointer.cpp).
void drawMenuPointer(Frontend& fe, ui::UiFrame& f);
// Automation helpers: host with the current config / join an address.
bool frontendHostSession(Frontend& fe, const std::string& password = {});
// A network session's traffic and cop densities and the shared traffic
// option (OpenMM2 extra) for its mode: a cruise takes the single-player
// cruise's defaults with the option on, the other modes none.
void applyNetTrafficDefaults(game::RaceConfig& cfg);
void frontendJoinSession(Frontend& fe, const std::string& address, const std::string& password);

// MM2's menu ids: the first column of tune/menu.csv and the second of
// tune/widget.csv (the UIMenu constructor arguments in MM2
// `mmInterface::mmInterface`). Widget positions are looked up with these and
// the widget's creation index in the original menu (ui::MenuLayout).
namespace menu_id {
inline constexpr int kNavBar = 0;
inline constexpr int kMain = 1;
inline constexpr int kOptions = 2;
inline constexpr int kAudio = 3;
inline constexpr int kGraphics = 4;
inline constexpr int kControl = 5;
inline constexpr int kRace = 7;
inline constexpr int kVehicle = 8;
inline constexpr int kShowcase = 9;
inline constexpr int kNetSelect = 10;
inline constexpr int kHostRace = 11;
inline constexpr int kNetArena = 12;
inline constexpr int kNewDriver = 17;
inline constexpr int kDriverRecord = 19;
inline constexpr int kHallOfFame = 20;
inline constexpr int kRaceEnvironment = 22;
inline constexpr int kLocked = 23;
inline constexpr int kQuit = 27;
inline constexpr int kDeleteDriver = 28;
inline constexpr int kDuplicateDriver = 29;
inline constexpr int kDefaults = 31;
inline constexpr int kAbout = 34;
inline constexpr int kCrashCourse = 39;
inline constexpr int kCrashIntro = 40;
inline constexpr int kControlCustom = 41;
} // namespace menu_id

// Common layout positions (tune/widget.csv; used when the table lacks a row).
namespace layout {
// Top-right navigation strip: OPTIONS, help, minimise, exit (widget.csv
// "Navigation Bar", menu 0, widgets 0-3; PREV is widget 4).
inline constexpr Vec2 kNavOptions{439, 1};
inline constexpr Vec2 kNavHelp{540, 1};
inline constexpr Vec2 kNavMinimize{564, 1};
inline constexpr Vec2 kNavExit{591, 1};
// Bottom navigation: back (left arrow) and continue (right arrow).
inline constexpr Vec2 kBack{290, 415};
inline constexpr Vec2 kNext{439, 415};
// Right-hand column of big arrow buttons (main screen and options).
inline constexpr float kColumnX = 439;
inline constexpr float kRow59 = 242, kRow58 = 301, kRow56 = 359, kRow65 = 415;
// Left panel rows (lamp items): x 40, 27 px apart.
inline constexpr float kLampX = 40;
// Value boxes in the right half.
inline constexpr float kBoxX = 400;
inline constexpr float kBoxWide = 210, kBoxMid = 129, kBoxSmall = 48, kBoxH = 28;
} // namespace layout

// What the strip's OPTIONS button does (MM2 `mmInterface::Switch`/`Update`).
enum class NavOptions {
    Open,   // opens the options menu
    Lit,    // the options menu itself: shown lit, does nothing
    Cancel, // an option sub-page: the page's CANCEL, back to the options menu
};
// Adds the top navigation strip (OPTIONS / HELP / MINIMISE / EXIT, MM2
// `uiNavBar`), after which any PREV button of the page follows in the strip's
// focus group. `cancel` is the sub-page's cancel action for NavOptions::Cancel.
void addNavStrip(Frontend& fe, Page& page, NavOptions options = NavOptions::Open, std::function<void()> cancel = {});
// Adds a BACK button that pops the page. With the mnav_prv sprite it is the
// navigation bar's PREV (290,415) and belongs to the strip's focus group.
ui::SpriteButton& addBack(Frontend& fe, Page& page, const char* sprite = "texture/mnav_prv.tga");

// Whether the next race was started from the Crash Course page (lessons and
// "work experience" alike). MM2 keeps this in a global that the Crash Course
// GO sets and the race menu's GO, QUICK RACE and MULTIPLAYER clear
// (mmInterface::Update); after the race mmInterface::ShowMain returns to the
// Crash Course page when it is set, else to the race menu. Kept for the
// process, like the original's global.
bool crashCourseReturn();
void setCrashCourseReturn(bool on);

// Display names.
const char* timeOfDayName(game::TimeOfDay t);
const char* weatherName(game::Weather w);
std::string modeDisplayName(Frontend& fe, game::GameMode m);
std::string formatTime(float seconds);

} // namespace mm2::app::frontend
