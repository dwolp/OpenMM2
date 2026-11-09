// The game's menus: page stack, shared state and the Screen wrapper.
#include "app/Screens.h"
#include "app/frontend/Frontend.h"
#include "app/frontend/PopupOptions.h"
#include "app/frontend/Showroom.h"
#include "audio/Music.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "game/session/RaceSetup.h"
#include "render/Projection.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace mm2::app {
namespace frontend {

// --- Frontend --------------------------------------------------------------------------

Frontend::Frontend(Context& c)
    : ctx(c), textures(c.device(), c.game->vfs), text(c.device()), layout(ui::MenuLayout::load(c.game->vfs)),
      store(game::ProfileStore::defaultDir()), progress(game::Progress::load(c.game->vfs)) {
    cities = city::listCities(c.game->vfs);
    // mmCityList::LoadAll reads sf.cinfo first, then the other tune/*.cinfo.
    std::ranges::stable_partition(cities, [](const city::CityInfo& i) { return str::iequals(i.mapName, "sf"); });
    for (const auto& info : cities)
        races.push_back(city::listRaces(c.game->vfs, info));
    hallOfFame.load(store.dir() / "records.ini");
    // mmStatePack::SetDefaults: the current city starts as San Francisco.
    config.city = "sf";
}

// Enters a page (MenuManager::Switch, MenuManager::Disable / Enable) or opens
// a dialog over it (MenuManager::OpenDialog). The stack stands in for
// MenuManager::SetPreviousMenu / GetPreviousMenu.
void Frontend::push(std::unique_ptr<Page> page) {
    if (page->helpOffOnEntry)
        page->menu.hideHelpUntilFocusMoves();
    m_pages.push_back(std::move(page));
    topChanged();
}

// Back to the previous menu (MenuManager::GetPreviousMenu) or out of a dialog
// (MenuManager::CloseDialog).
void Frontend::pop() {
    if (m_pages.empty())
        return;
    const bool dialog = m_pages.back()->dialog;
    m_graveyard.push_back(std::move(m_pages.back()));
    m_pages.pop_back();
    if (!m_pages.empty()) {
        // MM2 resets the focus every time a menu is entered (UIMenu::Enable);
        // closing a dialog only hands the focus back (MenuManager::
        // CloseDialog): the page keeps its keyboard position with nothing
        // lit (OpenDialog unlit it).
        Page& page = *m_pages.back();
        if (dialog) {
            page.menu.unlight();
        } else {
            page.menu.resetFocus();
            if (page.helpOffOnEntry)
                page.menu.hideHelpUntilFocusMoves();
        }
        page.onEnter(*this);
    }
    topChanged();
}

// A switch to a menu that is not stacked on this one (MenuManager::Switch).
void Frontend::replace(std::unique_ptr<Page> page) {
    if (!m_pages.empty()) {
        m_graveyard.push_back(std::move(m_pages.back()));
        m_pages.pop_back();
    }
    push(std::move(page));
}

void Frontend::popTo(std::size_t d) {
    while (m_pages.size() > d) {
        m_graveyard.push_back(std::move(m_pages.back()));
        m_pages.pop_back();
    }
    if (!m_pages.empty()) {
        m_pages.back()->menu.resetFocus();
        m_pages.back()->onEnter(*this);
    }
    topChanged();
}

void Frontend::topChanged() {
    // MenuManager::Switch -> PlayMenuSwitchSound: entering a menu with a new
    // id plays its sound unless that sound is already playing; dialogs do
    // not switch menus.
    int id = -1;
    for (auto it = m_pages.rbegin(); it != m_pages.rend(); ++it)
        if (!(*it)->dialog) {
            id = (*it)->menuId;
            break;
        }
    if (id == m_menuId)
        return;
    m_menuId = id;
    if (quietSwitches)
        return;
    struct SwitchSound {
        int menu;
        const char* sound;
        float volume;
    };
    // The sounds MenuManager::AllocateMenuSwitchAudio gives the menus.
    static constexpr SwitchSound kSounds[] = {
        {menu_id::kMain, "Selectionmade", 0.87f},  {menu_id::kOptions, "UIoptions", 0.9f},
        {menu_id::kAudio, "UIoptions", 0.9f},      {menu_id::kGraphics, "UIoptions", 0.9f},
        {menu_id::kControl, "UIoptions", 0.9f},    {menu_id::kRace, "UIraces", 0.9f},
        {menu_id::kCrashIntro, "UIraces", 0.9f},   {menu_id::kVehicle, "UIvehicles", 0.9f},
        {menu_id::kHostRace, "UIvehicles", 0.9f},  {menu_id::kNetSelect, "UImulti", 0.87f},
        {menu_id::kNetArena, "UImulti", 0.87f},
    };
    for (const auto& e : kSounds)
        if (e.menu == id && !soundPlaying(e.sound))
            playSound(e.sound, e.volume);
}

void Frontend::playSound(std::string_view name, float volume) {
    if (!ctx.mixer || name.empty())
        return;
    if (!m_soundBank)
        m_soundBank = std::make_unique<audio::SoundBank>(ctx.game->vfs);
    m_soundBank->setQuality(ctx.settings.audioHighQuality ? audio::SoundBank::Quality::High
                                                          : audio::SoundBank::Quality::Low);
    auto it = m_sounds.find(name);
    if (it == m_sounds.end()) {
        audio::game::SoundSlot slot;
        slot.load(*ctx.mixer, *m_soundBank, name, audio::Bus::Effects);
        it = m_sounds.emplace(std::string(name), std::move(slot)).first;
    }
    if (!it->second.valid())
        return;
    // MenuManager::PlaySound(2): the sliders' and rollers' "Switch" is
    // stopped, rewound and played again on every step; the other sounds
    // keep playing when asked again.
    if (name == "Switch")
        it->second.stop();
    it->second.playOnce(volume);
}

bool Frontend::soundPlaying(std::string_view name) const {
    const auto it = m_sounds.find(name);
    return it != m_sounds.end() && it->second.playing();
}

int Frontend::cityIndex(std::string_view name) const {
    for (std::size_t i = 0; i < cities.size(); ++i)
        if (str::iequals(cities[i].mapName, name))
            return static_cast<int>(i);
    return cities.empty() ? -1 : 0;
}

const city::CityInfo* Frontend::currentCity() const {
    const int i = cityIndex(config.city);
    return i >= 0 ? &cities[static_cast<std::size_t>(i)] : nullptr;
}

std::vector<const city::RaceDefinition*> Frontend::racesFor(game::GameMode mode, std::string_view cityName) const {
    std::vector<const city::RaceDefinition*> out;
    const int ci = cityIndex(cityName);
    if (ci < 0)
        return out;
    city::RaceMode rm;
    switch (mode) {
    case game::GameMode::Blitz: rm = city::RaceMode::Blitz; break;
    case game::GameMode::Circuit: rm = city::RaceMode::Circuit; break;
    case game::GameMode::Checkpoint: rm = city::RaceMode::Checkpoint; break;
    case game::GameMode::CrashCourse: rm = city::RaceMode::CrashCourse; break;
    default: return out;
    }
    for (const auto& r : races[static_cast<std::size_t>(ci)])
        if (r.mode == rm)
            out.push_back(&r);
    return out;
}

// mmInterface::PlayerLoadCB / PlayerLoad: the driver becomes the last one
// used and its state is applied (configFromProfile).
void Frontend::selectProfile(const std::string& name) {
    for (auto& p : store.list()) {
        if (p.name == name) {
            profile = std::move(p);
            store.setLastUsed(name);
            configFromProfile();
            return;
        }
    }
}

void Frontend::saveProfile() {
    if (profile && !profile->save())
        log::warn("frontend: cannot save driver '{}'", profile->name);
}

void Frontend::configFromProfile() {
    if (!profile)
        return;
    const game::Profile& p = *profile;
    // The car and paint job by name (VehicleSelectBase::AllSetCar).
    config.vehicle = p.selectedVehicle(); // PlayerSetState: vpbug before the first race
    config.vehicleColor = p.vehicleColor;
    config.automatic = p.automatic;
    tcpAddress = p.address;
    config.difficulty = p.difficulty;
    // The driver's city becomes the current one (Dialog_City2::SetCurrentCity,
    // through Dialog_City2::DoneCB); a city the game no longer has keeps the
    // current one.
    if (cityIndex(p.city) >= 0)
        config.city = cities[static_cast<std::size_t>(cityIndex(p.city))].mapName;
    config.mode = p.mode == game::GameMode::CopsAndRobbers ? game::GameMode::Cruise : p.mode;
    config.raceIndex = config.mode == game::GameMode::Cruise ? -1 : std::max(0, p.raceIndex);
    // mmInterface::PlayerSetState restores the event, city and car; the
    // environment, laps and opponents come from the race's data again.
    applyRaceDefaults(config);
}

void Frontend::applyRaceDefaults(game::RaceConfig& cfg) const {
    // Shared with the race, which tests a finish against them
    // (game::session::applyRaceTableDefaults).
    const city::RaceDefinition* def = nullptr;
    if (cfg.mode != game::GameMode::Cruise) {
        const auto list = racesFor(cfg.mode, cfg.city);
        if (cfg.raceIndex < 0 || cfg.raceIndex >= static_cast<int>(list.size()))
            return;
        def = list[static_cast<std::size_t>(cfg.raceIndex)];
    }
    game::session::applyRaceTableDefaults(cfg, def);
}

std::string Frontend::raceName(const game::RaceConfig& cfg) const {
    const auto& s = ctx.game->strings;
    switch (cfg.mode) {
    case game::GameMode::Cruise: return s.get(80, "Cruise");
    case game::GameMode::CopsAndRobbers: return s.get(79, "Cops & Robbers");
    case game::GameMode::CrashCourse: return s.get(78, "Crash Course");
    default: break;
    }
    const auto list = racesFor(cfg.mode, cfg.city);
    if (cfg.raceIndex >= 0 && cfg.raceIndex < static_cast<int>(list.size()))
        return list[static_cast<std::size_t>(cfg.raceIndex)]->name;
    return {};
}

std::optional<game::Reward> Frontend::recordResult(const game::RaceResult& result) {
    // The modes' RegisterFinish do nothing while bCheating is set.
    if (!profile || result.cheated)
        return std::nullopt;
    game::RaceConfig defaults = result.config;
    applyRaceDefaults(defaults);
    if (!game::Progress::recordable(result.config, defaults))
        return std::nullopt;
    auto reward = progress.record(*profile, result);
    // The race records (mmMiscData::NewRecord): races only; a circuit enters
    // every lap's time, the score with the first. Each entry keeps whether
    // the race was passed (mmRecord::SetPassed with the mode's ProgressCheck
    // result, which mmInterface::HOFFillRecords hands to the records list).
    const auto& cfg = result.config;
    const std::string mode = game::modeKey(cfg.mode);
    const bool race = cfg.mode == game::GameMode::Blitz || cfg.mode == game::GameMode::Checkpoint ||
                      cfg.mode == game::GameMode::Circuit;
    if (race && result.finished) {
        const int score = std::max(0, result.score);
        if (cfg.mode == game::GameMode::Circuit && !result.lapSeconds.empty()) {
            for (std::size_t i = 0; i < result.lapSeconds.size(); ++i)
                hallOfFame.submit(cfg.difficulty, cfg.city, mode, cfg.raceIndex,
                                  {profile->name, cfg.vehicle, result.lapSeconds[i], i == 0 ? score : 0,
                                   result.won});
        } else {
            hallOfFame.submit(cfg.difficulty, cfg.city, mode, cfg.raceIndex,
                              {profile->name, cfg.vehicle, result.timeSeconds, score, result.won});
        }
        if (!hallOfFame.save(store.dir() / "records.ini"))
            log::warn("frontend: cannot save the race records");
    }
    saveProfile();
    return reward;
}

void Frontend::unlockedNetCar() {
    // mmInterface::Switch (MULTIPLAYER, the lobby), LobbySwitch and ShowMain
    // after a network race: the car's lock (mmVehInfo +0xf4), then the paint
    // job's (+0x108 bit). OpenMM2 checks on entering the sessions page and
    // on leaving the lobby's garage (a race unlocks, never locks).
    if (!profile)
        return;
    if (!progress.vehicleUnlocked(*profile, config.vehicle)) {
        config.vehicle = "vpbug";
        config.vehicleColor = 0;
    } else if (!progress.variantUnlocked(*profile, config.vehicle, config.vehicleColor)) {
        config.vehicleColor = 0;
    }
}

void Frontend::saveNetEvent() {
    // BeDone for the host and every joiner: the lobby's car, paint job and
    // transmission and the session's mode, race and city (copied into the
    // state pack by mmInterface::GetSessionData), so the main menu shows the
    // network event as LAST RACE afterwards.
    if (!profile || !ctx.netGame)
        return;
    const game::RaceConfig cfg = ctx.netGame->raceConfig();
    const game::NetCar car = ctx.netGame->localCar();
    game::Profile& p = *profile;
    p.vehicle = car.vehicle;
    p.vehicleColor = car.color;
    p.automatic = car.automatic;
    p.city = cfg.city;
    p.mode = cfg.mode;
    p.raceIndex = cfg.raceIndex;
    p.address = tcpAddress;
    saveProfile();
}

void Frontend::applyLobbyCar() {
    if (!ctx.netGame || !ctx.netGame->inSession())
        return;
    unlockedNetCar();
    game::NetCar car = ctx.netGame->localCar();
    car.vehicle = config.vehicle;
    car.color = config.vehicleColor;
    car.automatic = config.automatic; // the garage's TRANSMISSION
    ctx.netGame->setLocalCar(car);
    if (profile) {
        profile->vehicle = config.vehicle;
        profile->vehicleColor = config.vehicleColor;
        profile->automatic = config.automatic;
        saveProfile();
    }
}

void Frontend::startRace() {
    // In a multiplayer lobby the host starts the race for everyone; the
    // garage (whose GO DRIVE is off there) only picks the car.
    if (ctx.netGame && ctx.netGame->inSession()) {
        applyLobbyCar();
        pop();
        return;
    }
    // mmInterface::BeDone: the driver's car, paint job, transmission, city,
    // event and TCP/IP address are saved.
    if (profile) {
        game::Profile& p = *profile;
        p.vehicle = config.vehicle;
        p.vehicleColor = config.vehicleColor;
        p.automatic = config.automatic;
        p.city = config.city;
        p.mode = config.mode;
        p.raceIndex = config.raceIndex;
        p.address = tcpAddress;
        config.difficulty = p.difficulty;
        saveProfile();
    }
    log::info("frontend: starting {} in {} with {}", game::modeKey(config.mode), config.city, config.vehicle);
    ctx.nextScreen = makeRaceScreen(ctx, config);
}

void Frontend::update(double dt) {
    time += dt;
    m_graveyard.clear();
    Page* page = top();
    if (!page)
        return;
    if (!m_soundFn)
        m_soundFn = [this](std::string_view name, float volume) { playSound(name, volume); };
    const render::UiLayout screen = render::computeUiLayout(ctx.device().outputExtent(), ctx.display.uiScale);
    stickPointer(ctx, dt); // OpenMM2: the gamepad's pointer
    const ui::NavInput nav = navReader.read(ctx.input, screen, dt);
    ui::UiFrame f{*ctx.overlay, textures, text, nav, time, &m_soundFn};
    // MenuManager::OpenDialog leaves the menu under a dialog enabled: the
    // garage keeps updating, so its car turns on under the LOCKED dialog.
    if (Page* scene = scenePage(); scene && scene != page)
        scene->update(*this, dt);
    page->update(*this, dt);
    if (top() == page)
        page->menu.update(f);
}

void Frontend::drawPage(Page& p, ui::UiFrame& f, bool active) {
    // The page's background (UIMenu::AssignBackground, shown by
    // MenuManager::CheckBG / SetBackgroundImage); a page with 3D has its
    // background drawn in the scene pass.
    if (!p.menu.background.empty() && !p.drawsScene())
        ui::drawImage(f.overlay, textures.get(p.menu.background), 0, 0, 640, 480);
    if (!p.dialogPicture.empty()) {
        const ui::UiTexture& t = textures.get(p.dialogPicture);
        ui::drawImage(f.overlay, t, p.origin.x, p.origin.y);
    }
    p.drawBelow(*this, f);
    // A page under a dialog shows no focus and no help picture
    // (MenuManager::OpenDialog clears them).
    p.menu.drawContent(f, active);
    p.drawAbove(*this, f);
}

void Frontend::draw() {
    auto& ov = *ctx.overlay;
    ov.begin(ctx.display.uiScale);
    const ui::NavInput none;
    ui::UiFrame f{ov, textures, text, none, time};
    // Draw from the topmost full-screen page up to the top dialog.
    std::size_t first = m_pages.empty() ? 0 : m_pages.size() - 1;
    while (first > 0 && m_pages[first]->dialog)
        --first;
    for (std::size_t i = first; i < m_pages.size(); ++i)
        drawPage(*m_pages[i], f, i + 1 == m_pages.size());
    drawMenuPointer(*this, f); // sfPointer
    ov.end();
}

Frontend::~Frontend() = default;

Page* Frontend::scenePage() const {
    std::size_t first = m_pages.empty() ? 0 : m_pages.size() - 1;
    while (first > 0 && m_pages[first]->dialog)
        --first;
    Page* p = m_pages.empty() ? nullptr : m_pages[first].get();
    return p && p->drawsScene() ? p : nullptr;
}

void Frontend::drawScene() {
    Page* p = scenePage();
    if (!p)
        return;
    // asCullManager::Update: clear to black, the camera's underlay (the
    // menu's background), then the 3D through the menu camera; the widgets
    // follow in the overlay pass.
    render::Device& dev = ctx.device();
    render::ClearValues clear;
    clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
    dev.beginScene(clear);
    if (!p->menu.background.empty()) {
        auto& ov = *ctx.overlay;
        ov.begin(ctx.display.uiScale);
        const render::Extent2D scene = dev.sceneExtent();
        dev.setViewport({0, 0, static_cast<float>(scene.width), static_cast<float>(scene.height), 0, 1});
        ui::drawImage(ov, textures.get(p->menu.background), 0, 0, 640, 480);
        ov.end();
    }
    p->drawScene(*this);
    dev.endScene();
}

Showroom& Frontend::showroom() {
    if (!m_showroom)
        m_showroom = std::make_unique<Showroom>(ctx.device(), ctx.game->vfs);
    return *m_showroom;
}

// --- Common widgets ------------------------------------------------------------------------

// The navigation strip (uiNavBar::uiNavBar: OPTIONS, HELP, MINIMISE, EXIT in
// tune/widget.csv's order); the buttons do what mmInterface::Update does for
// strip ids 100-103. Pages without it stand in for mmInterface::Switch's
// DisableNavBar.
void addNavStrip(Frontend& fe, Page& page, NavOptions options, std::function<void()> cancel) {
    using namespace layout;
    const auto before = page.menu.widgetsInGroup(1); // a PREV added earlier
    auto pos = [&fe](int index, Vec2 code) { return fe.layout.position(menu_id::kNavBar, index, code); };
    auto add = [&](ui::SpriteSheet sheet, int index, Vec2 code, std::function<void()> fn) -> ui::SpriteButton& {
        const Vec2 p = pos(index, code);
        auto& b = page.menu.add<ui::SpriteButton>(std::move(sheet), p.x, p.y, std::move(fn));
        b.group = 1;
        b.sound = "Selectionmade"; // UIBMButton sound slot 0
        return b;
    };
    // OPTIONS is a 5-frame toggle that is never disabled: lit on the options
    // menu (mmInterface::Switch, uiNavBar::OptionActive), CANCEL on an option
    // sub-page, otherwise it opens the options.
    std::function<void()> onOptions;
    switch (options) {
    case NavOptions::Open: onOptions = [&fe] { fe.push(makeOptionsPage(fe)); }; break;
    case NavOptions::Lit: onOptions = [] {}; break;
    case NavOptions::Cancel: onOptions = std::move(cancel); break;
    }
    auto& opt = add({"texture/mnav_opt.tga", 5}, 0, kNavOptions, std::move(onOptions));
    if (options == NavOptions::Lit)
        opt.lit = [] { return true; };
    // HELP: MM2 minimises and runs WinHelp on MM2HELP.HLP (uiNavBar::Help,
    // MenuManager::Help); OpenMM2 shows a short message instead.
    add({"texture/mnav_hlp.tga", 3}, 1, kNavHelp, [&fe] {
        fe.message("Use the arrow keys or the mouse to choose, Enter to select and Escape to go back.");
    });
    // MINIMISE (uiNavBar::Minimize), EXIT.
    add({"texture/mnav_sto.tga", 3}, 2, kNavMinimize, [&fe] { SDL_MinimizeWindow(fe.ctx.window().sdl()); });
    add({"texture/mnav_ext.tga", 3}, 3, kNavExit, [&fe] { fe.askQuit(); });
    // PREV, if the page has one, comes last in the strip's focus order.
    for (const auto* w : before)
        page.menu.moveToEnd(w);
}

// PREV (strip id 104, nav-bar widget 4) or a page's own back button. PREV is
// shown on the pages that have a previous menu (uiNavBar::Update, TurnOnPrev;
// TurnOffPrev elsewhere); the hot spots the option and network menus move it
// to (UIMenu::AddHotSpot, uiNavBar::SetPrevPos) are its own (290, 415).
ui::SpriteButton& addBack(Frontend& fe, Page& page, const char* sprite) {
    const bool prev = std::string_view(sprite) == "texture/mnav_prv.tga";
    const Vec2 p = prev ? fe.layout.position(menu_id::kNavBar, 4, layout::kBack) : layout::kBack;
    auto& b = page.menu.add<ui::SpriteButton>(ui::SpriteSheet{sprite, 4}, p.x, p.y, [&fe] { fe.pop(); });
    if (prev)
        b.group = 1;
    page.menu.onBack = [&fe] { fe.pop(); };
    return b;
}

// --- Dialogs ----------------------------------------------------------------------------------

namespace {

Vec2 pictureSize(Frontend& fe, const std::string& picture) {
    const ui::UiTexture& t = fe.textures.get(picture);
    return t ? Vec2{static_cast<float>(t.width), static_cast<float>(t.height)} : Vec2{400, 76};
}

// MM2 Dialog_Message: a picture with one or two buttons (Dialog_Message::Init
// loads the buttons' bitmaps and kills the second one in a one-button dialog).
class PictureDialog final : public Page {
public:
    PictureDialog(Frontend& fe, std::string picture, int id, std::vector<Frontend::DialogButton> buttons) {
        dialog = true;
        menuId = id;
        origin = ui::dialogOrigin(pictureSize(fe, picture));
        dialogPicture = std::move(picture);
        ui::SpriteButton* first = nullptr;
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            auto& b = buttons[i];
            const Vec2 p = fe.layout.position(id, static_cast<int>(i), b.position, origin);
            auto& w = menu.add<ui::SpriteButton>(ui::SpriteSheet{b.sprite, 4}, p.x, p.y, [&fe, then = b.then] {
                fe.pop();
                if (then)
                    then();
            });
            w.sound = "Selectionmade";
            if (!first)
                first = &w;
        }
        if (first)
            menu.setInitialFocus(first);
        auto cancel = buttons.empty() ? std::function<void()>{} : buttons.back().then;
        menu.onBack = [&fe, cancel] {
            fe.pop();
            if (cancel)
                cancel();
        };
    }
};

// OpenMM2's text messages: msg_dlg (unused by MM2 build 3393) with OK at the
// position of MM2's 400x76 message boxes.
class MessageDialog final : public Page {
public:
    MessageDialog(Frontend& fe, std::string text, std::function<void()> then, bool question)
        : m_text(std::move(text)) {
        dialog = true;
        dialogPicture = "jpg/msg_dlg.jpg";
        origin = ui::dialogOrigin({400, 76});
        auto ok = [&fe, then = std::move(then)] {
            fe.pop();
            if (then)
                then();
        };
        auto& okButton =
            menu.add<ui::SpriteButton>(ui::SpriteSheet{"texture/dlg_ok.tga", 4}, origin.x + 296, origin.y + 38, ok);
        okButton.sound = "Selectionmade";
        menu.setInitialFocus(&okButton);
        if (question) {
            menu.add<ui::SpriteButton>(ui::SpriteSheet{"texture/dlg_can.tga", 4}, origin.x + 196, origin.y + 38,
                                       [&fe] { fe.pop(); })
                .sound = "Selectionmade";
            menu.onBack = [&fe] { fe.pop(); };
        } else {
            menu.onBack = ok;
        }
    }
    void drawAbove(Frontend&, ui::UiFrame& f) override {
        f.text.drawWrapped(f.overlay, ui::style::smallFont(), m_text, origin.x + 20, origin.y + 10, 260,
                           ui::style::kValueText);
    }

private:
    std::string m_text;
};

} // namespace

