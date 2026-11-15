#pragma once

// In-race HUD: instrument cluster, damage meter, race clock, place / check /
// lap readouts and lap times, messages, the checkpoint arrow, the checkpoint
// stands, opponent and checkpoint icons, the overhead map and the in-car
// dashboard.
//
// Behaviour and layout follow MM2's own classes (MM2Recomp, build 3393):
// mmHUD, mmExternalView with mmLinearGauge / mmSlidingGauge /
// mmGearIndicator / mmSpeedIndicator, mmWPHUD, mmCircuitHUD, mmCollideHUD,
// mmTextNode, mmArrow, mmIcons, mmHudMap, mmDashView with RadialGauge, and
// mmWaypointObject / mmCheckpointInstance for the stands. See the HUD section
// of docs/gamemodes.md.
//
// MM2 draws its 2D HUD in screen pixels (bitmaps 1:1, fonts at a pixel
// height). OpenMM2 lays it out in the 640x480 virtual space and scales it to
// the window, i.e. it shows the game as it looks at 640x480
// (HudOptions::pixelSize changes the reference resolution). Elements MM2
// anchors to the screen edges use the edges of the whole output, so widescreen
// windows keep them in the corners.
//
// The first version was ported from MM1's mmHUD, mmArrow and RadialGauge:
// Open1560 - An Open Source Re-Implementation of Midtown Madness 1 Beta
// Copyright (C) 2020 Brick. GPL-3.0-or-later; OpenMM2 port under the same licence.
//
// Drawing order each frame:
//   scene pass:   world ... hud.drawWorld(camera)   // stands, icons, arrow, dashboard
//                 hud.drawMap(...)                   // last: sets its own viewport
//   overlay pass: hud.drawOverlay(...)               // outside overlay.begin()/end()

#include "game/CamPlayer.h"
#include "game/Camera.h"
#include "game/ModelLibrary.h"
#include "game/Strings.h"
#include "game/TextureLibrary.h"
#include "game/session/Session.h"
#include "game/session/Types.h"
#include "render/Device.h"
#include "render/DisplaySettings.h"
#include "render/Overlay2D.h"
#include "ui/Font.h"
#include "ui/Text.h"
#include "ui/TextureCache.h"
#include "vfs/Vfs.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm2::game::session {

// tune/<city>.mmhudmap (mmHudMap::FileIO; defaults from the constructor).
struct HudMapParams {
    Vec2 size{0.21f, 0.25f}; // fraction of the screen
    Vec2 pos{0.78f, 0.75f};
    bool zoomIn = false;
    float approachRate = 1.2f;
    float zoomInDist = 577.0f, zoomOutDist = 1195.0f;     // camera height, metres
    float iconScaleMin = 34.0f, iconScaleMax = 52.5f;     // icon size factor
    float zoomInDistFS = 786.0f, zoomOutDistFS = 1580.0f; // full-screen map
    float iconScaleMinFS = 15.0f, iconScaleMaxFS = 18.0f;
    Vec3 oceanColor{0.204f, 0.667f, 0.808f};
    // OpenMM2 Enhanced: MM1's mmHudMap::SetMode 3 puts the full-screen map's
    // 3D view in the top right corner (MM2 in the small map's place).
    bool insetTopRight = false;
};
// Reads the file; Ocean Color is then replaced by the colour mmHudMap::Init
// hard-codes for the city (the file's value is never used).
HudMapParams loadHudMapParams(const vfs::Vfs& vfs, const std::string& city);

// tune/<car>_dash.asnode (mmDashView::FileIO; defaults from the
// constructor and RadialGauge::Init) and the needle pivots.
struct DashParams {
    Vec3 dashPos{0.0f, -0.707f, -1.216f};
    Vec3 roofPos{0.0f, 0.189f, 0.0f};
    Vec3 wheelPos;
    Vec3 dmgOffset, speedOffset, tachOffset;
    Vec3 dmgPivotOffset, speedPivotOffset, tachPivotOffset, wheelPivotOffset, gearPivotOffset;
    float wheelFact = 0.9f;
    float rpmRotMin = -0.907f, rpmRotMax = 7.0f;
    float speedRotMin = -0.907f, speedRotMax = 7.0f;
    float damageRotMin = -0.907f, damageRotMax = 7.0f;
    // Centres of the parts' boxes in geometry/<car>_dash_<part>.mtx
    // (mmDashView::LoadPivotInfo); zero when a file is missing.
    Vec3 dmgPivot, speedPivot, tachPivot, wheelPivot;
};
std::optional<DashParams> loadDashParams(const vfs::Vfs& vfs, const std::string& car);