void Frontend::message(std::string t, std::function<void()> then) {
    push(std::make_unique<MessageDialog>(*this, std::move(t), std::move(then), false));
}

void Frontend::question(std::string t, std::function<void()> yes) {
    push(std::make_unique<MessageDialog>(*this, std::move(t), std::move(yes), true));
}

void Frontend::dialog(std::string picture, int id, std::vector<DialogButton> buttons) {
    push(std::make_unique<PictureDialog>(*this, std::move(picture), id, std::move(buttons)));
}

void Frontend::askQuit() {
    // quit_dlg: OK (first, focused) at +296,+38 quits; Cancel at +196,+38.
    dialog("jpg/quit_dlg.jpg", menu_id::kQuit,
           {{"texture/dlg_ok.tga", {296, 38}, [this] { ctx.quit = true; }}, {"texture/dlg_can.tga", {196, 38}, {}}});
}

void Frontend::notice(std::string picture, int id, std::function<void()> then) {
    const Vec2 size = pictureSize(*this, picture);
    const Vec2 ok = size.y > 100 ? Vec2{180, 176} : Vec2{296, 38};
    dialog(std::move(picture), id, {{"texture/dlg_ok.tga", ok, std::move(then)}});
}

// --- Crash Course return ------------------------------------------------------------------------

namespace {
bool g_crashCourseReturn = false;
} // namespace

bool crashCourseReturn() { return g_crashCourseReturn; }
void setCrashCourseReturn(bool on) { g_crashCourseReturn = on; }

// --- Names ------------------------------------------------------------------------------------

const char* timeOfDayName(game::TimeOfDay t) {
    switch (t) {
    case game::TimeOfDay::Morning: return "Morning";
    case game::TimeOfDay::Noon: return "Noon";
    case game::TimeOfDay::Evening: return "Evening";
    case game::TimeOfDay::Night: return "Night";
    }
    return "";
}

const char* weatherName(game::Weather w) {
    switch (w) {
    case game::Weather::Clear: return "Clear";
    case game::Weather::Cloudy: return "Cloudy";
    case game::Weather::Fog: return "Foggy";
    case game::Weather::Rain: return "Raining";
    case game::Weather::Snow: return "Snowing";
    }
    return "";
}

std::string modeDisplayName(Frontend& fe, game::GameMode m) {
    const auto& s = fe.ctx.game->strings;
    switch (m) {
    case game::GameMode::Cruise: return s.get(588, "Cruise");
    case game::GameMode::Blitz: return s.get(585, "Blitz");
    case game::GameMode::Circuit: return s.get(586, "Circuit");
    case game::GameMode::Checkpoint: return s.get(587, "Checkpoint");
    case game::GameMode::CrashCourse: return s.get(78, "Crash Course");
    case game::GameMode::CopsAndRobbers: return s.get(589, "Cops & Robbers");
    }
    return {};
}