// Other cars shown on the map and marked by the opponent icons (mmGame's
// OppIconInfo for the opponents and the network players; aiMap's police).
struct MapBlip {
    Mat34 transform;
    // Ambient traffic is accepted but not drawn (MM2's map shows only the
    // player, opponents and police). Police should be passed while they
    // chase (mmHudMap::DrawCops). Remote: a network player.
    enum class Kind : std::uint8_t { Opponent, Police, Ambient, Remote } kind = Kind::Opponent;
    // mmIcons (OppIconInfo): the place shown on the icon (1-8 the digits of
    // opp_icon, 9 its gold "$", the Cops and Robbers carrier; 10 none) and
    // the icon's colour (0xAARRGGBB; violet for single-player opponents).
    int place = 10;
    std::uint32_t iconColor = 0xFFB400FFu;
    // Network players: their slot in mmGameMulti's players (the map colours
    // them with IconType slot + 4) and their name (mmIcons' labels).
    int slot = 0;
    std::string name;
};

// mmHudMap map modes 0-3: "Map Toggle" cycles Off, Small, Split;
// "Full Screen Map" switches to FullScreen and back. They are view settings
// (PlayerCameras::setViewSetting changes them with the camera, the wide
// angle and the dashboard).
using MapMode = game::MapMode;

struct HudOptions {
    bool visible = true;              // mmHUD::Enable / Disable: the clock, map and icons stay
    bool cluster = true;              // "HUD Toggle" key: mmHUD::ToggleExternalView, the instrument cluster
    MapMode mapMode = MapMode::Off;   // new players start with the map off (mmStatePack)
    bool wideAngle = false;           // the wide view (camViewCS +0x18): letterboxed 3D view
    bool rotatingMap = true;          // "Rotating Map" (on in mmStatePack)
    bool zoomedIn = false;            // "Map Zoom" (tune/<city>.mmhudmap ZoomIn)
    bool opponentIcons = true;        // "Opponent Position" (mmIcons; on for new players)
    bool metric = false;              // km/h instead of mph (OpenMM2 option)
    bool dashboard = false;           // the dash model shows (mmDashView active, its camera current)
    // The HUD's dashboard flag (mmHUD::ActivateDash): hides the instrument
    // cluster and, for right-hand-drive cars, moves the small map left.
    bool dashActive = false;
    // Virtual units per original screen pixel: 1 shows the HUD as the game
    // draws it at 640x480, 0.5 as at 1280x960.
    float pixelSize = 1.0f;
    render::UiScaleMode uiScale = render::UiScaleMode::Fit;
    // OpenMM2 Enhanced: an MM1 city (Chicago) wears MM1's HUD (mmHUD), its
    // art from MM1's ui.ar (mm1/bmp16/).
    bool mm1 = false;
};

// Pure pieces of the HUD logic, exposed for tests.
namespace hud {

// mmHUD::Update: the race clock as "MM:SS:HH" (minutes, seconds, hundredths;
// negative times show zero).
std::string clockText(float seconds);
// GetLocTime: "M:SS:HH", or "  ---  " for no time.
std::string lapTimeText(float seconds);
// mmSpeedIndicator::Draw: hundreds, tens and units of the truncated speed;
// ' ' for leading zeros, which are not drawn.
std::array<char, 3> speedDigits(float speed);
// mmGearIndicator::Draw: the digitac_gear_<x>.tga suffix for a gear
// (-1 reverse, 0 neutral, 1.. forward). Neutral uses the "p" art (which shows N).
std::string gearArt(int gear, bool automatic);
// mmLinearGauge::Draw: lit length of a gauge bitmap (0..length).
int linearGaugeLength(float value, float maxValue, int length);
// mmSlidingGauge::Draw: source offset of the window into a wider bitmap.
int slidingGaugeOffset(float value, float maxValue, int bitmapLength, int window);
// RadialGauge::GetArrowAngle: needle angle, clamped to [rotMin, rotMax].
float gaugeAngle(float value, float floorValue, float maxValue, float rotMin, float rotMax);

// mmArrow::Update: the arrow's matrix in camera space and whether the
// target is behind the camera (yellow paint job); nullopt when the target
// is level with the camera, which keeps the previous colour.
struct ArrowPose {
    Mat34 local;
    std::optional<bool> behind;
};
ArrowPose arrowPose(const Mat34& camera, const Vec3& target);

// mmWaypointObject + mmCheckpointInstance::Draw: world matrix of a
// checkpoint stand (pt_check / pt_finish).
Mat34 standMatrix(const Checkpoint& cp);

// mmHudMap::Cull: the overhead camera (looks down -Y from `height`).
// Rotating: the car's heading points up; otherwise -Z points up.
Mat34 mapCamera(const Mat34& car, float height, bool rotating);
// mmHudMap::DrawIcon: world matrix of a car arrow on the map.
Mat34 mapIconMatrix(const Mat34& car, float iconScale);
// mmHudMap::SetMapMode: the map's virtual-space rectangle (x, y, w, h) in a
// layout; rightHandDrive is vehicle Flags 0x40.
Vec4 mapRect(const render::UiLayout& layout, const HudMapParams& params, const HudOptions& options,
             bool rightHandDrive);
// Linear approach used for the map zoom and icon size.
float approach(float current, float target, float rate, float dt);

// Which parts of the HUD a mode shows (mmHUD::Init, mmSingleStunt::InitHUD).
bool arrowShown(GameMode mode, const LessonEvent* lesson);
bool clockShown(GameMode mode, const LessonEvent* lesson);
bool checkReadoutShown(GameMode mode, const LessonEvent* lesson);

// mmHudMap::IconType: colours of the car arrows on the map.
enum class MapIcon : std::uint8_t {
    Outline = 0,  // black, behind the player's arrow
    Police = 1,   // red
    Player = 5,   // yellow
    Opponent = 7, // violet (single player)
    // Network players use slot + 4 (mmHudMap::DrawOpponents): red, yellow,
    // orange, violet, cyan, pink, then two values past the end of MM2's
    // ten-entry table (the bytes that follow it).
};
// The icon's colour as 0xAARRGGBB (the table DrawIcon indexes; 0 .. 11).
std::uint32_t mapIconColor(MapIcon icon);
std::uint32_t mapIconColor(int iconType);
// mmGame::mmGame: the opponent icons' colours of the network players by
// slot (blue, green, red, yellow, orange, violet, cyan, pink); Cops and
// Robbers team games paint them red or blue (mmMultiCR::GameMessage).
std::uint32_t netIconColor(int slot);
// mmIcons::Cull draws the icons in passes 0 .. slots - 1: pass k draws the
// icons not drawn yet whose place is k + 1 or above 7. Returns the indices
// in drawing order (icons placed past `slots` and below 8 are not drawn).
std::vector<std::size_t> iconDrawOrder(std::span<const int> places, int slots);
// The opp_icon.tex cell of a place 1 .. 9 (4 x 4 cells of 0.25): column
// (place - 1) % 4, row (place - 1) / 4.
Vec2 iconDigitCell(int place);

// mmHudMap::GetNextMapMode (game/CamPlayer.h).
using game::nextMapMode;

// The 3D view's pixel rectangle on a screen of `width` x `height`
// (gfxPipeline::VP): mmHudMap::SetMapMode puts it in the top half for the
// split map (0, 0, w, h / 2) and in the small map's place for the full-screen
// map (Pos x screen, Size x screen); otherwise mmPlayer::SetWideFOV letterboxes
// it for the wide angle (0, 0.18 h, w, 0.66 h) or gives it the whole screen.
// Sizes truncated (__ftol) as the original.
render::Rect sceneRect(int width, int height, const HudMapParams& params, MapMode mode, bool wideAngle);
// mmHUD::Update: the message nodes sit at 0.8 / 0.875 (or 0.2 / 0.35 for the
// upper messages) while the 3D view starts at the top of the screen, and at
// 0.05 / 0.1 when it does not (the wide angle's letterbox, the full-screen
// map's inset). `second` is the SetMessage2 line.
float messageTop(bool top, bool second, bool viewAtTop);

} // namespace hud