std::string formatTime(float seconds) {
    // GetLocTime: "M:SS:HH" (hundredths rounded by adding 0.005, then
    // truncated); "  ---  " when there is no time.
    if (!(seconds > 0.0f))
        return "  ---  ";
    double whole = 0.0;
    const double fraction = std::modf(static_cast<double>(seconds) + 0.005, &whole);
    const int hundredths = static_cast<int>(fraction * 100.0);
    const int minutes = static_cast<int>(whole) / 60;
    const int secs = static_cast<int>(whole - static_cast<double>(minutes * 60));
    return std::format("{}:{:02}:{:02}", minutes, secs, hundredths);
}

// --- Automation ------------------------------------------------------------------------------

// OPENMM2_FRONTEND_SCRIPT drives the menus for automated screenshots, e.g.
//   "profile:Test;page:races;wait:5;nav:down;nav:right"
// Commands: profile:<name> (create/select), page:<name>, nav:<up|down|left|
// right|accept|back|tab>, wait:<frames>, wait:race (until a race has been
// driven), mode:<cruise|blitz|circuit|race|crash>, city:<map>,
// vehicle:<name>, time:<morning|noon|evening|night>, result:<position>
// (opens the results screen),
// mp:<host|join:addr|chat:text|ready|start|team:n|mode:m|car:name[:paint]>
// (multiplayer).
class Script {
public:
    explicit Script(const char* text) {
        if (text)
            for (auto part : str::split(text, ';'))
                if (!str::trim(part).empty())
                    m_commands.emplace_back(str::trim(part));
    }
    bool active() const { return m_next < m_commands.size() || m_wait > 0 || m_waitRace; }
    // The menus came back after a race (ends a wait:race).
    void raceDriven() { m_waitRace = false; }