// mmMultiCR's objects and readouts for the HUD (Cops and Robbers).
struct CrDisplay {
    bool enabled = false;
    std::optional<Vec3> gold; // the gold's place (drawn while not delivered)
    Vec3 goldOnMap;           // the gold object's place, always shown on the map
    struct Base {
        std::string model; // pt_bank / pt_hideout, pt_blue / pt_red in Robber Teams
        Vec3 position;
    };
    std::vector<Base> bases; // the bank (blue) first, then the hideout (red)
    std::optional<Vec3> arrowInterest; // mmArrow::SetInterest
    float time = 0.0f;                 // the powerup's spin (ElapsedTime)
    // mmCRHUD's readouts: the team totals (team 0 blue, team 1 red) or, in
    // Free-For-All, the player's score.
    bool teams = true;
    bool copsVsRobbers = true; // "COPS" / "ROBBERS", else "BLUE" / "RED"
    int blueScore = 0, redScore = 0, playerScore = 0;
    float timeLeft = -1.0f; // the time limit's clock (none below 0)
    // mmCRHUD::SetName: the player's name, blue, or red on team 1.
    std::string playerName;
    bool playerRed = false;
    // mmCRHUD::AddPlayer / SetScore / ActivateRosterGold: the other players
    // in the order they joined (seven rows), each in its icon colour
    // (0xAARRGGBB) with its score, "$" on the gold carrier's row.
    struct RosterEntry {
        std::string name;
        int score = 0;
        std::uint32_t color = 0xFFFFFFFFu;
        bool gold = false;
    };
    std::vector<RosterEntry> roster;
    // mmCRHUD::ActivateGold: the player carries the gold.
    bool carryingGold = false;
};

class Hud {
public:
    Hud(render::Device& device, TextureLibrary& textures, ModelLibrary& models, const vfs::Vfs& vfs,
        const Strings& strings, const std::string& city, const std::string& vehicle);

    HudOptions& options() { return m_options; }

    // Loads every model and texture the HUD uses. Call while loading the
    // race, outside a frame (creating GPU resources mid-frame is avoided).
    void preload(ui::TextureCache* art = nullptr);

    // Scene pass, with the world's frame constants set: checkpoint stands,
    // opponent / checkpoint icons, the checkpoint arrow (camera space) and,
    // in dashboard view, the dash. `blips` are the opponents for the icons.
    void drawWorld(const Session& session, const Camera& camera, const PlayerState& player,
                   float steering = 0.0f, std::span<const MapBlip> blips = {});

    // Scene pass, after everything else: the overhead map in its own
    // viewport. Leaves viewport, scissor and frame constants changed.
    void drawMap(const Session& session, const PlayerState& player, std::span<const MapBlip> blips, float dt);

    // Overlay pass, outside overlay.begin()/end().
    void drawOverlay(render::Overlay2D& overlay, ui::TextRenderer& text, ui::TextureCache& art,
                     const Session& session, const PlayerState& player);

    // Virtual-space rectangle of the map (x, y, w, h) for the current options.
    Vec4 mapRect(const render::UiLayout& layout) const;
    // The 3D view's pixel rectangle on the scene for the current options
    // (hud::sceneRect).
    render::Rect sceneRect(render::Extent2D scene) const;

    // The scene's view * projection, for the labels mmIcons projects onto
    // the screen. Set each frame before drawOverlay.
    void setViewProjection(const Mat44& viewProj) {
        m_viewProj = viewProj;
        m_viewProjValid = true;
    }
    // The scene's frame constants and the same projection with a 0.01 m
    // near plane, which mmDashView::Cull sets while it draws the dash
    // (gfxViewport::Perspective); drawWorld restores the frame after.
    void setDashFrame(const render::FrameConstants& frame, const Mat44& nearProj) {
        m_sceneFrame = frame;
        m_dashProj = nearProj;
        m_dashFrameValid = true;
    }

    // The in-race view keys the HUD handles itself, as mmViewMgr::SetViewSetting
    // does (the map's mode is changed by PlayerCameras::setViewSetting).
    // "Map Zoom" (7, mmHudMap::ToggleMapRes) and "Rotating Map" (8,
    // ToggleMapOrient): nothing while the map is off.
    void toggleMapZoom();
    void toggleMapRotation();
    // "HUD Toggle" (4, mmHUD::ToggleExternalView): the instrument cluster;
    // it comes back only without the HUD's dashboard flag.
    void toggleCluster();
    // "Opponent Position" (mmGame::UpdateGameInput, SetIconsState).
    void toggleOpponentIcons() { m_options.opponentIcons = !m_options.opponentIcons; }
    void setCopsAndRobbers(CrDisplay display) { m_cr = std::move(display); }
    // OpenMM2 Enhanced: MM1's map (mmHudMap) for an MM1 city: its extent
    // (city/<city>.ext: x min, z min, x max, z max) and the bitmaps
    // mm1/bmp16/<city>320.jpg and <city>640.jpg. Sets MM1's layout: the
    // small map a quarter of the screen in the lower right corner.
    void setMm1Map(const std::string& city, const std::array<float, 4>& extent);

    // mmHUD::PostChatMessage: the chat node's five lines scroll up, the new
    // one last, and the node shows again; mmHUD::Update hides it 15 s after
    // the last line.
    void postChat(std::string line);
    void updateChat(float dt);
    // The HUD's own clock (asSimulation's elapsed time, for MM1's damage
    // warning flash); call once per frame.
    void tick(float dt) { m_elapsed += dt; }
    const std::array<std::string, 5>& chatLines() const { return m_chat; }
    bool chatShown() const { return m_chatShown; }

private:
    void drawStands(const Session& session);
    void drawIcons(const Session& session, const Camera& camera, std::span<const MapBlip> blips);
    void drawArrow(const Session& session, const Camera& camera);
    void drawDash(const Camera& camera, const PlayerState& player, float steering);
    // OpenMM2 Enhanced: MM1's mmHudMap (2D: a window of the city bitmap
    // around the player, the dots and the cars' icons).
    void drawMm1Map(render::Overlay2D& ov, ui::TextureCache& art, const Session& session, const PlayerState& player);
    // OpenMM2 Enhanced: MM1's mmExternalView in the lower left corner.
    void drawMm1Cluster(render::Overlay2D& ov, ui::TextureCache& art, const PlayerState& player);
    void drawCluster(render::Overlay2D& ov, ui::TextureCache& art, const PlayerState& player, float x,
                     float y);
    void drawClock(render::Overlay2D& ov, ui::TextureCache& art, float seconds, float centerX, float y);
    void drawReadouts(render::Overlay2D& ov, ui::TextRenderer& text, const Session& session);
    // mmIcons::Cull's labels (network games only): the players' names.
    void drawIconLabels(render::Overlay2D& ov, ui::TextRenderer& text, const Session& session);
    void drawCardQuad(const std::array<Vec3, 4>& corners, const std::array<Vec2, 4>& uvs,
                      const WorldTexture& texture);
    // `second`: the SetMessage2 line, in its own one-line node under the message.
    void drawMessage(render::Overlay2D& ov, ui::TextRenderer& text, const HudMessage& message, bool second = false);
    void drawTriangle(const Vec3& a, const Vec3& b, const Vec3& c, std::uint32_t argb);
    void drawChat(render::Overlay2D& ov, ui::TextRenderer& text);
    void drawCrObjects(const Camera& camera);
    void drawCrReadouts(render::Overlay2D& ov, ui::TextRenderer& text, ui::TextureCache& art);
    void drawCrGoldIcon(const Camera& camera);
    void trackLapTimes(const Session& session);
    ui::FontSpec font(std::uint32_t id, const char* fallback) const;
    float px(float pixels) const { return pixels * m_options.pixelSize; }

    render::Device& m_device;
    TextureLibrary& m_textures;
    ModelLibrary& m_models;
    const Strings& m_strings;
    std::string m_city;
    std::string m_vehicle;
    bool m_rightHandDrive = false; // tune/<car>.info Flags 0x40 (British cars)
    HudOptions m_options;
    HudMapParams m_map;
    std::optional<DashParams> m_dash;
    // mmHudMap: current camera height and icon size, approaching the targets.
    float m_mapZoom = 0.0f, m_mapIconScale = 0.0f;
    std::optional<MapMode> m_mapModeApplied; // snaps zoom and icon size on change
    Mat44 m_viewProj;
    bool m_viewProjValid = false;
    render::FrameConstants m_sceneFrame;
    Mat44 m_dashProj;
    bool m_dashFrameValid = false;
    std::vector<MapBlip> m_labelBlips; // the network players' labels (drawIcons -> drawIconLabels)
    Vec3 m_labelEye;
    int m_arrowPaint = 0;                    // mmArrow colour state
    std::vector<float> m_lapTimes;           // completed laps (mmCircuitHUD::SetLapTime)
    float m_lastLapSeen = 0.0f;
    std::array<std::string, 5> m_chat; // mmHUD's chat node (+0x8b0)
    CrDisplay m_cr;
    Mat34 m_crGoldSpin; // mmCRHUD +0x148: the HUD gold's spin and place in camera space
    bool m_chatShown = false;
    float m_chatTime = 0.0f;
    float m_elapsed = 0.0f; // tick()
    std::string m_mm1MapCity;           // setMm1Map
    std::array<float, 4> m_mm1Extent{}; // x min, z min, x max, z max
    std::vector<MapBlip> m_mm1Blips;    // drawMap's, for drawMm1Map
};

} // namespace mm2::game::session