    // Runs commands for this frame; may inject navigation into `nav`.
    void step(Frontend& fe) {
        if (m_waitRace)
            return;
        if (m_wait > 0) {
            --m_wait;
            return;
        }
        while (m_next < m_commands.size()) {
            const std::string& c = m_commands[m_next++];
            const auto colon = c.find(':');
            const std::string cmd = c.substr(0, colon);
            const std::string arg = colon == std::string::npos ? "" : c.substr(colon + 1);
            if (cmd == "wait" && arg == "race") {
                // wait:race: until a race has started and the menus are back.
                m_waitRace = true;
                return;
            }
            if (cmd == "wait") {
                m_wait = static_cast<int>(str::parseInt(arg).value_or(1));
                return;
            }
            run(fe, cmd, arg);
            if (cmd == "nav") // one key per frame
                return;
        }
    }

    platform::Key pendingKey = platform::Key::Unknown;

private:
    void run(Frontend& fe, const std::string& cmd, const std::string& arg);
    std::vector<std::string> m_commands;
    std::size_t m_next = 0;
    int m_wait = 0;
    bool m_waitRace = false;
};

void Script::run(Frontend& fe, const std::string& cmd, const std::string& arg) {
    if (cmd == "profile") {
        bool found = false;
        for (const auto& p : fe.store.list())
            found |= p.name == arg;
        if (!found)
            fe.store.create(arg);
        fe.selectProfile(arg);
    } else if (cmd == "mode") {
        using game::GameMode;
        fe.config.mode = arg == "blitz"     ? GameMode::Blitz
                         : arg == "circuit" ? GameMode::Circuit
                         : arg == "race"    ? GameMode::Checkpoint
                         : arg == "crash"   ? GameMode::CrashCourse
                                            : GameMode::Cruise;
        fe.config.raceIndex = fe.config.mode == GameMode::Cruise ? -1 : 0;
        fe.applyRaceDefaults(fe.config);
    } else if (cmd == "city") {
        fe.config.city = arg;
        fe.applyRaceDefaults(fe.config);
    } else if (cmd == "vehicle") {
        fe.config.vehicle = arg;
    } else if (cmd == "time") {
        // morning, noon, evening or night (after mode/city, which reset it).
        static constexpr std::string_view kTimes[] = {"morning", "noon", "evening", "night"};
        for (std::size_t k = 0; k < std::size(kTimes); ++k)
            if (arg == kTimes[k])
                fe.config.timeOfDay = static_cast<game::TimeOfDay>(k);
    } else if (cmd == "go") {
        fe.startRace();
    } else if (cmd == "mp") {
        // Multiplayer automation: mp:host[:<password>], mp:join:<address>[|<password>], mp:chat:<text>,
        // mp:ready, mp:start, mp:team:<0|1>, mp:mode:<cruise|blitz|circuit|race|cr|crteams|crffa>,
        // mp:car:<vehicle>[:<paint>], mp:traffic:<on|off>[:<traffic density>:<cop density>] (the shared
        // cruise traffic), mp:race:<index>[:<laps>].
        const auto sub = arg.substr(0, arg.find(':'));
        const auto rest = arg.find(':') == std::string::npos ? std::string() : arg.substr(arg.find(':') + 1);
        if (sub == "host") {
            if (frontendHostSession(fe, rest))
                fe.push(makeLobbyPage(fe));
        } else if (sub == "join") {
            // mp:join:<address>[|<password>]
            const auto bar = rest.find('|');
            frontendJoinSession(fe, rest.substr(0, bar), bar == std::string::npos ? "" : rest.substr(bar + 1));
        } else if (fe.ctx.netGame && sub == "chat") {
            fe.ctx.netGame->sendChat(rest);
        } else if (fe.ctx.netGame && sub == "ready") {
            fe.ctx.netGame->setReady(true);
        } else if (fe.ctx.netGame && sub == "start") {
            fe.ctx.netGame->startRace();
        } else if (fe.ctx.netGame && sub == "car") {
            // mp:car:<vehicle>[:<paint>]: what the lobby's SELECT VEHICLE (a
            // joiner's ready is cleared), a pick in the garage and its PREV
            // (Frontend::applyLobbyCar) do.
            const auto paint = rest.find(':');
            if (!fe.ctx.netGame->isHost())
                fe.ctx.netGame->setReady(false);
            fe.config.vehicle = rest.substr(0, paint);
            fe.config.vehicleColor =
                paint == std::string::npos ? 0 : static_cast<int>(str::parseInt(rest.substr(paint + 1)).value_or(0));
            fe.applyLobbyCar();
        } else if (fe.ctx.netGame && sub == "team") {
            game::NetCar car = fe.ctx.netGame->localCar();
            car.team = static_cast<int>(str::parseInt(rest).value_or(0));
            fe.ctx.netGame->setLocalCar(car);
        } else if (fe.ctx.netGame && sub == "mode") {
            game::RaceConfig c = fe.ctx.netGame->raceConfig();
            using game::GameMode;
            using CR = game::CopsAndRobbersMode;
            c.mode = rest == "blitz" ? GameMode::Blitz : rest == "circuit" ? GameMode::Circuit
                   : rest == "race" ? GameMode::Checkpoint
                   : rest == "cr" || rest == "crteams" || rest == "crffa" ? GameMode::CopsAndRobbers
                                                                         : GameMode::Cruise;
            c.copsAndRobbers = rest == "crteams" ? CR::RobberTeams : rest == "crffa" ? CR::FreeForAll : CR::CopsVsRobbers;
            c.raceIndex = c.mode == GameMode::Cruise || c.mode == GameMode::CopsAndRobbers ? -1 : 0;
            if (c.mode == GameMode::CopsAndRobbers)
                c.timeLimitMinutes = 10;
            const bool traffic = c.netTraffic;
            applyNetTrafficDefaults(c); // as the host settings' game type lamps
            c.netTraffic = traffic;
            fe.ctx.netGame->setRaceConfig(c);
        } else if (fe.ctx.netGame && sub == "race") {
            // mp:race:<index>[:<laps>]: the host's race (after mp:mode).
            game::RaceConfig c = fe.ctx.netGame->raceConfig();
            const auto parts = str::split(rest, ':');
            c.raceIndex = static_cast<int>(str::parseInt(parts.empty() ? "" : parts[0]).value_or(0));
            if (parts.size() > 1)
                c.laps = std::max(1, static_cast<int>(str::parseInt(parts[1]).value_or(3)));
            fe.ctx.netGame->setRaceConfig(c);
        } else if (fe.ctx.netGame && sub == "traffic") {
            game::RaceConfig c = fe.ctx.netGame->raceConfig();
            const auto parts = str::split(rest, ':');
            c.netTraffic = parts.empty() || parts[0] != "off";
            if (parts.size() >= 3) {
                auto density = [&](std::size_t i, double fallback) {
                    return std::clamp(static_cast<float>(str::parseDouble(parts[i]).value_or(fallback)), 0.0f, 1.0f);
                };
                c.trafficDensity = density(1, 0.5);
                c.copDensity = density(2, 1.0);
            }
            fe.ctx.netGame->setRaceConfig(c);
        }
    } else if (cmd == "result") {
        game::RaceResult r;
        r.config = fe.config;
        r.ended = r.finished = true;
        r.position = static_cast<int>(str::parseInt(arg).value_or(1));
        r.won = r.position <= 3;
        r.timeSeconds = 125.43f;
        for (int place = 1; place <= std::max(r.position, 4); ++place)
            r.standings.push_back({place == r.position ? -1 : place - 1, place, 120.0f + 3.1f * static_cast<float>(place)});
        fe.push(makeResultsPage(fe, r, {}));
    } else if (cmd == "nav") {
        using platform::Key;
        pendingKey = arg == "up"       ? Key::Up
                     : arg == "down"   ? Key::Down
                     : arg == "left"   ? Key::Left
                     : arg == "right"  ? Key::Right
                     : arg == "accept" ? Key::Return
                     : arg == "back"   ? Key::Escape
                     : arg == "tab"    ? Key::Tab
                                       : Key::Unknown;
    } else if (cmd == "page") {
        if (arg == "title")
            fe.push(makeTitlePage(fe));
        else if (arg == "driver")
            fe.push(makeDriverPage(fe));
        else if (arg == "newdriver")
            fe.push(makeNewDriverDialog(fe));
        else if (arg == "stats")
            fe.push(makeDriverStatsDialog(fe));
        else if (arg == "records")
            fe.push(makeRaceRecordsDialog(fe));
        else if (arg == "races")
            fe.push(makeRacesPage(fe));
        else if (arg == "vehicle")
            fe.push(makeVehiclePage(fe));
        else if (arg == "showcase")
            fe.push(makeShowcasePage(fe, fe.config.vehicle));
        else if (arg == "options")
            fe.push(makeOptionsPage(fe));
        else if (arg == "graphics")
            fe.push(makeGraphicsPage(fe));
        else if (arg == "tweaks")
            fe.push(makeTweaksOptionsPage(fe));
        else if (arg == "audio")
            fe.push(makeAudioPage(fe));
        else if (arg == "control")
            fe.push(makeControlPage(fe));
        else if (arg == "customize")
            fe.push(makeCustomizeControlsPage(fe));
        else if (arg == "about")
            fe.push(makeAboutPage(fe));
        else if (arg == "crashintro")
            fe.push(makeCrashIntroPage(fe));
        else if (arg == "crashlondon")
            fe.push(makeCrashCoursePage(fe, "london"));
        else if (arg == "crashsf")
            fe.push(makeCrashCoursePage(fe, "sf"));
        else if (arg == "sessions")
            fe.push(makeSessionsPage(fe));
        else if (arg == "hostoptions")
            fe.push(makeHostOptionsDialog(fe));
        else if (arg == "address")
            fe.push(makeAddressDialog(fe));
        else if (arg == "hostsettings" && fe.ctx.netGame && fe.ctx.netGame->inSession())
            fe.push(makeHostSettingsPage(fe));
        else if (arg == "eject" && fe.ctx.netGame && fe.ctx.netGame->inSession())
            fe.push(makeEjectDialog(fe));
        else if (arg == "quit")
            fe.askQuit();
        else if (arg == "message")
            fe.message("You cannot pick a locked vehicle");
        else if (arg == "delete")
            fe.dialog("jpg/delp_dlg.jpg", menu_id::kDeleteDriver,
                      {{"texture/dlg_yes.tga", {180, 176}, {}}, {"texture/dlg_no.tga", {18, 176}, {}}});
        else
            log::warn("script: unknown page '{}'", arg);
    } else {
        log::warn("script: unknown command '{}'", cmd);
    }
}

} // namespace frontend

namespace {

using frontend::Frontend;

// The automation script runs once per process: when a scripted race ends,
// the menus come back on the results screen (or the multiplayer lobby) and
// the script carries on with the commands after the one that started the
// race, instead of replaying it.
frontend::Script& processScript() {
    static frontend::Script script(std::getenv("OPENMM2_FRONTEND_SCRIPT"));
    return script;
}

class FrontendScreen final : public Screen {
public:
    FrontendScreen(Context& ctx, const game::RaceResult* result)
        : m_fe(ctx), m_script(processScript()) {
        ctx.input.startTextInput(ctx.window());
        // mmInterface::PlayUIMusic.
        if (auto* music = ctx.music()) {
            music->setAmbience("");
            music->playMenu();
        }
        // mmInterface::InitPlayerInfo: the first start creates "DriverX"
        // (strings 65, 66); afterwards the last-used driver is loaded, or the
        // newest one if that name is gone.
        if (m_fe.store.list().empty()) {
            const auto& s = ctx.game->strings;
            if (auto p = m_fe.store.create(s.get(65, "DriverX"))) {
                // No last car or event yet: cruise in "vpbug", the profile's
                // defaults (mmInterface::PlayerSetState).
                p->netName = s.get(66, "DriverX");
                p->save();
            }
        }
        if (result)
            m_script.raceDriven();
        const std::string last = m_fe.store.lastUsed();
        if (!last.empty())
            m_fe.selectProfile(last);
        if (!m_fe.profile) {
            auto all = m_fe.store.list();
            if (!all.empty())
                m_fe.selectProfile(all.back().name);
        }
        if (result && result->config.multiplayer && ctx.netGame && ctx.netGame->inSession()) {
            // Back from a multiplayer race: the lobby, over the sessions list,
            // and a race or Cops and Robbers game that ended shows its
            // results first (mmGameMulti::UpdateResults, mmMultiCR::
            // FillResults; MM2 shows them over the race). The menus keep the
            // driver's own state (configFromProfile, with the network event
            // and car saveNetEvent stored): the network race's setup
            // (multiplayer, no traffic, police or opponents, the host's
            // difficulty) must not become the next single-player race's.
            m_fe.push(frontend::makeDriverPage(m_fe));
            m_fe.push(frontend::makeSessionsPage(m_fe));
            m_fe.push(frontend::makeLobbyPage(m_fe));
            if (result->ended && result->config.mode != game::GameMode::Cruise && !result->standings.empty())
                m_fe.push(frontend::makeResultsPage(m_fe, *result, {}));
        } else if (result && result->config.multiplayer) {
            // The session ended during a multiplayer race: the sessions list.
            m_fe.push(frontend::makeDriverPage(m_fe));
            m_fe.push(frontend::makeSessionsPage(m_fe));
        } else if (result && m_fe.profile) {
            // Back from a race (mmInterface::ShowMain): the race menu, or the
            // Crash Course over its intro, with the main menu underneath; the
            // results first when the race reached its end (MM2 shows them in
            // the game; quitting goes straight back).
            m_fe.config = result->config;
            auto reward = m_fe.recordResult(*result);
            // Only the page the player lands on plays its switch sound.
            m_fe.quietSwitches = true;
            m_fe.push(frontend::makeDriverPage(m_fe));
            if (frontend::crashCourseReturn() || result->config.mode == game::GameMode::CrashCourse) {
                // A lesson or "work experience" race started from the Crash
                // Course page returns to it, on the event just driven.
                m_fe.push(frontend::makeCrashIntroPage(m_fe));
                m_fe.config = result->config; // the intro set up lesson 0
                m_fe.quietSwitches = result->ended;
                m_fe.push(frontend::makeCrashCoursePage(m_fe, result->config.city));
            } else {
                m_fe.quietSwitches = result->ended;
                m_fe.push(frontend::makeRacesPage(m_fe));
            }
            m_fe.quietSwitches = false;
            if (result->ended) {
                m_fe.push(frontend::makeResultsPage(m_fe, *result, std::move(reward)));
                if (auto* music = ctx.music())
                    music->setState(audio::MusicState::Results);
            }
        } else if (m_script.active()) {
            // Automation starts from the driver page.
            m_fe.push(frontend::makeDriverPage(m_fe));
        } else {
            m_fe.push(frontend::makeTitlePage(m_fe));
        }
    }

    ~FrontendScreen() override { m_fe.ctx.input.stopTextInput(m_fe.ctx.window()); }

    void update(Context& ctx, double dt) override {
        // Multiplayer: keep the session alive on every page and follow the
        // host into the race when the countdown starts.
        if (ctx.netGame) {
            ctx.netGame->update();
            if (ctx.netGame->takeRaceStart()) {
                // The transmission as the menus have it now: the garage and
                // the control options both change it.
                game::NetCar car = ctx.netGame->localCar();
                car.automatic = m_fe.config.automatic;
                ctx.netGame->setLocalCar(car);
                m_fe.saveNetEvent();
                ctx.nextScreen = makeRaceScreen(ctx, ctx.netGame->raceConfig());
                return;
            }
        }
        if (m_script.active()) {
            m_script.step(m_fe);
            if (m_script.pendingKey != platform::Key::Unknown) {
                injectKey(ctx, m_script.pendingKey);
                m_script.pendingKey = platform::Key::Unknown;
            }
        }
        m_fe.update(dt);
    }

    // The garage's 3D car (Frontend::scenePage).
    bool usesScene() const override { return m_fe.scenePage() != nullptr; }
    void drawScene(Context&) override { m_fe.drawScene(); }
    void drawOverlay(Context&) override { m_fe.draw(); }

private:
    // Synthesizes a key press through SDL so it flows through the normal input path.
    static void injectKey(Context& ctx, platform::Key key) {
        SDL_Event down{};
        down.type = SDL_EVENT_KEY_DOWN;
        down.key.scancode = static_cast<SDL_Scancode>(key);
        down.key.down = true;
        ctx.input.handleEvent(down);
        SDL_Event up = down;
        up.type = SDL_EVENT_KEY_UP;
        up.key.down = false;
        // Release on the next frame so keyDown() is seen once.
        SDL_PushEvent(&up);
    }

    Frontend m_fe;
    frontend::Script& m_script;
};

} // namespace

std::unique_ptr<Screen> makeFrontendScreen(Context& ctx) { return std::make_unique<FrontendScreen>(ctx, nullptr); }

std::unique_ptr<Screen> makeFrontendScreen(Context& ctx, const game::RaceResult& result) {
    return std::make_unique<FrontendScreen>(ctx, &result);
}

} // namespace mm2::app
