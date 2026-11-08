// A session in the city.
#include "app/Controls.h"
#include "app/DrawTrace.h"
#include "app/ForceFeedback.h"
#include "app/GameInput.h"
#include "app/Screens.h"
#include "app/Tweaks.h"
#include "app/frontend/PopupOptions.h"
#include "app/frontend/Results.h"
#include "city/CityData.h"
#include "city/RoomInfo.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "asset/Pkg.h"
#include "asset/VehicleModel.h"
#include "audio/MusicDirector.h"
#include "audio/SoundBank.h"
#include "audio/game/Ambience.h"
#include "audio/game/AudioManager.h"
#include "audio/game/CarAudio.h"
#include "audio/game/Object3D.h"
#include "audio/AngelRandom.h"
#include "audio/game/PedAudio.h"
#include "audio/game/Voices.h"
#include "game/CamMirror.h"
#include "game/Profile.h"
#include "data/DatFile.h"
#include "data/TextTables.h"
#include "ai/Opponent.h"
#include "ai/Police.h"
#include "ai/World.h"
#include "game/AiRenderer.h"
#include "game/session/CopsAndRobbers.h"
#include "game/session/Hud.h"
#include "game/session/Session.h"
#include "game/TrafficBodies.h"
#include "game/bangers/BangerSet.h"
#include "game/bangers/PropPlacement.h"
#include "game/bangers/RoadDecals.h"
#include "game/fx/EffectLibrary.h"
#include "game/fx/ParticleRenderer.h"
#include "game/fx/SkidMarks.h"
#include "game/fx/VehicleEffects.h"
#include "game/fx/Weather.h"
#include "game/net/Autopilot.h"
#include "game/net/DamageSync.h"
#include "game/net/NetProps.h"
#include "game/net/NetGame.h"
#include "game/net/NetRules.h"
#include "game/net/NetTrafficCars.h"
#include "game/net/PlayerCars.h"
#include "game/net/RaceStart.h"
#include "game/net/RulesTrace.h"
#include "game/net/TrafficProxies.h"
#include "game/net/TrafficSync.h"
#include "game/net/TrafficTrace.h"
#include "game/CamMirror.h"
#include "game/CamPlayer.h"
#include "game/CityRenderer.h"
#include "game/CityLevel.h"
#include "game/Interpolation.h"
#include "game/PlayerVehicle.h"
#include "game/MeshDraw.h"
#include "game/SunShadow.h"
#include "game/VehicleRenderer.h"
#include "game/world/CableCars.h"
#include "game/world/Gizmos.h"
#include "phys/World.h"
#include "render/Projection.h"
#include "ui/Text.h"
#include "ui/TextureCache.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace mm2::app {
namespace {

// OpenMM2's shared traffic of a network cruise: the AI's ids of the other
// players' cars (ai::TrackedCar), the network ids of the police cars (after
// the traffic pool's), and how often the host sends (the snapshot rate).
constexpr int kNetPlayerTrackedId = 30000;
constexpr int kNetPoliceId = 400;
constexpr std::uint64_t kNetTrafficIntervalMs = 50;
// A shared-traffic client: how long after its car's hit (on the host's clock:
// the host simulates the same hit at that time) a host state may still show
// the car on its rail before the client's knock is withdrawn (a message
// interval, two AI steps and the jitter of the client's lead).
constexpr double kNetKnockConfirmMs = 150.0;

const char* modePrefix(game::GameMode m) {
    switch (m) {
    case game::GameMode::Blitz: return "blitz";
    case game::GameMode::Circuit: return "circuit";
    case game::GameMode::Checkpoint: return "race";
    case game::GameMode::CrashCourse: return "crash";
    default: return "";
    }
}

class RaceScreen final : public Screen {
public:
    // The in-race popup's pages (mmPopup: PUMain, PUChat, and the pages
    // frontend::PopupOptions builds: OPTIONS, PUQuit, PUKey, PURoster).
    enum class Popup : std::uint8_t { None, Main, Chat, Options };

private:
    struct RemoteVehicle; // another player's car (below)

public:
    RaceScreen(Context& ctx, const game::RaceConfig& config)
        : m_ui(ctx.device(), ctx.game->vfs), m_text(ctx.device()) {
        m_result.config = config;
        // The network race this screen runs (FrontendScreen starts it when
        // the countdown arrives).
        if (config.multiplayer && ctx.netGame) {
            m_traceNet = ctx.netGame.get();
            m_netRace = ctx.netGame->raceNumber();
            m_chatSeen = ctx.netGame->chatSerial(); // the lobby's lines stay there
        }
        // GetLoadScreenName: <city>_<mode><n>.jpg (cruise "roam" and Cops and
        // Robbers "multicop" without a number), else the generic one.
        const std::string prefix = modePrefix(config.mode);
        m_loadingImage = "jpg/loading.jpg";
        std::string specific;
        if (!prefix.empty() && config.raceIndex >= 0)
            specific = std::format("jpg/{}_{}{}.jpg", config.city, prefix, config.raceIndex);
        else if (config.mode == game::GameMode::Cruise)
            specific = "jpg/" + config.city + "_roam.jpg";
        else if (config.mode == game::GameMode::CopsAndRobbers)
            specific = "jpg/" + config.city + "_multicop.jpg";
        if (!specific.empty() && ctx.game->vfs.exists(specific))
            m_loadingImage = specific;
    }

    ~RaceScreen() override {
        if (m_cityLevel && m_trafficBodies)
            m_cityLevel->removeSource(m_trafficBodies.get());
        if (m_cityLevel && m_trafficProxies)
            m_cityLevel->removeSource(m_trafficProxies.get());
        for (auto& [id, c] : m_netCops)
            if (c.audio)
                c.audio->stop();
        for (auto& [model, c] : m_spareNetCops)
            if (c.audio)
                c.audio->stop();
        if (m_cityLevel && m_bangers)
            m_cityLevel->removeSource(m_bangers.get());
        if (m_cityLevel && m_gizmos)
            m_cityLevel->removeSource(m_gizmos.get());
        m_gizmos.reset(); // its sounds hold slots of m_audioSlots
        if (m_cityLevel && m_cableCars)
            m_cityLevel->removeSource(m_cableCars.get());
        m_cableCars.reset();
        m_carAudio.stop();
        for (auto& o : m_opponents)
            if (o.audio)
                o.audio->stop();
        for (auto& c : m_cops)
            if (c.audio)
                c.audio->stop();
        m_rain.stop();
        m_announcer.stop();
        m_pedAudio.stop();
        if (m_ctxMixer)
            m_ctxMixer->stopAll();
        // The police drivers hand their cars' impact callbacks back when they
        // go: before the cars (m_cops) are destroyed.
        m_police.reset();
        m_ff.stopAll(); // mmInput::StopAllFF
    }

    // RestoreFocus: when the display surfaces come back after the game lost
    // them (another application took the full screen), a running
    // single-player game that is not paused opens the in-race menu, paused
    // (mmGameManager::ForcePopupUI: ProcessEscape(1)). OpenMM2 does it when
    // the window is activated again in a full-screen mode (inferred: a
    // window keeps its surfaces).
    void activated(Context& ctx) override {
        if (m_state != State::Running || multiplayer(ctx) || m_paused || m_popup != Popup::None)
            return;
        if (ctx.window().mode() == platform::WindowMode::Windowed)
            return;
        openPopup(ctx, true);
    }

    // The loading picture covers the screen until the race is loaded.
    bool usesScene() const override { return m_city != nullptr && m_state == State::Running; }

    void update(Context& ctx, double dt) override {
        m_time += dt;
        m_frameDt = static_cast<float>(dt);
        m_frameSteps = 0;
        if (m_state == State::ShowLoading) {
            // BeginPhase: the loading picture with the bar at 10 %, drawn
            // once before the loading blocks.
            m_state = State::Load;
            m_loadPercent = 10;
            return;
        }
        if (m_state == State::Load) {
            loadStep(ctx);
            // A network race keeps its session serviced between the parts
            // (acknowledgements and pings), so that the others do not time
            // this machine out while it loads more slowly than they do. What
            // arrives meanwhile waits for the first frame of the race, except
            // the end of the race (the host took everyone back, or left).
            if (multiplayer(ctx) && !ctx.nextScreen) {
                ctx.netGame->update();
                if (ctx.netGame->backToLobby(m_netRace) || !ctx.netGame->inSession()) {
                    log::info("race: the network race is over before it has loaded ({})",
                              ctx.netGame->inSession() ? "back to the lobby" : "the session ended");
                    leaveRace(ctx, m_result);
                }
            }
            return;
        }
        // GameLoop: AudManager::Update before the game's update, then again
        // as asRoot's first node, with the pause state the frame starts with
        // (every sound stops while paused).
        if (ctx.mixer)
            m_audioManager.updateFrame(m_paused, *ctx.mixer, m_announcerOk ? &m_announcer : nullptr,
                                       static_cast<float>(dt));
        // mmInput::Update: the controller's bindings against the devices.
        {
            const auto size = ctx.window().size();
            m_gameInput.update(controls::readFrame(ctx.input, m_gameInput.controller(), m_controlOptions.deadZone,
                                                   static_cast<float>(size.width), static_cast<float>(size.height)),
                               static_cast<float>(dt));
        }
        // mmReplayManager::Update, the next node of asRoot: a restart asked
        // for last frame resets the whole game before anything of this frame
        // updates (mmReplayManager::Reset -> mmGameManager -> the mode's
        // Reset).
        if (m_restartPending)
            applyRestart(ctx);
        // mmPopup: Escape opens the main menu (pausing a single-player game,
        // mmPopup::ProcessEscape(1)); while it is up the game's keys are off.
        stepPopupScript(ctx); // OPENMM2_POPUP_SCRIPT automation (its keys reach the popup this frame)
        m_popupGraveyard.clear();
        // OpenMM2: F9 switches between the tweaked look and the original.
        if (ctx.input.keyPressed(platform::Key::F9) && m_state == State::Running) {
            ctx.originalLook = !ctx.originalLook;
            applyTweaksToCars(ctx);
            m_lookMessage = 2.0f;
        }
        m_lookMessage = std::max(0.0f, m_lookMessage - static_cast<float>(m_frameDt));
        if (!m_flyCamera && ctx.input.keyPressed(platform::Key::F1)) {
            // mmPopup::ProcessKeymap (F1, from the game, the paused game and
            // the menu): PUKey, which F1 closes again.
            processKeymap(ctx);
        }
        // MenuManager::ScanGlobalKeys: F4 in the popup sets the menu's state
        // to 6, which mmPopup::Update acts on in PUMain only: the race
        // restarts as with Restart Race (OpenMM2: not in network games).
        if (m_popup == Popup::Main && ctx.input.keyPressed(platform::Key::F4) && m_session &&
            !multiplayer(ctx))
            restartFromMenu(ctx);
        if (m_popup != Popup::None) {
            updatePopup(ctx, dt);
            if (ctx.nextScreen)
                return;
        } else if (ctx.input.keyPressed(platform::Key::Escape)) {
            // mmGame::UpdateDebugInput: Escape stops the announcer
            // (mmSpeechContainer::Stop), then mmPopup::ProcessEscape.
            m_announcer.stop();
            openPopup(ctx, true);
        } else if (!m_flyCamera && m_gameInput.fired(controls::Action::EnterChat)) {
            openChat(ctx);
        } else if (!m_flyCamera) {
            debugKeys(ctx);
        }
        if (m_textInput && m_popup != Popup::Chat) {
            ctx.input.stopTextInput(ctx.window());
            m_textInput = false;
        }
        postIncomingChat(ctx);
        updateNetPlayers(ctx);
        if (m_hud)
            m_hud->updateChat(static_cast<float>(dt));
        // Development aid: OPENMM2_DEBUG_NET_SHOT_MS=<session ms> ends a
        // network race at that session time (with --screenshot: every
        // machine's picture of the same moment); "+<ms>" counts from the
        // race's order (GO DRIVE), and also before the race has started.
        // Several times, comma-separated: the earlier ones save numbered
        // pictures (<screenshot>-1.png, ...) and the race goes on.
        if (const char* shots = std::getenv("OPENMM2_DEBUG_NET_SHOT_MS"); shots && multiplayer(ctx)) {
            const auto list = str::split(shots, ',');
            const auto k = static_cast<std::size_t>(m_netShotsTaken);
            const std::string_view shot = k < list.size() ? list[k] : std::string_view{};
            const bool fromOrder = shot.starts_with('+');
            const long long at = str::parseInt(fromOrder ? shot.substr(1) : shot).value_or(0) +
                                 (fromOrder ? static_cast<long long>(ctx.netGame->raceOrderTime()) : 0LL);
            const auto now = static_cast<long long>(ctx.netGame->sessionTime());
            if (!shot.empty() && (fromOrder || ctx.netGame->raceStarted()) && now >= at) {
                if (++m_netShotsTaken == static_cast<int>(list.size()))
                    ctx.lastFrameRequested = true;
                else
                    ctx.captureTag = m_netShotsTaken;
            }
        }
        debugRespawn(ctx);
        if (multiplayer(ctx)) {
            ctx.netGame->update();
            if (ctx.netGame->backToLobby(m_netRace) || !ctx.netGame->inSession()) {
                log::info("race: the network race is over ({})",
                          ctx.netGame->inSession() ? "back to the lobby" : "the session ended");
                leaveRace(ctx, netRaceResult(ctx));
                return;
            }
            takeNetEvents(ctx);
            updateNetStart(ctx, static_cast<float>(dt));
        }
        if (ctx.input.keyPressed(platform::Key::F2))
            m_flyCamera = !m_flyCamera;
        if (!m_flyCamera && m_popup == Popup::None)
            updateGameInput(ctx);
        // A restart asked for this frame: nothing of the old race moves on
        // before the reset (the menu's Restart leaves the game paused until
        // mmReplayManager has reset it).
        if (m_paused || m_restartPending) {
            // asRoot paused (the menu or the full-screen map in single
            // player): the rules, the AI, the physics and the clocks stand
            // still. mmGame::Update still updates the announcer's queue
            // (mmSpeechContainer::Update) and runs its fall and water checks,
            // and mmHUD::Update counts the message down.
            if (m_announcerOk)
                m_announcer.update(static_cast<float>(dt));
            if (m_session && m_player && !m_restartPending) {
                m_playerState = playerState();
                m_session->updatePaused(static_cast<float>(dt), m_playerState);
                handleSessionEvents(ctx);
            }
            // mmGameManager::Update runs cityLevel::Update (the sky turns,
            // lvlSky::Update), cityLevel::PreDraw (the rain and snow,
            // asParticles::Update, and the texture movies) and the camera
            // (camViewCS::Update) whether or not the game is paused.
            if (m_flyCamera || !m_player)
                updateFlyCamera(ctx, static_cast<float>(dt));
            else
                updateCarCamera(ctx, static_cast<float>(dt));
            if (m_weather)
                m_weather->update(static_cast<float>(dt), m_camera.transform);
            m_textures->update(m_time);
            if (m_cityRenderer)
                m_cityRenderer->update(m_frameDt);
            updateForceFeedback(ctx, static_cast<float>(dt), true);
            return;
        }
        updatePlayer(ctx, static_cast<float>(dt));
        // aiMap::Update: the ambient traffic and the pedestrians first, then
        // the racers and the police, before the physics step.
        updateAmbient(ctx, static_cast<float>(dt));
        updateAiDrivers(static_cast<float>(dt));
        // aiMap::Update's cable cars, after the racers.
        if (m_cableCars && m_world) {
            const ai::TrackedCar player = m_player ? trackedCar(m_player->sim(), 0, true) : ai::TrackedCar{};
            m_cableCars->update(static_cast<float>(dt), m_player ? &player : nullptr, *m_world);
        }
        if (m_ai && !netTrafficClient(ctx))
            m_ai->updateLights(); // the light sets last (aiMap::Update); a shared-traffic client: the host's
        // The gizmo managers, nodes of mmGame (bridges, trains, ferries,
        // sailboats); the player's car is the bridges' proximity trigger.
        if (m_gizmos) {
            std::optional<Vec3> trigger;
            if (m_player)
                trigger = m_player->sim().body.ics.matrix.m3;
            m_gizmos->update(static_cast<float>(dt), trigger);
        }
        updateRemoteCars(ctx, static_cast<float>(dt));
        updateNetTraffic(ctx, static_cast<float>(dt)); // OpenMM2: a client's shared traffic
        updateNetProps(ctx, static_cast<float>(dt));   // OpenMM2: a client's props are the host's
        if (multiplayer(ctx))
            m_netDamage.settle(ctx.netGame->frameTime()); // OpenMM2: the damage of cars not drawn
        // aiVehicleManager::Update and the rail cars' rooms, before the
        // collision manager runs.
        if (m_trafficBodies)
            m_trafficBodies->beforeStep();
        // vehCar::Update: cars in a water room float once below its level.
        if (m_player)
            m_player->sim().setWaterLevel(waterLevelAt(m_player->sim().modelMatrix().m3));
        for (auto& o : m_opponents)
            if (o.sim)
                o.sim->sim().setWaterLevel(waterLevelAt(o.sim->sim().modelMatrix().m3));
        for (auto& c : m_cops)
            if (c.sim)
                c.sim->sim().setWaterLevel(waterLevelAt(c.sim->sim().modelMatrix().m3));
        // dgTrailerJoint::Update's debug key: Ctrl+B breaks every trailer
        // hitch (held until a physics sample has seen it).
        {
            using platform::Key;
            const auto& in = ctx.input;
            if (!m_flyCamera && (in.keyDown(Key::LCtrl) || in.keyDown(Key::RCtrl)) && in.keyPressed(Key::B))
                phys::Trailer::breakKeyPressed = true;
        }
        // OpenMM2: a network client's car takes the host's word on its
        // earlier samples first (game/net/PlayerCars).
        reconcileNetCar(ctx);
        m_frameSteps = m_world ? m_world->advanceFixed(physicsDt(ctx, static_cast<float>(dt))) : 0;
        if (m_frameSteps > 0)
            phys::Trailer::breakKeyPressed = false;
        // The props and traffic cars the collisions set moving follow their
        // bodies; the ones that came to rest stop being simulated.
        if (m_bangers)
            m_bangers->update(static_cast<float>(dt));
        sendNetProps(ctx); // OpenMM2: the host's props to the others
        if (m_trafficBodies)
            m_trafficBodies->afterStep();
        if (m_player) {
            m_pose = m_player->pose();
            m_trailerPose = m_player->trailerPose();
        }
        afterNetSamples(ctx); // OpenMM2: the host's states, a client's inputs
        updateDrawnPoses(); // OpenMM2: between the last two simulation steps
        traceNetDrawn(ctx); // OPENMM2_NET_TRACE: every player's car as drawn
        sendNetTraffic(ctx); // OpenMM2: the host's shared traffic
        if (m_flyCamera || !m_player)
            updateFlyCamera(ctx, static_cast<float>(dt));
        else
            updateCarCamera(ctx, static_cast<float>(dt));
        updateAudio(ctx, static_cast<float>(dt));
        updateEffects(static_cast<float>(dt));
        sendNetDamage(ctx); // OpenMM2: what this machine's cars painted and broke
        receiveNetRules(ctx); // OpenMM2: a client takes the host's word on the rules
        updateSession(ctx, static_cast<float>(dt));
        updateCopsAndRobbers(ctx, static_cast<float>(dt));
        sendNetRules(ctx, static_cast<float>(dt)); // OpenMM2: the host decides them
        // Development aid: OPENMM2_DEBUG_FOCUS=ped|car frames the nearest
        // pedestrian or traffic car (for screenshots); net:<player id> a
        // network player's car (this machine's own, or another's as drawn),
        // police:<ambient id> a shared-traffic police car (400 + its place),
        // knocked:<player id> the knocked traffic car nearest that player,
        // prop:<index> a placed prop or its first piece.
        if (const char* focus = std::getenv("OPENMM2_DEBUG_FOCUS"); focus && (m_ai || multiplayer(ctx))) {
            const Vec3 ref = m_player ? m_pose.body.m3 : m_camera.position();
            std::optional<Mat34> target;
            float best = 1e30f;
            auto consider = [&](const Mat34& m) {
                const float d = m.m3.dist2(ref);
                if (d < best) {
                    best = d;
                    target = m;
                }
            };
            const std::string_view what(focus);
            if (what.starts_with("net:") && multiplayer(ctx)) {
                const int id = str::parseInt(what.substr(4)).value_or(-1);
                if (id == ctx.netGame->localId() && m_player)
                    consider(m_drawPose.body);
                for (const auto& rc : m_remoteCars)
                    if (rc.id == id && rc.hasState)
                        consider(netCarDrawn(rc.id).value_or(rc.transform));
            } else if (what.starts_with("police:")) {
                const int id = str::parseInt(what.substr(7)).value_or(-1);
                const auto i = static_cast<std::size_t>(id - kNetPoliceId);
                if (id >= kNetPoliceId && i < m_cops.size())
                    consider(drawnPose(game::Drawn::Police, i, *m_cops[i].sim).body);
                if (const auto it = m_netCops.find(id); it != m_netCops.end() && it->second.sim)
                    consider(netCopDrawn(id).value_or(it->second.state.transform));
            } else if (what.starts_with("knocked:") && multiplayer(ctx)) {
                // The knocked traffic car with a body within 60 m of a
                // player's car with the lowest id (shared traffic: the host's
                // simulated, a client's received: the same car on both).
                const int id = str::parseInt(what.substr(8)).value_or(-1);
                std::optional<Mat34> player;
                if (id == ctx.netGame->localId() && m_player)
                    player = m_drawPose.body;
                else if (id >= 0 && id < 256)
                    player = netCarDrawn(static_cast<std::uint8_t>(id));
                if (player && (m_trafficClient || m_ai)) {
                    const Vec3 at = player->m3;
                    int lowest = std::numeric_limits<int>::max();
                    for (const auto& c : m_trafficClient ? m_netCarsDrawn : m_ai->cars()) {
                        const auto physical = physicalTrafficCar(c.id);
                        const bool near = c.transform.m3.dist(at) < 60.0f;
                        if (physical && physical->active && c.id < lowest && near) {
                            lowest = c.id;
                            target = physical->transform;
                        }
                    }
                }
            } else if (what.starts_with("traffic:") && (m_trafficClient || m_ai)) {
                // A traffic car by id (shared traffic: the same car on every machine).
                const int id = str::parseInt(what.substr(8)).value_or(-1);
                for (const auto& c : m_trafficClient ? m_netCarsDrawn : m_ai->cars())
                    if (c.id == id) {
                        const auto physical = physicalTrafficCar(c.id);
                        target = physical ? physical->transform : c.transform;
                    }
            } else if (what.starts_with("prop:") && m_bangers) {
                // A placed prop by index (the same on every machine), or
                // once knocked over its first piece shown.
                const auto id = static_cast<std::size_t>(str::parseInt(what.substr(5)).value_or(-1));
                const auto& instances = m_bangers->instances();
                if (id < instances.size()) {
                    Vec3 at = instances[id].ground.m3;
                    for (const auto& inst : instances)
                        if (!m_bangers->standing(id) && inst.source == static_cast<int>(id) &&
                            inst.state != game::bangers::BangerSet::State::Gone) {
                            at = inst.drawn.value_or(inst.matrix).m3;
                            break;
                        }
                    target = Mat34::translation(at); // seen from the same side on every machine
                }
            } else if (what == "ped" && m_ai) {
                for (const auto& p : m_ai->peds())
                    consider(p.transform);
            } else if (m_ai) {
                for (const auto& c : m_ai->cars())
                    consider(c.transform);
            }
            if (target) {
                const Vec3 eye = target->m3 + target->transformDir({2.5f, 1.6f, -3.5f});
                m_camera.transform = game::Camera::lookAt(eye, target->m3 + Vec3{0, 0.9f, 0});
            }
        }
        m_textures->update(m_time);
        if (m_cityRenderer)
            m_cityRenderer->update(m_frameDt);
        if (ctx.input.keyPressed(platform::Key::F3))
            m_showDebug = !m_showDebug;
        if (m_showDebug)
            drawDebugUi();
    }

    void drawScene(Context& ctx) override {
        render::Device& dev = ctx.device();
        render::ClearValues clear;
        clear.color = m_env.clearColor;
        const auto extent = dev.sceneExtent();
        render::Rect band{0, 0, extent.width, extent.height};
        // The 3D view (gfxPipeline::VP) on black: letterboxed to 66% of the
        // height, 18% down, for the wide angle (mmPlayer::SetWideFOV), the
        // top half with the split map, the small map's place with the
        // full-screen map (mmHudMap::SetMapMode; Hud::sceneRect).
        syncHudView();
        if (m_hud && !m_flyCamera)
            band = m_hud->sceneRect(extent);
        const bool letterbox = band != render::Rect{0, 0, extent.width, extent.height};
        if (letterbox)
            clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        // OpenMM2 sun shadows: the shadow map first (outside the scene pass).
        const std::optional<SunShadows> sun = drawSunShadows(ctx);
        game::VehicleRenderer::setSunShadowsActive(sun.has_value());
        const IniFile& tweaks = drawnTweaks(ctx.settings.ini, ctx.originalLook);
        dev.beginScene(clear);
        std::vector<game::session::MapBlip> blips = hudBlips(ctx);
        // OpenMM2: the map and the dashboard follow the car as drawn.
        game::session::PlayerState shown = m_playerState;
        if (m_player)
            shown.transform = drawnIcs(m_drawPose.body, *m_player);
        // mmGameManager::Cull draws the full-screen map before the level.
        const bool hudShown = m_hud && m_session && m_player && !m_flyCamera;
        const bool mapShown = hudShown && (m_popup == Popup::None || m_popup == Popup::Chat);
        const bool fullMap = mapShown && m_cams.mapMode() == game::MapMode::FullScreen;
        if (fullMap)
            m_hud->drawMap(*m_session, shown, blips, m_frameDt);
        if (letterbox) {
            dev.setViewport({static_cast<float>(band.x), static_cast<float>(band.y),
                             static_cast<float>(band.width), static_cast<float>(band.height)});
            dev.setScissor(&band);
            render::ClearValues sky;
            sky.color = m_env.clearColor;
            dev.clear(sky);
        }
        const float aspect = band.height ? static_cast<float>(band.width) / static_cast<float>(band.height) : 1.0f;
        const auto proj = render::computeProjection(m_camera.horizontalFov, aspect, ctx.display.fovMode,
                                                    ctx.display.maxAspect);
        m_camera.farPlane = m_env.farClip;
        render::FrameConstants frame = m_env.frame;
        frame.view = m_camera.view();
        frame.proj = Mat44::perspective(proj.fovY, proj.aspect, m_camera.nearPlane, m_camera.farPlane, true);
        frame.cameraPosition = m_camera.position();
        if (tweakOn(tweaks, "Surfaces"))
            frame.surface = {sliderValue(tweaks, "SurfaceDetail"), sliderValue(tweaks, "SurfaceRelief"), 1.0f,
                             sliderValue(tweaks, "SurfaceDistance")};
        if (sun) {
            frame.shadowViewProj = sun->view.viewProj();
            frame.shadowStrength = sun->strength;
            frame.shadowTexel = sun->view.texel;
            frame.shadowSize = static_cast<float>(sun->size);
            frame.shadowSoftness = sliderValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), "ShadowSoftness");
        }
        dev.setFrameConstants(frame);
        // OpenMM2 ambient occlusion reads the depth with this projection; it
        // assumes the view fills the scene target, so not while letterboxed.
        if (!letterbox) {
            dev.setSceneProjection(frame.proj);
            dev.setSceneCamera(m_camera.transform);
            // The sun for the shafts: the key light, as bright as it is a sun
            // (none at night, as the sun shadows).
            const float sunLevel = game::sunShadowDarkness(m_env.frame);
            dev.setSceneSun(m_env.frame.lights[0].direction, m_env.frame.lights[0].color * sunLevel);
        }
        // OpenMM2 wet roads "In rain": dry unless it rains.
        if (tweakValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), "WetRoads") == 1 && m_result.config.weather != game::Weather::Rain) {
            render::PostProcess post = dev.postProcess();
            if (post.wetness > 0.0f) {
                post.wetness = 0.0f;
                const auto projection = dev.sceneProjection();
                const auto camera = dev.sceneCamera();
                const auto sunInfo = dev.sceneSun();
                const auto unfiltered = dev.unfilteredRects();
                dev.setPostProcess(post); // forgets them: set them again
                for (const render::Rect& r : unfiltered)
                    dev.addUnfilteredRect(r);
                if (projection)
                    dev.setSceneProjection(*projection);
                if (camera)
                    dev.setSceneCamera(*camera);
                if (sunInfo)
                    dev.setSceneSun(sunInfo->direction, sunInfo->color);
            }
        }
        const game::Frustum frustum(frame.view * frame.proj);
        const bool playerBody = m_flyCamera || m_cams.display() == game::CarDisplay::Body;
        // The sirens' lens flares (vehSiren::Draw, ltLensFlare) are queued
        // while the level draws and added over it afterwards.
        std::vector<game::fx::LensFlareQuad> flares;
        const Mat44 viewProj = frame.view * frame.proj;
        game::VehicleRenderer::setLensFlareTarget(&viewProj, proj.aspect, &flares);
        drawLevel(ctx, m_camera, frustum, playerBody, m_frameDt);
        game::VehicleRenderer::setLensFlareTarget(nullptr, 1.0f, nullptr);
        if (m_drawTrace && m_player)
            m_drawTrace->frame(m_frameDt, m_frameSteps, m_drawnPhys.alpha(), m_camera.transform,
                               m_drawPose.body, m_player->sim().speed(), nearestDrawnCar(ctx));
        if (!flares.empty()) {
            game::fx::drawLensFlares(dev, *m_textures, flares);
            dev.setFrameConstants(frame);
        }
        if (m_hud && m_session && m_player) {
            // The arrow, icons, stands and dash are drawn in the 3D view.
            m_hud->setViewProjection(frame.view * frame.proj);
            m_hud->setDashFrame(frame, Mat44::perspective(proj.fovY, proj.aspect, 0.01f, m_camera.farPlane, true));
            // mmDashView turns the wheel by the recorded steering (mmPlayer +0x2264).
            m_hud->drawWorld(*m_session, m_camera, shown, m_steerApplied, blips);
        }
        if (letterbox) {
            dev.setScissor(nullptr);
            dev.setViewport(
                {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)});
        }
        // mmPopup::ProcessEscape deactivates the map (the chat line does not).
        if (mapShown && !fullMap)
            m_hud->drawMap(*m_session, shown, blips, m_frameDt);
        // mmGameManager::Update declares the mirror after the dashboard and
        // the HUD map.
        drawMirror(ctx, frame);
        dev.endScene();
    }

    // The HUD's view options from the cameras' view settings (mmViewMgr):
    // the map's mode, the wide angle and the dashboard.
    void syncHudView() {
        if (!m_hud)
            return;
        auto& o = m_hud->options();
        o.mapMode = m_cams.mapMode();
        o.wideAngle = !m_flyCamera && m_cams.wideAngle();
        o.dashActive = !m_flyCamera && m_cams.dashboard();
        o.dashboard = !m_flyCamera && m_cams.display() == game::CarDisplay::Dash;
        // mmGame::UpdateGameInput: looking around from a point-of-view
        // camera disables the HUD (mmHUD::Disable), straight ahead enables
        // it again. mmPopup::ProcessEscape disables it too.
        o.visible = (m_flyCamera || m_cams.display() == game::CarDisplay::Body || m_camPan == 0.0f) &&
                    (m_popup == Popup::None || m_popup == Popup::Chat);
    }

    // The other cars for the HUD's map and icons (mmGame's OppIconInfo).
    std::vector<game::session::MapBlip> hudBlips(Context& ctx) const {
        using game::session::MapBlip;
        std::vector<MapBlip> blips;
        // mmHudMap and mmIcons follow the cars' phInertialCS matrices (OpenMM2:
        // as drawn); the icons show the opponents' places (mmSingleRace /
        // mmSingleCircuit ::UpdateScore).
        for (std::size_t i = 0; i < m_opponents.size(); ++i) {
            const auto& car = *m_opponents[i].sim;
            MapBlip b{drawnIcs(drawnPose(game::Drawn::Opponent, i, car).body, car), MapBlip::Kind::Opponent};
            if (m_session)
                b.place = m_session->opponentPlace(m_opponents[i].sessionIndex);
            blips.push_back(std::move(b));
        }
        // The network players (mmGameMulti::RegisterMapNetObjects): their
        // start slot (the place in the host's player list, NetStartArray),
        // coloured by slot, or red / blue by team in Cops and Robbers team
        // games, the gold carrier marked "$" (place 9, mmMultiCR::
        // OppStealGold); in the races their rank (mmGameMulti::UpdateScore;
        // an icon turned off, place 0, once that player has finished).
        if (multiplayer(ctx)) {
            const auto& players = ctx.netGame->players();
            for (const auto& rc : m_remoteCars) {
                if (!rc.hasState)
                    continue;
                int slot = 0, other = 0, racer = -1;
                for (std::size_t k = 0; k < players.size(); ++k) {
                    if (players[k].id == rc.id) {
                        slot = static_cast<int>(k);
                        racer = other;
                    }
                    if (players[k].id != ctx.netGame->localId())
                        ++other;
                }
                const auto drawn = m_remoteDrawn.find(rc.id);
                MapBlip b{drawn != m_remoteDrawn.end() ? drawn->second : rc.transform, MapBlip::Kind::Remote};
                b.slot = slot;
                b.name = rc.name;
                b.iconColor = game::session::hud::netIconColor(slot);
                if (m_session && racer >= 0)
                    b.place = m_session->netRacerPlace(static_cast<std::size_t>(racer));
                if (b.place == 0)
                    continue; // finished: OppIconInfo Enabled 0 (map and icon)
                if (m_cr) {
                    if (m_result.config.copsAndRobbers != game::CopsAndRobbersMode::FreeForAll)
                        b.iconColor = m_cr->teamOf(rc.id) == game::session::CrTeam::Robber ? 0xFFEF0000u
                                                                                         : 0xFF0000EFu;
                    if (m_cr->goldCarrier() == rc.id)
                        b.place = 9;
                }
                blips.push_back(std::move(b));
            }
        }
        // mmHudMap::DrawCops: the police in pursuit (aiPoliceOfficer::InPersuit:
        // state 0x977a not 0, which also holds for a wrecked, out-of-action cop).
        for (std::size_t i = 0; i < m_cops.size(); ++i) {
            const auto& car = *m_cops[i].sim;
            if (m_cops[i].driver->mode() != ai::PoliceCar::Mode::Parked)
                blips.push_back({drawnIcs(drawnPose(game::Drawn::Police, i, car).body, car),
                                 game::session::MapBlip::Kind::Police});
        }
        // OpenMM2's shared traffic: the host's police in pursuit, on a client.
        for (const auto& [id, c] : m_netCops)
            if (c.sim && (c.state.flags & net::kAmbientPursuit))
                blips.push_back({c.sim->sim().body.ics.matrix, game::session::MapBlip::Kind::Police});
        return blips;
    }

    // OPENMM2_DEBUG_DRAW_TRACE: the other car drawn nearest the player's.
    std::optional<Mat34> nearestDrawnCar(Context& ctx) const {
        std::optional<Mat34> best;
        float bestDist = 1e30f;
        auto consider = [&](const Mat34& m) {
            const float d = m.m3.dist2(m_pose.body.m3);
            if (d < bestDist) {
                bestDist = d;
                best = m;
            }
        };
        for (std::size_t i = 0; i < m_opponents.size(); ++i)
            consider(drawnPose(game::Drawn::Opponent, i, *m_opponents[i].sim).body);
        for (std::size_t i = 0; i < m_cops.size(); ++i)
            consider(drawnPose(game::Drawn::Police, i, *m_cops[i].sim).body);
        if (multiplayer(ctx))
            for (const auto& [id, transform] : m_remoteDrawn)
                consider(transform);
        return best;
    }

    // --- OpenMM2: drawing between simulation steps (game/Interpolation.h) ---
    //
    // The physics' step observer: the state of everything it moves, as it
    // stands before the sample.
    void recordStepPoses() {
        using game::Drawn;
        using game::drawnKey;
        m_drawnPhys.beginStep();
        if (m_player) {
            const std::uint32_t resets = m_player->sim().resets;
            m_drawnPhys.record(drawnKey(Drawn::Player, 0), m_player->pose(), resets);
            if (m_player->trailer())
                m_drawnPhys.record(drawnKey(Drawn::PlayerTrailer, 0), m_player->trailerPose(), resets);
        }
        for (std::size_t i = 0; i < m_opponents.size(); ++i)
            m_drawnPhys.record(drawnKey(Drawn::Opponent, i), m_opponents[i].sim->pose(),
                               m_opponents[i].sim->sim().resets);
        for (std::size_t i = 0; i < m_cops.size(); ++i)
            m_drawnPhys.record(drawnKey(Drawn::Police, i), m_cops[i].sim->pose(),
                               m_cops[i].sim->sim().resets);
        // A network car's body is placed once a frame (updateRemoteCars) and
        // its wheels follow in the samples: they are kept with the body they
        // were simulated on.
        std::map<std::uint8_t, Mat34> stepBodies;
        for (const auto& [id, rv] : m_remotes) {
            if (!rv.sim)
                continue;
            game::VehiclePose pose = rv.sim->pose();
            stepBodies[id] = pose.body;
            if (const auto it = m_remoteStepBodies.find(id);
                it != m_remoteStepBodies.end() && !rv.simulated && !rv.predicted)
                pose.body = it->second;
            const std::uint32_t resets = rv.sim->sim().resets;
            m_drawnPhys.record(drawnKey(Drawn::RemoteCar, id), pose, resets);
            if (rv.sim->trailer())
                m_drawnPhys.record(drawnKey(Drawn::RemoteTrailer, id), rv.sim->trailerPose(), resets);
        }
        m_remoteStepBodies = std::move(stepBodies);
        recordNetCopSteps(false); // a shared-traffic client's police simulated here
        if (m_netTrafficCars && m_trafficBodies)
            game::recordTrafficBodies(m_drawnPhys, *m_trafficBodies); // a shared-traffic client's
        else if (m_ai && m_trafficBodies)
            game::recordTrafficBodies(m_drawnPhys, *m_ai, *m_trafficBodies);
        if (m_bangers && m_world)
            game::recordProps(m_drawnPhys, *m_bangers, *m_world);
    }

    // A car's phInertialCS matrix from its model matrix `model` (vehCarSim::
    // SetWorldMatrix's offset taken off).
    static Mat34 drawnIcs(const Mat34& model, const game::SimVehicle& car) {
        Mat34 m = model;
        m.m3 = model.m3 - model.transformDir(car.sim().centerOfGravity);
        return m;
    }

    // A simulated car between the physics' last two samples.
    game::VehiclePose drawnPose(game::Drawn kind, std::uint64_t id, const game::SimVehicle& car) const {
        return m_drawnPhys.pose(game::drawnKey(kind, id), car.pose(), car.sim().resets);
    }

    // After the frame's steps: how far the simulations are into their next
    // steps, and the player's car as drawn (the cameras follow it).
    void updateDrawnPoses() {
        if (m_world)
            m_drawnPhys.setAlpha(m_world->interpolationAlpha());
        if (m_ai)
            m_drawnAi.setAlpha(m_ai->interpolationAlpha());
        // OpenMM2: the host draws the players' cars it simulates between
        // their last two samples, like its own.
        for (const auto& [id, rv] : m_remotes)
            if (rv.simulated && rv.placed && rv.sim)
                m_remoteDrawn[id] = drawnPose(game::Drawn::RemoteCar, id, *rv.sim).body;
        // A client: the other players' cars with what the host's states moved
        // drawn away (the ones it simulates, between their last two samples).
        for (auto& [id, rv] : m_remotes) {
            if (rv.simulated || !rv.sim)
                continue;
            updateDrawAhead(id, rv);
            rv.blend.update(m_frameDt);
            if (const auto base = remoteDrawnBase(id, rv))
                m_remoteDrawn[id] = rv.blend.apply(*base);
        }
        for (auto& [id, c] : m_netCops)
            c.blend.update(m_frameDt);
        if (!m_player)
            return;
        m_drawPose = drawnPose(game::Drawn::Player, 0, *m_player);
        m_drawTrailerPose = m_drawnPhys.pose(game::drawnKey(game::Drawn::PlayerTrailer, 0),
                                             m_player->trailerPose(), m_player->sim().resets);
        // OpenMM2: a network client draws what the host's corrections moved
        // its car by away over a few frames.
        m_correction.update(m_frameDt);
        if (const Mat34 smoothed = m_correction.apply(m_drawPose.body); m_correction.offset() > 0.0f) {
            m_drawPose = game::placePose(m_drawPose, smoothed);
            m_drawTrailerPose = game::placePose(m_drawTrailerPose,
                                                m_correction.apply(m_drawTrailerPose.body));
        }
    }

    // The level as lvlLevel::Draw draws it for one view: the city, traffic,
    // props, the cars and their effects (lvlLevel's callbacks) and the rain.
    // `playerBody` false hides the player's car; `dt` advances the shared
    // police cars' wheels (0 for a second view of the same frame).
    // OpenMM2 sun shadows (the SUN SHADOWS tweak; not in MM2): the level
    // drawn once more, from the key light, into the shadow map, as the
    // mirror draws it from its camera. nullopt when off, at night (no sun
    // above the horizon) or where the backend has no shadow map.
    struct SunShadows {
        game::SunShadowView view;
        float strength = 0;
        std::uint32_t size = 0;
    };
    std::optional<SunShadows> drawSunShadows(Context& ctx) {
        const int setting = tweakValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), "SunShadows");
        if (setting == 0 || !m_cityRenderer || m_state != State::Running)
            return std::nullopt;
        const float strength = sliderValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), "ShadowDarkness") * game::sunShadowDarkness(m_env.frame);
        if (strength <= 0.02f)
            return std::nullopt;
        game::SunShadowSettings settings;
        // High: twice the texels over a larger area (same sharpness, farther).
        settings.size = setting == 2 ? 4096u : 2048u;
        settings.radius = sliderValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), "ShadowDistance");
        settings.ahead = 0.35f;
        const game::SunShadowView view =
            game::sunShadowView(m_env.frame.lights[0].direction, m_camera.transform, settings);
        render::Device& dev = ctx.device();
        if (!dev.beginShadowPass(settings.size))
            return std::nullopt;
        render::FrameConstants f = m_env.frame;
        f.view = view.view;
        f.proj = view.proj;
        f.cameraPosition = view.eye;
        dev.setFrameConstants(f);
        const bool playerBody = m_flyCamera || m_cams.display() == game::CarDisplay::Body;
        // Every room in the sun's view, not only those the camera's room
        // sees (the CPVS): a building behind the camera or out of its sight
        // still casts its shadow into view, and the shadows must not pop as
        // the camera moves from room to room.
        const bool pvs = m_detail.usePvs;
        m_detail.usePvs = false;
        drawLevel(ctx, m_camera, game::Frustum(view.viewProj()), playerBody, 0.0f);
        m_detail.usePvs = pvs;
        dev.endShadowPass();
        return SunShadows{view, strength, settings.size};
    }

    void drawLevel(Context& ctx, const game::Camera& camera, const game::Frustum& frustum, bool playerBody,
                   float dt) {
        render::Device& dev = ctx.device();
        m_cityRenderer->draw(camera, frustum, m_env, m_detail);
        m_roadDecals.draw(dev, *m_textures);
        if (m_ai && m_aiRenderer)
            m_aiRenderer->draw(*m_ai, camera, frustum, m_result.config.timeOfDay, carLights(), m_detail.objects,
                               [this](int id) { return physicalTrafficCar(id); });
        drawRemoteCars(ctx, camera);
        drawNetCops(dt, camera);
        const bool night = m_result.config.timeOfDay == game::TimeOfDay::Night;
        if (m_bangers)
            m_bangers->draw(dev, *m_models, *m_textures, m_cards, frustum, camera,
                            {m_detail.objects, night, &m_cityRenderer->rooms(),
                             [this](std::size_t i, const Mat34& m) {
                                 return m_drawnPhys.transform(game::drawnKey(game::Drawn::Prop, i), m);
                             }});
        // The gizmos: the managers' bridges and ferries, and the train cars
        // and sailboats from their rooms; the cable cars from theirs.
        if (m_gizmos)
            m_gizmos->draw(dev, *m_models, *m_textures, frustum, camera, m_detail.objects,
                           &m_cityRenderer->rooms());
        if (m_cableCars)
            m_cableCars->draw(dev, *m_models, *m_textures, frustum, camera, m_detail.objects,
                              &m_cityRenderer->rooms());
        const bool lights = carLights();
        if (m_vehicle && playerBody) {
            m_drawPose.headlights = lights;
            m_vehicle->draw(m_drawPose, camera.transform);
        }
        if (m_trailer && (m_flyCamera || m_cams.display() == game::CarDisplay::Body || !playerBody)) {
            m_drawTrailerPose.headlights = lights;
            m_trailer->draw(m_drawTrailerPose, camera.transform);
        }
        for (std::size_t i = 0; i < m_opponents.size(); ++i) {
            const auto& o = m_opponents[i];
            game::VehiclePose pose = drawnPose(game::Drawn::Opponent, i, *o.sim);
            pose.headlights = lights;
            o.renderer->draw(pose, camera.transform);
        }
        for (std::size_t i = 0; i < m_cops.size(); ++i) {
            const auto& c = m_cops[i];
            game::VehiclePose pose = drawnPose(game::Drawn::Police, i, *c.sim);
            pose.headlights = lights;
            pose.siren = c.driver->siren();
            pose.sirenAngle = c.sirenAngle;
            c.renderer->draw(pose, camera.transform);
        }
        if (m_vehicleFx)
            m_vehicleFx->draw(dev, *m_textures, m_cards, m_skids, camera.transform);
        for (const auto& o : m_opponents)
            if (o.fx)
                o.fx->draw(dev, *m_textures, m_cards, m_skids, camera.transform);
        for (const auto& c : m_cops)
            if (c.fx)
                c.fx->draw(dev, *m_textures, m_cards, m_skids, camera.transform);
        drawNetFx(dev, camera); // OpenMM2: the network cars' damage effects
        if (m_weather && rainVisible(camera.position()))
            m_weather->draw(dev, *m_textures, m_cards, camera.transform);
    }

    // aiTrafficLightInstance is an unhit Y banger of its model's banger data
    // (Init: the pole's base + R * CG, SetMatrix keeping the Y rotation;
    // SetFourWay moves it to the room of its CG, GetPosition): a prop the
    // cars collide with and knock over into its BREAKnn parts. AiRenderer
    // draws it with its signal while it stands (BangerSet leaves it out).
    void addTrafficLightProps() {
        m_signalProps.clear();
        if (!m_ai || !m_bangers)
            return;
        for (const ai::Signal& s : m_ai->signals()) {
            game::bangers::PlacedProp p;
            p.model = s.model;
            p.transform = s.transform;
            p.room = m_cityLevel ? m_cityLevel->findRoom(s.position(), 0) : 0;
            p.ownerDrawn = true;
            m_signalProps.push_back(m_bangers->addOne(p));
        }
        if (m_aiRenderer)
            m_aiRenderer->setSignalFrames([this](int i) -> std::optional<Mat34> {
                const auto& signals = m_ai->signals();
                const auto k = static_cast<std::size_t>(i);
                if (k >= m_signalProps.size() || !m_signalProps[k] || !m_bangers)
                    return k < signals.size() ? std::optional<Mat34>(signals[k].frame()) : std::nullopt;
                const std::size_t prop = *m_signalProps[k];
                if (!m_bangers->standing(prop))
                    return std::nullopt;
                return m_bangers->instances()[prop].matrix;
            });
    }

    // A traffic car TrafficBodies holds: where it is, and its wheels while
    // it has a body (aiVehicleInstance::Draw), between the physics' last two
    // samples.
    std::optional<game::AiRenderer::PhysicalCar> physicalTrafficCar(int id) const {
        // OpenMM2: a shared-traffic client's knocked cars, as the host has them
        // (or, until the host's messages lead, as this machine knocked them).
        if (m_trafficClient && !(m_netTrafficCars && m_netTrafficCars->simulated(id))) {
            const auto it = m_netPhysical.find(id);
            return it != m_netPhysical.end() ? std::optional(it->second) : std::nullopt;
        }
        const Mat34* m = m_trafficBodies ? m_trafficBodies->transformOf(id) : nullptr;
        if (!m)
            return std::nullopt;
        game::AiRenderer::PhysicalCar car;
        car.transform = *m;
        if (const auto pose = game::trafficBodyPose(*m_trafficBodies, id)) {
            const auto key = game::drawnKey(game::Drawn::TrafficCar, static_cast<std::uint64_t>(id));
            const game::VehiclePose drawn = m_drawnPhys.pose(key, *pose);
            car.active = true;
            car.transform = drawn.body;
            car.wheels = drawn.wheelWorld;
            car.wheelValid = drawn.wheelValid;
        }
        return car;
    }

    // mmMirror::Cull: the rear-view mirror's inset at the top right, cleared
    // to black, the level drawn from the mirror's frame on the player's car
    // with its fixed-aspect projection, the winding swapped (the frame is
    // mirrored) and the player's car hidden.
    void drawMirror(Context& ctx, const render::FrameConstants& mainFrame) {
        if (!m_mirror.enabled() || !m_player || !m_vehicle || m_flyCamera)
            return;
        render::Device& dev = ctx.device();
        const auto extent = dev.sceneExtent();
        const auto inset = m_mirror.viewport(static_cast<int>(extent.width), static_cast<int>(extent.height));
        if (inset.width <= 0 || inset.height <= 0)
            return;
        const auto& params = m_mirror.params();
        game::Camera camera;
        camera.transform = m_mirror.worldMatrix(m_drawPose.body);
        camera.nearPlane = params.nearClip;
        camera.farPlane = params.farClip;
        render::FrameConstants frame = mainFrame;
        frame.view = camera.view();
        frame.proj = Mat44::perspective(params.fov * 0.017453292f, params.aspect, camera.nearPlane,
                                        camera.farPlane, true);
        frame.cameraPosition = camera.position();
        const render::Rect rect{inset.x, inset.y, static_cast<std::uint32_t>(inset.width),
                                static_cast<std::uint32_t>(inset.height)};
        dev.setViewport({static_cast<float>(inset.x), static_cast<float>(inset.y), static_cast<float>(inset.width),
                         static_cast<float>(inset.height)});
        dev.setScissor(&rect);
        render::ClearValues clear;
        clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        dev.clear(clear);
        dev.setFrameConstants(frame);
        dev.setFrontFaceFlipped(true);
        drawLevel(ctx, camera, game::Frustum(frame.view * frame.proj), false, 0.0f);
        dev.setFrontFaceFlipped(false);
        dev.setScissor(nullptr);
        dev.setViewport({0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)});
        dev.setFrameConstants(mainFrame);
    }

    // OpenMM2: the F9 look switch, as the cars read it (their lighting and
    // shine are set up once; the rest is read every frame).
    void applyTweaksToCars(Context& ctx) {
        for (game::VehicleRenderer* r : {m_vehicle.get(), m_trailer.get()})
            if (r)
                setupVehicleRenderer(ctx, *r);
        for (auto& o : m_opponents)
            if (o.renderer)
                setupVehicleRenderer(ctx, *o.renderer);
        for (auto& c : m_cops)
            if (c.renderer)
                setupVehicleRenderer(ctx, *c.renderer);
        if (m_aiRenderer) {
            const IniFile& tweaks = drawnTweaks(ctx.settings.ini, ctx.originalLook);
            m_aiRenderer->setEnhancedCarLighting(tweakOn(tweaks, "EnhancedCarLighting"));
            m_aiRenderer->setCarShine(sliderValue(tweaks, "OtherCarShine"));
        }
    }
    // Which look F9 switched to, for two seconds at the top of the screen.
    void drawLookMessage(Context& ctx) {
        if (m_lookMessage <= 0.0f)
            return;
        auto& ov = *ctx.overlay;
        ov.begin(ctx.display.uiScale);
        const ui::FontSpec font{"Arial Bold", 16, 16, 0, 400};
        const std::string_view text = ctx.originalLook ? "ORIGINAL LOOK" : "REMASTERED LOOK";
        const float alpha = std::min(1.0f, m_lookMessage * 2.0f);
        const auto a = static_cast<std::uint8_t>(alpha * 255.0f);
        const float w = m_text.measure(ov, font, text);
        ov.rect(320 - w * 0.5f - 14, 40, w + 28, 28, render::packColor(13, 15, 24, static_cast<std::uint8_t>(alpha * 220.0f)));
        ov.rect(320 - w * 0.5f - 14, 40, w + 28, 2, render::packColor(255, 168, 38, a));
        m_text.draw(ov, font, text, 320 - w * 0.5f, 46, render::packColor(234, 238, 246, a));
        ov.end();
    }

    void drawOverlay(Context& ctx) override {
        drawOverlayContent(ctx);
        if (m_state == State::Running)
            drawLookMessage(ctx);
    }
    void drawOverlayContent(Context& ctx) {
        if (m_state == State::Running && m_hud && m_session && m_player && !m_showDebugOnly) {
            m_hud->drawOverlay(*ctx.overlay, m_text, m_ui, *m_session, m_playerState);
            if (m_popup != Popup::None)
                drawPopup(ctx);
            return;
        }
        if (m_state != State::Running) {
            auto& ov = *ctx.overlay;
            ov.begin(ctx.display.uiScale);
            ui::drawImage(ov, m_ui.get(m_loadingImage), 0, 0, 640, 480);
            // ProgressCB in the race phase: a flat bar at (0.55 W, 0.896 H),
            // 0.02 H tall, percent x 0.01 x 0.4234375 W long (each value
            // truncated), in 0xFF0D2CBA as a 16-bit surface keeps it
            // (ProgressRect; a 32-bit one would get white).
            if (m_loadPercent > 0) {
                constexpr float w = 640.0f, h = 480.0f;
                const float x = std::trunc(w * 0.55f), y = std::trunc(h * 0.896f), bh = std::trunc(h * 0.02f);
                const float bw = std::trunc(static_cast<float>(m_loadPercent) * 0.01f * (w * 0.4234375f));
                ov.rect(x, y, bw, bh, render::packColor(8, 44, 184));
            }
            ov.end();
        }
    }

private:
    enum class State { ShowLoading, Load, Running };

    // The race loads over several frames so that the loading picture can show
    // its progress bar between the parts (lvlProgress::UpdateTask ->
    // ProgressCB): mmGame::Init's 10, cityLevel::Load's steps up to 100,
    // then aiMap::Init's own run from 10 to 100. The values OpenMM2 shows
    // after each of its parts are inferred from where MM2's would stand.
    void loadStep(Context& ctx) {
        switch (m_loadStep++) {
        case 0: loadCityPart(ctx); return;
        case 1: loadLevelPart(ctx); return;
        case 2:
            createSession(ctx);
            loadVehicle(ctx); // places the camera behind the car
            m_loadPercent = 100;
            return;
        case 3:
            loadWorldObjects(ctx);
            loadAi(ctx);
            m_loadPercent = 50;
            return;
        case 4:
            loadEffects(ctx);
            loadPedestrianProps(ctx);
            // mmGame::Init: aiMap::Reset right after aiMap::Init (its
            // population waits for the first step, except where the cruise
            // start needs it: placeRespawnStart).
            if (m_ai)
                m_ai->reset();
            spawnOpponents(ctx);
            spawnPolice(ctx);
            // Then the mode's InitOtherPlayers.
            placeRespawnStart(ctx);
            m_loadPercent = 100;
            return;
        default:
            if (debugLoadStalled()) {
                --m_loadStep; // this step again next frame
                return;
            }
            loadFinish(ctx);
            return;
        }
    }

    // Development aid: OPENMM2_DEBUG_LOAD_DELAY_MS=<ms> keeps the loading
    // screen up until that long after the loading began (the frames go on,
    // and a network race keeps its session serviced), to try a slow loader.
    bool debugLoadStalled() const {
        static const long long delayMs = [] {
            const char* env = std::getenv("OPENMM2_DEBUG_LOAD_DELAY_MS");
            return env ? str::parseInt(env).value_or(0) : 0;
        }();
        if (delayMs <= 0)
            return false;
        const auto elapsed = std::chrono::steady_clock::now() - m_loadStart;
        return elapsed < std::chrono::milliseconds(delayMs);
    }

    void loadCityPart(Context& ctx) {
        m_loadStart = std::chrono::steady_clock::now();
        std::string error;
        auto city = city::loadCity(ctx.game->vfs, m_result.config.city, &error);
        if (!city) {
            log::error("race: cannot load city '{}': {}", m_result.config.city, error);
            // Back to the menus; the host of a network race takes everyone
            // back to the lobby, which could otherwise never start another.
            if (multiplayer(ctx)) {
                ctx.netGame->addNotice(std::format("Cannot load the city '{}': {}", m_result.config.city, error));
                if (!ctx.netGame->isHost())
                    ctx.netGame->sendLeftRace(); // the others stop waiting for this machine
            }
            leaveRace(ctx, m_result);
            return;
        }
        for (const auto& w : city->warnings)
            log::debug("city: {}", w);
        m_city = std::make_unique<city::CityData>(std::move(*city));
        m_vfs = &ctx.game->vfs;
        m_textures = std::make_unique<game::TextureLibrary>(ctx.device(), ctx.game->vfs);
        m_models = std::make_unique<game::ModelLibrary>(ctx.device(), ctx.game->vfs);
        m_bangerData = std::make_unique<game::bangers::BangerDataLibrary>(ctx.game->vfs);
        // mmMultiCircuit::Init: the concrete barricades are 26 times as heavy
        // and as hard to knock loose in a multiplayer circuit.
        if (multiplayer(ctx) && m_result.config.mode == game::GameMode::Circuit) {
            m_bangerData->scaleMass("sp_barricadeconcl_f", 26.0f);
            m_bangerData->scaleMass("sp_barricadeconcr_f", 26.0f);
        }
        m_loadPercent = 30;
    }

    void loadLevelPart(Context& ctx) {
        // cityLevel::Load: gfxTexReduceSize = 32 << the Texture Quality
        // option (gfxTextureQuality) while the city loads, no limit after.
        const int textureQuality =
            static_cast<int>(std::clamp(ctx.settings.ini.getInt("Graphics", "TextureQuality", 2), 0LL, 3LL));
        m_textures->setSizeLimit(32 << textureQuality);
        m_cityRenderer = std::make_unique<game::CityRenderer>(ctx.device(), *m_textures, *m_models, *m_city,
                                                              [this](std::string_view n) { return m_bangerData->has(n); });
        m_textures->setSizeLimit(0);
        m_objectDetail = std::clamp(static_cast<int>(ctx.settings.ini.getInt("Graphics", "ObjectDetail", 3)), 0, 3);
        m_detail.objects = game::ObjectDetail::forLevel(m_objectDetail);
        // Development aid for screenshots: "<timeOfDay 0-3>,<weather 0-3>".
        if (const char* env = std::getenv("OPENMM2_DEBUG_ENV"); env && std::strlen(env) >= 3) {
            m_result.config.timeOfDay = static_cast<game::TimeOfDay>(std::clamp(env[0] - '0', 0, 3));
            m_result.config.weather = static_cast<game::Weather>(std::clamp(env[2] - '0', 0, 4));
        }
        {
            // mmGame::SetLevelGraphics: cityLevel::sm_LightQuality is the
            // LIGHTING QUALITY option (0-3); the far clip is the FAR CLIP
            // option in metres (PUGraphics::SetFarClip, mmGame::FarClipCB).
            // The keys and defaults are the Graphics options page's.
            m_envOptions.lightQuality =
                static_cast<int>(std::clamp(ctx.settings.ini.getInt("Graphics", "LightingQuality", 3), 0LL, 3LL));
            m_envOptions.farClip = static_cast<float>(
                std::clamp(ctx.settings.ini.getDouble("Graphics", "FarClip", 1000.0), 100.0, 1000.0));
            // mmGame::SetLevelGraphics: vglCloudMapEnable by CLOUD SHADOWS.
            const auto clouds = ctx.settings.ini.getInt("Graphics", "CloudShadows", 2);
            m_envOptions.cloudShadows = static_cast<int>(std::clamp(clouds, 0LL, 2LL));
            // mmGame::SetLevelGraphics: cityLevel::EnableSky(TEXTURED SKY).
            m_envOptions.texturedSky = ctx.settings.ini.getBool("Graphics", "TexturedSky", true);
        }
        applyEnvironment();
        m_position = m_city->psdl.sphereCenter + Vec3{0, 3, 0};
        m_yaw = 0.0f;
        m_pitch = -0.15f;
        m_controlOptions = controls::Options::load(ctx.settings.ini);
        m_gameInput.load(ctx.settings.ini, controls::readJoystick(ctx.input, m_controlOptions.controller,
                                                                  m_controlOptions.deadZone));
        // mmPlayer::Init: the force feedback's switches and the road wave.
        m_ff.configure(m_gameInput.controller(), m_controlOptions);
        m_ff.setDevice(ctx.input.forceFeedback(m_gameInput.controller() == controls::Controller::GamePad));
        m_ff.start();
        m_loadPercent = 70;
    }

    void loadFinish(Context& ctx) {
        // Every vehCar::Init builds a vehSiren, whose constructor sets the
        // light glow scales to 0.2 / 0.6; aiMap::Init ends with
        // aiVehicleManager::Init, which sets 0.2 / 0.95 (network cars set up
        // later bring the 0.6 back, see drawRemoteCars).
        game::VehicleRenderer::setLightGlowScales(0.2f, 0.95f);
        m_hud = std::make_unique<game::session::Hud>(ctx.device(), *m_textures, *m_models, ctx.game->vfs,
                                                     ctx.game->strings, m_result.config.city, m_result.config.vehicle);
        m_hud->options().metric = ctx.settings.metricUnits;
        m_hud->options().uiScale = ctx.display.uiScale;
        loadViewSettings(ctx);
        // mmSingleBlitz::InitHUD turns the icons on (iconState = 1).
        if (m_result.config.mode == game::GameMode::Blitz)
            m_hud->options().opponentIcons = true;
        m_hud->preload(&m_ui);
        if (m_session) {
            phys::setElasticityCap(phys::kElasticityCap); // mmGame::Reset
            m_session->start();
            if (m_player)
                m_player->sim().damage.enabled = m_session->playerDamageEnabled();
            setupCopsAndRobbers(ctx);
            beginNetRules(ctx);
            // mmPlayer::SetPreRaceCam (every single-player mode but cruise).
            if (m_result.config.mode != game::GameMode::Cruise && !multiplayer(ctx))
                m_cams.startPreRace();
        }
        // A network race's start begins with its first frame (state 0), with
        // or without race rules: it reports this machine loaded.
        if (multiplayer(ctx))
            m_netStart.emplace(game::NetRaceStart::kindOf(m_result.config.mode));
        // OpenMM2: every player's car takes its input once a sample, on the
        // host from its player's (game/net/PlayerCars).
        if (multiplayer(ctx) && m_player && m_world) {
            m_netDriver.attach(*m_player);
            m_netAutomatic = m_result.config.automatic;
            m_player->sim().ownRandom = true;
            m_player->sim().randomState = 1;
            m_world->setSampleHooks([this] { beforeNetSample(); }, [this] { afterNetSample(); });
        }
        // -nomusic: mmGameMusicData::Load loads neither the song nor the
        // city's ambience segment, so the race plays neither.
        if (auto* music = ctx.music(); music && !ctx.commandLine.noMusic) {
            // The song is chosen now; MusicDirector starts it 1.25 s in. Cops
            // and Robbers plays the cruise music too (mmMultiCR's music data
            // is mmSingleRoamMusicData: "singleroam").
            const bool cruise = m_result.config.mode == game::GameMode::Cruise ||
                                m_result.config.mode == game::GameMode::CopsAndRobbers;
            music->startRace(-1, cruise, false);
            music->setAmbience(m_result.config.city);
            m_musicDirector = std::make_unique<audio::MusicDirector>(cruise);
        }
        // Development aid for reproducible screenshots: "x,y,z,yaw,pitch".
        if (const char* cam = std::getenv("OPENMM2_DEBUG_CAMERA")) {
            const auto parts = str::split(cam, ',');
            if (parts.size() == 5) {
                auto f = [&](int i) { return static_cast<float>(str::parseDouble(parts[i]).value_or(0.0)); };
                m_position = {f(0), f(1), f(2)};
                m_yaw = f(3);
                m_pitch = f(4);
            }
        }
        m_state = State::Running;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_loadStart).count();
        log::info("race: loaded {} in {:.2f} s", m_result.config.city, seconds);
    }

    // Spawn point and heading. Until race starts are wired up, cruise uses
    // the first waypoint of the city's first Blitz race.
    std::pair<Vec3, float> spawnPoint(Context& ctx) const {
        for (const auto& race : m_city->races) {
            if (race.waypoints.empty())
                continue;
            auto bytes = ctx.game->vfs.readAll(race.waypoints);
            if (!bytes)
                continue;
            const auto csv = data::CsvTable::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            if (csv.rows().size() < 2)
                continue;
            const Vec3 a{csv.cellFloat(0, 0), csv.cellFloat(0, 1), csv.cellFloat(0, 2)};
            const Vec3 b{csv.cellFloat(1, 0), csv.cellFloat(1, 1), csv.cellFloat(1, 2)};
            return {a, std::atan2(-(b.x - a.x), -(b.z - a.z))};
        }
        return {m_city->psdl.sphereCenter, 0.0f};
    }

    void loadVehicle(Context& ctx) {
        // Physics world: the city as the collision manager sees it (rooms,
        // collision polygons, collidable objects; materials), and the probe
        // geometry the wheels use.
        m_cityLevel = std::make_unique<game::CityLevel>(*m_city, ctx.game->vfs,
                                                        [this](std::string_view n) { return m_bangerData->has(n); });
        m_world = std::make_unique<phys::World>(m_cityLevel->takeMaterials());
        m_world->setStepObserver([this] { recordStepPoses(); });
        m_world->setStatic(m_cityLevel->takeProbeSoup());
        m_world->setLevel(m_cityLevel.get());

        std::string error;
        // mmPlayer::Init: no trailer in multiplayer cruise or Cops and Robbers.
        const auto mode = m_result.config.mode;
        const bool withTrailer =
            !(multiplayer(ctx) && (mode == game::GameMode::Cruise || mode == game::GameMode::CopsAndRobbers));
        m_player = game::SimVehicle::loadPlayer(ctx.game->vfs, m_result.config.vehicle, &error, withTrailer);
        if (!m_player) {
            log::error("race: vehicle '{}': {}", m_result.config.vehicle, error);
            // A network race without the player's car cannot be driven or
            // seen by the others: back to the lobby (the host takes everyone).
            if (multiplayer(ctx)) {
                ctx.netGame->addNotice(std::format("Cannot load the car '{}': {}", m_result.config.vehicle, error));
                if (!ctx.netGame->isHost())
                    ctx.netGame->sendLeftRace();
                leaveRace(ctx, m_result);
            }
            return;
        }
        m_player->sim().options.player = true; // mmPlayer::Update's input overrides
        // mmGame::Init: vehTransmission::Automatic with the player's
        // transmission choice; the AUTO REVERSE option (mmInput +0x18C).
        m_player->sim().trans.automatic(m_result.config.automatic);
        m_player->controls().autoReverse = m_controlOptions.autoReverse;
        // The player's car collides with its polygonal bound (vehCar::Init
        // with vehBound) and marks what it hits (dgPhysManager's PlayerInst):
        // mmGame::Update declares it each frame as the type-4 mover, whose
        // room and neighbours keep knocked-over props simulated.
        m_player->sim().setPolygonalBound(true);
        m_player->sim().body.declare(4, 0x1b);
        if (auto* trailer = m_player->trailer())
            trailer->body.declare(2, 0x1b); // mmGame::Update: the trailer as type 2, 0x1b
        m_player->sim().options.weatherFriction = weatherFriction();
        m_vehicle = std::make_unique<game::VehicleRenderer>(ctx.device(), *m_textures, *m_models, m_player->model(),
                                                            m_result.config.vehicleColor);
        setupVehicleRenderer(ctx, *m_vehicle);
        if (const auto* trailer = m_player->trailerModel()) {
            m_trailer = std::make_unique<game::VehicleRenderer>(ctx.device(), *m_textures, *m_models, *trailer,
                                                                m_result.config.vehicleColor, "TRAILER", "TWHL");
            setupVehicleRenderer(ctx, *m_trailer);
        }
        // The mode's place for the car (game::session::RaceSetup::playerPlace,
        // the modes' InitGameObjects / InitNetworkPlayers) and how it settles
        // (playerDrop): the race modes' InitOtherPlayers settle it 0.9 m above
        // the road, cruise keeps RespawnXYZ's point, the multiplayer grids go
        // through mmGame::FindGroundPos first.
        auto [pos, heading] = spawnPoint(ctx);
        auto drop = game::session::StartDrop::FindGround;
        if (m_session) {
            const auto& setup = m_session->setup();
            pos = setup.playerPlace.position;
            heading = setup.playerPlace.angle;
            drop = setup.playerDrop;
            // mmMultiBlitz / mmMultiCircuit / mmMultiRace::InitNetworkPlayers:
            // the player's start slot on the grid behind the start
            // (mmGameMulti::StartXYZ). The slot is NetStartArray's, which
            // mmInterface::SendStartMsg fills in the order the host
            // enumerates the players: here the place in the host's player
            // list (join order, inferred from DirectPlay's enumeration).
            const auto raceMode = m_result.config.mode;
            if (multiplayer(ctx) && (raceMode == game::GameMode::Blitz || raceMode == game::GameMode::Circuit ||
                                     raceMode == game::GameMode::Checkpoint)) {
                const bool longVehicle = m_player->sim().body.radius() > 6.0f || m_player->trailerModel();
                pos += Mat34::rotationY(heading).transformDir(
                    game::session::multiplayerGridOffset(startSlot(ctx), longVehicle));
            }
        }
        // Development aid: OPENMM2_DEBUG_SPAWN="x,y,z,heading" (dropped onto
        // the ground below it).
        if (const char* sp = std::getenv("OPENMM2_DEBUG_SPAWN")) {
            const auto parts = str::split(sp, ',');
            if (parts.size() == 4) {
                auto f = [&](int i) { return static_cast<float>(str::parseDouble(parts[i]).value_or(0.0)); };
                pos = {f(0), f(1), f(2)};
                heading = f(3);
                drop = game::session::StartDrop::FindGround;
            }
        }
        if (drop == game::session::StartDrop::FindGround)
            pos = game::session::findGroundPos(pos, groundProbe());
        m_player->addTo(*m_world);
        // vehCarSim::SetResetPos at the start, the reset rotation, vehCar::Reset.
        m_player->setResetPos(pos, heading);
        m_player->reset();
        // mmGame::InitOtherPlayers: the player's start dropped onto the road,
        // 0.9 m up (probed from the body's centre).
        if (drop == game::session::StartDrop::OnGround)
            m_player->settleOnGround(*m_world, m_player->sim().body.ics.matrix.m3);
        m_pose = m_player->pose();
        std::vector<std::string> missing;
        // mmPlayer::Init: the dashboard eye depends on the screen's shape.
        float aspect = 4.0f / 3.0f;
        if (const auto extent = ctx.device().sceneExtent(); extent.height)
            aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        m_cams.load(ctx.game->vfs, m_result.config.vehicle, &missing, aspect);
        // mmMirror::Init: the defaults, then tune/<car>.mmmirror.
        m_mirror.load(ctx.game->vfs, m_result.config.vehicle);
        // mmPlayer::Init: the player node, named after the car, loads
        // tune/<car>.asnode (its steering tuning, mmPlayer::FileIO).
        if (auto bytes = ctx.game->vfs.readAll("tune/" + str::lower(m_result.config.vehicle) + ".asnode")) {
            const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            if (auto f = data::parseDat(text); f && f->top())
                m_gameInput.setPlayerTune(*f->top());
        }
        if (const auto* info = ctx.game->catalog.vehicle(m_result.config.vehicle))
            m_cams.setVehicleFlags(static_cast<int>(info->flags));
        for (const auto& m : missing)
            log::debug("race: camera file {} missing (engine defaults)", m);
        // mmPlayerConfig::SetViewSettings: the driver's camera, wide angle and
        // dashboard before mmPlayer::Init and Reset read them; mmViewMgr::Init
        // leaves the mirror on only when the driver had it on (off for a new
        // driver).
        loadProfile();
        m_mirror.setEnabled(m_profile && m_profile->mirror);
        if (m_profile)
            m_cams.setViewSettings({m_profile->camera, m_profile->wideAngle, m_profile->dashboard});
        m_cams.reset(cameraTarget());
        loadAudio(ctx);
        m_position = pos + Mat34::rotationY(heading).transformDir({0, 2.2f, 7.0f});
        m_yaw = heading;
    }

    // mmSingleRoam / mmGameMulti::InitOtherPlayers (cruise, Cops and
    // Robbers): the car moves from its InitGameObjects place to
    // mmGame::RespawnXYZ's start, SetResetPos and vehCar::Reset, and stays
    // there 2 m above the intersection. The single-player start is drawn from
    // MM2's one random stream (m_random) as mmGame::Init's aiMap::Reset left
    // it: set to 1, then the traffic and pedestrians placed round the car's
    // InitGameObjects place (ai::World::resetAndPopulate). mmGame::Reset's
    // aiMap::Reset then sets it to 1 again, and the first step places them
    // round the start.
    void placeRespawnStart(Context& ctx) {
        if (!m_session || !m_player || !m_session->setup().respawnStart || std::getenv("OPENMM2_DEBUG_SPAWN"))
            return;
        std::uint32_t seed = 1;
        if (m_ai && !multiplayer(ctx)) {
            m_ai->resetAndPopulate(m_player->sim().resetPos());
            seed = m_random.state();
        }
        const auto pick = m_session->placeRespawnStart(*m_city, seed, [this](const Vec3& p) {
            return m_cityRenderer ? m_cityRenderer->roomAt(p) : 0;
        });
        if (m_ai)
            m_ai->reset(); // mmGame::Reset (mmGameManager::Reset)
        if (!pick)
            return;
        log::info("race: start at intersection {} ({:.3f}, {:.3f}, {:.3f})", pick->intersection,
                  pick->position.x, pick->position.y, pick->position.z);
        Vec3 position = pick->position;
        float angle = pick->angle;
        // Development aid: OPENMM2_DEBUG_START_NEAR_POLICE=<post>:<metres>
        // starts the car that far in front of a police post's car, facing it
        // (every machine knows the posts; a network cruise's damage tests).
        if (const char* near = std::getenv("OPENMM2_DEBUG_START_NEAR_POLICE")) {
            const auto parts = str::split(near, ':');
            const auto post = static_cast<std::size_t>(str::parseInt(parts[0]).value_or(0));
            const float metres =
                parts.size() > 1 ? static_cast<float>(str::parseDouble(parts[1]).value_or(20.0)) : 20.0f;
            if (post < m_session->police().size()) {
                const Mat34& spawn = m_session->police()[post].spawn;
                position = spawn.m3 - spawn.m2 * metres;
                angle = phys::resetRotationOf(spawn) + 3.1415927f;
            }
        }
        // ... and OPENMM2_DEBUG_START=<x>,<y>,<z>,<angle> at that place.
        if (const char* at = std::getenv("OPENMM2_DEBUG_START")) {
            const auto v = str::split(at, ',');
            auto f = [&](std::size_t i) {
                return i < v.size() ? static_cast<float>(str::parseDouble(v[i]).value_or(0.0)) : 0.0f;
            };
            position = {f(0), f(1), f(2)};
            angle = f(3);
        }
        log::info("race: start angle {:.4f}", angle);
        m_player->setResetPos(position, angle);
        m_player->reset();
        m_pose = m_player->pose();
        m_cams.reset(cameraTarget());
        m_position = position + Mat34::rotationY(angle).transformDir({0, 2.2f, 7.0f});
        m_yaw = angle;
    }

    void createSession(Context& ctx) {
        std::string error;
        game::session::SessionOptions opts;
        if (const auto* info = ctx.game->catalog.vehicle(m_result.config.vehicle))
            opts.scoringBias = info->scoringBias;
        opts.playerName = ctx.settings.playerName;
        opts.netHost = multiplayer(ctx) && ctx.netGame->isHost();
        // OpenMM2: the host decides the rules (game/net/NetRules).
        opts.netRules = multiplayer(ctx);
        // mmMultiRoam / mmMultiCR: RespawnXYZ draws the start from a random
        // stream seeded with the player's id (MM2: its DirectPlay id), so
        // every player starts elsewhere.
        if (multiplayer(ctx))
            opts.seed = 1u + ctx.netGame->localId();
        // Crash course pursuit checks look through the level (the world is
        // built after the session).
        opts.lineOfSight = [this](const Vec3& a, const Vec3& b) {
            phys::RayHit hit;
            return !m_world || !m_world->probe(a, b, hit);
        };
        m_session = game::session::Session::create(m_result.config, *m_city, ctx.game->vfs, ctx.game->strings, &error,
                                                  opts);
        if (!m_session)
            log::warn("race: no race rules: {}", error);
    }

    // Loads an AI-driven car at `spawn`: aiRouteRacer::Init (its route's
    // first point) and aiPoliceOfficer::Reset (its post) set the reset
    // position and rotation there and vehCar::Reset places it (racers are
    // then settled onto the road, see spawnOpponents; police are not).
    std::unique_ptr<game::SimVehicle> loadAiCar(Context& ctx, const std::string& vehicle, std::string_view tune,
                                                const Mat34& spawn) {
        std::string error;
        auto car = game::SimVehicle::load(ctx.game->vfs, vehicle, &error, tune);
        if (!car) {
            log::warn("race: AI car {}: {}", vehicle, error);
            return nullptr;
        }
        car->sim().options.weatherFriction = weatherFriction();
        // vehCarModel::InitBound builds a model's bound once, and the
        // player's car is set up first: AI cars of the player's model collide
        // with its polygonal bound too.
        if (vehicle == m_result.config.vehicle)
            car->sim().setPolygonalBound(true);
        car->addTo(*m_world);
        car->setResetPos(spawn);
        car->reset();
        return car;
    }

    // NetStartArray::GetIndex for the local player: its place in the host's
    // player list (mmInterface::SendStartMsg assigns the slots in that order).
    int startSlot(Context& ctx) const {
        const auto& players = ctx.netGame->players();
        for (std::size_t i = 0; i < players.size(); ++i)
            if (players[i].id == ctx.netGame->localId())
                return static_cast<int>(i);
        return 0; // GetIndex's answer for an unknown player
    }

    // dgPhysManager::Collide with the wheels' mask (0x20): the level and the
    // objects the wheels collide with.
    game::session::GroundProbe groundProbe() const {
        return [this](const Vec3& from, const Vec3& to) -> std::optional<Vec3> {
            phys::RayHit hit;
            if (!m_world || !m_world->wheelProbe(from, to, hit, nullptr, nullptr))
                return std::nullopt;
            return hit.position;
        };
    }


    // lvlRoomInfo's flags of the room `p` is in (city::LevelRoomFlag), not
    // the PSDL's room flags.
    int levelRoomFlagsAt(const Vec3& p) const {
        if (!m_cityRenderer)
            return 0;
        const int room = m_cityRenderer->roomAt(p);
        if (room <= 0 || static_cast<std::size_t>(room) >= m_city->levelRoomFlags.size())
            return 0;
        return m_city->levelRoomFlags[static_cast<std::size_t>(room)];
    }

    // The water level under `p` if it lies in a Water of Death room
    // (lvlRoomInfo flag 4: a deepwater first texture, or listed in
    // city/<map>.water) and cityLevel::GetWaterLevel, the .water file's
    // level for every room.
    std::optional<float> waterLevelAt(const Vec3& p) const {
        if (!m_city->water || !(levelRoomFlagsAt(p) & city::LevelRoomFlag::WaterOfDeath))
            return std::nullopt;
        return m_city->water->height;
    }

    // mmGame::InitWeather: the tyres' WeatherFriction is 0.8 in rain, 0.75 in
    // rain at night, 1 otherwise.
    float weatherFriction() const {
        if (m_result.config.weather != game::Weather::Rain)
            return 1.0f;
        return m_result.config.timeOfDay == game::TimeOfDay::Night ? 0.75f : 0.8f;
    }

    std::unique_ptr<audio::game::OpponentCarAudio> loadAiCarAudio(Context& ctx, const std::string& vehicle,
                                                                   bool police) {
        if (!m_bank || !ctx.mixer)
            return nullptr;
        audio::game::CarAudioOptions opts;
        opts.city = m_result.config.city;
        opts.weather = surfaceWeather();
        opts.manager = &m_audioSlots;
        auto audio = std::make_unique<audio::game::OpponentCarAudio>();
        std::string error;
        if (!audio->load(ctx.game->vfs, *m_bank, *ctx.mixer, vehicle, police, opts, &error)) {
            log::debug("race: AI car audio {}: {}", vehicle, error);
            return nullptr;
        }
        return audio;
    }

    // Racers from the session (aiVehicleOpponent), driven by ai::Opponent.
    void spawnOpponents(Context& ctx) {
        if (!m_session || !m_world)
            return;
        const auto& setups = m_session->opponents();
        // Each racer's vehCar::Init drew on MM2's global stream in turn
        // (loadAi): its siren flares come from there.
        game::fx::Rand initDraws(m_racerInitState);
        for (std::size_t i = 0; i < setups.size(); ++i) {
            const auto& s = setups[i];
            const std::uint32_t flares = game::takeVehCarInitDraws(initDraws);
            Opponent opp;
            opp.sessionIndex = i;
            // aiRouteRacer::Init places the racer on its first .opp row.
            Mat34 spawn = s.spawn;
            // aiVehiclePhysics::Init: vehCar::Init(<car>), the car's own tune
            // (the retail *_opp.vehCarSim files are not used by MM2).
            opp.sim = loadAiCar(ctx, s.vehicle, {}, spawn);
            if (!opp.sim)
                continue;
            // mmGame::CollideAIOpponents (from InitOtherPlayers): the racer's
            // start dropped onto the road below its model origin, 0.9 m up.
            if (!multiplayer(ctx) && m_result.config.mode != game::GameMode::Cruise)
                opp.sim->settleOnGround(*m_world, opp.sim->sim().modelMatrix().m3);
            opp.spawn = spawn;
            // aiVehiclePhysics::Init: vehCar::Init's paint job is the racer's
            // id (its index) & 3.
            opp.renderer = std::make_unique<game::VehicleRenderer>(ctx.device(), *m_textures, *m_models,
                                                                    opp.sim->model(), static_cast<int>(i) & 3);
            opp.renderer->setSirenFlares(flares);
            setupVehicleRenderer(ctx, *opp.renderer);
            opp.audio = loadAiCarAudio(ctx, s.vehicle, false);
            opp.fx = loadVehicleFx(ctx, s.vehicle, opp.sim->model(), *opp.renderer);
            opp.sim->sim().onImpactCallback = [fx = opp.fx.get(), sim = &opp.sim->sim(),
                                                sounds = opp.impacts](const phys::CarImpact& impact) {
                if (impact.sound)
                    sounds->push_back({impact.soundStrength, impact.audioId, impact.position});
                fx->impact(impact, *sim);
            };
            if (m_ai) {
                std::string error;
                opp.driver = ai::Opponent::create(m_ai->map(), opp.sim->sim(), s.path, s.params,
                                                  m_session->laps(), 1 + static_cast<int>(i),
                                                  static_cast<int>(i), &error, m_world.get(), s.vehicle);
                if (opp.driver)
                    opp.driver->setResetCar([&v = *opp.sim, a = opp.audio.get()](const Mat34& m) {
                        v.reset(m);
                        if (a)
                            a->reset(); // vehCar::Reset -> vehCarAudioContainer::Reset
                    });
                else
                    log::warn("race: opponent {} cannot drive: {}", s.vehicle, error);
                // aiMap::SetWaypoints: the line aiRouteRacer::Finished tests
                // (mmSingleRace: the last checkpoint; mmSingleCircuit: the first).
                const auto& cps = m_session->checkpoints();
                const auto mode = m_result.config.mode;
                if (opp.driver && !cps.empty() &&
                    (mode == game::GameMode::Checkpoint || mode == game::GameMode::Circuit)) {
                    const auto& cp = mode == game::GameMode::Circuit ? cps.front() : cps.back();
                    opp.driver->setFinishLine(cp.position, cp.headingDeg);
                }
            }
            m_opponents.push_back(std::move(opp));
        }
        log::info("race: {} opponents", m_opponents.size());
    }

    // The race's police posts (.aimap [Police]): aiMap::Init places the first
    // trunc(count * clamp(CopDensity, 0, 1)) of them, CopDensity being the
    // menu's cop density in every mode (the race menu starts it at the race
    // table's cop count, the crash course sets 1). Cops stay for the whole
    // race.
    void spawnPolice(Context& ctx) {
        if (!m_session || !m_world || !m_ai)
            return;
        // A network client drives no police: the shared traffic's come from
        // the host (OpenMM2 extra; the other network games have no posts).
        if (multiplayer(ctx) && !ctx.netGame->isHost())
            return;
        const auto& posts = m_session->police();
        const std::size_t count = ai::PoliceSquad::countForDensity(posts.size(), m_result.config.copDensity);
        std::optional<float> chaseDistance;
        if (m_session->setup().aiMap)
            chaseDistance = m_session->setup().aiMap->copChaseDistance;
        m_police = std::make_unique<ai::PoliceSquad>(m_ai->map());
        // The game's room flags (lvlRoomInfo): a cop in a "water of death"
        // room drops out (aiPoliceOfficer::Update).
        m_police->setRoomFlags(m_city->levelRoomFlags);
        // Each police car's vehCar::Init drew on MM2's global stream in turn
        // at the end of aiMap::Init (loadEffects): its siren flares come
        // from there.
        game::fx::Rand initDraws(m_policeInitState);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& p = posts[i];
            const std::uint32_t flares = game::takeVehCarInitDraws(initDraws);
            Cop cop;
            Mat34 post = p.spawn;
            // aiVehiclePhysics::Init: vehCar::Init(<car>), the car's own tune
            // (MM2 never loads vpcop_cop.vehCarSim).
            cop.sim = loadAiCar(ctx, p.vehicle, {}, post);
            if (!cop.sim)
                continue;
            // OpenMM2: a shared-traffic host's police draw their wheels' bump
            // numbers from their own streams, which travel with their states
            // (net/TrafficFull.h): a client simulates them alike.
            if (netTrafficHost(ctx)) {
                cop.sim->sim().ownRandom = true;
                cop.sim->sim().randomState = 1;
            }
            ai::PoliceSettings settings = ai::PoliceSettings::fromData(p.params, chaseDistance);
            settings.seed += i;
            cop.driver = &m_police->add(cop.sim->sim(), post, 100 + static_cast<int>(m_cops.size()), settings,
                                        p.vehicle);
            // aiVehiclePhysics::Init: the paint job is the officer's id (its
            // index) & 3, except for vpcop: 1 (the London livery, vpcop_ln_*)
            // when the AI map has no cable cars, else 0 (California,
            // vpcop_ca_*). OpenMM2 has no cable cars yet; of the retail cities
            // only San Francisco has them.
            int livery = static_cast<int>(i & 3);
            if (str::iequals(p.vehicle, "vpcop"))
                livery = str::iequals(m_result.config.city, "sf") ? 0 : 1;
            cop.renderer = std::make_unique<game::VehicleRenderer>(ctx.device(), *m_textures, *m_models,
                                                                    cop.sim->model(), livery);
            cop.vehicle = p.vehicle;
            cop.livery = livery;
            cop.renderer->setSirenFlares(flares);
            setupVehicleRenderer(ctx, *cop.renderer);
            cop.audio = loadAiCarAudio(ctx, p.vehicle, true);
            cop.fx = loadVehicleFx(ctx, p.vehicle, cop.sim->model(), *cop.renderer);
            // Development aid: OPENMM2_DEBUG_POLICE_TOUGHNESS=<factor> scales
            // the police cars' MedDamage and MaxDamage (a wreck sooner, for
            // the shared traffic's damage tests).
            if (const char* tough = std::getenv("OPENMM2_DEBUG_POLICE_TOUGHNESS")) {
                const auto f = static_cast<float>(str::parseDouble(tough).value_or(1.0));
                auto& d = cop.sim->sim().damage.params;
                d.medDamage *= f;
                d.maxDamage *= f;
            }
            // OpenMM2: a shared-traffic host shows its police's damage on the
            // clients (the car's ambient id, as sendNetTraffic shares it).
            const int netId = kNetPoliceId + static_cast<int>(m_cops.size());
            if (netTrafficHost(ctx) && netId < static_cast<int>(net::kMaxAmbientIds))
                cop.damage = &m_netDamage.police(static_cast<std::uint16_t>(netId));
            cop.sim->sim().onImpactCallback = [fx = cop.fx.get(), sim = &cop.sim->sim(), sounds = cop.impacts,
                                                damage = cop.damage,
                                                time = &m_netStateTime](const phys::CarImpact& impact) {
                if (impact.sound)
                    sounds->push_back({impact.soundStrength, impact.audioId, impact.position});
                fx->impact(impact, *sim);
                if (damage && impact.damaging)
                    damage->impact(*time, game::damageImpactOf(impact, *sim));
            };
            m_cops.push_back(std::move(cop));
        }
        log::info("race: {} police cars", m_cops.size());
    }

    // A car as MM2's AI sees it (ai::trackedCar: its inertial frame, speed,
    // bumper and side distances).
    static ai::TrackedCar trackedCar(const phys::CarSim& sim, int id, bool player = false) {
        return ai::trackedCar(sim, id, player);
    }

    // aiMap::StopRoadTraffic: every racer's and cop's road window holds (or
    // lets go) the controlled roads of the intersections it drives into.
    void stopRoadTraffic(bool stop) {
        if (!m_ai)
            return;
        ai::Traffic& traffic = m_ai->traffic();
        auto stopSources = [&traffic](int node, bool s) { traffic.stopSources(node, s); };
        for (const auto& o : m_opponents)
            if (o.driver)
                o.driver->driver().stopRoadTraffic(stop, stopSources);
        for (const auto& c : m_cops)
            c.driver->driver().stopRoadTraffic(stop, stopSources);
    }

    // The ambient traffic, pedestrians and traffic lights (ai::World; the
    // lights run with them, where aiMap::Update runs its light sets after the
    // racers and the police).
    void updateAmbient(Context& ctx, float dt) {
        if (!m_ai || !m_player)
            return;
        const auto& sim = m_player->sim();
        ai::PlayerCar pc;
        pc.transform = sim.body.ics.matrix;
        pc.velocity = sim.body.ics.frameVelocity;
        // aiVehiclePlayer's side and bumper distances: half vehCarSim's Size
        // (its InertiaBox).
        pc.width = sim.params.inertiaBox.x;
        pc.length = sim.params.inertiaBox.z;
        // lvlInstance::GetRadius: the car's geometry radius.
        pc.radius = sim.body.radius();
        pc.steering = sim.steering;
        pc.reversing = sim.trans.getCurrentGear() < 0;
        pc.horn = hornDown(ctx);
        std::vector<Vec3> racers;
        for (const auto& o : m_opponents)
            racers.push_back(o.sim->sim().body.ics.matrix.m3);
        m_ai->setOpponents(racers);
        // OpenMM2's shared traffic: on the host, the other players too.
        if (netTrafficHost(ctx)) {
            collectNetPlayers(ctx);
            m_ai->setOtherPlayers(m_netTrafficPlayers);
        }
        m_ai->update(dt, pc);
    }

    // Opponent and police AI: reads every car, writes the AI cars' inputs.
    void updateAiDrivers(float dt) {
        if (!m_player || (m_opponents.empty() && m_cops.empty()))
            return;
        std::vector<ai::TrackedCar> cars;
        ai::TrackedCar player = trackedCar(m_player->sim(), 0, true);
        player.isPlayer = true;
        player.suspect = true;
        player.reversing = m_player->sim().trans.getCurrentGear() == -1;
        const int impacts = m_vehicleImpacts + m_objectImpacts;
        player.collided = impacts != m_lastImpacts;
        m_lastImpacts = impacts;
        // aiVehiclePlayer::Update: the road the player is on, for the
        // drivers' obstacle checks.
        if (m_ai)
            m_ai->map().trackPlayer(player);
        cars.push_back(player);
        // OpenMM2's shared traffic: the host's police chase the other
        // players too (after the local one, aiMap::Player(0)).
        for (const auto& t : m_netTrackedPlayers)
            cars.push_back(t);
        for (const auto& o : m_opponents) {
            ai::TrackedCar t = trackedCar(o.sim->sim(), 1 + static_cast<int>(o.sessionIndex));
            t.suspect = true;
            if (o.driver)
                o.driver->describe(t);
            cars.push_back(t);
        }
        for (const auto& c : m_cops) {
            ai::TrackedCar t = trackedCar(c.sim->sim(), c.driver->selfId());
            t.isPolice = true;
            cars.push_back(t);
        }
        if (m_ai) {
            for (const ai::AmbientCar& c : m_ai->cars())
                cars.push_back(ai::trackedAmbient(c, 10000 + c.id));
        }
        // The cable cars share the ambient cars' obstacle map
        // (aiCableCar::UpdateObstacleMap): the drivers see them there.
        if (m_cableCars)
            for (std::size_t i = 0; i < m_cableCars->size(); ++i)
                cars.push_back(m_cableCars->tracked(i, 20000 + static_cast<int>(i)));
        // aiMap::Update: the physics cars let the intersections ahead of them
        // go (aiMap::StopRoadTraffic(false)), drive, and hold them again
        // (StopRoadTraffic(true)) while the ambient traffic updates.
        stopRoadTraffic(false);
        for (auto& o : m_opponents) {
            if (!o.driver) {
                o.sim->sim().setInputs(0.0f, 1.0f, 0.0f, 1.0f);
                continue;
            }
            o.driver->setHeld(m_session &&
                              (!m_session->racersReleased() || !m_session->opponentActive(o.sessionIndex)));
            o.driver->update(dt, cars);
        }
        if (m_police)
            m_police->update(dt, cars, m_world.get(), !m_session || m_session->policeActive());
        stopRoadTraffic(true);
        // dgPhysManager::DeclareMover for the racers (aiRouteRacer::Update)
        // and the police (aiPoliceOfficer::Update), by their distance to the
        // player; a cop MM2 does not declare neither updates nor collides.
        for (auto& o : m_opponents) {
            if (!o.driver)
                continue;
            const ai::MoverDeclaration m = o.driver->mover();
            o.sim->sim().body.declare(m.type, m.flags);
        }
        for (auto& c : m_cops) {
            const ai::MoverDeclaration m = c.driver->mover();
            if (m.type != 0)
                c.sim->sim().body.declare(m.type, m.flags);
            else
                c.sim->sim().body.declared = false;
            // OpenMM2: the controls its driver set for this frame's samples,
            // which a shared-traffic host sends with its state
            // (sendNetTrafficFull).
            const phys::CarSim& sim = c.sim->sim();
            const std::array<float, 4> set{sim.engine.throttle, sim.brakes, sim.steering, sim.handBrake};
            c.controls = set;
            for (std::size_t k = 0; k < set.size(); ++k)
                c.controlSum[k] += set[k];
            ++c.controlFrames;
        }
        // Development aid: OPENMM2_DEBUG_AI logs the AI cars once a second.
        if (std::getenv("OPENMM2_DEBUG_AI") && std::floor(m_time) != std::floor(m_time - dt)) {
            const Vec3 p = m_player->sim().modelMatrix().m3;
            log::info("ai: t {:.0f} player ({:.0f},{:.0f}) {:.0f} mph", m_time, p.x, p.z, m_player->sim().speedMph());
            for (const auto& o : m_opponents) {
                const Vec3 q = o.sim->sim().modelMatrix().m3;
                log::info("ai:   opp {} ({:.0f},{:.0f}) {:.0f} mph mode {} progress {:.0f} m", o.sessionIndex, q.x, q.z,
                          o.sim->sim().speedMph(), o.driver ? static_cast<int>(o.driver->mode()) : -1,
                          o.driver ? o.driver->progress() : 0.0f);
            }
            for (const auto& c : m_cops) {
                const Vec3 q = c.sim->sim().modelMatrix().m3;
                log::info("ai:   cop {} ({:.0f},{:.0f}) {:.0f} mph mode {} siren {}", c.driver->selfId(), q.x, q.z,
                          c.sim->sim().speedMph(), static_cast<int>(c.driver->mode()), c.driver->siren());
            }
        }
    }

    // The pedestrians' screams (aiPedAudio): a dodge this step asks for a
    // sound slot and queues a line; the voices take the player's speed.
    void updatePedestrianAudio(float dt) {
        if (!m_ai || !m_player)
            return;
        m_pedSounds.clear();
        for (const auto& p : m_ai->peds())
            m_pedSounds.push_back({p.id, p.typeName, p.transform.m3, p.scream, p.placed});
        m_pedAudio.update(m_pedSounds, m_camera.transform, m_player->sim().speed(), dt, m_tunnel);
    }

    // Engine, tyre and siren sounds of the opponents and police, positioned.
    void updateAiAudio(float dt) {
        const Mat34& listener = m_camera.transform;
        auto feed = [&](audio::game::OpponentCarAudio& audio, const phys::CarSim& sim, bool siren,
                        bool pursuingPlayer, bool wrecked, std::vector<audio::game::ImpactInput>& impacts) {
            audio::game::CarAudioInputs in = carAudioInputs(sim);
            in.wrecked = wrecked;
            in.throttle = sim.engine.throttle;
            in.brake = sim.brakes;
            in.transform = sim.modelMatrix();
            in.siren = siren;
            // aiPoliceOfficer::StartSiren(IsPlayer): only a chase of the
            // player counts for the cop chase music.
            in.sirenPursuingPlayer = pursuingPlayer;
            // vehCarDamage::ApplyImpact's AudImpact::Play for this car.
            in.impacts = std::move(impacts);
            impacts.clear();
            audio.update(in, dt, listener);
        };
        for (auto& o : m_opponents)
            if (o.audio)
                feed(*o.audio, o.sim->sim(), false, false, o.sim->sim().damage.wrecked(), *o.impacts);
        // A cop's explosion follows its driver's wrecked flag (aiVehiclePhysics
        // +0x9686: set the first update past MaxDamage, cleared by Reset).
        for (auto& c : m_cops)
            if (c.audio)
                feed(*c.audio, c.sim->sim(), c.driver->siren(), c.driver->target() == 0, c.driver->driver().wrecked(),
                     *c.impacts);
        // The network players' cars (mmNetObject::PositionUpdate: the
        // pedals, horn, siren and wreck from the packet; the engine follows
        // the car's own vehCarSim, here the kinematic one placed from the
        // snapshots).
        for (const auto& rc : m_remoteCars) {
            const auto it = m_remotes.find(rc.id);
            if (!rc.hasState || it == m_remotes.end() || !it->second.audio || !it->second.sim)
                continue;
            audio::game::CarAudioInputs in = carAudioInputs(it->second.sim->sim());
            in.throttle = rc.controls.throttle;
            in.brake = rc.controls.brake;
            in.gear = rc.controls.gear;
            in.speed = rc.velocity.mag();
            in.horn = (rc.flags & net::kVehicleHorn) != 0;
            in.siren = (rc.flags & net::kVehicleSiren) != 0;
            in.sirenPursuingPlayer = false;
            in.wrecked = (rc.flags & net::kVehicleWrecked) != 0;
            in.transform = rc.transform;
            in.velocity = rc.velocity;
            in.impacts = std::exchange(it->second.impacts, {}); // OpenMM2: its owner's, replayed
            it->second.audio->update(in, dt, listener);
        }
    }

    // The ambient cars' sounds (aiAmbientVehicleAudio, which aiVehicleSpline::Init
    // gives every pooled car; OpenMM2 loads it when the car first drives):
    // engine, horn and impacts, and the driver's voice (AudCreature).
    void updateAmbientAudio(Context& ctx, float dt) {
        if (!m_ai || !m_bank || !ctx.mixer || !m_player)
            return;
        const Mat34& listener = m_camera.transform;
        // aiMap::Update: aiAmbientVehicleAudio::UpdateStatics and
        // AudCreatureContainer::UpdateStatics, with the player's speed, each
        // advance the shared impact-line clock (so it runs twice a frame) and
        // update the voices.
        const float playerSpeed = m_player->sim().speed();
        audio::game::CreatureVoice::advanceClock(dt);
        audio::game::CreatureVoice::advanceClock(dt);
        std::vector<char> seen;
        // (A shared-traffic client hears the cars the host sends.)
        const auto& ambient = netTrafficClient(ctx) ? m_netCars : m_ai->cars();
        for (const ai::AmbientCar& c : ambient) {
            AmbientAudio* a = ambientAudio(ctx, c);
            if (!a)
                continue;
            seen.resize(m_ambientAudio.size(), 0);
            seen[static_cast<std::size_t>(c.id)] = 1;
            a->active = true;
            if (a->hasVoice)
                a->voice.update(playerSpeed, dt);
            // The voice follows the car's attenuation, pan and echo while
            // the car holds its sound slot (AmbientCarAudio::setVoice).
            a->car.update(c.speed, c.transform, c.velocity, dt, listener);
        }
        for (std::size_t i = 0; i < m_ambientAudio.size(); ++i) {
            AmbientAudio* a = m_ambientAudio[i].get();
            if (a && a->active && (i >= seen.size() || !seen[i])) {
                // Back in the pool (aiVehicleSpline::Reset ->
                // aiAmbientVehicleAudio::Reset).
                a->car.reset();
                a->active = false;
            }
        }
        // aiGoalAvoidPlayer::Reset: PlayAvoidanceHorn, and when a horn
        // pattern starts, PlayAvoidanceReaction.
        for (int id : m_ai->takeAvoidEvents()) {
            if (AmbientAudio* a = ambientAudioOf(id); a && a->car.honk())
                a->car.avoidReaction();
        }
        // A shared-traffic client: the horns the host's cars sounded.
        if (netTrafficClient(ctx))
            for (const ai::AmbientCar& c : m_netCars)
                if (AmbientAudio* a = c.horn ? ambientAudioOf(c.id) : nullptr; a && a->car.honk())
                    a->car.avoidReaction();
        // aiVehicleActive's impact callback: AudImpact::Play and
        // PlayImpactHorn with |x| + |y| + |z| of the impulse, then
        // PlayImpactReaction.
        for (const game::TrafficImpact& e : m_trafficImpacts) {
            AmbientAudio* a = ambientAudioOf(e.carId);
            if (!a)
                continue;
            a->car.impact({e.strength, e.audioId, {}});
            a->car.impactReaction(e.strength);
        }
        m_trafficImpacts.clear();
    }

    struct AmbientAudio : audio::game::CreatureVoice::Owner {
        audio::game::AmbientCarAudio car;
        audio::game::CreatureVoice voice;
        bool hasVoice = false;
        bool active = false;
        // The voice's container is the car's audio (its 3D slot).
        bool requestSlot() override { return car.audible(); }
    };

    AmbientAudio* ambientAudioOf(int id) {
        if (id < 0 || static_cast<std::size_t>(id) >= m_ambientAudio.size())
            return nullptr;
        return m_ambientAudio[static_cast<std::size_t>(id)].get();
    }

    // The audio of ambient car `c`, loaded on first use: the engine and horn
    // of its type (else the defaults) and its driver's voice
    // (aiAmbientVehicleAudio::Init).
    AmbientAudio* ambientAudio(Context& ctx, const ai::AmbientCar& c) {
        if (c.id < 0 || !c.data)
            return nullptr;
        const auto id = static_cast<std::size_t>(c.id);
        if (id >= m_ambientAudio.size())
            m_ambientAudio.resize(id + 1);
        if (!m_ambientAudio[id]) {
            auto a = std::make_unique<AmbientAudio>();
            if (!a->car.load(ctx.game->vfs, *m_bank, *ctx.mixer, c.data->model, &m_audioSlots))
                return nullptr;
            if (const auto* def = ambientVoice(ctx.game->vfs, c.data->model)) {
                a->voice.load(*ctx.mixer, *m_bank, *def);
                a->voice.setOwner(a.get());
                a->car.setVoice(&a->voice);
                a->hasVoice = true;
            }
            m_ambientAudio[id] = std::move(a);
        }
        return m_ambientAudio[id].get();
    }

    // aiAmbientVehicleAudio::LoadVoices: aud/creaturedata/<type>_ambcarvoice<c>,
    // else default_ambcarvoice<c><n>, with c "_l" in London and "_s"
    // elsewhere (mmGame::Init's SetCSVCatString) and n drawn once per session
    // from numambcarvoicefiles<c> (LoadNumVFileChoices).
    const audio::game::CreatureVoiceDef* ambientVoice(const vfs::Vfs& vfs, const std::string& type) {
        if (m_voiceFileNum < 0) {
            m_voiceCategory = m_result.config.city == "london" ? "_l" : "_s";
            m_voiceFileNum = 0;
            const auto text =
                audio::game::readText(vfs, "aud/creaturedata/numambcarvoicefiles" + m_voiceCategory + ".csv");
            if (const auto n = text ? audio::game::parseNumFileChoices(*text) : std::nullopt)
                m_voiceFileNum = static_cast<int>(audio::randomizeNumber(1.0f, *n + 0.25f));
        }
        auto [it, fresh] = m_voiceDefs.try_emplace(type);
        if (fresh) {
            std::string path = std::format("aud/creaturedata/{}_ambcarvoice{}.csv", type, m_voiceCategory);
            if (!vfs.exists(path))
                path = std::format("aud/creaturedata/default_ambcarvoice{}{}.csv", m_voiceCategory,
                                   m_voiceFileNum);
            if (const auto text = audio::game::readText(vfs, path))
                it->second = audio::game::parseCreatureVoice(*text);
        }
        return it->second ? &*it->second : nullptr;
    }

    game::session::PlayerState playerState() const {
        game::session::PlayerState ps;
        const auto& sim = m_player->sim();
        // The rules (mmWaypoints, mmGame::Update, mmSingleStunt) and the map
        // read the car's phInertialCS matrix (vehCarSim +0x6C).
        ps.transform = sim.body.ics.matrix;
        ps.velocity = sim.body.ics.frameVelocity;
        ps.speedMph = sim.speedMph();
        ps.rpm = std::max(sim.engine.rpm, sim.params.engine.idleRPM);
        ps.maxRpm = sim.params.engine.maxRPM;
        ps.gear = sim.trans.getCurrentGear();
        ps.automatic = m_result.config.automatic;
        ps.throttle = m_lastPedals.accelerator;
        ps.damage01 = sim.damage.damage;
        // mmPlayer::IsMaxDamaged: CurrentDamage strictly past MaxDamage.
        ps.wrecked = sim.damage.maxDamaged();
        // mmGame::Update reads the vehSplash active flag, which vehCar::Update
        // latches once the model origin goes under a water room's level and
        // only vehCar::Reset clears: a car that floats back up stays "in".
        ps.inWater = sim.splash.active();
        ps.vehicleImpacts = m_vehicleImpacts;
        ps.objectImpacts = m_objectImpacts;
        ps.inertiaBox = sim.params.inertiaBox;
        // mmExternalView::Cull draws the steering bar for the mouse.
        if (m_gameInput.controller() == controls::Controller::Mouse)
            ps.mouseSteer = m_steerApplied;
        return ps;
    }

    void updateSession(Context& ctx, float dt) {
        if (!m_session || !m_player)
            return;
        updateNetRace(ctx);
        m_playerState = playerState();
        auto carState = [](const phys::CarSim& sim) {
            game::session::OpponentState s;
            s.transform = sim.body.ics.matrix; // as mmWaypoints::AIWPHit gets it
            s.velocity = sim.body.ics.frameVelocity;
            s.damage01 = sim.damage.damage;
            s.wrecked = sim.damage.wrecked();
            s.currentDamage = sim.damage.currentDamage;
            s.inertiaBox = sim.params.inertiaBox;
            return s;
        };
        // In Session::opponents() order; a car that failed to load stays parked at its spawn.
        std::vector<game::session::OpponentState> opps;
        for (const auto& s : m_session->opponents()) {
            game::session::OpponentState parked;
            parked.transform = s.spawn;
            opps.push_back(parked);
        }
        for (const auto& o : m_opponents) {
            auto& st = opps[o.sessionIndex] = carState(o.sim->sim());
            st.finished = o.driver && o.driver->finished(); // aiRouteRacer::Finished
        }
        std::vector<game::session::OpponentState> cops;
        for (const auto& c : m_cops) {
            auto& st = cops.emplace_back(carState(c.sim->sim()));
            // mmSingleStunt::CheckCopPursuit asks aiPoliceOfficer::InPersuit:
            // any chase (of anyone) and a wrecked, out-of-action cop count.
            st.pursuing = c.driver->mode() != ai::PoliceCar::Mode::Parked;
        }
        presentOwnWaterReset();
        const auto phaseBefore = m_session->phase();
        m_session->setPreRaceCamera(m_cams.preRace());
        // The multiplayer countdown (2.5 s) starts with the host's start
        // message; OpenMM2's follows the shared start time (updateNetStart).
        if (multiplayer(ctx)) {
            m_session->setNetStart(m_netToGo);
            m_session->setNetSample(ctx.netGame->isHost() ? m_netOwnSample : m_prediction.nextSeq() - 1);
        } else {
            m_session->setStartSignal(true);
        }
        m_session->update(dt, m_playerState, opps, cops);
        if (multiplayer(ctx) &&
            (m_session->position() != m_tracedPlace || m_session->racerCount() != m_tracedRacers)) {
            m_tracedPlace = m_session->position();
            m_tracedRacers = m_session->racerCount();
            game::traceRuleStanding(ctx.netGame->traceFile(), ctx.netGame->frameTime(), m_tracedPlace,
                                    m_tracedRacers);
        }
        // DisableRacers / EnableRacers: the player's vehCarDamage switch.
        m_player->sim().damage.enabled = m_session->playerDamageEnabled();
        // mmSingleStunt::UpdateEvade turns the map on during its first line.
        if (m_hud && m_session->wantsMap() && m_cams.mapMode() == game::MapMode::Off)
            m_cams.cycleMap();
        // mmPlayer::SetPostRaceCam / mmGameMulti::SetFinishCam at the endings
        // that set it (Session::postRaceCamera).
        if (phaseBefore != game::session::Phase::PostRace && m_session->phase() == game::session::Phase::PostRace &&
            m_result.config.mode != game::GameMode::Cruise && m_session->postRaceCamera())
            startFinishCamera(ctx);
        handleSessionEvents(ctx);
        if (auto* music = ctx.music(); music && m_musicDirector) {
            using game::session::Phase;
            auto& director = *m_musicDirector;
            const Phase phase = m_session->phase();
            if (phase != Phase::Countdown)
                director.raceStarted();
            if (phase == Phase::PostRace && !m_musicFinished) {
                // The single-player race modes stop the music at the finish
                // (StopSegment(0)), a wreck with an ending on the next beat
                // (StopSegment(1)); the water, a late Blitz, the crash course
                // and the multiplayer modes leave it playing.
                if (m_session->damagedOut())
                    director.damagedOut();
                else if (m_session->musicStopped())
                    director.finish();
                m_musicFinished = true;
            }
            // mmPopup::ShowResults (a finish); a loss opens the main menu,
            // whose pause music openPopup starts.
            if (phase == Phase::Done && !m_musicResults && m_session->raceOver()) {
                director.results();
                m_musicResults = true;
            }
            director.update(dt, m_player->sim().speed(), audio::game::SirenPlayer::copsPursuingPlayer(),
                            m_carAudio.airborne());
            for (const auto& c : director.takeCommands())
                music->setState(c.state, c.timing);
            if (director.takeBigAir())
                music->triggerBigAir();
        }
        if (m_session->finished() && !m_resultsShown) {
            m_resultsShown = true;
            m_result = m_session->result();
            // A lost race or lesson (state 4) opens the main menu without
            // pausing (mmPopup::ProcessEscape(0)); a finished one shows the
            // results (mmPopup::ShowResults), which OpenMM2 shows as the
            // first page of the menus.
            if (!m_result.finished && !multiplayer(ctx))
                openPopup(ctx, false);
            else
                leaveRace(ctx, m_result);
        }
    }

    // The session's events: what the rules ask of the race (the cameras, the
    // cars, sounds and speech, the network). updateSession hands them over
    // after the rules, applyRestart after a restart, the paused frame after
    // its checks.
    void handleSessionEvents(Context& ctx) {
        if (!m_session || !m_player)
            return;
        for (const auto& e : m_session->takeEvents()) {
            using game::session::EventType;
            if (e.type == EventType::HitWater)
                m_cams.startWaterCam();
            // mmSingleStunt::InitHUD turns the icons on for the follow and
            // destroy events (iconState, mmGame::SetIconsState).
            if (e.type == EventType::LessonEventStarted && m_hud) {
                const auto& events = m_session->setup().lessonEvents;
                if (e.index >= 0 && static_cast<std::size_t>(e.index) < events.size() &&
                    (events[static_cast<std::size_t>(e.index)].type == game::session::LessonType::Follow ||
                     events[static_cast<std::size_t>(e.index)].type == game::session::LessonType::Destroy))
                    m_hud->options().opponentIcons = true;
            }
            if (e.type == EventType::Respawn) {
                // Without waypoints mmGame::HitWaterHandler is mmPlayer::Reset
                // (back to the reset position); with them mmSingleCircuit /
                // mmGameMulti::HitWaterHandler reset the car at the last
                // checkpoint and put the reset position back.
                // (A network race's water and fall run on the car's samples:
                // NetCarDriver::setWaterHandler, presentOwnWaterReset.)
                if (m_session->setup().checkpoints.empty()) {
                    netCommand({0, net::CarCommandKind::Reset, {}, 0.0f});
                } else {
                    const Mat34 at = m_session->respawnTransform();
                    netCommand({0, net::CarCommandKind::RespawnAt, at.m3, phys::resetRotationOf(at)});
                }
                if (m_vehicleFx)
                    m_vehicleFx->reset(); // vehCar::Reset
                clearVehicleDamage();
                m_cams.reset(cameraTarget());
            } else if (e.type == EventType::Restart) {
                // The race starts over (mmGame::Reset): every prop back in its
                // place (lvlLevel::ResetInstances), every car to its start,
                // and the elasticity cap back to 1 (the "/blubber" cheat's 4).
                if (m_bangers)
                    m_bangers->reset();
                // The gizmo managers (nodes of mmGame) and aiMap::Reset's
                // cable cars.
                if (m_gizmos)
                    m_gizmos->reset();
                if (m_cableCars && m_world)
                    m_cableCars->reset(*m_world);
                phys::setElasticityCap(phys::kElasticityCap);
                // mmPlayer::Reset, aiVehiclePhysics::Reset: vehCar::Reset at
                // the reset positions.
                m_player->reset();
                if (m_vehicleFx)
                    m_vehicleFx->reset();
                clearVehicleDamage();
                for (auto& o : m_opponents) {
                    o.sim->reset();
                    if (o.fx)
                        o.fx->reset();
                    if (o.audio)
                        o.audio->reset(); // vehCarAudioContainer::Reset
                    o.renderer->resetDamage();
                    if (o.driver)
                        o.driver->reset();
                }
                // aiMap::Reset: aiVehicleManager (a child node), the police
                // force and officers, the roads, traffic and pedestrians.
                if (m_trafficBodies)
                    m_trafficBodies->reset();
                if (m_ai)
                    m_ai->reset();
                if (m_police)
                    m_police->reset(); // aiPoliceForce::Reset, then each aiPoliceOfficer::Reset
                for (auto& c : m_cops) {
                    if (c.fx)
                        c.fx->reset(); // vehCar::Reset (aiVehiclePhysics::Reset)
                    c.renderer->resetDamage();
                    ++c.resets; // a new car for the shared traffic's clients
                    if (c.damage)
                        c.damage->reset(m_netStateTime);
                    if (c.audio)
                        c.audio->reset(); // aiPoliceOfficer::Reset -> vehPoliceCarAudio::Reset
                }
                // mmGame::Reset: StartMusic again.
                if (m_musicDirector)
                    m_musicDirector->restart();
                m_musicFinished = m_musicResults = false;
                m_drawnPhys.clear(); // OpenMM2: drawn where everything now is
                m_drawnAi.clear();
                m_cams.reset(cameraTarget());
                // The race modes' Reset: mmPlayer::SetPreRaceCam again.
                if (m_result.config.mode != game::GameMode::Cruise && !multiplayer(ctx))
                    m_cams.startPreRace();
                m_gameInput.reset();
                m_ff.reset(); // mmPlayer::Reset: ResetFF, mmCarRoadFF::Reset
            } else if (e.type == EventType::DamageReset) {
                netCommand({0, net::CarCommandKind::ClearDamage, {}, 0.0f});
                clearVehicleDamage(); // vehCar::ClearDamage
            } else if (e.type == EventType::PlayerDamageLimits) {
                auto& d = m_player->sim().damage.params;
                d.maxDamage = e.value;
                d.medDamage = e.value * 0.5f;
                d.impactThreshold = 0.0f;
            } else if (e.type == EventType::OpponentDamageLimits) {
                for (auto& o : m_opponents)
                    if (o.sessionIndex == static_cast<std::size_t>(e.index)) {
                        o.sim->sim().damage.params.maxDamage = e.value;
                        o.sim->sim().damage.params.medDamage = e.value * 0.5f;
                    }
            } else if (e.type == EventType::Sound) {
                playGameSound(ctx, static_cast<game::session::GameSound>(e.index), e.value);
            } else if (e.type == EventType::Speech) {
                announce(static_cast<game::session::SpeechCue>(e.index), e.value);
            } else if (e.type == EventType::CheckpointCleared) {
                // mmWaypoints::DisplayHUDMessage: the crash course's location
                // line for the checkpoint (mmCCSpeech::PlayCheckPoint, 0.01 s).
                if (m_announcerOk && m_result.config.mode == game::GameMode::CrashCourse)
                    m_announcer.playCrashCourseCheckPoint(e.index, 0.01f);
                // OpenMM2: the host counts every car's waypoints itself
                // (MM2's SendPosition carried each machine's own count).
                if (multiplayer(ctx))
                    game::traceRuleShown(ctx.netGame->traceFile(), ctx.netGame->frameTime(),
                                         ctx.netGame->localId(), e.index, m_session->waypointsPassed(),
                                         e.value > 0.0f ? 1 : 0);
            } else if (e.type == EventType::CheckpointTakenBack && multiplayer(ctx)) {
                game::traceRuleTakenBack(ctx.netGame->traceFile(), ctx.netGame->frameTime(),
                                         ctx.netGame->localId(), e.index);
            } else if (e.type == EventType::FinalCheckpoint || e.type == EventType::FinalLap) {
                // mmWaypoints::Update: the last stretch switches the music to
                // the cop chase segment; the final checkpoint is announced
                // (mmRaceSpeech::PlayFinalCheckPoint; the race speech exists
                // outside the crash course). PlayFinalLap has no caller.
                if (m_musicDirector)
                    m_musicDirector->finalStretch();
                if (e.type == EventType::FinalCheckpoint && m_announcerOk &&
                    m_result.config.mode != game::GameMode::CrashCourse)
                    m_announcer.playFinalCheckpoint();
            }
            // OpponentFinished needs nothing: the game only asks
            // aiRouteRacer::Finished (OpponentState::finished), and the car
            // drives on to its destination.
        }
    }

    // The camera at the end of a race: mmPlayer::SetPostRaceCam, or in a
    // multiplayer checkpoint race or circuit mmGameMulti::SetFinishCam: the
    // orbit camera on the finish (the last waypoint, a circuit's first) at
    // azimuth (heading + 180) x -0.017453292, which the keyboard then turns.
    void startFinishCamera(Context& ctx) {
        const auto& cps = m_session->checkpoints();
        const auto mode = m_result.config.mode;
        if (!multiplayer(ctx) || mode == game::GameMode::Blitz || cps.empty()) {
            m_cams.startPostRace();
            return;
        }
        const auto& finish = mode == game::GameMode::Circuit ? cps.front() : cps.back();
        m_cams.startMultiplayerPostRace(finish.position, (finish.headingDeg + 180.0f) * -0.017453292f);
    }

    // The modes' and mmWaypoints' 2D sounds (AudSoundBase): `mode` 0 plays
    // once (the timer warning only when it is not still playing), 1 loops,
    // -1 stops.
    void playGameSound(Context& ctx, game::session::GameSound sound, float mode) {
        if (!ctx.mixer || !m_bank)
            return;
        const std::string name = game::session::gameSoundName(sound);
        auto it = m_gameSounds.find(name);
        if (it == m_gameSounds.end()) {
            audio::game::SoundSlot slot;
            // AudSoundBase::SetPriority(0x17) after loading them.
            slot.load(*ctx.mixer, *m_bank, name, audio::Bus::Effects, audio::game::kGameSoundPriority);
            it = m_gameSounds.emplace(name, std::move(slot)).first;
        }
        auto& slot = it->second;
        if (!slot.valid())
            return;
        const float volume =
            game::session::gameSoundVolume(sound, m_result.config.mode == game::GameMode::CrashCourse);
        if (mode < 0.0f)
            slot.stop();
        else if (mode > 0.0f)
            slot.playLoop(volume, 1.0f);
        else if (sound != game::session::GameSound::TimerWarning || !slot.playing())
            slot.playOnce(volume);
    }

    // The announcer's lines the modes ask for (mmRaceSpeech, mmCCSpeech).
    void announce(game::session::SpeechCue cue, float value) {
        if (!m_announcerOk)
            return;
        using game::session::SpeechCue;
        const bool lessons = m_result.config.mode == game::GameMode::CrashCourse;
        switch (cue) {
        case SpeechCue::PreRace:
            if (lessons)
                m_announcer.playCrashCoursePreRace();
            else
                m_announcer.playPreRace();
            break;
        case SpeechCue::Results:
            if (!lessons)
                announceResults(static_cast<int>(value));
            break;
        case SpeechCue::ResultsPoor:
            if (!lessons)
                m_announcer.playResults(10, 10);
            break;
        case SpeechCue::DamagePenalty:
            if (!lessons)
                m_announcer.playDamagePenalty();
            break;
        case SpeechCue::LessonResults:
            if (lessons)
                m_announcer.playCrashCourseResults(value != 0.0f);
            break;
        }
    }

    // The modes' RegisterFinish: a finish under the race's table settings is
    // registered with the driver, and mmGameSingle::UpdateRewards then has
    // the announcer name the car or paint job it unlocks, or else play the
    // results for the place (PlayResults(place, (int)OpponentDensity)).
    // OpenMM2's frontend stores the finish when the race is left; the race
    // works out the same outcome on a copy of the driver.
    void announceResults(int place) {
        if (!m_profile || !m_session)
            return;
        game::RaceResult result = m_session->result();
        if (result.cheated)
            return; // RegisterFinish registers nothing while bCheating is set
        game::RaceConfig defaults = result.config;
        game::session::applyRaceTableDefaults(defaults, m_session->setup().race);
        if (!game::Progress::recordable(result.config, defaults))
            return;
        if (!m_progress)
            m_progress = game::Progress::load(*m_vfs);
        game::Profile copy = *m_profile;
        if (const auto reward = m_progress->record(copy, result)) {
            if (reward->variant == 0 && m_announcer.loadVehicleUnlock(reward->vehicle)) {
                m_announcer.playUnlockVehicle();
                return;
            }
            if (reward->variant != 0 && m_announcer.loadTextureUnlock(reward->vehicle)) {
                m_announcer.playUnlockTexture();
                return;
            }
        }
        m_announcer.playResults(place, m_result.config.opponents);
    }

    // mmPopup::ProcessChat ("Enter Chat Msg"): the chat line pops up without
    // pausing the game (the pause music plays all the same).
    void openChat(Context& ctx) {
        m_popup = Popup::Chat;
        m_popupPaused = false;
        m_gameInput.flush(); // mmPopup::ProcessChat: mmInput::Flush, StopAllFF
        m_ff.stopAll();
        popupMusic(ctx, true);
        m_chatText.clear();
        ctx.input.startTextInput(ctx.window());
        m_textInput = true;
        buildPopup(ctx);
    }

    // The game's SendChatMessage. mmGame (single player) only checks for the
    // "/blubber" cheat: the cheat flag, the elasticity cap 4 and elasticity 4
    // on the player's bound. mmGameMulti::ParseChatMessage keeps "/rc ..."
    // (debug commands) to itself, sends "/wav ..." without posting it, and
    // sends and posts anything else (OpenMM2's session echoes the line back
    // and the race posts it then).
    void sendChatMessage(Context& ctx, const std::string& text) {
        if (!multiplayer(ctx)) {
            if (text.starts_with("/blubber")) {
                game::session::setCheating(true);
                phys::setElasticityCap(phys::kBlubberElasticityCap);
                if (m_player)
                    m_player->sim().setBoundElasticity(phys::kBlubberElasticityCap);
            }
            return;
        }
        if (text.size() > 4 && text.starts_with("/rc"))
            return;
        ctx.netGame->sendChat(text);
    }

    // mmGameMulti's chat message (0x1f8): "name: text" on the HUD; the
    // player's own lines (echoed by the session) as typed.
    void postIncomingChat(Context& ctx) {
        if (!multiplayer(ctx) || !m_hud)
            return;
        for (const auto& line : ctx.netGame->chat()) {
            if (line.serial < m_chatSeen || line.system || line.text.starts_with("/wav"))
                continue;
            const bool own = line.from == ctx.netGame->localId();
            m_hud->postChat(own ? line.text : std::format("{}: {}", line.name, line.text));
            // mmGameMulti::GameMessageCB 0x1f8: another player's line comes
            // with mmHUD::PlayNetAlert.
            if (!own)
                playGameSound(ctx, game::session::GameSound::NetAlert, 0.0f);
        }
        m_chatSeen = ctx.netGame->chatSerial();
    }

    // mmMultiRoam / Race / Circuit / Blitz / CR::SystemMessage 0x2d: a
    // player who leaves while the game runs gets "<name>" / "has left the
    // game" (38) for 5 s at the bottom with mmHUD::PlayNetAlert.
    // The network race's start on this machine (game::NetRaceStart): the
    // loaded report, "Waiting for N players" while others still load, the
    // countdown on the shared start, and a cruise's "<name> has joined".
    void updateNetStart(Context& ctx, float dt) {
        if (!m_netStart)
            return;
        const auto out = m_netStart->update(dt, *ctx.netGame);
        m_netHeld = out.held;
        m_netToGo = out.secondsToGo;
        if (!m_session)
            return;
        // mmMulti*::UpdateGame state 0: mmHUD::SetMessage(text, 5.0, 0)
        // every frame while the count is above 0.
        if (out.waitingFor > 0) {
            const auto& strings = ctx.game->strings;
            m_session->showMessage(game::NetRaceStart::waitingText(strings, out.waitingFor), 5.0f, false);
        }
        // mmGameMulti::GameMessageCB 0x1fa: the net alert, the name for 5 s
        // and "has joined" (string 42) under it.
        for (const auto id : out.joined) {
            const auto* p = ctx.netGame->player(id);
            if (!p)
                continue;
            playGameSound(ctx, game::session::GameSound::NetAlert, 0.0f);
            m_session->showMessage(p->name, 5.0f, false);
            m_session->showMessage2(ctx.game->strings.get(42, "has joined"));
        }
    }

    void updateNetPlayers(Context& ctx) {
        if (!multiplayer(ctx) || !m_session)
            return;
        std::map<std::uint8_t, std::string> now;
        for (const auto& p : ctx.netGame->players())
            if (!m_netLeft.contains(p.id))
                now[p.id] = p.name;
        // The race modes say it while racing (state 3); Cops and Robbers in
        // any state (mmMultiCR::SystemMessage 0x2d).
        if (m_netPlayersKnown && (m_session->phase() == game::session::Phase::Racing ||
                                  m_result.config.mode == game::GameMode::CopsAndRobbers)) {
            for (const auto& [id, name] : m_netPlayers) {
                if (now.contains(id))
                    continue;
                m_session->showMessage(name, 5.0f, false);
                m_session->showMessage2(ctx.game->strings.get(38, "has left the game"));
                playGameSound(ctx, game::session::GameSound::NetAlert, 0.0f);
            }
        }
        m_netPlayers = std::move(now);
        m_netPlayersKnown = true;
    }

    // --- Cops and Robbers (mmMultiCR) ------------------------------------------------------

    game::session::CrTeam crTeam(Context& ctx, const std::string& vehicle, int lobbyTeam) const {
        // mmMultiCR::InitMyPlayer: Cops vs. Robbers and Free-For-All take the
        // team from the car (vehicle flag 0x08, a police car: team 0); Robber
        // Teams from the lobby. (OpenMM2's lobby gives Cops vs. Robbers
        // players the car of their team.)
        using game::session::CrTeam;
        const auto* info = ctx.game->catalog.vehicle(vehicle);
        const bool police = info && (info->flags & 0x08);
        switch (m_result.config.copsAndRobbers) {
        case game::CopsAndRobbersMode::RobberTeams: return lobbyTeam == 0 ? CrTeam::Blue : CrTeam::Red;
        case game::CopsAndRobbersMode::CopsVsRobbers: return lobbyTeam == 0 || police ? CrTeam::Cop : CrTeam::Robber;
        default: return police ? CrTeam::Cop : CrTeam::Robber;
        }
    }

    void setupCopsAndRobbers(Context& ctx) {
        if (!multiplayer(ctx) || m_result.config.mode != game::GameMode::CopsAndRobbers)
            return;
        auto locations = game::session::loadCrLocations(ctx.game->vfs, m_city->info.raceDir);
        if (!locations) {
            // mmMultiCR::LoadCSV: with fewer than three rows every place
            // comes from the AI intersections.
            log::info("race: no Cops and Robbers places for {}: intersections only", m_city->info.raceDir);
            locations = game::session::CrLocations{};
        }
        game::session::CrSettings st;
        st.mode = m_result.config.copsAndRobbers;
        st.goldMass = ctx.netGame->goldMass();
        st.timeLimitSeconds = m_result.config.timeLimitMinutes * 60.0f;
        st.pointLimit = m_result.config.pointLimit;
        // mmMultiCR::UpdateLimit: the host decides the limits and tells the
        // others (sync review S6).
        st.limitsFromHost = !ctx.netGame->isHost();
        // Every machine starts with the same places (OpenMM2: the time the
        // host ordered the race seeds them, which every machine knows when it
        // loads; the host's sets follow by message).
        st.seed = std::max(1u, ctx.netGame->raceOrderTime());
        // Development aid: OPENMM2_DEBUG_CR_SEED=<n> on every machine draws the
        // same first places in every run (automated runs).
        if (const char* seed = std::getenv("OPENMM2_DEBUG_CR_SEED"))
            st.seed = std::max(1u, static_cast<std::uint32_t>(str::parseInt(seed).value_or(1)));
        m_crRng = st.seed;
        // GetRandomPoints' picker: mmGame::RespawnXYZ(false, false, false)
        // less its 2 m: an intersection whose room (FindRoomId of its
        // centre) is neither water of death nor a terrain instance's (level
        // flags 0x24). MM2's host draws it from the one global stream as its
        // frames left it, respawnCounter() + 1 numbers a pick; every OpenMM2
        // machine draws the first set itself from the shared start time, one
        // number a pick, since the counter is each machine's own.
        st.randomIntersection = [this]() -> std::optional<Vec3> {
            const auto pick = game::session::respawnXYZ(
                *m_city, [this](const Vec3& p) { return m_cityRenderer ? m_cityRenderer->roomAt(p) : 0; }, {},
                m_crRng, 1);
            if (!pick)
                return std::nullopt;
            return pick->position - Vec3{0.0f, 2.0f, 0.0f};
        };
        // mmMultiCR::DropGold: aiMap::PositionToAIMapComp always answers for
        // a room, so the gold stays where it fell unless that room is deep
        // water (level flag 0x04).
        st.canDropAt = [this](const Vec3& p) { return !(levelRoomFlagsAt(p) & city::LevelRoomFlag::WaterOfDeath); };
        // mmMultiCR::FindGround: the wheels' probe from 2 m above to 10 m below.
        st.findGround = [this](const Vec3& p) {
            phys::RayHit hit;
            if (m_world && m_world->wheelProbe(p + Vec3{0.0f, 2.0f, 0.0f}, p - Vec3{0.0f, 10.0f, 0.0f}, hit, nullptr,
                                               nullptr))
                return hit.position;
            return p;
        };
        auto roomOf = [this](const Vec3& p) { return m_cityRenderer ? m_cityRenderer->roomAt(p) : 0; };
        st.sameRoom = [roomOf](const Vec3& gold, const Vec3& car) {
            return roomOf(gold + Vec3{0.0f, 1.5f, 0.0f}) == roomOf(car);
        };
        st.baseReachable = [this, roomOf](const Vec3& base, const Vec3& car) {
            const Vec3 at = base + Vec3{0.0f, 3.75f, 0.0f};
            constexpr int kCovered = city::LevelRoomFlag::Subterranean | city::LevelRoomFlag::Covered;
            return (levelRoomFlagsAt(at) & kCovered) != 0 || roomOf(at) == roomOf(car);
        };
        m_cr = std::make_unique<game::session::CopsAndRobbers>(st, *locations);
        m_crSelf = ctx.netGame->localId();
        // Every machine counts each player with the car it drives
        // (NetGame::playerCar: Cops vs. Robbers' cars by team).
        for (const auto& p : ctx.netGame->players()) {
            m_cr->addCar(p.id, crTeam(ctx, ctx.netGame->playerCar(p.id).vehicle, p.team));
            m_crPlayers.insert(p.id);
        }
        m_crMyTeam = m_cr->teamOf(m_crSelf);
        m_regen = true; // mmMultiCR::InitMyPlayer: mmPlayer::EnableRegen(1)
        // mmSpeechContainer::InitCNR loads the Cops and Robbers lines; build
        // 3393 never plays them (nothing calls mmCNRSpeech::Play).
        if (m_announcerOk || ctx.settings.commentary)
            m_announcer.beginCopsAndRobbers();
    }

    // mmMultiCR::FillResults: the game's scores for the results page.
    game::RaceResult crResult(Context& ctx) const {
        game::RaceResult r = m_session->result();
        r.ended = true;
        std::vector<frontend::CrResultPlayer> players;
        for (const auto& p : ctx.netGame->players())
            players.push_back({p.name, m_cr->playerScore(p.id), p.id == m_crSelf});
        const auto& strings = ctx.game->strings;
        r.standings = frontend::crResultRows(
            m_result.config.copsAndRobbers, m_cr->score(game::session::CrTeam::Cop),
            m_cr->score(game::session::CrTeam::Robber), players,
            [&strings](std::uint32_t id, const char* fallback) { return strings.get(id, fallback); });
        return r;
    }

    void updateCopsAndRobbers(Context& ctx, float dt) {
        if (!m_cr || !m_player || !m_session)
            return;
        using game::session::CopsAndRobbers;
        const bool host = ctx.netGame->isHost();
        // mmPlayer::UpdateRegen while regeneration is on: the car's input
        // carries it (net::kInputRegen; game::NetCarDriver).
        // mmMultiCR::UpdateGame state 2 starts the clock (mmTimer::Start) when
        // it enables the racers. OpenMM2's cars go at the shared start time,
        // and every machine runs its rules on the session clock from there,
        // so the time limit runs out on all of them together (each counting
        // frames from its own load did not agree by the loads' difference).
        const double sinceStart =
            ctx.netGame->frameTime() - static_cast<double>(ctx.netGame->raceStartTime());
        const double raceClock = ctx.netGame->raceStartKnown() ? std::max(0.0, sinceStart / 1000.0) : 0.0;
        const float ruleDt = static_cast<float>(std::max(0.0, raceClock - m_crClock));
        m_crClock = std::max(m_crClock, raceClock);
        // mmMultiCR::SystemMessage 0x2d: a player left.
        std::vector<std::uint8_t> left;
        {
            std::set<std::uint8_t> now;
            for (const auto& p : ctx.netGame->players())
                if (!m_netLeft.contains(p.id))
                    now.insert(p.id);
            for (const auto id : m_crPlayers)
                if (!now.contains(id))
                    left.push_back(id);
            m_crPlayers = std::move(now);
        }
        const Vec3 own = m_player->sim().body.ics.matrix.m3;
        if (host) {
            // OpenMM2: the host simulates every player's car, so it runs
            // mmMultiCR's rules for each as that car's own machine did
            // (CopsAndRobbers::updateHost) and tells everyone
            // (game/net/NetRules). mmMultiCR::HitWaterHandler /
            // DropThruCityHandler drop the gold back at its place when the
            // water handler fires (5 s in the water, or a fall out of the
            // city): this car's, or a reset another player's car took.
            std::vector<CopsAndRobbers::Car> cars;
            const bool ownWater = std::exchange(m_crWaterHandled, false);
            cars.push_back({m_crSelf, m_crMyTeam, own, m_playerState.wrecked, ownWater, {}});
            for (const auto& [id, rv] : m_remotes)
                if (rv.simulated && rv.placed && rv.sim && !m_netLeft.contains(id))
                    cars.push_back({id, m_cr->teamOf(id), rv.sim->sim().body.ics.matrix.m3,
                                    rv.sim->sim().damage.wrecked(), m_crResets.contains(id), {}});
            m_crResets.clear();
            auto decisions = m_cr->updateHost(ruleDt, cars, m_crImpacts);
            for (const auto id : left) {
                const auto dropped = m_cr->playerLeft(id, true);
                decisions.insert(decisions.end(), dropped.begin(), dropped.end());
            }
            for (const auto& d : decisions)
                if (d.type == CopsAndRobbers::Message::Type::GoldDelivered && d.car != m_crSelf && d.car >= 0)
                    repairRemoteCar(static_cast<std::uint8_t>(d.car));
            m_netRules.hostDecided(decisions);
        } else {
            // The host's word came with the frame's events
            // (receiveNetRules); this machine predicts only its own car
            // taking free gold, which the host confirms or undoes.
            // The other cars where the host will have them when it runs
            // this machine's sample: the ones simulated here with its car
            // (predictNearCars) as they are, the others run on from where
            // they are drawn (a playout delay and this car's lead behind).
            std::vector<CopsAndRobbers::Car> cars;
            cars.push_back({m_crSelf, m_crMyTeam, own, m_playerState.wrecked, false, {}});
            const double hostMs = ctx.netGame->frameTime() + m_netCarLead.value_or(0.0);
            for (const auto& rc : m_remoteCars) {
                if (!rc.hasState)
                    continue;
                const bool wrecked = (rc.flags & net::kVehicleWrecked) != 0;
                if (const auto it = m_remotes.find(rc.id);
                    it != m_remotes.end() && it->second.predicted && it->second.sim) {
                    const auto& ics = it->second.sim->sim().body.ics;
                    cars.push_back({rc.id, m_cr->teamOf(rc.id), ics.matrix.m3, wrecked, false,
                                    ics.linearVelocity});
                    continue;
                }
                const float ahead = static_cast<float>(std::clamp((hostMs - rc.time) * 0.001, 0.0, 0.5));
                cars.push_back({rc.id, m_cr->teamOf(rc.id), rc.transform.m3 + rc.velocity * ahead, wrecked,
                                false, rc.velocity});
            }
            int players = 0;
            for (const auto& p : ctx.netGame->players())
                players += m_netLeft.contains(p.id) ? 0 : 1;
            if (m_cr->updatePredicted(ruleDt, cars.front(), cars, players))
                m_netRules.predictedPickup(m_prediction.nextSeq() - 1);
            m_crWaterHandled = false;
        }
        m_crImpacts.clear();
        if (m_autopilot) // OPENMM2_DEBUG_AUTOPILOT: to the gold, then to the base
            m_autopilot->setTarget(m_player->sim(),
                                   m_cr->goldCarrier() == m_crSelf ? m_cr->deliveryTarget(m_crMyTeam)
                                                                   : m_cr->goldPosition(),
                                   m_world.get());
        // What happened, on this machine's HUD and car.
        auto name = [&](int id) {
            const auto* p = ctx.netGame->player(static_cast<std::uint8_t>(id));
            return p ? p->name : std::string();
        };
        const auto& s = ctx.game->strings;
        std::FILE* trace = ctx.netGame->traceFile();
        for (const auto& e : m_cr->takeEvents()) {
            using E = CopsAndRobbers::EventType;
            const bool me = e.car == m_crSelf;
            if (e.type != E::TimeWarning)
                game::traceCopsEvent(trace, ctx.netGame->frameTime(), static_cast<int>(e.type), e.car,
                                     e.value, m_cr->goldPosition());
            switch (e.type) {
            case E::GoldTaken:
                if (me) {
                    // StealGold: the mass, no regeneration (below), "You
                    // have the Gold!" (OpenMM2: a client shows it when it
                    // predicts the pickup, MM2 when the host's 0x25a came).
                    m_session->showMessage(s.get(host ? 115 : 134, "You have the Gold!"), 5.0f, false);
                } else {
                    m_session->showMessage(std::format("{} {}", name(e.car), s.get(135, "has the Gold!")), 5.0f, false);
                }
                break;
            case E::GoldDropped:
                if (me) {
                    // Only ImpactCallback says so; the wreck and the water
                    // drop it without a line.
                    if (e.value == 1)
                        m_session->showMessage(s.get(112, "You dropped the gold!"), 5.0f, false);
                } else {
                    // GameMessage 0x259: with mmHUD::PlayNetAlert.
                    m_session->showMessage(std::format("{} {}", name(e.car), s.get(136, "dropped the Gold!")), 5.0f,
                                           false);
                    playGameSound(ctx, game::session::GameSound::NetAlert, 0.0f);
                }
                break;
            case E::GoldDelivered:
                if (me) {
                    // UpdateBank / UpdateHideout: regeneration, a repaired
                    // car (OpenMM2: the host repairs the car it simulates,
                    // and a client's follows its states).
                    if (host)
                        netCommand({0, net::CarCommandKind::ClearDamage, {}, 0.0f});
                    clearVehicleDamage();
                    m_session->showMessage(s.get(117, "Gold delivered!"), 5.0f, false);
                } else {
                    // GameMessage 600: with mmHUD::PlayNetAlert.
                    m_session->showMessage(std::format("{} {}", name(e.car), s.get(137, "delivered the Gold!")),
                                           5.0f, false);
                    playGameSound(ctx, game::session::GameSound::NetAlert, 0.0f);
                }
                break;
            case E::TimeWarning: {
                // UpdateTimeWarning: 20, 15, 10, 5, 1 minutes (138-142).
                static constexpr std::pair<int, std::uint32_t> kIds[] = {{20, 138}, {15, 139}, {10, 140}, {5, 141}, {1, 142}};
                // DisplayTimeWarning: 2 s at the bottom.
                for (const auto& [minutes, id] : kIds)
                    if (minutes == e.value)
                        m_session->showMessage(s.get(id, ""), 2.0f, false);
                break;
            }
            case E::TimeUp:
            case E::PointLimit:
                // SendLimitReached: the host tells the others (its rules
                // message). UpdateLimit: the message for 3 s, then 3 s to the results
                // (state 9).
                m_session->showMessage(s.get(e.type == E::TimeUp ? 118 : 119, ""), 3.0f, false);
                m_crEnd = 3.0f;
                break;
            case E::NewSet: break;
            case E::PickupUndone:
                log::info("race: the host did not give this car the gold it reached");
                break;
            }
        }
        // mmMultiCR::FondleCarMass and mmPlayer::EnableRegen: the gold's mass
        // and throttle cap on the car that carries it, the regeneration on
        // every other (this machine's car takes them with its inputs; the
        // host's own rules set them on the cars it simulates).
        {
            const bool carrying = m_cr->goldCarrier() == m_crSelf;
            const float kg = carrying ? m_cr->carrierExtraMassKg() : 0.0f;
            m_netGold = static_cast<std::uint16_t>(
                std::clamp(std::lround(kg), 0L, static_cast<long>(net::kMaxExtraMass)));
            m_throttleCap = carrying && kg > 0.0f ? m_cr->carrierThrottleCap() : 1.0f;
            m_regen = !carrying;
        }
        if (trace) {
            std::vector<std::pair<int, int>> scores;
            for (const auto& p : ctx.netGame->players())
                scores.emplace_back(p.id, m_cr->playerScore(p.id));
            if (scores != m_tracedScores) {
                game::traceCopsScores(trace, ctx.netGame->frameTime(), scores);
                m_tracedScores = std::move(scores);
            }
        }
        if (m_crEnd >= 0.0f) {
            m_crEnd -= dt;
            if (m_crEnd < 0.0f) {
                // FillResults; mmPlayer +0x2258; ShowResults.
                m_crFinished = true;
                leaveRace(ctx, crResult(ctx));
                return;
            }
        }
        // The objects and readouts (mmWaypointObject, mmArrow, mmCRHUD).
        if (m_hud) {
            game::session::CrDisplay d;
            d.enabled = true;
            d.time = static_cast<float>(m_time);
            // The carrier does not see the gold it carries (StealGold and
            // 0x25a deactivate the gold's waypoint for it); the others see it
            // above the carrier.
            if (m_cr->goldActive() || (m_cr->goldCarrier() >= 0 && m_cr->goldCarrier() != m_crSelf))
                d.gold = m_cr->goldPosition();
            d.goldOnMap = m_cr->goldPosition(); // mmHudMap::DrawCopsnRobbers
            const bool teams = m_result.config.copsAndRobbers != game::CopsAndRobbersMode::FreeForAll;
            const bool colours = m_result.config.copsAndRobbers == game::CopsAndRobbersMode::RobberTeams;
            d.bases.push_back({colours ? "pt_blue" : "pt_bank", m_cr->set().bank});
            d.bases.push_back({colours ? "pt_red" : "pt_hideout", m_cr->set().hideout});
            d.arrowInterest = m_cr->goldCarrier() == m_crSelf ? m_cr->deliveryTarget(m_crMyTeam) : m_cr->goldPosition();
            d.teams = teams;
            d.copsVsRobbers = m_result.config.copsAndRobbers == game::CopsAndRobbersMode::CopsVsRobbers;
            d.blueScore = m_cr->score(game::session::CrTeam::Cop);
            d.redScore = m_cr->score(game::session::CrTeam::Robber);
            d.playerScore = m_cr->playerScore(m_crSelf);
            d.timeLeft = m_cr->timeRemaining();
            // mmCRHUD::SetName (red on team 1), AddPlayer / SetScore /
            // ActivateRosterGold for the others, ActivateGold while the
            // player carries the gold.
            d.playerName = ctx.netGame->player(static_cast<std::uint8_t>(m_crSelf))
                               ? ctx.netGame->player(static_cast<std::uint8_t>(m_crSelf))->name
                               : std::string();
            d.playerRed = teams && m_crMyTeam == game::session::CrTeam::Robber;
            d.carryingGold = m_cr->goldCarrier() == m_crSelf;
            for (const auto& p : ctx.netGame->players()) {
                if (p.id == m_crSelf)
                    continue;
                game::session::CrDisplay::RosterEntry r;
                r.name = p.name;
                r.score = m_cr->playerScore(p.id);
                r.color = game::session::hud::netIconColor(p.id);
                if (teams)
                    r.color = m_cr->teamOf(p.id) == game::session::CrTeam::Robber ? 0xFFEF0000u : 0xFF0000EFu;
                r.gold = m_cr->goldCarrier() == p.id;
                d.roster.push_back(std::move(r));
            }
            m_hud->setCopsAndRobbers(std::move(d));
        }
    }

    // --- The in-race popup (mmPopup, PUMain, PUExit) ---------------------------------------

    // buildPopup enables the page (MenuManager::EnablePU).
    void openPopup(Context& ctx, bool pause, Popup page = Popup::Main) {
        m_popup = page;
        // ProcessEscape: pauses unless the game already is (the full-screen
        // map), and remembers it so closing does not resume it.
        m_popupPaused = pause && !multiplayer(ctx) && !m_paused;
        m_ff.stopAll(); // mmPopup::ProcessEscape: mmInput::StopAllFF and Flush
        m_gameInput.flush();
        if (m_popupPaused)
            m_paused = true;
        popupMusic(ctx, true);
        buildPopup(ctx);
    }

    // mmGame::UpdateDebugInput's other keys (with no popup up): F4 restarts
    // the race (asRoot::Reset and mmReplayManager's reset; OpenMM2: single
    // player, as the menu's Restart), F6 opens the roster in a network game
    // (mmPopup::ShowRoster) and Ctrl+Alt+Shift+F7 the chat line in single
    // player (where "/blubber" is typed).
    void debugKeys(Context& ctx) {
        using platform::Key;
        const auto& in = ctx.input;
        if (in.keyPressed(Key::F4) && m_session && !multiplayer(ctx)) {
            // asRoot::Reset ends a pause at once (the full-screen map's; the
            // map stays up), then the reset waits for the next frame.
            m_paused = false;
            requestRestart();
            return;
        }
        if (in.keyPressed(Key::F6) && multiplayer(ctx)) {
            // ShowRoster: no pause, no pause music.
            m_popupPaused = false;
            showPopupPage(ctx, frontend::PopupPage::Roster);
            return;
        }
        const bool ctrl = in.keyDown(Key::LCtrl) || in.keyDown(Key::RCtrl);
        const bool alt = in.keyDown(Key::LAlt) || in.keyDown(Key::RAlt);
        const bool shift = in.keyDown(Key::LShift) || in.keyDown(Key::RShift);
        if (in.keyPressed(Key::F7) && ctrl && alt && shift && !multiplayer(ctx))
            openChat(ctx);
    }

    // mmReplayManager's reset flag (+0x19), which Restart Race, F4 in the
    // menu and F4 in the game set: mmReplayManager::Update acts on it at the
    // start of the next frame.
    void requestRestart() {
        m_restartPending = true;
        m_resultsShown = false;
    }

    // PUMain's Restart (and F4 over it, mmPopup::Update): the reset flag and
    // DisablePU(0), the menu closed without the return music (mmGame::Reset
    // starts the music again).
    void restartFromMenu(Context& ctx) {
        closePopup(ctx, false);
        requestRestart();
    }

    // mmReplayManager::Update with the reset flag: mmReplayManager::Reset
    // (the seed back to 1) and the tree's Reset, which reaches the mode's
    // Reset: the session and every object of the race back to the start
    // before anything of this frame updates. mmGame::Reset's aiMap::Reset
    // sets MM2's stream (m_random) to 1 again (ai::World::reset, once per
    // restart) and the next AI step places the traffic and pedestrians from
    // it round the start; OpenMM2's other streams reseed in their own
    // resets.
    void applyRestart(Context& ctx) {
        m_restartPending = false;
        if (!m_session)
            return;
        m_session->restart();
        handleSessionEvents(ctx);
    }

    // MenuManager::Switch from one page of the open popup to another.
    void switchPopup(Context& ctx, Popup page) {
        m_popup = page;
        buildPopup(ctx);
    }

    // mmPopup::DisablePU(returnMusic) (MenuManager::DisablePU, mmInput::Flush).
    void closePopup(Context& ctx, bool returnMusic) {
        m_popup = Popup::None;
        m_gameInput.flush();
        // Buttons close the popup from inside its update: keep the menu
        // until the next frame.
        if (m_popupMenu)
            m_popupGraveyard.push_back(std::move(m_popupMenu));
        if (m_popupPaused)
            m_paused = false;
        m_popupPaused = false;
        if (returnMusic)
            popupMusic(ctx, false);
    }

    // mmPopup::PlayPauseMusic / PlayReturnMusic: the song's pause segment on
    // the next beat, and back to the segment before it. With CITY SOUNDS the
    // ambience segment stops (StopSegment(0)) and starts again (PlaySegment).
    void popupMusic(Context& ctx, bool pause) {
        auto* music = ctx.music();
        if (!music)
            return;
        if (m_musicDirector) {
            if (pause)
                m_musicDirector->pause();
            else
                m_musicDirector->resume();
            for (const auto& c : m_musicDirector->takeCommands())
                music->setState(c.state, c.timing);
        }
        if (!m_ambienceStopped)
            music->setAmbience(pause ? std::string_view{} : std::string_view(m_result.config.city));
    }

    // mmPopup::Lock: the single-player race modes (states 4 and 5) and the
    // crash course lock the main menu once the race is over: "Resume
    // Driving" is off and Escape does nothing, or shows the results when the
    // race-over flag (mmGame +0x7c) is set. Cruise and the multiplayer modes
    // never lock it; a restart unlocks it (mmPopup::Reset).
    bool popupLocked(Context& ctx) const {
        if (multiplayer(ctx) || !m_session)
            return false;
        const auto phase = m_session->phase();
        return phase == game::session::Phase::PostRace || phase == game::session::Phase::Done;
    }

    // mmPlayerConfig::GetViewSettings when the game ends: the driver keeps
    // the camera, wide angle, dashboard and mirror choices.
    void storeViewSettings() {
        if (!m_profile)
            return;
        const auto v = m_cams.viewSettings();
        m_profile->camera = v.camera;
        m_profile->wideAngle = v.wideAngle;
        m_profile->dashboard = v.dashboard;
        m_profile->mirror = m_mirror.enabled();
        if (!m_profile->save())
            log::warn("race: cannot save driver '{}'", m_profile->name);
    }

    // The quit button: back to the race menu (or the crash course page),
    // without the results.
    void quitToMenu(Context& ctx) {
        game::RaceResult r = m_session ? m_session->result() : m_result;
        r.ended = false;
        // A joiner who quits leaves the others driving (the host's quit ends
        // the race for everyone, leaveRace): they take its car out and stop
        // waiting for its finish, as MM2's players did when one left the
        // session (mmGameMulti::QuitNetwork, SystemMessage 0x2d).
        if (multiplayer(ctx) && !ctx.netGame->isHost())
            ctx.netGame->sendLeftRace();
        leaveRace(ctx, r);
    }

    // This frame's game events from the others, for the race and Cops and
    // Robbers rules; the players who quit the race are noted here.
    void takeNetEvents(Context& ctx) {
        m_netEvents = ctx.netGame->takeGameEvents();
        // OpenMM2: the network cars' damage (game/net/DamageSync).
        m_netDamage.setVerbose(m_debugNetDamage);
        m_netDamage.receive(m_netEvents, ctx.netGame->frameTime());
        for (const auto& ev : m_netEvents)
            if (ev.type == net::GameEventType::LeftRace && ev.from != ctx.netGame->localId()) {
                log::info("race: player {} quit the race", ev.from);
                m_netLeft.insert(ev.from);
            }
    }

    // What a network race leaves with when the host ends it (or the session
    // ends): a race this machine has already finished or lost, or a Cops and
    // Robbers game whose limit it has announced, shows its results as at its
    // own ending. The host ends the race once every player is counted
    // (mmMultiRace / mmMultiCircuit 0x211, which takes every machine to its
    // results); the last finisher, still in its post-race wait, used to go
    // back without them.
    game::RaceResult netRaceResult(Context& ctx) const {
        if (m_resultsShown || !m_session)
            return m_result;
        if (m_cr && m_crEnd >= 0.0f)
            return crResult(ctx);
        const auto phase = m_session->phase();
        if (phase == game::session::Phase::PostRace || phase == game::session::Phase::Done)
            return m_session->result();
        return m_result;
    }

    // Back to the menus, the view settings stored before the frontend reads
    // the driver again. The host of a network game takes everyone back to
    // the lobby (mmGameMulti::BeDone(1): Quit2Lobby, 0x20c).
    void leaveRace(Context& ctx, const game::RaceResult& result) {
        storeViewSettings();
        if (multiplayer(ctx) && ctx.netGame->isHost()) {
            const auto phase = ctx.netGame->phase();
            if (phase == game::NetGame::Phase::Countdown || phase == game::NetGame::Phase::Racing) {
                log::info("race: the host leaves the race: everyone back to the lobby");
                ctx.netGame->returnToLobby();
            }
        }
        ctx.nextScreen = makeFrontendScreen(ctx, result);
    }

    // The multiplayer races' standings (mmGameMulti::UpdateScore's icons):
    // OpenMM2's host decides the finishes and ranks every car
    // (game/net/NetRules); the other players' icons show its numbers.
    void updateNetRace(Context& ctx) {
        const auto mode = m_result.config.mode;
        if (!multiplayer(ctx) || !m_session ||
            !(mode == game::GameMode::Blitz || mode == game::GameMode::Circuit || mode == game::GameMode::Checkpoint))
            return;
        std::vector<game::session::Session::NetRacer> racers;
        for (const auto& p : ctx.netGame->players()) {
            if (p.id == ctx.netGame->localId() || m_netLeft.contains(p.id))
                continue;
            game::session::Session::NetRacer r;
            r.name = p.name;
            const auto it = m_remotes.find(p.id);
            r.present = it != m_remotes.end() && it->second.sim;
            if (r.present)
                r.position = it->second.sim->sim().body.ics.matrix.m3;
            r.finished = m_netRules.finished(p.id);
            r.hostPlace = m_netRules.iconPlace(p.id);
            racers.push_back(std::move(r));
        }
        m_session->setNetRacers(std::move(racers));
    }

    // --- OpenMM2: the rules the host decides (game/net/NetRules) ---------------------------

    void beginNetRules(Context& ctx) {
        if (!multiplayer(ctx) || !m_session)
            return;
        const auto mode = m_result.config.mode;
        game::NetRules::Setup s;
        s.host = ctx.netGame->isHost();
        s.self = ctx.netGame->localId();
        s.race = m_netRace;
        const bool race = mode == game::GameMode::Blitz || mode == game::GameMode::Circuit ||
                          mode == game::GameMode::Checkpoint;
        if (race)
            s.session = m_session.get();
        else
            s.cops = m_cr.get();
        for (const auto& p : ctx.netGame->players())
            s.players.push_back(p.id);
        s.sampleSeconds = phys::kFixedSampleStep;
        m_netRules.begin(s);
        // Development aid: OPENMM2_DEBUG_AUTOPILOT=<line>[,<m/s>] drives this
        // machine's car along the race's opponent line through its inputs
        // (game/net/Autopilot.h), for automated runs.
        // In Cops and Robbers it drives to the gold, then to its base.
        if (const char* ap = std::getenv("OPENMM2_DEBUG_AUTOPILOT"); ap && m_player && m_world) {
            const auto parts = str::split(ap, ',');
            const int line = static_cast<int>(str::parseInt(parts.empty() ? "" : parts[0]).value_or(0));
            const float speed =
                parts.size() > 1 ? static_cast<float>(str::parseDouble(parts[1]).value_or(0.0)) : 0.0f;
            std::string error;
            const auto& setup = m_session->setup();
            const auto& files = ctx.game->vfs;
            m_autopilot = s.session ? game::NetAutopilot::create(*m_city, files, setup, m_player->sim(), line,
                                                                 speed, m_world.get(), &error)
                                    : game::NetAutopilot::createFree(*m_city, files, setup, speed, &error);
            if (m_autopilot)
                log::info("race: the autopilot drives opponent line {}", line);
            else
                log::warn("race: no autopilot: {}", error);
        }
        // Development aid: OPENMM2_DEBUG_START_GOLD=<metres> starts a Cops and
        // Robbers car that far from the first gold, facing it, each player on
        // its own side, facing each other (every machine knows the first places when it loads).
        if (const char* g = std::getenv("OPENMM2_DEBUG_START_GOLD"); g && m_cr && m_player && m_world) {
            const float r = static_cast<float>(str::parseDouble(g).value_or(30.0));
            const float around = 3.1415927f * static_cast<float>(ctx.netGame->localId());
            const Vec3 gold = m_cr->set().gold;
            Vec3 at = gold + Vec3{r * std::cos(around), 0.0f, r * std::sin(around)};
            phys::RayHit hit;
            if (m_world->wheelProbe(at + Vec3{0.0f, 30.0f, 0.0f}, at - Vec3{0.0f, 30.0f, 0.0f}, hit, nullptr,
                                    nullptr))
                at.y = hit.position.y + 1.0f;
            const float angle = std::atan2(-(gold.x - at.x), -(gold.z - at.z));
            log::info("race: starts {:.0f} m from the gold at ({:.1f}, {:.1f}, {:.1f})", r, at.x, at.y, at.z);
            m_player->setResetPos(at, angle);
            m_player->reset();
            m_pose = m_player->pose();
            m_cams.reset(cameraTarget());
        }
    }

    // Client: the host's word on the rules among the frame's events.
    void receiveNetRules(Context& ctx) {
        if (!multiplayer(ctx) || !m_netRules.active())
            return;
        for (const auto& p : ctx.netGame->players())
            m_netRules.setName(p.id, p.name);
        m_netRules.setTrace(ctx.netGame->traceFile(), ctx.netGame->frameTime());
        if (!ctx.netGame->isHost() && m_player) {
            m_netRules.receive(m_netEvents, m_player->sim().body.ics.matrix.m3);
            m_netRules.receiveStates(ctx.netGame->takeRulesStates(), m_player->sim().body.ics.matrix.m3);
        }
    }

    // Host: its referee's frame, its word on its own player, each player's message.
    void sendNetRules(Context& ctx, float dt) {
        if (!multiplayer(ctx) || !m_netRules.active() || !ctx.netGame->isHost() || !m_player)
            return;
        for (const auto id : m_netRules.players())
            if (m_netLeft.contains(id) || !ctx.netGame->player(id))
                m_netRules.playerLeft(id);
        m_netRules.hostUpdate(dt, net::monotonicMs(), m_player->sim().body.ics.matrix.m3,
                              [&ctx](std::uint8_t player, std::vector<std::byte> payload) {
                                  ctx.netGame->sendEvent(net::kRulesEvent, std::move(payload), player);
                              },
                              [&ctx](std::uint8_t player, const net::RulesStateMsg& state) {
                                  ctx.netGame->sendRulesState(player, state);
                              });
    }

    // Host: after every physics sample, every player's car for the referee.
    void netRulesSample() {
        if (!m_player || !m_traceNet)
            return;
        ++m_netOwnSample;
        m_netRules.hostSample(m_traceNet->localId(), m_netOwnSample, m_player->sim().body.ics.matrix,
                              m_player->sim().params.inertiaBox, (m_netInput.flags & net::kInputHeld) != 0);
        for (const auto& [id, rv] : m_remotes)
            if (rv.simulated && rv.placed && rv.sim)
                m_netRules.hostSample(id, rv.inputs.lastApplied(), rv.sim->sim().body.ics.matrix,
                                      rv.sim->sim().params.inertiaBox,
                                      (rv.input.flags & net::kInputHeld) != 0);
    }

    // Host: the gold's mass, its throttle cap and the regeneration on a
    // player's car it simulates are its own rules' (who carries the gold),
    // whatever that player's input says (mmMultiCR::FondleCarMass,
    // mmPlayer::EnableRegen).
    void hostGoldInput(std::uint8_t id, net::CarInputFrame& frame) const {
        if (!m_cr)
            return;
        const bool carrying = m_cr->goldCarrier() == id;
        const float kg = carrying ? m_cr->carrierExtraMassKg() : 0.0f;
        const long mass = std::clamp(std::lround(kg), 0L, static_cast<long>(net::kMaxExtraMass));
        frame.extraMass = static_cast<std::uint16_t>(mass);
        const long cap = std::lround(m_cr->carrierThrottleCap() * 255.0f);
        frame.throttleCap =
            carrying && kg > 0.0f ? static_cast<std::uint8_t>(std::clamp(cap, 0L, 255L)) : std::uint8_t{255};
        frame.flags = carrying ? static_cast<std::uint8_t>(frame.flags & ~net::kInputRegen)
                               : static_cast<std::uint8_t>(frame.flags | net::kInputRegen);
    }

    // Host: 600 on every machine (mmMultiCR::GameMessage): the deliverer's
    // car repaired (vehCar::ClearDamage), here on the car the host simulates.
    void repairRemoteCar(std::uint8_t id) {
        const auto it = m_remotes.find(id);
        if (it == m_remotes.end() || !it->second.sim)
            return;
        const net::CarCommand repair{it->second.inputs.lastApplied(), net::CarCommandKind::ClearDamage, {},
                                     0.0f};
        game::NetCarDriver::command(*it->second.sim, repair);
        hostCarReset(id, it->second);
    }

    void buildPopup(Context& ctx) {
        // mmPopup(game, 0.2, 0.1, 0.6, 0.8): the popup card covers x 0.2-0.8
        // and y 0.1-0.9 of the screen. PUMain's buttons sit at 0.125, 0.25,
        // 0.375 and 0.5 of it, "Resume Driving" (PUMenuBase::AddExit) at
        // x 0.5, y 0.9; PUExit's question at 0.2 and Yes / No at 0.7.
        // PUMenuBase buttons are 0.1 of the card high (+0xa4) in GetFont 24.
        const auto& s = ctx.game->strings;
        const bool crash = m_result.config.mode == game::GameMode::CrashCourse;
        const bool net = multiplayer(ctx);
        if (m_popupMenu)
            m_popupGraveyard.push_back(std::move(m_popupMenu));
        if (m_popup == Popup::Options) {
            // PUOptions and its pages (frontend::PopupOptions).
            if (!m_popupOptions)
                m_popupOptions = std::make_unique<frontend::PopupOptions>(ctx);
            m_popupMenu = m_popupOptions->build(m_popupPage, popupOptionsHost(ctx));
            return;
        }
        m_popupMenu = std::make_unique<ui::Menu>();
        auto& menu = *m_popupMenu;
        menu.popupSounds = true;
        const ui::Box card = frontend::popup::kCard;
        auto button = [&](float x, float y, float w, float h, std::string label, int type, std::function<void()> fn)
            -> ui::TextButton& { return frontend::popup::addButton(menu, card, x, y, w, h, std::move(label), type, std::move(fn)); };
        if (m_popup == Popup::Chat) {
            // PUChat (mmPopup::mmPopup: 0.75 wide, the popup line height
            // high, which PUMenuBase::PUMenuBase centres on the screen; the
            // x and y mmPopup passes are unused): one text field of up to
            // 40 characters filling it, no title, no label.
            const float lineHeight = ui::style::kPopupLineHeight;
            auto& entry = menu.add<ui::TextEntry>(
                ui::Box{(1.0f - 0.75f) * 0.5f * 640.0f, 240.0f - lineHeight * 0.5f, 0.75f * 640.0f, lineHeight},
                &m_chatText, 40);
            entry.popup = true;
            entry.onCommit = [this, &ctx] {
                // mmPopup::ChatCB: an empty line just closes it; either way
                // DisablePU(0), so the pause music keeps playing (as MM2).
                const std::string text = m_chatText;
                closePopup(ctx, false);
                if (!text.empty())
                    sendChatMessage(ctx, text);
            };
            menu.setInitialFocus(&entry);
            entry.beginEdit();
            menu.onBack = [this, &ctx] { closePopup(ctx, true); };
        } else if (m_popup == Popup::Main) {
            // PUMain: Resume Driving first (AddExit, type 1), then the rows
            // across the card (type 2).
            const bool locked = popupLocked(ctx);
            auto& resume = button(0.5f, 0.9f, 0.5f, 0.1f, s.get(473, "Resume Driving"), 1,
                                  [this, &ctx] { closePopup(ctx, true); });
            // PUMenuBase::DisableExit while locked.
            resume.enabled = !locked;
            auto& restart = button(0.0f, 0.125f, 1.0f, 0.1f,
                                   crash ? s.get(655, "Restart Lesson") : s.get(464, "Restart Race"), 2,
                                   [this, &ctx] { restartFromMenu(ctx); });
            // PUMain::RestartRO: no restart in a network game.
            restart.enabled = !net;
            // mmPopup::Update, PUMain id 0xb: the OPTIONS pages (menu 5).
            button(0.0f, 0.25f, 1.0f, 0.1f, s.get(466, "Options"), 2,
                   [this, &ctx] { showPopupPage(ctx, frontend::PopupPage::Options); });
            // mmPopup::Update, PUMain id 0xd: the host of a network race
            // gets PUQuit (menu 2); everyone else leaves.
            button(0.0f, 0.375f, 1.0f, 0.1f, crash ? s.get(656, "Back to School") : s.get(468, "Quit to Race Menu"), 2,
                   [this, &ctx, net] {
                       if (net && ctx.netGame->isHost())
                           showPopupPage(ctx, frontend::PopupPage::Quit);
                       else
                           quitToMenu(ctx);
                   });
            // PUMain's exit (id 0xe): the game ends at once (the exit flag and
            // mmGame::BeDone, which stores the driver's settings). Nothing
            // switches to PUExit's question (menu 3).
            button(0.0f, 0.5f, 1.0f, 0.1f, s.get(469, "Exit to Windows"), 2, [this, &ctx] {
                storeViewSettings();
                ctx.quit = true;
            });
            menu.setInitialFocus(locked ? &restart : &resume);
            menu.unlight(); // MenuManager::EnablePU: nothing lit on entry
            menu.onBack = [this, &ctx] {
                if (!popupLocked(ctx)) {
                    closePopup(ctx, true);
                } else if (m_session && m_session->raceOver()) {
                    // The race is over with a finish: Escape shows the results.
                    closePopup(ctx, false);
                    m_resultsShown = true;
                    m_result = m_session->result();
                    leaveRace(ctx, m_result);
                }
            };
        }
    }

    // mmPopup::Update's switches between PUMain and the OPTIONS pages, and
    // what those pages change in the running race.
    void showPopupPage(Context& ctx, std::optional<frontend::PopupPage> page) {
        m_popup = page ? Popup::Options : Popup::Main;
        if (page)
            m_popupPage = *page;
        buildPopup(ctx);
    }

    frontend::PopupOptionsHost popupOptionsHost(Context& ctx) {
        frontend::PopupOptionsHost host;
        host.graphicsChanged = [this, &ctx] { applyGraphicsOptions(ctx); };
        host.controlsChanged = [this, &ctx] { applyControlOptions(ctx); };
        host.show = [this, &ctx](std::optional<frontend::PopupPage> page) { showPopupPage(ctx, page); };
        // mmPopup::DisablePU(1): the key map's and the roster's Resume
        // Driving and Escape, with the return music.
        host.close = [this, &ctx] { closePopup(ctx, true); };
        // PUKey: mmIO::GetDescription of the control the race reads for a slot.
        host.keyText = [this, &ctx](int slot) {
            const auto& s = ctx.game->strings;
            auto string = [&s](std::uint32_t id, const char* fallback) { return s.get(id, fallback); };
            return controls::describe(m_gameInput.binding(static_cast<controls::Action>(slot)), string);
        };
        // PUQuit: everyone back to the lobby, or the session ended (as
        // host, NetGame::leave ends it for everyone).
        host.quitToLobby = [this, &ctx] {
            if (ctx.netGame)
                ctx.netGame->returnToLobby();
            quitToMenu(ctx);
        };
        host.endSession = [this, &ctx] {
            if (ctx.netGame)
                ctx.netGame->leave();
            quitToMenu(ctx);
        };
        host.roster = [&ctx] {
            std::vector<frontend::PopupOptionsHost::RosterEntry> out;
            if (!ctx.netGame)
                return out;
            const auto self = ctx.netGame->localId();
            for (const auto& p : ctx.netGame->players())
                if (p.id == self)
                    out.push_back({p.id, p.name, p.host});
            for (const auto& p : ctx.netGame->players())
                if (p.id != self)
                    out.push_back({p.id, p.name, p.host});
            return out;
        };
        host.boot = [&ctx](std::uint8_t id) {
            if (ctx.netGame && ctx.netGame->isHost() && id != ctx.netGame->localId())
                ctx.netGame->kick(id);
        };
        return host;
    }

    // mmPopup::ProcessKeymap: F1 opens the key map (PUKey), pausing like
    // Escape when nothing is up; over another popup page it switches to it;
    // on the key map it closes the popup.
    void processKeymap(Context& ctx) {
        if (m_popup == Popup::Options && m_popupPage == frontend::PopupPage::KeyMap) {
            closePopup(ctx, true);
            return;
        }
        if (m_popup == Popup::None)
            openPopup(ctx, true);
        showPopupPage(ctx, frontend::PopupPage::KeyMap);
    }

    // The in-race GRAPHICS OPTIONS (PUGraphics) as mmGame's callbacks apply
    // them: FarClipCB (the far plane), SetLevelGraphics (the sky, lighting
    // quality, cloud shadows, environment maps) and
    // lvlLevel::SetObjectDetail.
    void applyGraphicsOptions(Context& ctx) {
        const auto& ini = ctx.settings.ini;
        m_objectDetail = std::clamp(static_cast<int>(ini.getInt("Graphics", "ObjectDetail", 3)), 0, 3);
        m_detail.objects = game::ObjectDetail::forLevel(m_objectDetail);
        m_envOptions.lightQuality = static_cast<int>(std::clamp(ini.getInt("Graphics", "LightingQuality", 3), 0LL, 3LL));
        m_envOptions.farClip =
            static_cast<float>(std::clamp(ini.getDouble("Graphics", "FarClip", 1000.0), 100.0, 1000.0));
        m_envOptions.cloudShadows = static_cast<int>(std::clamp(ini.getInt("Graphics", "CloudShadows", 2), 0LL, 2LL));
        m_envOptions.texturedSky = ini.getBool("Graphics", "TexturedSky", true);
        applyEnvironment();
        auto update = [&](game::VehicleRenderer* r) {
            if (r)
                setupVehicleRenderer(ctx, *r);
        };
        update(m_vehicle.get());
        update(m_trailer.get());
        for (auto& o : m_opponents)
            update(o.renderer.get());
        for (auto& c : m_cops)
            update(c.renderer.get());
        for (auto& [id, rv] : m_remotes) {
            update(rv.renderer.get());
            update(rv.trailer.get());
        }
    }

    // The in-race CONTROL OPTIONS (PUControl): mmInput::Init with the chosen
    // controller, which falls back to the keyboard when a joystick type is
    // chosen without a joystick ("Default config invalid: no such device"),
    // and the sensitivity and dead zone mmInput reads.
    void applyControlOptions(Context& ctx) {
        m_controlOptions = controls::Options::load(ctx.settings.ini);
        using controls::Controller;
        const bool stick = !ctx.input.joysticks().empty() || !ctx.input.gamepads().empty();
        if (m_controlOptions.controller != Controller::Mouse && m_controlOptions.controller != Controller::Keyboard &&
            !stick)
            m_controlOptions.controller = Controller::Keyboard;
    }

    // OPENMM2_POPUP_SCRIPT: opens popup pages and presses keys in the race.
    void stepPopupScript(Context& ctx) {
        if (!m_popupScript || !m_popupScript->active())
            return;
        const auto step = m_popupScript->step();
        if (!step.open.empty()) {
            if (m_popup == Popup::None || m_popup == Popup::Chat)
                openPopup(ctx, true);
            using frontend::PopupPage;
            const std::string& p = step.open;
            if (p == "options" || p == "audio" || p == "control" || p == "graphics" || p == "keymap" ||
                       p == "quit" || p == "roster") {
                showPopupPage(ctx, p == "audio"      ? PopupPage::Audio
                                   : p == "control"  ? PopupPage::Control
                                   : p == "graphics" ? PopupPage::Graphics
                                   : p == "keymap"   ? PopupPage::KeyMap
                                   : p == "quit"     ? PopupPage::Quit
                                   : p == "roster"   ? PopupPage::Roster
                                                     : PopupPage::Options);
            } else {
                showPopupPage(ctx, std::nullopt);
            }
        }
        if (step.key != platform::Key::Unknown)
            frontend::injectKey(ctx, step.key);
    }

    // MenuManager::CheckInput while the popup is up.
    void updatePopup(Context& ctx, double dt) {
        if (!m_popupMenu)
            return;
        if (!m_popupSounds)
            m_popupSounds = std::make_unique<frontend::PopupSounds>(ctx);
        const render::UiLayout layout = render::computeUiLayout(ctx.device().outputExtent(), ctx.display.uiScale);
        const ui::NavInput nav = m_nav.read(ctx.input, layout, dt);
        ui::UiFrame f{*ctx.overlay, m_ui, m_text, nav, m_time, m_popupSounds->fn()};
        m_popupMenu->update(f); // a button may replace or close it (see m_popupGraveyard)
    }

    void drawPopup(Context& ctx) {
        if (!m_popupMenu)
            return;
        auto& ov = *ctx.overlay;
        ov.begin(ctx.display.uiScale);
        const ui::NavInput none;
        ui::UiFrame f{ov, m_ui, m_text, none, m_time};
        // The popup card (MenuManager::AdjustPopupCard, Card2D::Cull). The
        // chat line has none (its text field draws its own). PUMain has no
        // title (PUMenuBase::CreateTitle(0) adds none).
        const ui::Box card = frontend::popup::kCard;
        if (m_popup == Popup::Options && m_popupOptions)
            m_popupOptions->draw(m_popupPage, f);
        else if (m_popup != Popup::Chat)
            frontend::popup::drawCard(ov, card);
        m_popupMenu->drawContent(f);
        frontend::drawMenuPointer(ctx, f); // sfPointer, last
        ov.end();
    }

    // The street props and the gizmos, with the start of MM2's global random
    // stream in mmGame::Init's order: cityLevel::Load places the street
    // props, setting the seed to 1 before every road, so the stream is left
    // as the last road's walk leaves it; mmPlayer::Init's vehCar::Init then
    // draws for the car's siren flares and splash; mmGame::InitGizmos for the
    // sailboats, ferries and parked cars. aiMap::Init draws next (loadAi).
    void loadWorldObjects(Context& ctx) {
        // Inferred: without the propulator's files the stream would be
        // whatever the menus left it at; OpenMM2 takes 1.
        std::uint32_t state = 1;
        if (m_world) {
            m_bangers = std::make_unique<game::bangers::BangerSet>(*m_bangerData);
            // With the race's own props (race/<city>/<mode><N>.pathset).
            m_bangers->add(game::bangers::placeCityProps(
                *m_city, ctx.game->vfs, *m_bangerData,
                game::bangers::racePropsName(m_result.config.mode, m_result.config.raceIndex), &state));
        }
        m_random.seed(state);
        // mmPlayer::Init -> vehCar::Init (the trailer, a vehTrailer, draws
        // nothing).
        const std::uint32_t playerFlares = game::takeVehCarInitDraws(m_random);
        if (m_vehicle)
            m_vehicle->setSirenFlares(playerFlares);
        if (m_world) {
            // mmGame::InitGizmos: sailboats, drawbridges, tube trains,
            // ferries, and the parked cars (props) along the streets.
            m_gizmos = game::world::initGizmos(ctx.game->vfs, *m_city, m_result.config, multiplayer(ctx),
                                               *m_bangers, *m_bangerData, m_cityLevel.get(), m_random);
        }
    }

    void loadAi(Context& ctx) {
        const auto mode = m_result.config.mode;
        // mmMultiBlitz / mmMultiCircuit / mmMultiRace::Init clear mmGame +0x277
        // before mmGameMulti::Init: mmGame::Init then skips aiMap::Init, so a
        // multiplayer race has no traffic, pedestrians, police, racers or
        // light sets.
        if (multiplayer(ctx) &&
            (mode == game::GameMode::Blitz || mode == game::GameMode::Circuit || mode == game::GameMode::Checkpoint))
            return;
        ai::Settings settings;
        settings.trafficDensity = m_result.config.trafficDensity;
        settings.pedestrianDensity = m_result.config.pedestrianDensity;
        // -pedpool: aiCityData's pool, over the city's [Ped Pool] (a negative
        // number, which MM2 would not survive, counts as none).
        if (ctx.commandLine.pedPool)
            settings.maxPeds = std::max(0, *ctx.commandLine.pedPool);
        // mmGameMulti::Init: no traffic (nor cops, racers or rail cars) in
        // multiplayer cruise and Cops and Robbers; the pedestrians stay, at
        // the host's density, each machine its own. OpenMM2 extra (the host's
        // lobby option, on by default): in cruise the host runs the traffic
        // at its traffic density, and the police, for every player
        // (sendNetTraffic); the clients run none and show the host's
        // (updateNetTraffic).
        if (multiplayer(ctx) && !netTrafficHost(ctx))
            settings.trafficDensity = 0.0f;
        // mmSingleStunt::LoadEventFile sets the traffic density to the last
        // event's AmbDensity before aiMap::Init reads it.
        if (m_session && !m_session->setup().lessonEvents.empty())
            settings.trafficDensity = m_session->setup().lessonEvents.back().ambientDensity;
        // The SHOW PEDESTRIANS graphics option (mmStatePack +0x64): aiMap::Init
        // creates no pedestrians without it.
        if (!ctx.settings.ini.getBool("Graphics", "ShowPedestrians", true))
            settings.pedestrianDensity = 0.0f;
        // aiMap::Init: no pedestrians in circuit races; the winter models in snow.
        if (m_result.config.mode == game::GameMode::Circuit)
            settings.pedestrianDensity = 0.0f;
        settings.winterPeds = m_result.config.weather == game::Weather::Snow;
        // aiMap::Init draws on MM2's global stream: the racers' cars first
        // (aiRouteRacer::Init -> vehCar::Init; spawnOpponents makes them),
        // then the ambient pool and the pedestrians (ai::World::create).
        m_racerInitState = m_random.state();
        if (m_session)
            m_random.discard(static_cast<int>(m_session->opponents().size()) * game::kVehCarInitDraws);
        settings.random = &m_random;
        std::string error;
        const city::AiMapConfig* raceMap =
            m_session && m_session->setup().aiMap ? &*m_session->setup().aiMap : nullptr;
        m_ai = ai::World::create(*m_city, ctx.game->vfs, settings, raceMap, &error);
        if (!m_ai) {
            log::warn("race: AI unavailable: {}", error);
            return;
        }
        m_ai->setLightsDeferred(true); // updated after the racers and police
        m_ai->setStepObserver([this] { game::recordAiStep(m_drawnAi, *m_ai); });
        m_aiRenderer = std::make_unique<game::AiRenderer>(ctx.device(), *m_textures, *m_models, ctx.game->vfs);
        m_aiRenderer->setInterpolation(&m_drawnAi);
        m_aiRenderer->setEnhancedCarLighting(tweakOn(drawnTweaks(ctx.settings.ini, ctx.originalLook), "EnhancedCarLighting")); // OpenMM2 tweak
        m_aiRenderer->setCarShine(sliderValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), "OtherCarShine"));
        if (m_cityRenderer)
            m_aiRenderer->setRooms(&m_cityRenderer->rooms()); // cityLevel::DrawRooms' room gates
        m_aiRenderer->setGroundProbe([this](const Vec3& from, const Vec3& to, Vec3& point, Vec3& normal) {
            phys::RayHit hit;
            if (!m_world || !m_world->probe(from, to, hit))
                return false;
            point = hit.position;
            normal = hit.normal;
            return true;
        });
        if (m_world) {
            // OpenMM2: a shared-traffic client's are the received cars,
            // which its own car may knock loose ahead of the host.
            if (netTrafficClient(ctx)) {
                m_netTrafficCars = std::make_unique<game::NetTrafficCars>();
                m_trafficBodies = std::make_unique<game::TrafficBodies>(*m_netTrafficCars, *m_world);
            } else {
                m_trafficBodies = std::make_unique<game::TrafficBodies>(*m_ai, *m_world);
            }
            m_trafficBodies->setWeatherFriction(weatherFriction());
            // aiVehicleActive's impacts, for the ambient cars' sounds.
            m_trafficBodies->setImpactCallback(
                [this](const game::TrafficImpact& e) { m_trafficImpacts.push_back(e); });
            // The rail cars are instances of the level's rooms.
            if (m_cityLevel)
                m_cityLevel->addSource(m_trafficBodies.get());
            m_ai->setProbe([this](const Vec3& from, const Vec3& to, Vec3& at) {
                phys::RayHit hit;
                if (!m_world->wheelProbe(from, to, hit, nullptr, nullptr))
                    return false;
                at = hit.position;
                return true;
            });
            // aiPedestrian's wall probe is dgPhysManager::Collide with the
            // wheels' mask: lvlSDL::CollideProbe's polygons and the objects
            // flagged 0x20. (MM2 keeps a segment cache per pedestrian whose
            // start room is the pedestrian's; here each probe finds its rooms
            // afresh.)
            m_ai->pedestrians().setProbe([this](const Vec3& from, const Vec3& to, Vec3& at) {
                phys::RayHit hit;
                if (!m_world->wheelProbe(from, to, hit, nullptr, nullptr))
                    return false;
                at = hit.position;
                return true;
            });
        }
        setupNetTraffic(ctx);
    }

    // aiMap's load lists the props standing in the city for the pedestrians
    // to step round (aiPath / aiIntersection::AddBangersToObsMap; inferred:
    // the race's own props are placed by then too).
    void loadPedestrianProps(Context& ctx) {
        if (!m_ai || !m_bangers)
            return;
        std::map<std::string, float> radii;
        auto modelRadius = [&](const std::string& model) {
            // lvlInstance::GetRadius of an unhit banger: its geometry set's
            // radius over the model's levels of detail.
            const auto it = radii.find(model);
            if (it != radii.end())
                return it->second;
            float radius = 0.0f;
            if (const auto bytes = ctx.game->vfs.readAll("geometry/" + str::lower(model) + ".pkg"))
                if (const auto pkg = asset::parsePkg(*bytes))
                    for (const auto& mesh : pkg->meshes)
                        if (mesh.part.empty())
                            radius = std::max(radius, mesh.radius());
            return radii.emplace(model, radius).first->second;
        };
        std::vector<ai::PedObstacle> props;
        for (const auto& inst : m_bangers->instances()) {
            ai::PedObstacle o;
            o.room = inst.room;
            const Mat34& m = inst.matrix;
            o.position = m.m3;
            if (inst.data) {
                const Vec3& cg = inst.data->cg;
                // aiBanger::Position: the centre of gravity's frame less the
                // data's CG offset.
                o.origin = {((m.m3.x - m.m0.x * cg.x) - m.m1.x * cg.y) - m.m2.x * cg.z,
                            ((m.m3.y - m.m0.y * cg.x) - m.m1.y * cg.y) - m.m2.y * cg.z,
                            ((m.m3.z - m.m0.z * cg.x) - m.m1.z * cg.y) - m.m2.z * cg.z};
                o.yRadius = inst.data->yRadius;
                o.impulseLimit2 = inst.data->impulseLimit2;
                o.drivable = (inst.data->collisionType & 0x20) != 0;
            } else {
                o.origin = m.m3;
            }
            o.modelRadius = modelRadius(inst.model);
            props.push_back(o);
        }
        const game::bangers::BangerSet* bangers = m_bangers.get();
        m_ai->pedestrians().setObstacles(std::move(props), [bangers](std::size_t i) {
            return i < bangers->instances().size() &&
                   bangers->instances()[i].state == game::bangers::BangerSet::State::Unhit;
        });
    }

    void loadEffects(Context& ctx) {
        m_effects.load(ctx.game->vfs);
        // Road decals (city/<map>/decals.pathset, dgRoadDecalInstance).
        if (auto bytes = ctx.game->vfs.readAll("city/" + str::lower(m_city->info.mapName) + "/decals.pathset"))
            if (auto set = city::parsePathSet(*bytes))
                m_roadDecals.load(*set);
        if (m_world && m_bangers && m_gizmos) {
            if (m_cityLevel)
                m_cityLevel->addSource(m_gizmos.get());
            if (m_bank && ctx.mixer)
                m_gizmos->loadAudio(ctx.game->vfs, *m_bank, *ctx.mixer, &m_audioSlots);
            // aiMap::Init: the traffic lights (aiTrafficLightSet::SetFourWay).
            addTrafficLightProps();
            // aiMap::Init: the cable cars, unless the network game cleared
            // the state pack's EnableCableCars (mmGameMulti::Init); their
            // draws follow the pedestrians'.
            if (m_ai && !multiplayer(ctx)) {
                m_cableCars = std::make_unique<game::world::CableCars>(*m_ai, *m_bangerData, *m_bangers);
                m_cableCars->create(m_random);
                m_cableCars->reset(*m_world);
                if (m_cityLevel)
                    m_cityLevel->addSource(m_cableCars.get());
                if (m_bank && ctx.mixer)
                    m_cableCars->loadAudio(*m_bank, *ctx.mixer, &m_audioSlots);
            }
            // The props are instances of the level's rooms.
            if (m_cityLevel)
                m_cityLevel->addSource(m_bangers.get());
            m_bangers->setWorld(m_world.get());
            // OpenMM2: a network race's props are the host's (game/net/NetProps).
            if (multiplayer(ctx)) {
                m_netLocalId = ctx.netGame->localId();
                m_netProps.setup(
                    *ctx.netGame, *m_bangers,
                    [this](const phys::Instance& other) { return propMover(&other); },
                    [this](std::uint8_t id) { return netCarAt(id); });
                m_netProps.setInRace([this](std::uint8_t id) { return !m_netLeft.contains(id); });
                m_netProps.setClientCars([this](const phys::Instance* b) { return propMoverCar(b); },
                                         [this](int id) { return hostCarState(id); },
                                         [this]() -> std::optional<Vec3> {
                                             if (!m_player)
                                                 return std::nullopt;
                                             return m_player->sim().body.ics.matrix.m3;
                                         });
            }
        }
        // aiMap::Init ends with the police cars (aiPoliceOfficer::Init ->
        // vehCar::Init; spawnPolice), after the subways (none in retail
        // data: both cities' [Subway] is commented out).
        m_policeInitState = m_random.state();
        if (m_player && m_vehicle) {
            m_vehicleFx = loadVehicleFx(ctx, m_result.config.vehicle, m_player->model(), *m_vehicle);
            m_player->sim().onImpactCallback = [this](const phys::CarImpact& impact) { playerImpact(impact); };
        }
        const auto w = m_result.config.weather;
        m_weather = std::make_unique<game::fx::Weather>(
            m_effects, w == game::Weather::Rain   ? game::fx::Weather::Kind::Rain
                       : w == game::Weather::Snow ? game::fx::Weather::Kind::Snow
                                                  : game::fx::Weather::Kind::None);
    }

    // Lighting, fog, texture variants and street shading for the race's time
    // and weather.
    void applyEnvironment() {
        m_env = game::makeEnvironment(*m_city, m_result.config.timeOfDay, m_result.config.weather, m_envOptions);
        // mmGame's texture variants: night textures (and darkening) at
        // night, wet (_fa) textures in rain.
        m_textures->setVariants(m_result.config.timeOfDay == game::TimeOfDay::Night,
                                m_result.config.weather == game::Weather::Rain);
        if (m_cityRenderer)
            m_cityRenderer->setEnvironment(m_env);
    }

    // mmGame::InitWeather's light flag: evening, night or fog turn the car
    // lights on.
    bool carLights() const {
        const auto t = m_result.config.timeOfDay;
        return t == game::TimeOfDay::Evening || t == game::TimeOfDay::Night || m_result.config.weather == game::Weather::Fog;
    }

    // Object Detail, reflections and the shadow's ground probe of a car renderer.
    void setupVehicleRenderer(Context& ctx, game::VehicleRenderer& r) {
        r.setDetail(m_detail.objects);
        if (m_cityRenderer)
            r.setRooms(&m_cityRenderer->rooms()); // cityLevel::DrawRooms' room gates
        r.setReflections(ctx.settings.ini.getBool("Graphics", "VehicleReflections", true));
        r.setEnhancedLighting(tweakOn(drawnTweaks(ctx.settings.ini, ctx.originalLook), "EnhancedCarLighting")); // OpenMM2 tweak, off by default
        const bool own = &r == m_vehicle.get() || &r == m_trailer.get();
        r.setShine(sliderValue(drawnTweaks(ctx.settings.ini, ctx.originalLook), own ? "CarShine" : "OtherCarShine"));
        r.setGroundProbe([this](const Vec3& from, const Vec3& to, Vec3& point, Vec3& normal) {
            phys::RayHit hit;
            if (!m_world || !m_world->probe(from, to, hit))
                return false;
            point = hit.position;
            normal = hit.normal;
            return true;
        });
    }

    // vehCar's effects (tracks, wheel particles, damage and exhaust smoke) for
    // one car: the .vehCarDamage particle fields over the engine smoke
    // defaults, the exhaust pivots of its model.
    std::unique_ptr<game::fx::VehicleEffects> loadVehicleFx(Context& ctx, const std::string& vehicle,
                                                            const asset::VehicleModel& model,
                                                            const game::VehicleRenderer& renderer) {
        game::fx::VehicleFxSetup setup;
        if (!m_sparkColors)
            m_sparkColors = game::fx::SparkLut::load(ctx.game->vfs);
        setup.sparkColors = *m_sparkColors;
        setup.shardTextures = renderer.materialTextures();
        if (auto bytes = ctx.game->vfs.readAll("tune/vehicle/" + vehicle + ".vehcardamage"))
            if (auto f = data::parseDat(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
                f && f->top())
                game::fx::loadBirthRule(*f->top(), setup.smokeRule);
        for (int i = 0; i < 2; ++i)
            if (const auto* pivot = model.pivot(std::format("exhaust{}", i)))
                setup.exhaust[static_cast<std::size_t>(i)] = pivot->origin;
        setup.rain = m_result.config.weather == game::Weather::Rain;
        return std::make_unique<game::fx::VehicleEffects>(m_effects, setup);
    }

    // Car parts that fly off as bangers (tune/banger/<car>_<part>):
    // vehBreakableMgr::Impact ejects the breakable nearest an impact of
    // 10000 or more; a wrecked car loses wheels, hubs and fenders by speed
    // (vehCarModel::EjectOneshot), thrown at 1.3 times its speed. Parts
    // without banger data stay on.
    // `recorder` (OpenMM2): the parts the others' machines take off too;
    // `owner` (OpenMM2): whose car it is in a network race, which names the
    // thrown parts across the machines (game/net/PropSync).
    void breakParts(game::fx::VehicleEffects& fx, game::VehicleRenderer& r, const phys::CarSim& sim,
                    const std::string& vehicle, game::DamageRecorder* recorder = nullptr,
                    std::optional<std::pair<net::PropOwner, int>> owner = {}) {
        const auto impacts = fx.takeImpacts();
        if (!m_bangers || !m_bangerData)
            return;
        const Mat34 body = sim.modelMatrix(); // vehBreakableMgr::Init: the car's matrix
        auto eject = [&](const game::VehicleRenderer::Breakable& b, float speed) {
            const std::uint32_t tag =
                owner ? netPartTag(owner->first, owner->second, b.part, r.paintjob()) : 0;
            if (!ejectCarPart(r, vehicle, b, body, speed, sim.body.room, tag))
                return false;
            if (recorder)
                recorder->part(m_netStateTime, b.part);
            return true;
        };
        for (const auto& impact : impacts)
            if (impact.value >= 10000.0f)
                if (auto b = r.nearestBreakable(impact.point))
                    eject(*b, 4.0f);
        if (sim.damage.enabled && sim.damage.params.maxDamage <= sim.damage.currentDamage)
            for (const auto& b : r.wreckParts(sim.speedMph(), m_ejectRand))
                eject(b, sim.speed() * 1.3f);
    }

    // vehBreakableMgr::Eject: part `b` of a car (its model matrix `body`)
    // thrown off at `speed` as a banger of its tune/banger data
    // (<car>_<part>); false (the part stays on) without data.
    bool ejectCarPart(game::VehicleRenderer& r, const std::string& vehicle,
                      const game::VehicleRenderer::Breakable& b, const Mat34& body, float speed, int room,
                      std::uint32_t tag = 0) {
        const auto* data = m_bangers && m_bangerData ? m_bangerData->find(vehicle + "_" + str::lower(b.part))
                                                     : nullptr;
        if (!data)
            return false;
        // vehBreakableMgr::Reset (the car's damage cleared) takes the
        // ejected part out of the world again (dgHitBangerInstance::Detach).
        r.setEjectedPartReset([this](std::size_t i) {
            if (m_bangers)
                m_bangers->detachHit(i);
        });
        r.detach(b.part, m_bangers->ejectPart(*data, vehicle, b.part, r.paintjob(),
                                              Mat34::translation(b.pivot) * body, speed, room, tag));
        return true;
    }

    // vehCar::UpdateTrack lays no tracks in the rooms gizBridge::Init flags
    // (level room flag 0x10: at the opening bridges).
    game::fx::VehicleFxContext vehicleFxContext(const phys::CarSim& sim) const {
        game::fx::VehicleFxContext c;
        c.tracksAllowed = (levelRoomFlagsAt(sim.body.ics.matrix.m3) & city::LevelRoomFlag::Bridge) == 0;
        return c;
    }

    void updateEffects(float dt) {
        // vehCarDamage::Update paints the first impact since the last frame
        // into the body (fxTexelDamage::ApplyDamage, TextelDamageRadius).
        // OpenMM2: with a `recorder` the other machines paint the same patch
        // (at the point as it travels, from the same random state).
        using Owner = std::optional<std::pair<net::PropOwner, int>>;
        auto paint = [this](game::fx::VehicleEffects& fx, game::VehicleRenderer& r, const phys::CarSim& sim,
                            const std::string& vehicle, game::DamageRecorder* recorder = nullptr,
                            Owner owner = {}) {
            if (auto p = fx.takeDamagePoint()) {
                const Vec3 point = recorder ? recorder->patch(m_netStateTime, *p, r.texelDamageState()) : *p;
                r.applyDamage(point, sim.damage.params.textelDamageRadius);
            }
            breakParts(fx, r, sim, vehicle, recorder, owner);
        };
        if (m_player && m_vehicleFx) {
            m_vehicleFx->update(dt, m_player->sim(), vehicleFxContext(m_player->sim()));
            if (m_traceNet && !m_traceNet->isHost()) {
                // OpenMM2: the host decides a network client's dents and
                // lost parts (updateOwnNetDamage).
                (void)m_vehicleFx->takeDamagePoint();
                (void)m_vehicleFx->takeImpacts();
            } else if (m_vehicle) {
                paint(*m_vehicleFx, *m_vehicle, m_player->sim(), m_player->model().baseName,
                      m_result.config.multiplayer ? &m_netDamage.own() : nullptr,
                      m_result.config.multiplayer ? Owner{{net::PropOwner::Player, m_netLocalId}} : Owner{});
            }
        }
        // OpenMM2: the network host's simulated players' cars, whose damage
        // everyone is sent.
        for (auto& [id, rv] : m_remotes)
            if (rv.simulated && rv.placed && rv.fx && rv.renderer && rv.sim) {
                rv.fx->update(dt, rv.sim->sim(), vehicleFxContext(rv.sim->sim()));
                paint(*rv.fx, *rv.renderer, rv.sim->sim(), rv.sim->model().baseName, &m_netDamage.player(id),
                      Owner{{net::PropOwner::Player, static_cast<int>(id)}});
            }
        for (auto& o : m_opponents)
            if (o.fx) {
                o.fx->update(dt, o.sim->sim(), vehicleFxContext(o.sim->sim()));
                paint(*o.fx, *o.renderer, o.sim->sim(), o.sim->model().baseName);
            }
        for (auto& c : m_cops) {
            if (c.fx) {
                c.fx->update(dt, c.sim->sim(), vehicleFxContext(c.sim->sim()));
                const int model = m_trafficCatalog.find(c.sim->model().baseName);
                paint(*c.fx, *c.renderer, c.sim->sim(), c.sim->model().baseName, c.damage,
                      m_trafficHost ? Owner{{net::PropOwner::Catalog, model}} : Owner{});
            }
            if (c.driver->siren())
                c.sirenAngle = std::fmod(c.sirenAngle + dt * 2.5f * 3.1415927f, 6.2831855f);
        }
        updateNetFx(dt); // OpenMM2: the network cars' damage smoke
        if (m_weather)
            m_weather->update(dt, m_camera.transform);
    }

    void loadAudio(Context& ctx) {
        if (!ctx.mixer)
            return;
        m_ctxMixer = ctx.mixer.get();
        m_bank = std::make_unique<audio::SoundBank>(ctx.game->vfs);
        m_bank->setQuality(ctx.settings.audioHighQuality ? audio::SoundBank::Quality::High
                                                         : audio::SoundBank::Quality::Low);
        audio::game::CarAudioOptions opts;
        opts.city = m_result.config.city;
        opts.weather = surfaceWeather();
        opts.manager = &m_audioSlots; // the tunnel echo state (Object3DManager::setTunnel)
        std::string error;
        m_carAudioOk = m_carAudio.load(ctx.game->vfs, *m_bank, *ctx.mixer, m_result.config.vehicle, opts, &error);
        if (!m_carAudioOk)
            log::warn("race: car audio: {}", error);
        // mmGame::Init: the session's pedestrian voice files (aiPedAudio).
        m_pedAudio.load(ctx.game->vfs, *m_bank, *ctx.mixer, &m_audioSlots);
        // mmPlayer::Init creates the city ambience only with CITY SOUNDS on.
        if (ctx.settings.citySounds)
            m_ambience.load(ctx.game->vfs, *m_bank, *ctx.mixer, m_result.config.city, &m_audioSlots);
        m_rain.load(*m_bank, *ctx.mixer, m_result.config.timeOfDay == game::TimeOfDay::Night);
        // mmPlayer::InitSpeechAudio, with COMMENTARY on: mmSpeechContainer
        // gives cruise and the races an mmRaceSpeech (InitRace), the crash
        // course an mmCCSpeech (InitCC); Cops and Robbers' mmCNRSpeech is not
        // wired (the mode is not playable yet).
        if (ctx.settings.commentary && m_announcer.load(ctx.game->vfs, *m_bank, *ctx.mixer, m_result.config.city)) {
            using audio::game::AnnouncerMode;
            std::optional<AnnouncerMode> mode;
            switch (m_result.config.mode) {
            case game::GameMode::Cruise: mode = AnnouncerMode::Cruise; break;
            case game::GameMode::Blitz: mode = AnnouncerMode::Blitz; break;
            case game::GameMode::Checkpoint: mode = AnnouncerMode::Checkpoint; break;
            case game::GameMode::Circuit: mode = AnnouncerMode::Circuit; break;
            default: break;
            }
            m_announcer.beginSession();
            if (mode)
                m_announcer.beginRace(*mode, m_result.config.vehicle, static_cast<int>(m_result.config.timeOfDay),
                                      static_cast<int>(m_result.config.weather));
            const bool lessons = m_result.config.mode == game::GameMode::CrashCourse;
            m_announcerOk = mode || (lessons && m_announcer.beginCrashCourse(std::max(0, m_result.config.raceIndex)));
        }
        // Impacts reported by the simulation feed the impact sounds.
        m_player->sim().onImpactCallback = [this](const phys::CarImpact& impact) { playerImpact(impact); };
    }

    // OPENMM2_NET_TRACE: every player's car as this frame draws it, for the
    // divergence between machines (netprobe syncreport). The own car's
    // velocity is its body's frame velocity, which includes the sample's
    // pushes (a contact's push-out shows as speed).
    void traceNetDrawn(Context& ctx) {
        if (!multiplayer(ctx) || !ctx.netGame->tracing())
            return;
        if (m_player)
            ctx.netGame->traceDrawn(ctx.netGame->localId(), true, m_drawPose.body,
                                    m_player->sim().body.ics.frameVelocity);
        for (const auto& rc : m_remoteCars)
            if (const auto drawn = netCarDrawn(rc.id)) {
                const auto it = m_remotes.find(rc.id);
                const bool here = it != m_remotes.end() && it->second.predicted && it->second.sim;
                ctx.netGame->traceDrawn(rc.id, false, *drawn,
                                        here ? it->second.sim->sim().body.ics.frameVelocity : rc.velocity);
            }
    }

    // vehCarDamage::ApplyImpact for the player's car: AudImpact (the impact
    // sounds), the damage effects, and the game's impact callback
    // (mmPlayer::ImpactCallback), which counts the hits.
    void playerImpact(const phys::CarImpact& impact) {
        // OpenMM2: a network client running its samples again has had them.
        if (m_netReplaying)
            return;
        if (m_traceNet)
            traceNetImpact(m_traceNet->localId(), impact); // OPENMM2_NET_TRACE
        // mmMultiCR::ImpactCallback (from vehCarDamage::ApplyImpact's damaging
        // branch): a hit from another player's car, its summed total.
        if (m_cr && impact.otherBody && impact.damaging)
            for (const auto& [id, rv] : m_remotes)
                if (rv.sim && &rv.sim->sim().body == impact.otherBody)
                    m_crImpacts.push_back({m_crSelf, id, impact.total});
        if (impact.sound)
            m_impacts.push_back({impact.soundStrength, impact.audioId, impact.position});
        if (m_vehicleFx)
            m_vehicleFx->impact(impact, m_player->sim());
        // OpenMM2: the others see it too (the host's: it decides every car's).
        if (m_result.config.multiplayer && impact.damaging && m_traceNet && m_traceNet->isHost())
            m_netDamage.own().impact(m_netStateTime, game::damageImpactOf(impact, m_player->sim()));
        if (impact.damaging && !m_netReplaying)
            if (auto* trace = m_netProps.trace()) // OPENMM2_NET_TRACE
                trace->impact(m_netStateTime, std::format("player{}", m_netLocalId), impactCause(impact),
                              impact.value, m_player->sim().damage.currentDamage);
        if (impact.damaging) {
            ++(impact.otherIsBody ? m_vehicleImpacts : m_objectImpacts);
            m_ff.impact(impact.total, m_player->sim().speedMph()); // mmPlayer::FFImpactCallback
        }
    }

    audio::game::SurfaceWeather surfaceWeather() const {
        const auto w = m_result.config.weather;
        return w == game::Weather::Snow   ? audio::game::SurfaceWeather::Snow
               : w == game::Weather::Rain ? audio::game::SurfaceWeather::Wet
                                          : audio::game::SurfaceWeather::Dry;
    }

    // Audio inputs shared by every simulated car (engine, gears, tyres).
    audio::game::CarAudioInputs carAudioInputs(const phys::CarSim& sim) const {
        audio::game::CarAudioInputs in;
        in.rpm = sim.engine.rpm;
        in.speed = sim.speed();
        in.gear = sim.trans.getCurrentGear();
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& w = sim.wheels[i];
            auto& wi = in.wheels[i];
            wi.onGround = w.onGround;
            // vehWheel's sliding amount (0 while the tyre grips), as
            // vehSurfaceAudio reads it.
            wi.slip = w.slide;
            wi.surface = w.material ? audio::game::surfaceSoundIndex(w.material->name, w.material->sound) : 0;
            wi.suspensionSpeed = w.suspensionVelocity;
            wi.brakeCoef = w.params.brakeCoef;
        }
        // vehSurfaceAudio::UpdateTireWobble: damage past MedDamage.
        const float damageRange = sim.damage.maxDamage() - sim.damage.medDamage();
        in.tireWobble = damageRange > 0.0f ? (sim.damage.currentDamage - sim.damage.medDamage()) / damageRange : 0.0f;
        in.wheelRadius = sim.wheels[2].radius;
        in.wrecked = sim.damage.wrecked();
        in.velocity = sim.body.ics.frameVelocity;
        in.inTunnel = m_tunnel; // audio flag 0x80: every car's surface sound
        return in;
    }

    void updateAudio(Context& ctx, float dt) {
        if (!m_player)
            return;
        const auto& sim = m_player->sim();
        // mmPlayer::Update: with the player's car in a room flagged 0x02
        // (subterranean) the audio flag 0x80 is set (the tunnel: surface
        // sounds, ambience areas, rain shelter, Aud3DObjectManager::EchoOn).
        // The room is the car's (its ICS position), not the camera's.
        m_tunnel = (levelRoomFlagsAt(sim.body.ics.matrix.m3) & city::LevelRoomFlag::Subterranean) != 0;
        // Aud3DObjectManager::EchoOn(0.5) / EchoOff: every positioned sound's
        // echo follows the same flag.
        m_audioSlots.setTunnel(m_tunnel);
        // MMDMusicManager::UpdateAmbientSFX: the city's ambience segment stops
        // underground (StopSegment(0)) and starts again outside (PlaySegment).
        if (auto* music = ctx.music(); music && !ctx.commandLine.noMusic && m_tunnel != m_ambienceStopped) {
            music->setAmbience(m_tunnel ? std::string_view{} : std::string_view(m_result.config.city));
            m_ambienceStopped = m_tunnel;
        }
        if (m_carAudioOk) {
            audio::game::CarAudioInputs in = carAudioInputs(sim);
            in.throttle = m_lastPedals.accelerator;
            in.brake = m_lastPedals.brake;
            in.impacts = std::move(m_impacts);
            m_impacts.clear();
            in.horn = hornDown(ctx);
            in.transform = m_pose.body;
            // vehSurfaceAudio::UpdateAir: something 3 to 33 m below ("big air";
            // its segments start 3 m down, so a closer surface is skipped).
            phys::RayHit hit;
            const Vec3 at = sim.modelMatrix().m3;
            if (m_world && m_world->probe(at - Vec3{0, 3, 0}, at - Vec3{0, 33, 0}, hit))
                in.groundBelow = at.y - hit.position.y;
            m_carAudio.silenceEngine(m_session && m_session->engineSilenced());
            m_carAudio.update(in, dt);
        }
        // The listener follows the camera.
        ctx.mixer->setListener(m_camera.transform, m_player->sim().body.ics.frameVelocity);
        updateAiAudio(dt);
        updateNetCopsAudio(ctx, dt);
        updateAmbientAudio(ctx, dt);
        updatePedestrianAudio(dt);
        // mmGame::Update's mmSpeechContainer::Update (AudSpeech::Update); the
        // frame's first two were AudManager::Update's (m_audioManager).
        if (m_announcerOk)
            m_announcer.update(dt);
        m_ambience.update(m_camera.transform, dt, m_tunnel);
        if (m_gizmos)
            m_gizmos->updateAudio(m_camera.transform, dt, m_tunnel);
        if (m_cableCars)
            m_cableCars->updateAudio(m_camera.transform, dt);
        // mmPlayer::SetCamera sets mmRainAudio's interior flag: on for the
        // hood camera (car view 1) and the dashboard, off for the others.
        const auto view = m_cams.view();
        const bool interior = view == game::PlayerCameras::View::Pov || view == game::PlayerCameras::View::Dash;
        m_rain.update(m_result.config.weather == game::Weather::Rain, interior, m_tunnel, dt);
    }

    // --- Shared traffic of a network cruise (OpenMM2 extra) ---------------------------------
    // MM2's network cruise has neither traffic nor police (mmGameMulti::Init).
    // With the host's lobby option on, the host runs both for every player
    // and sends each client the cars near it (game/net/TrafficSync,
    // docs/multiplayer.md "Shared traffic"); the clients draw, hear and
    // collide with them, and their pedestrians stay their own.

    bool netTraffic(Context& ctx) const {
        return multiplayer(ctx) && m_result.config.mode == game::GameMode::Cruise &&
               m_result.config.netTraffic;
    }
    bool netTrafficHost(Context& ctx) const { return netTraffic(ctx) && ctx.netGame->isHost(); }
    bool netTrafficClient(Context& ctx) const { return netTraffic(ctx) && !ctx.netGame->isHost(); }

    // The models a shared car can be (the traffic's types, then the police
    // posts' cars), and the host's or the client's side.
    void setupNetTraffic(Context& ctx) {
        if (!netTraffic(ctx) || !m_ai)
            return;
        std::vector<std::string> police;
        if (m_session)
            for (const auto& p : m_session->police())
                police.push_back(p.vehicle);
        m_trafficCatalog = game::buildTrafficCatalog(m_ai->traffic().types(), police);
        if (ctx.netGame->isHost()) {
            m_trafficHost.emplace();
        } else {
            m_trafficClient.emplace(m_trafficCatalog.size(), m_trafficCatalog.checksum());
            if (m_aiRenderer)
                m_aiRenderer->setCars(&m_netCarsDrawn);
            if (m_cityLevel) {
                m_trafficProxies = std::make_unique<game::TrafficProxies>(m_cityLevel.get());
                m_cityLevel->addSource(m_trafficProxies.get());
            }
            // The pedestrians shy away from the received cars that are out of
            // normal driving, as from the host's (aiPedestrian::Accident).
            m_ai->pedestrians().setAccidentQuery(
                [this](int node, int path, int) { return netAccidentAt(node, path); });
        }
        const char* role = ctx.netGame->isHost() ? "host" : "client";
        log::info("race: shared traffic, {} ({} models, catalog {:04x})", role, m_trafficCatalog.size(),
                  m_trafficCatalog.checksum());
    }

    // The other players as the host's traffic and police see them: the cars
    // it simulates (updateHostCars) with their players' controls.
    void collectNetPlayers(Context&) {
        m_netTrafficPlayers.clear();
        m_netTrackedPlayers.clear();
        for (const auto& rc : m_remoteCars) {
            const auto it = m_remotes.find(rc.id);
            if (!rc.hasState || it == m_remotes.end() || !it->second.sim || rc.id == 0 ||
                rc.id >= ai::kMaxTrafficPlayers)
                continue;
            const phys::CarSim& sim = it->second.sim->sim();
            ai::PlayerCar pc;
            pc.transform = sim.body.ics.matrix;
            pc.velocity = rc.velocity;
            pc.width = sim.params.inertiaBox.x;
            pc.length = sim.params.inertiaBox.z;
            pc.radius = sim.body.radius();
            pc.steering = rc.controls.steering;
            pc.reversing = rc.controls.gear < 0;
            pc.horn = (rc.flags & net::kVehicleHorn) != 0;
            m_netTrafficPlayers.push_back({rc.id, pc});
            ai::TrackedCar t = trackedCar(sim, kNetPlayerTrackedId + rc.id, true);
            t.velocity = rc.velocity;
            t.speed = std::abs(rc.velocity.dot(sim.body.ics.matrix.m2));
            t.suspect = true;
            t.reversing = rc.controls.gear < 0;
            if (m_ai)
                m_ai->map().trackPlayer(t);
            m_netTrackedPlayers.push_back(t);
        }
    }

    // Host: every 50 ms, each client its cars (the police chasing it, then
    // the nearest) with the light sets' steps.
    // Host: the session time the AI's cars are at after this frame's update
    // (its last fixed step), and the physics bodies' (the last simulation
    // step).
    double netAiStateTime(Context& ctx) const {
        return ctx.netGame->frameTime() -
               static_cast<double>(m_ai->interpolationAlpha() * ai::kAiStepSeconds) * 1000.0;
    }
    double netBodyStateTime(Context& ctx) const {
        const double lag = m_world ? static_cast<double>(m_world->remainder()) * 1000.0 : 0.0;
        return ctx.netGame->frameTime() - lag;
    }

    // OPENMM2_NET_TRACE on the host: the shared cars near any player, each at
    // the session time its state belongs to (the AI's last step, or the
    // physics step's for a body).
    void traceNetTraffic(Context& ctx, std::FILE* trace, std::span<const game::SharedCar> cars) {
        const double aiTime = netAiStateTime(ctx), bodyTime = netBodyStateTime(ctx);
        std::vector<Vec3> players;
        if (m_player)
            players.push_back(m_player->sim().modelMatrix().m3);
        for (const auto& rc : m_remoteCars) // the host's simulated players' cars
            if (rc.hasState)
                players.push_back(rc.transform.m3);
        // The AI's cars once per AI step; the bodies every frame.
        const bool aiStepped = m_ai->lightSteps() != m_traceAiSteps;
        m_traceAiSteps = m_ai->lightSteps();
        for (const game::SharedCar& c : cars) {
            const auto near = [&](const Vec3& p) { return p.dist2(c.transform.m3) < 9e4f; };
            if ((c.body || aiStepped) && std::ranges::any_of(players, near))
                game::traceHostCar(trace, c.body ? bodyTime : aiTime, c);
        }
    }

    // Host, with each client's CarStates (the same physics sample): the
    // police cars and the knocked traffic cars near its car in full
    // (net/TrafficFull.h), which the client simulates with its own. One
    // message a state (kTrafficFullBytes, 24 KB/s at most), nearest first (a
    // police car chasing the client counting half as far); a car sent the
    // last time is kept kFullKeep metres further, so that one at the edge
    // does not switch between being simulated and placed on the client.
    // Farther away a police car simulated on its driver's last controls
    // strayed more than one predicted along its velocity (chasing 60-100 m
    // behind: 2.2 m at the 90th percentile against 0.7 m), and it cannot
    // meet the client's car before its next state.
    static constexpr float kFullPoliceRadius = 60.0f; // m
    static constexpr float kFullBodyRadius = 50.0f;   // m
    static constexpr float kFullKeep = 20.0f;         // m
    void sendNetTrafficFull(Context& ctx) {
        if (!netTrafficHost(ctx) || !m_ai || !m_trafficBodies)
            return;
        for (const auto& rc : m_remoteCars) {
            if (!rc.hasState)
                continue;
            const Vec3 at = rc.transform.m3;
            std::set<int>& sent = m_trafficFullSent[rc.id];
            const auto within = [&](int id, float d, float radius) {
                return d <= radius + (sent.contains(id) ? kFullKeep : 0.0f);
            };
            std::vector<std::pair<float, net::TrafficFullCar>> cars;
            for (std::size_t i = 0; i < m_cops.size(); ++i) {
                const Cop& cop = m_cops[i];
                const int id = kNetPoliceId + static_cast<int>(i);
                // Not one the world's last step did not run (MM2 neither
                // updates nor collides a cop it does not declare, nor a body
                // beyond its 32 movers): it stands where it is, whatever its
                // velocity, and a client must not run it.
                if (!cop.sim || id >= static_cast<int>(net::kMaxAmbientIds) || !m_world ||
                    !m_world->isActive(&cop.sim->sim().body))
                    continue;
                const phys::CarSim& sim = cop.sim->sim();
                const float d = sim.body.ics.matrix.m3.dist(at);
                const int target = cop.driver->target();
                const bool chasing = target > kNetPlayerTrackedId &&
                                     target - kNetPlayerTrackedId == static_cast<int>(rc.id);
                if (!within(id, d, kFullPoliceRadius))
                    continue;
                net::TrafficFullCar f;
                f.id = static_cast<std::uint16_t>(id);
                f.generation =
                    static_cast<std::uint8_t>(static_cast<unsigned>(cop.resets) % net::kAmbientGenerations);
                f.kind = net::AmbientKind::Police;
                f.car = game::ownCarState(*cop.sim, static_cast<std::uint32_t>(cop.resets));
                // The controls its driver set (not what the car's own rules
                // made of them in the samples), on average over the frames
                // since the last message: the client runs it on them until
                // the next, and the driver's throttle and brake go on and
                // off from frame to frame (in chases the average met the
                // host's car 0.29 m off at the 90th percentile, the last
                // controls 0.49 m).
                std::array<float, 4> controls = cop.controls;
                if (cop.controlFrames > 0)
                    for (std::size_t k = 0; k < controls.size(); ++k)
                        controls[k] = cop.controlSum[k] / static_cast<float>(cop.controlFrames);
                f.throttle = controls[0];
                f.brake = controls[1];
                f.steering = controls[2];
                f.handBrake = controls[3];
                f.maxThrottle = sim.engine.maxThrottle;
                cars.emplace_back(chasing ? d * 0.5f : d, std::move(f));
            }
            for (const ai::AmbientCar& c : m_ai->cars()) {
                if (!c.physical || c.id < 0 || c.id >= kNetPoliceId)
                    continue;
                const auto state = m_trafficBodies->bodyState(c.id);
                if (!state)
                    continue;
                const float d = state->matrix.m3.dist(at);
                if (!within(c.id, d, kFullBodyRadius))
                    continue;
                net::TrafficFullCar f;
                f.id = static_cast<std::uint16_t>(c.id);
                f.generation =
                    static_cast<std::uint8_t>(static_cast<unsigned>(c.spawns) % net::kAmbientGenerations);
                f.kind = net::AmbientKind::Traffic;
                f.body = *state;
                cars.emplace_back(d, std::move(f));
            }
            std::ranges::sort(cars, {}, &std::pair<float, net::TrafficFullCar>::first);
            net::TrafficFullMsg msg;
            msg.time = m_netStateTime;
            sent.clear();
            for (auto& [d, f] : cars) {
                if (msg.cars.size() == net::kMaxTrafficFullCars)
                    break;
                msg.cars.push_back(std::move(f));
                if (net::encodeMessage(msg).size() > net::kTrafficFullBytes)
                    msg.cars.pop_back(); // a smaller one further away may fit
                else
                    sent.insert(msg.cars.back().id);
            }
            if (msg.cars.empty())
                continue;
            auto& st = m_trafficSent[rc.id];
            st.fullBytes += ctx.netGame->sendTrafficFull(rc.id, msg);
            st.fullCars += msg.cars.size();
        }
        for (Cop& cop : m_cops) {
            cop.controlSum = {};
            cop.controlFrames = 0;
        }
    }

    void sendNetTraffic(Context& ctx) {
        if (!m_trafficHost || !m_ai || !netTrafficHost(ctx))
            return;
        // aiGoalAvoidPlayer::Reset's horn (the car's horn flag, for the step
        // it started avoiding) waits for the next message.
        for (const ai::AmbientCar& c : m_ai->cars())
            if (c.horn)
                m_hornLatch.insert(c.id);
        // How each car on its rail is moving, from the AI's last two steps:
        // the clients predict it along its rail with that.
        m_railMotion.update(m_ai->cars(), netAiStateTime(ctx));
        const std::uint64_t now = net::monotonicMs();
        std::FILE* trace = ctx.netGame->traceFile(); // OPENMM2_NET_TRACE: every frame
        const bool send = now - m_trafficSentAt >= kNetTrafficIntervalMs;
        if (!send && !trace)
            return;
        if (send)
            m_trafficSentAt = now;
        std::vector<game::SharedCar> cars;
        cars.reserve(m_ai->cars().size() + m_cops.size());
        for (const ai::AmbientCar& c : m_ai->cars()) {
            const int model = m_trafficCatalog.find(c.model);
            if (model < 0 || c.id < 0 || c.id >= kNetPoliceId)
                continue;
            int paint = 0;
            if (m_aiRenderer) {
                const int jobs = m_aiRenderer->paintJobs(c.model);
                paint = jobs > 1 ? static_cast<int>(c.paint * static_cast<float>(jobs - 1)) : 0;
            }
            std::optional<game::TrafficBodyState> body;
            if (const Mat34* m = m_trafficBodies ? m_trafficBodies->transformOf(c.id) : nullptr) {
                body.emplace();
                body->transform = *m;
                m_trafficBodies->motionOf(c.id, body->velocity, body->angularVelocity);
                // A body that stands still is shared standing (game::StillBodies).
                if (m_stillBodies.still(c.id, m->m3, netBodyStateTime(ctx))) {
                    body->velocity = {};
                    body->angularVelocity = {};
                }
                // Its wheels while the body is simulated (aiVehicleInstance::Draw).
                if (const auto wheels = m_trafficBodies->wheelsOf(c.id); wheels && c.data)
                    body->wheels = game::trafficWheelOffsets(*m, wheels->matrix, *c.data);
            }
            const bool horn = m_hornLatch.contains(c.id);
            cars.push_back(game::shareTrafficCar(c, model, paint, body ? &*body : nullptr, horn));
            game::SharedCar& shared = cars.back();
            shared.motion = m_railMotion.motion(c.id);
            // A car the AI drives off its rail (avoiding a player, regaining
            // its lane) turns as its heading does.
            if (!body)
                shared.angularVelocity.y = shared.motion.curvature * c.speed;
        }
        if (send)
            m_hornLatch.clear();
        m_stillBodies.prune(netBodyStateTime(ctx));
        if (m_debugNetTraffic || trace) {
            std::unordered_set<int> knocked;
            for (const ai::AmbientCar& c : m_ai->cars())
                if (c.physical) {
                    knocked.insert(c.id);
                    if (!m_debugKnocked.contains(c.id) && m_debugNetTraffic)
                        log::info("nettraffic: host traffic car {} knocked off its rail at session t {}",
                                  c.id, ctx.netGame->sessionTime());
                    if (!m_debugKnocked.contains(c.id))
                        game::traceKnock(trace, ctx.netGame->frameTime(), c.id, c.spawns, 255);
                }
            m_debugKnocked = std::move(knocked);
        }
        for (std::size_t i = 0; i < m_cops.size(); ++i) {
            const Cop& cop = m_cops[i];
            const int model = m_trafficCatalog.find(cop.vehicle);
            if (model < 0 || kNetPoliceId + static_cast<int>(i) >= static_cast<int>(net::kMaxAmbientIds))
                continue;
            const phys::CarSim& sim = cop.sim->sim();
            game::SharedCar s;
            s.id = kNetPoliceId + static_cast<int>(i);
            s.kind = net::AmbientKind::Police;
            s.generation = cop.resets;
            s.model = model;
            s.paint = cop.livery;
            s.transform = sim.modelMatrix();
            s.velocity = sim.body.ics.linearVelocity;
            s.angularVelocity = sim.body.ics.angularVelocity;
            // One that stands still is shared standing too: a police car
            // pressed against a wall kept 15 m/s for 10 s while it stood, and
            // a client predicted it 3.9 m ahead.
            if (m_stillBodies.still(s.id, s.transform.m3, netBodyStateTime(ctx))) {
                s.velocity = {};
                s.angularVelocity = {};
            }
            s.speed = sim.speed();
            const auto mode = cop.driver->mode();
            if (cop.driver->siren())
                s.flags |= net::kAmbientSiren;
            if (mode != ai::PoliceCar::Mode::Parked) // aiPoliceOfficer::InPersuit
                s.flags |= net::kAmbientPursuit;
            if (mode == ai::PoliceCar::Mode::Disabled)
                s.flags |= net::kAmbientWrecked;
            if (sim.brakes > 0.05f)
                s.flags |= net::kAmbientBrake;
            const int target = cop.driver->target();
            if (target == 0)
                s.target = ctx.netGame->localId();
            else if (target > kNetPlayerTrackedId && target < kNetPlayerTrackedId + ai::kMaxTrafficPlayers)
                s.target = static_cast<std::uint8_t>(target - kNetPlayerTrackedId);
            s.damage = sim.damage.damage;
            s.rpm = sim.engine.rpm;
            s.throttle = sim.engine.throttle;
            s.gear = sim.trans.getCurrentGear();
            s.body = true;
            cars.push_back(s);
        }
        if (trace)
            traceNetTraffic(ctx, trace, cars);
        if (!send)
            return;
        // The message's time is the AI step's the rail cars are at (stamping
        // the frame's time, up to a step and a frame later, put every car
        // 17 ms behind on the clients on average and made them surge); the
        // police and the knocked cars are moved to it along their velocity
        // from the physics step's (at most a step either way). The light
        // steps go with it.
        const double aiTime = netAiStateTime(ctx);
        const auto shift = static_cast<float>((aiTime - netBodyStateTime(ctx)) / 1000.0);
        for (game::SharedCar& c : cars)
            if (c.body)
                c.transform.m3 += c.velocity * shift;
        const auto time = static_cast<std::uint32_t>(std::llround(std::max(0.0, aiTime)));
        // Each client's cars round its car as the host simulates it
        // (collectHostCars, after the frame's samples).
        std::set<std::uint8_t> viewers;
        for (const auto& rc : m_remoteCars) {
            if (!rc.hasState)
                continue;
            viewers.insert(rc.id);
            const auto msg = m_trafficHost->build({rc.id, rc.transform.m3}, cars, time, m_ai->lightSteps(),
                                                  m_trafficCatalog.checksum());
            const std::size_t bytes = ctx.netGame->sendAmbientState(rc.id, msg);
            auto& st = m_trafficSent[rc.id];
            st.bytes += bytes;
            st.messages += bytes > 0 ? 1 : 0;
            st.cars += msg.entities.size();
            // What protocol 4 added: a knocked car's wheel bit and wheels,
            // a police car's 4 more bits of damage.
            if (bytes > 0)
                for (const auto& e : msg.entities) {
                    if (!e.hasState)
                        continue;
                    if (e.kind == net::AmbientKind::Police)
                        st.damageBits += 4;
                    else if (e.flags & net::kAmbientOffRail)
                        st.damageBits += e.wheels ? 81 : 1;
                    st.wheels += e.wheels ? 1 : 0;
                }
            if (m_debugNetTraffic) // every message: the clients' logs are compared with these
                for (const auto& e : msg.entities)
                    if (e.position.dist(rc.transform.m3) < 120.0f)
                        log::info("nettraffic: host t {} to {} id {} gen {} at {:.2f} {:.2f} {:.2f}{}", time,
                                  rc.id, e.id, e.generation, e.position.x, e.position.y, e.position.z,
                                  e.wheels ? " wheels" : "");
        }
        for (auto it = m_trafficSent.begin(); it != m_trafficSent.end();) {
            if (viewers.contains(it->first)) {
                ++it;
                continue;
            }
            m_trafficHost->forget(it->first);
            it = m_trafficSent.erase(it);
        }
        // Bandwidth, every 10 s.
        if (m_trafficStatsAt == 0)
            m_trafficStatsAt = now;
        if (now - m_trafficStatsAt >= 10000) {
            const double seconds = static_cast<double>(now - m_trafficStatsAt) / 1000.0;
            for (auto& [id, st] : m_trafficSent) {
                const double perMessage =
                    st.messages ? static_cast<double>(st.cars) / static_cast<double>(st.messages) : 0.0;
                log::info("nettraffic: to player {}: {:.1f} msg/s, {:.0f} B/s, {:.1f} cars a message; police "
                          "damage and knocked cars' wheels {:.0f} B/s ({} cars with wheels sent); in full "
                          "{:.0f} B/s ({:.1f} cars a second)",
                          id, static_cast<double>(st.messages) / seconds,
                          static_cast<double>(st.bytes) / seconds, perMessage,
                          static_cast<double>(st.damageBits) / 8.0 / seconds, st.wheels,
                          static_cast<double>(st.fullBytes) / seconds,
                          static_cast<double>(st.fullCars) / seconds);
                st = {};
            }
            m_trafficStatsAt = now;
        }
    }

    // The client's vehicle data of a traffic model (null: not a traffic model).
    const ai::VehicleData* netVehicleData(const std::string& model) const {
        for (const ai::VehicleData& d : m_ai->traffic().types())
            if (str::iequals(d.model, model))
                return &d;
        return nullptr;
    }

    // Client, before the physics step: the host's messages, the cars at
    // the interpolation delay, the light sets at the host's steps, the
    // traffic's physics proxies and the police cars.
    void updateNetTraffic(Context& ctx, float dt) {
        if (!m_trafficClient || !m_ai || !netTrafficClient(ctx))
            return;
        for (const auto& m : ctx.netGame->takeAmbientStates())
            m_trafficClient->receive(m);
        receiveNetTrafficFull(ctx);
        // The cars this machine's car knocked loose in the last frame's
        // samples (NetTrafficCars): only traced, since the host simulates
        // that car too and knocks them itself.
        if (m_netTrafficCars)
            for (int id : m_netTrafficCars->takeKnocks()) {
                const auto it =
                    std::ranges::find_if(m_netCars, [id](const ai::AmbientCar& c) { return c.id == id; });
                if (it != m_netCars.end())
                    game::traceHit(ctx.netGame->traceFile(), m_trafficRenderTime, id,
                                   it->spawns % static_cast<int>(net::kAmbientGenerations));
                if (m_debugNetTraffic)
                    log::info("nettraffic: client knocked traffic car {} loose at session t {:.0f}", id,
                              m_trafficRenderTime);
            }
        if (m_trafficClient->catalogMismatch() && !m_trafficCatalogWarned) {
            log::warn("race: the host's traffic models differ from this game's data; some cars may look "
                      "wrong");
            m_trafficCatalogWarned = true;
        }
        // The received cars are shown where they are at the time this
        // machine's car reaches in this frame's steps (predicted beyond the
        // newest message, game::TrafficPrediction): drawn a trip and a
        // playout delay in the past, they were met 2-4 m from where the host
        // had them (docs/review/multiplayer-desync-traffic.md).
        const double frame = ctx.netGame->frameTime();
        const double own = netTrafficOwnTime(ctx, dt);
        const double car = netTrafficCarTime(ctx, dt); // own on the host's clock
        const double renderTime = netTrafficPresentTime(ctx, dt);
        m_trafficClient->update(renderTime);
        m_trafficRenderTime = renderTime;
        if (std::FILE* trace = ctx.netGame->traceFile(); trace && m_player) {
            // Where the received cars meet this machine's car in this frame's
            // steps, and the session time that car reaches in them.
            const Vec3 me = m_player->sim().modelMatrix().m3;
            game::traceClientView(trace, frame, car, me);
            for (const auto& c : m_trafficClient->cars()) {
                if (c.transform.m3.dist2(me) >= 9e4f)
                    continue;
                // A car this machine knocked loose: where its body is (mode 3).
                const Mat34* local = m_netTrafficCars && m_netTrafficCars->simulated(c.id) && m_trafficBodies
                                         ? m_trafficBodies->transformOf(c.id)
                                         : nullptr;
                game::TrafficClient::Car shown = c;
                if (local)
                    shown.transform = *local;
                // A police car simulated here (predictNetTraffic): its body.
                std::optional<Mat34> cop;
                if (const auto it = m_netCops.find(c.id); it != m_netCops.end() && it->second.predicted)
                    cop = it->second.sim->sim().modelMatrix();
                if (cop) {
                    shown.transform = *cop;
                    local = &*cop;
                }
                game::traceClientCar(trace, frame, renderTime, shown, local ? 3 : c.extrapolated ? 2 : 0);
            }
        }
        if (const auto steps = m_trafficClient->lightSteps(renderTime))
            m_ai->advanceLightsTo(*steps);
        m_netCars.clear();
        m_netPhysical.clear();
        std::unordered_set<int> seen;
        std::vector<std::pair<std::size_t, std::array<Vec3, 4>>> wheeled; // knocked cars with their wheels
        std::vector<game::NetTrafficCars::Received> received;
        for (const auto& c : m_trafficClient->cars()) {
            const std::string* model = m_trafficCatalog.name(c.model);
            if (c.kind != net::AmbientKind::Traffic || !model)
                continue;
            // aiVehicleSpline::Update's tyre turn, kept here.
            float& turn = m_netTireRotation[c.id];
            if (c.fresh)
                turn = 0.0f;
            turn += dt * c.speed;
            if (turn > ai::kTireRotationWrap)
                turn -= ai::kTireRotationWrap;
            m_netCars.push_back(game::ambientCarOf(c, *model, netVehicleData(*model),
                                                   m_aiRenderer ? m_aiRenderer->paintJobs(*model) : 1, turn));
            seen.insert(c.id);
            if (c.wheels && m_netCars.back().data)
                wheeled.push_back({m_netCars.size() - 1, c.wheelOffsets});
            received.push_back({m_netCars.back(), c.stateTime, netBodyInFull(ctx, c.id, c.generation)});
        }
        std::erase_if(m_netTireRotation, [&](const auto& e) { return !seen.contains(e.first); });
        // The cars on their rails are the level's instances in TrafficBodies,
        // which this machine's car may knock loose (NetTrafficCars); the ones
        // off them on the host move as it says (TrafficProxies). A knocked
        // car goes back to the host's messages once they show it knocked too,
        // or still on its rail a trip after the hit.
        if (m_netTrafficCars) {
            m_netTrafficCars->update(received, car, kNetKnockConfirmMs);
            for (const auto& h : m_netTrafficCars->takeHandovers()) {
                const float off = m_trafficClient->setDrawn(h.id, h.pose);
                game::traceHandover(ctx.netGame->traceFile(), car, h.id, h.confirmed, off);
                if (m_debugNetTraffic)
                    log::info("nettraffic: client's knock of traffic car {} {} the host ({:.2f} m off)", h.id,
                              h.confirmed ? "confirmed by" : "withdrawn: not knocked on", off);
            }
        }
        if (m_trafficProxies) {
            std::vector<ai::AmbientCar> moving;
            for (const ai::AmbientCar& c : m_netCars)
                if (!m_netTrafficCars ||
                    (c.goal != ai::AmbientGoal::RandomDrive && !m_netTrafficCars->simulated(c.id) &&
                     !m_netTrafficCars->standing(c.id)))
                    moving.push_back(c);
            m_trafficProxies->update(moving);
        }
        // Drawn where the rest of the scene is, a physics sample behind the
        // frame (game::StepHistory), as far from the cars' time as the scene
        // is from this machine's car's.
        m_netDrawTime = renderTime - (own - (frame - static_cast<double>(phys::kFixedSampleStep) * 1000.0));
        m_netCarsDrawn = m_netCars;
        for (ai::AmbientCar& c : m_netCarsDrawn)
            if (const auto m = m_trafficClient->transformAt(c.id, m_netDrawTime))
                c.transform = *m;
        // A knocked car with a body on the host: drawn on the wheels its
        // vehWheelCheaps put there (aiVehicleInstance::Draw), carried by the
        // car as drawn.
        for (const auto& [index, offsets] : wheeled) {
            const ai::AmbientCar& c = m_netCarsDrawn[index];
            game::AiRenderer::PhysicalCar p;
            p.transform = c.transform;
            p.active = true;
            game::trafficWheelMatrices(c.transform, offsets, *c.data, p.wheels, p.wheelValid);
            m_netPhysical[c.id] = p;
        }
        // The pedestrians' accidents: the components of the cars off their rails.
        m_netAccidentNodes.clear();
        m_netAccidentPaths.clear();
        for (const ai::AmbientCar& c : m_netCars) {
            if (c.goal == ai::AmbientGoal::RandomDrive)
                continue;
            int id = 0, type = ai::kNoComponent;
            int& room = m_netAccidentRooms[c.id];
            room = m_ai->map().mapComponent(c.transform.m3, id, type, room);
            if (type == ai::kIntersectionComponent)
                m_netAccidentNodes.insert(id);
            else if (type == ai::kRoadComponent || type == ai::kShortcutComponent)
                m_netAccidentPaths.insert(id);
        }
        updateNetCops(ctx, dt);
        if (m_debugNetTraffic) {
            const std::uint64_t now = net::monotonicMs();
            if (now - m_trafficLoggedAt >= 500) {
                m_trafficLoggedAt = now;
                const Vec3 me = m_player ? m_player->sim().modelMatrix().m3 : m_camera.position();
                for (const auto& c : m_trafficClient->cars())
                    if (c.transform.m3.dist(me) < 120.0f)
                        log::info(
                            "nettraffic: client t {:.0f} id {} gen {} at {:.2f} {:.2f} {:.2f} flags {:#x}"
                            "{}{}{}",
                            renderTime, c.id, c.generation, c.transform.m3.x, c.transform.m3.y, c.transform.m3.z,
                            c.flags, c.wheels ? " wheels" : "", c.extrapolated ? " (extrapolated)" : "",
                            c.kind == net::AmbientKind::Police ? std::format(" police target {}", c.target)
                                                               : std::string());
                log::info("nettraffic: client cops pursuing this player: {}",
                          audio::game::SirenPlayer::copsPursuingPlayer());
            }
        }
        const std::uint64_t now = net::monotonicMs();
        if (m_trafficStatsAt == 0)
            m_trafficStatsAt = now;
        if (now - m_trafficStatsAt >= 10000) {
            const auto& st = m_trafficClient->stats();
            log::info("nettraffic: received {} messages ({} cars), {} refused, {} late, {} moved; showing {} "
                      "cars ({} on their bodies' wheels), {} police",
                      st.messages, st.entities, st.refused, st.outdated, st.teleports, m_netCars.size(),
                      m_netPhysical.size(), m_netCops.size());
            m_trafficStatsAt = now;
        }
    }

    // A client: the session time this machine's car reaches in this frame's
    // fixed steps (as sendLocalState stamps it).
    double netTrafficOwnTime(Context& ctx, float dt) const {
        const double lag =
            m_world ? static_cast<double>(m_world->remainderAfter(physicsDt(ctx, dt))) * 1000.0 : 0.0;
        return ctx.netGame->frameTime() - lag;
    }
    // ... and the session time that car's state is at on the host: the
    // client's car is predicted ahead of the host's, by the time its inputs
    // take to reach the host and wait there (m_netCarLead, measured from the
    // host's states of it). The received cars are shown at that time, so
    // that the car meets them where the host's simulation of it will.
    double netTrafficCarTime(Context& ctx, float dt) const {
        return netTrafficOwnTime(ctx, dt) + (ctx.netGame->isHost() ? 0.0 : m_netCarLead.value_or(0.0));
    }
    // OPENMM2_DEBUG_TRAFFIC_LEAD_MS=<ms> shows them that much later
    // (negative: earlier), for measuring the prediction over other horizons.
    double netTrafficPresentTime(Context& ctx, float dt) const {
        static const double lead = [] {
            const char* v = std::getenv("OPENMM2_DEBUG_TRAFFIC_LEAD_MS");
            return v ? str::parseDouble(v).value_or(0.0) : 0.0;
        }();
        return netTrafficCarTime(ctx, dt) + lead;
    }

    // aiPedestrian::Accident's question on a client: a received car out of
    // normal driving at the intersection or on the road (inferred: the
    // host's section lists are not sent, so the whole road counts).
    bool netAccidentAt(int node, int path) const {
        return m_netAccidentNodes.contains(node) || (path >= 0 && m_netAccidentPaths.contains(path));
    }

    // The host's police on a client: kinematic cars (as the network
    // players', updateRemoteCars) at their interpolated places, drawn and
    // heard as the host's cops.
    // The host's police car shown on a client.
    struct NetCop {
        std::string model;
        int paint = -1;
        int generation = -1;
        std::unique_ptr<game::SimVehicle> sim; // kinematic body
        std::unique_ptr<game::VehicleRenderer> renderer;
        std::unique_ptr<audio::game::OpponentCarAudio> audio;
        game::TrafficClient::Car state;
        std::array<float, 6> spin{};
        float sirenAngle = 0.0f;
        // Its damage (as the network players' cars, RemoteVehicle).
        std::unique_ptr<game::fx::VehicleEffects> fx;
        std::vector<audio::game::ImpactInput> impacts;
        bool fresh = true;
        // Protocol 12: simulated here from the host's full states
        // (predictNetTraffic) on the controls its driver set last, a body of
        // its mass; else a kinematic body at its predicted place.
        bool predicted = false;
        double fullAt = 0.0; // session ms its last full state was put in
        game::CorrectionBlend blend;
        std::optional<Mat34> blendFrom; // drawn before a replay
        bool switched = false;          // to being simulated, in that replay
    };

    // A police car's parts out of the world, kept for another one of its
    // model: each model is loaded once per car shown at a time.
    void releaseNetCop(NetCop& cop) {
        if (cop.sim && m_world)
            cop.sim->removeFrom(*m_world);
        if (cop.audio)
            cop.audio->stop();
        if (cop.sim) {
            setNetCopPredicted(0, cop, false); // kinematic again
            const std::string model = cop.model;
            cop.fresh = true; // another car's damage when it is used again
            cop.impacts.clear();
            m_spareNetCops.emplace(model, std::move(cop));
        }
        cop = {};
    }

    void updateNetCops(Context& ctx, float dt) {
        std::unordered_set<int> seen;
        for (const auto& c : m_trafficClient->cars()) {
            const std::string* model = m_trafficCatalog.name(c.model);
            if (c.kind != net::AmbientKind::Police || !model || !m_world)
                continue;
            // As many as MM2's posts could give (a hostile host gets no more).
            if (!m_netCops.contains(c.id) && m_netCops.size() >= kMaxNetCops)
                continue;
            seen.insert(c.id);
            NetCop& cop = m_netCops[c.id];
            if (cop.model != *model) {
                releaseNetCop(cop);
                cop.model = *model;
                if (const auto spare = m_spareNetCops.find(*model); spare != m_spareNetCops.end()) {
                    cop = std::move(spare->second);
                    m_spareNetCops.erase(spare);
                } else if (!m_badNetCopModels.contains(*model)) {
                    std::string error;
                    cop.sim = game::SimVehicle::load(ctx.game->vfs, *model, &error, {}, false, false);
                    if (!cop.sim) {
                        // Tried once, not again every frame.
                        log::warn("race: shared police car {}: {}", *model, error);
                        m_badNetCopModels.insert(*model);
                        continue;
                    }
                    auto& sim = cop.sim->sim();
                    sim.options.weatherFriction = weatherFriction();
                    // As the host's loadAiCar: of its player's model, the
                    // polygonal bound (simulated here, predictNetTraffic).
                    if (str::iequals(*model, ctx.netGame->playerCar(net::kHostPlayerId).vehicle))
                        sim.setPolygonalBound(true);
                    sim.body.kinematic = true;
                    sim.body.resetCollider();
                    cop.renderer = std::make_unique<game::VehicleRenderer>(
                        ctx.device(), *m_textures, *m_models, cop.sim->model(), c.paint);
                    setupVehicleRenderer(ctx, *cop.renderer);
                    cop.paint = c.paint;
                    cop.audio = loadAiCarAudio(ctx, *model, true);
                    cop.fx = loadVehicleFx(ctx, *model, cop.sim->model(), *cop.renderer);
                }
                if (!cop.sim)
                    continue;
                cop.sim->addTo(*m_world);
                cop.generation = -1; // placed below
            }
            if (!cop.sim)
                continue;
            if (cop.paint != c.paint) {
                cop.renderer->setPaintjob(c.paint);
                cop.paint = c.paint;
                cop.fresh = true;
            }
            if (cop.generation != c.generation) {
                // A new car in the slot (or the host's cop put back): it
                // starts afresh where it is.
                cop.generation = c.generation;
                cop.sim->reset(c.transform);
                if (cop.audio)
                    cop.audio->reset();
                if (cop.fx)
                    cop.fx->reset();
                cop.spin = {};
                cop.sirenAngle = 0.0f;
                cop.fresh = true; // its damage drawn from its record again
            }
            auto& sim = cop.sim->sim();
            // Simulated here while its full states keep coming
            // (predictNetTraffic); else placed where it is predicted.
            if (cop.predicted && ctx.netGame->frameTime() - cop.fullAt > kNetFullStaleMs)
                setNetCopPredicted(c.id, cop, false);
            if (!cop.predicted) {
                Mat34 ics = c.transform;
                ics.m3 = c.transform.m3 - c.transform.transformDir(sim.centerOfGravity);
                sim.body.place(ics);
                sim.body.ics.linearVelocity = c.velocity;
                sim.body.ics.angularVelocity = c.angularVelocity;
                sim.body.kinematicMoves = true;
                sim.body.kinematicVelocity = c.velocity;
                sim.body.kinematicSpin = c.angularVelocity;
            }
            sim.body.declare(2, 0x1b);
            cop.state = c;
            // OpenMM2: its damage as the host shows it.
            updateNetCarDamage(ctx, game::DamageReplica::ambientKey(static_cast<std::uint16_t>(c.id)),
                               *cop.renderer, cop.fx.get(), sim, cop.model, netCopDrawn(c.id).value_or(c.transform),
                               c.velocity, c.damage, m_trafficRenderTime, cop.fresh, cop.impacts);
            // vehSiren::Update: the beams turn 2.5 pi a second while on.
            if (c.flags & net::kAmbientSiren)
                cop.sirenAngle = std::fmod(cop.sirenAngle + dt * 2.5f * 3.1415927f, 6.2831855f);
        }
        for (auto it = m_netCops.begin(); it != m_netCops.end();) {
            if (seen.contains(it->first)) {
                ++it;
                continue;
            }
            releaseNetCop(it->second);
            it = m_netCops.erase(it);
        }
    }

    // A shared-traffic client's police car switched between being simulated
    // here (from the host's full states, a body of its mass, predictNetTraffic)
    // and placed at its predicted place (kinematic, updateNetCops). Switched
    // off, the drawing blends from where it was drawn.
    void setNetCopPredicted(int id, NetCop& cop, bool on) {
        if (!cop.sim || cop.predicted == on)
            return;
        const Mat34 drawnBefore = netCopPose(id, cop).body;
        auto& sim = cop.sim->sim();
        sim.body.kinematic = !on;
        sim.body.kinematicMoves = !on;
        sim.ownRandom = on;
        // What it hits as this machine's car's (World::collideInstances'
        // PlayerInst): a traffic car it runs into is knocked loose here, as
        // the host's police car knocks it (NetTrafficCars takes it as a
        // knock of this machine's, for the host's messages to confirm).
        sim.body.player = on;
        sim.body.resetCollider();
        cop.predicted = on;
        if (on) {
            orderPlayerCars(); // the players' cars after it in the movers, as on the host
            return;
        }
        cop.blend.halfLife = 0.15f;
        cop.blend.snapDistance = 20.0f;
        cop.blend.add(drawnBefore, netCopDrawn(id).value_or(cop.state.transform));
    }
    static std::uint64_t netCopDrawnId(int id) { return kNetCopDrawnKey + static_cast<std::uint64_t>(id); }
    // A police car's body as drawn: predicted, between its last two
    // samples; else at its interpolated state; the corrections and switches
    // drawn away.
    game::VehiclePose netCopPose(int id, const NetCop& cop) const {
        game::VehiclePose pose;
        if (cop.predicted && cop.sim)
            pose = drawnPose(game::Drawn::Police, netCopDrawnId(id), *cop.sim);
        else if (m_trafficClient)
            pose.body = m_trafficClient->transformAt(id, m_netDrawTime).value_or(cop.state.transform);
        else
            pose.body = cop.state.transform;
        return game::placePose(pose, cop.blend.apply(pose.body));
    }

    // Client, before the samples: the host's police and knocked cars in full
    // (net/TrafficFull.h), kept by the host sample they belong to until its
    // car's state acknowledges it (predictNetTraffic).
    void receiveNetTrafficFull(Context& ctx) {
        for (auto& m : ctx.netGame->takeTrafficFull()) {
            auto& list = m_trafficFull[m.time];
            for (auto& c : m.cars) {
                if (std::ranges::any_of(list, [&](const auto& o) { return o.id == c.id; }))
                    continue; // the same car twice: the first counts
                m_trafficFullSeen[c.id] = {ctx.netGame->frameTime(), c.generation, c.kind};
                list.push_back(std::move(c));
            }
        }
        // A second's worth (CarStates come 20 times a second).
        while (m_trafficFull.size() > 24)
            m_trafficFull.erase(m_trafficFull.begin());
        std::erase_if(m_trafficFullSeen, [&](const auto& e) {
            return ctx.netGame->frameTime() - e.second.at > kNetFullStaleMs;
        });
    }
    // Whether the host lately sent traffic car `id` (generation
    // `generation`) in full: a knocked car simulated here.
    bool netBodyInFull(Context& ctx, int id, int generation) const {
        const auto it = m_trafficFullSeen.find(id);
        return it != m_trafficFullSeen.end() && it->second.kind == net::AmbientKind::Traffic &&
               it->second.generation == static_cast<unsigned>(generation) % net::kAmbientGenerations &&
               ctx.netGame->frameTime() - it->second.at <= kNetFullStaleMs;
    }

    // Client, in its car's acknowledgement (reconcileNetCar): the police and
    // knocked cars the host sent in full at the acknowledged sample (`time`)
    // run again with its car from those states, as the players' cars near
    // it do; a police car on the controls its driver set last, which it keeps
    // until the next.
    void predictNetTraffic(Context& ctx, std::uint32_t time,
                           std::vector<game::CarPrediction::Companion>& out) {
        m_netFullBodies.clear();
        const auto it = m_trafficFull.find(time);
        if (it == m_trafficFull.end())
            return;
        for (const net::TrafficFullCar& f : it->second) {
            if (f.kind == net::AmbientKind::Police) {
                const auto cop = m_netCops.find(f.id);
                if (cop == m_netCops.end() || !cop->second.sim ||
                    static_cast<unsigned>(cop->second.generation) % net::kAmbientGenerations != f.generation)
                    continue;
                NetCop& c = cop->second;
                c.blendFrom = netCopPose(f.id, c).body;
                c.switched = !c.predicted;
                setNetCopPredicted(f.id, c, true);
                c.fullAt = ctx.netGame->frameTime();
                phys::CarSim* sim = &c.sim->sim();
                m_netFullBodies.push_back(&sim->body);
                game::CarPrediction::Companion companion;
                companion.car = c.sim.get();
                companion.state = &f.car;
                companion.first = true; // the police come before the players' cars in the movers
                companion.drive = [sim, &f] {
                    sim->setInputs(f.throttle, f.brake, f.steering, f.handBrake);
                    sim->engine.maxThrottle = f.maxThrottle;
                };
                out.push_back(std::move(companion));
                continue;
            }
            if (!m_netTrafficCars || !m_trafficBodies || !m_netTrafficCars->simulated(f.id))
                continue;
            // (A car without a body here takes one: put in by the replay.)
            if (!m_trafficBodies->body(f.id) && !m_trafficBodies->setBodyState(f.id, f.body))
                continue;
            game::CarPrediction::Companion companion;
            companion.body = m_trafficBodies->body(f.id);
            if (!companion.body)
                continue;
            m_netFullBodies.push_back(companion.body);
            companion.rebase = [this, &f] { m_trafficBodies->setBodyState(f.id, f.body); };
            companion.position = f.body.matrix.m3;
            companion.velocity = f.body.linearVelocity;
            out.push_back(std::move(companion));
        }
    }
    // ... and after it: what the host's states moved the police by (and a
    // switch to simulating one, more slowly) drawn away; the full states up
    // to that sample forgotten.
    void afterNetTrafficReplay(std::uint32_t time) {
        for (auto& [id, c] : m_netCops) {
            if (!c.blendFrom || !c.predicted || !c.sim)
                continue;
            const game::VehiclePose now =
                drawnPose(game::Drawn::Police, netCopDrawnId(id), *c.sim);
            c.blend.halfLife = c.switched ? 0.15f : 0.08f;
            c.blend.snapDistance = 20.0f;
            c.blend.add(*c.blendFrom, now.body);
            c.blendFrom.reset();
        }
        m_trafficFull.erase(m_trafficFull.begin(), m_trafficFull.upper_bound(time));
        m_netFullBodies.clear();
    }
    // The predicted police's poses for the drawing between samples
    // (recordStepPoses; before a replay's last sample, the ones it runs).
    void recordNetCopSteps(bool replayed) {
        for (const auto& [id, c] : m_netCops) {
            if (!c.predicted || !c.sim ||
                (replayed && std::ranges::find(m_netFullBodies, &c.sim->sim().body) == m_netFullBodies.end()))
                continue;
            m_drawnPhys.record(game::drawnKey(game::Drawn::Police, netCopDrawnId(id)), c.sim->pose(),
                               c.sim->sim().resets);
        }
    }

    void drawNetCops(float dt, const game::Camera& camera) {
        const bool lights = carLights();
        for (auto& [id, c] : m_netCops) {
            if (!c.sim || !c.renderer)
                continue;
            game::VehiclePose pose;
            if (c.predicted) {
                // Simulated here (predictNetTraffic): between its last two
                // samples, with what the host's states moved it by drawn away.
                pose = netCopPose(id, c);
            } else {
                pose.body = netCopPose(id, c).body;
                // The wheels roll with the forward speed (as the network players').
                for (const auto& w : c.sim->model().wheels) {
                    const auto i = static_cast<std::size_t>(std::clamp(w.index, 0, 5));
                    c.spin[i] -= c.state.speed / std::max(w.radius, 0.1f) * dt;
                    pose.wheelSpin[i] = c.spin[i];
                }
            }
            pose.headlights = lights;
            pose.brakeLights = (c.state.flags & net::kAmbientBrake) != 0;
            pose.reverseLights = c.state.gear < 0;
            pose.siren = (c.state.flags & net::kAmbientSiren) != 0;
            pose.sirenAngle = c.sirenAngle;
            c.renderer->draw(pose, camera.transform);
        }
    }

    // The host's police cars' engines and sirens on a client; a cop chasing
    // this player counts for the cop chase music (aiPoliceOfficer::StartSiren).
    void updateNetCopsAudio(Context& ctx, float dt) {
        if (!netTrafficClient(ctx))
            return;
        for (auto& [id, c] : m_netCops) {
            if (!c.audio)
                continue;
            audio::game::CarAudioInputs in;
            in.rpm = c.state.rpm;
            in.throttle = c.state.throttle;
            in.brake = (c.state.flags & net::kAmbientBrake) != 0 ? 1.0f : 0.0f;
            in.speed = std::abs(c.state.speed);
            in.gear = c.state.gear;
            for (auto& w : in.wheels)
                w.onGround = true;
            in.velocity = c.state.velocity;
            in.inTunnel = m_tunnel;
            in.transform = c.state.transform;
            in.siren = (c.state.flags & net::kAmbientSiren) != 0;
            in.sirenPursuingPlayer = c.state.target == ctx.netGame->localId();
            in.wrecked = (c.state.flags & net::kAmbientWrecked) != 0;
            in.impacts = std::exchange(c.impacts, {}); // the host's, replayed
            c.audio->update(in, dt, m_camera.transform);
        }
    }

    // mmNetObject: every other player's car is a vehCar of the level, declared
    // each frame as a type-3 mover (its room and the neighbours stay active)
    // with its trailer. MM2 drove it on every machine with its player's
    // inputs and pulled it toward the received positions (mmNetObject::
    // Predict, Update). OpenMM2's host simulates it from its player's inputs
    // (updateHostCars); a client places it at the host's interpolated states
    // as a kinematic body, which its own car collides with as with a body
    // moving at their velocity (it does not give way there: the host's
    // collision is the one that counts).
    //
    // The cars are sampled once a frame (the HUD, the rules and the drawing
    // use the same sample) at the time the simulation will have reached
    // after this frame's fixed steps: the local car, the camera and every
    // simulated object move in whole simulation steps, and a remote car
    // sampled at the frame's own time would shake against them by up to a
    // step's travel.
    void updateRemoteCars(Context& ctx, float dt) {
        if (!multiplayer(ctx) || !m_world)
            return;
        if (ctx.netGame->isHost()) {
            updateHostCars(ctx, dt); // OpenMM2: the host simulates them
            return;
        }
        const double lagMs = static_cast<double>(m_world->remainderAfter(physicsDt(ctx, dt))) * 1000.0;
        // Development aid (an experiment, docs/review/multiplayer-desync-cars.md):
        // OPENMM2_NET_OTHERS=ahead places and draws the other cars where the
        // host will have them when it runs this machine's sample (extrapolated
        // from its newest states), =ghost keeps this machine's car from
        // touching them (only the host's collision counts).
        static const std::string others = [] {
            const char* v = std::getenv("OPENMM2_NET_OTHERS");
            return std::string(v ? v : "");
        }();
        const bool ahead = others == "ahead";
        m_remoteCars =
            ahead ? ctx.netGame->remoteCarsAhead(lagMs, m_netLead) : ctx.netGame->remoteCars(lagMs);
        // The session time the simulation's state will belong to after this
        // frame's steps.
        m_netStateTime = static_cast<std::uint32_t>(std::max(0.0, ctx.netGame->frameTime() - lagMs));
        // Drawn where the rest of the scene is, a sample behind the frame
        // (game::StepHistory): alpha x step = the remainder.
        m_remoteInterp.clear();
        const double stepMs = static_cast<double>(phys::kFixedSampleStep) * 1000.0;
        const auto drawn =
            ahead ? ctx.netGame->remoteCarsAhead(stepMs, m_netLead) : ctx.netGame->remoteCars(stepMs);
        for (const auto& rc : drawn)
            if (rc.hasState)
                m_remoteInterp[rc.id] = rc.transform;
        // A car simulated here keeps where it was drawn (updateDrawnPoses
        // draws it again after the samples).
        std::erase_if(m_remoteDrawn, [this](const auto& e) {
            const auto it = m_remotes.find(e.first);
            return it == m_remotes.end() || !it->second.predicted;
        });
        for (const auto& [id, transform] : m_remoteInterp)
            m_remoteDrawn.try_emplace(id, transform);
        // A player who quit the race takes its car out of it.
        std::erase_if(m_remoteCars, [this](const game::NetRemoteCar& c) { return m_netLeft.contains(c.id); });
        std::vector<std::uint8_t> present;
        for (const auto& rc : m_remoteCars) {
            if (!rc.hasState)
                continue;
            present.push_back(rc.id);
            RemoteVehicle& rv = m_remotes[rc.id];
            if (rv.base != rc.car.vehicle || rv.color != rc.car.color)
                loadRemoteCar(ctx, rc.id, rv, rc.car, false);
            if (!rv.sim)
                continue;
            auto& sim = rv.sim->sim();
            if (rv.predicted) {
                // Simulated here (predictNearCars): it runs on its input.
                sim.body.declare(3, 0x1b); // mmNetObject::Update
                sim.setWaterLevel(waterLevelAt(sim.modelMatrix().m3));
            } else {
                placeNetCar(rv, rc);
                sim.body.declare(3, others == "ghost" ? 0x0b : 0x1b); // mmNetObject::Update
            }
            updateNetCarDamage(ctx, game::DamageReplica::playerKey(rc.id), *rv.renderer, rv.fx.get(), sim,
                               rv.sim->model().baseName, netCarDrawn(rc.id).value_or(rc.transform), rc.velocity,
                               rc.damage, rc.time, rv.fresh, rv.impacts);
        }
        dropRemoteCars(present);
        updateOwnNetDamage(ctx);
    }

    // Client: another player's car placed at its interpolated state (`rc`),
    // a kinematic body moving at its velocity.
    void placeNetCar(RemoteVehicle& rv, const game::NetRemoteCar& rc) {
        auto& sim = rv.sim->sim();
        // The snapshot is the model matrix; the body is at the centre of
        // mass (vehCarSim::SetWorldMatrix's offset).
        Mat34 ics = rc.transform;
        ics.m3 = rc.transform.m3 - rc.transform.transformDir(sim.centerOfGravity);
        const bool jumped = sim.body.ics.matrix.m3.dist2(ics.m3) > 20.0f * 20.0f;
        sim.body.place(ics);
        sim.body.ics.linearVelocity = rc.velocity;
        sim.body.ics.angularVelocity = rc.angularVelocity;
        // Everything meets the car as a moving one (the local car, and
        // the shared traffic and police): as a still wall, a car closing
        // on it at 1 m/s while both did 30 m/s lost 12.6 m/s, a rear tap
        // threw the chaser back and rubbing side by side dragged as a
        // barrier would.
        sim.body.kinematicMoves = true;
        sim.body.kinematicVelocity = rc.velocity;
        sim.body.kinematicSpin = rc.angularVelocity;
        sim.setInputs(rc.controls.throttle, rc.controls.brake, rc.controls.steering, rc.controls.handbrake);
        if (auto* trailer = rv.sim->trailer()) {
            trailer->body.declare(3, 0x1b);
            if (jumped)
                trailer->reset(); // respawned: hitched again behind it
        }
    }

    // Client: which other players' cars it simulates along with its own (the
    // ones the host's newest state sent in full, `near`; none with
    // OPENMM2_NET_OTHERS set, the experiments), switching each between that
    // and its interpolated states; the ones simulated, as the
    // reconciliation's companions.
    void predictNearCars(const std::vector<std::uint8_t>& nearIds, const std::vector<net::NearCarState>& near,
                         std::uint32_t ack, std::vector<game::CarPrediction::Companion>& companions) {
        static const bool others = [] {
            const char* v = std::getenv("OPENMM2_NET_OTHERS");
            return v && *v;
        }();
        for (auto& [id, rv] : m_remotes) {
            if (rv.simulated || !rv.sim)
                continue;
            const auto it = std::ranges::find(near, id, &net::NearCarState::id);
            const auto rc = std::ranges::find(m_remoteCars, id, &game::NetRemoteCar::id);
            // A near car joins with a state of its own; one in the near set
            // whose state waits for a later message runs on meanwhile.
            const bool listed = std::ranges::find(nearIds, id) != nearIds.end() &&
                                (it != near.end() || rv.predicted);
            const bool wanted = !others && listed && !rv.sim->trailer() && rc != m_remoteCars.end() &&
                                rc->hasState && !m_netLeft.contains(id);
            auto& sim = rv.sim->sim();
            const bool justPredicted = wanted && !rv.predicted;
            if (wanted && !rv.predicted) {
                // As the host simulates it (loadRemoteCar's `simulated`), but
                // its damage stays the host's (updateNetCarDamage).
                sim.body.kinematic = false;
                sim.body.kinematicMoves = false;
                sim.body.resetCollider();
                sim.options.player = true;
                sim.ownRandom = true;
                // What it hits counts as hit by a player (as the simulated
                // police): it knocks the shared traffic loose here too.
                sim.body.player = true;
                rv.driver.attach(*rv.sim);
                rv.predicted = true;
                rv.trail.clear();
                rv.drawAhead = 0.0f;
            } else if (!wanted && rv.predicted) {
                sim.body.kinematic = true;
                sim.body.resetCollider();
                sim.options.player = false;
                sim.ownRandom = false;
                sim.body.player = false;
                rv.predicted = false;
                if (rc != m_remoteCars.end() && rc->hasState)
                    placeNetCar(rv, *rc);
            }
            if (rv.predicted && it != near.end()) {
                rv.input = it->input;
                game::CarPrediction::Companion c;
                c.car = rv.sim.get();
                c.driver = &rv.driver;
                c.state = &it->state;
                c.input = it->input;
                c.upcoming = it->upcoming;
                c.first = id < m_traceNet->localId();
                c.differs = !justPredicted && nearCarDiffers(rv, ack, *it);
                companions.push_back(c);
            }
        }
    }
    // OpenMM2: every machine keeps the players' cars in the world's movers in
    // player order (the host's first), so that two of them collide in the
    // same order everywhere: World::step runs a mover's collisions with the
    // city, then with the movers after it, then with the instances, and the
    // order changes the outcome. A car that joins puts the ones after it
    // behind it again.
    void orderPlayerCars() {
        if (!m_world || !m_traceNet)
            return;
        std::vector<std::pair<std::uint8_t, game::SimVehicle*>> cars;
        if (m_player)
            cars.emplace_back(m_traceNet->localId(), m_player.get());
        for (const auto& [id, rv] : m_remotes)
            if (rv.sim && m_world->contains(&rv.sim->sim().body))
                cars.emplace_back(id, rv.sim.get());
        std::ranges::sort(cars, {}, &std::pair<std::uint8_t, game::SimVehicle*>::first);
        for (const auto& [id, car] : cars) {
            car->removeFrom(*m_world);
            car->addTo(*m_world);
        }
    }
    // Client: a simulated car is drawn at this machine's car's time once the
    // two may meet within half a second (8 m apart, or closing on the 5 m
    // that leaves at more than that rate), at its player's present once they
    // are 12 m apart and a second from meeting, sliding between over a
    // quarter of a second (OPENMM2_NET_NEAR_DRAW=ahead draws it at this
    // machine's car's time always, the comparison). Another client's car is
    // at this machine's car's time always: that client's screen runs ahead
    // of the host's as this one's does (each predicts its car its inputs'
    // trip and the host's margin ahead), so that is its player's present
    // (three players ramming: 0.02-0.06 m from it at the median, against
    // 0.38-0.54 drawn at the host's present); the host's car's present is
    // the host's.
    void updateDrawAhead(std::uint8_t id, RemoteVehicle& rv) const {
        static const bool always = [] {
            const char* v = std::getenv("OPENMM2_NET_NEAR_DRAW");
            return v && std::string_view(v) == "ahead";
        }();
        if (!rv.predicted || !m_player) {
            rv.drawAhead = 0.0f;
            return;
        }
        const auto& own = m_player->sim().body.ics;
        const auto& other = rv.sim->sim().body.ics;
        const Vec3 apart = other.matrix.m3 - own.matrix.m3;
        const float d = apart.mag();
        const float closing = d > 0.01f ? -(other.linearVelocity - own.linearVelocity).dot(apart) / d : 0.0f;
        const float toMeet = closing > 0.01f ? std::max(0.0f, d - 5.0f) / closing : 1e9f;
        float& w = rv.drawAhead;
        if (always || id != net::kHostPlayerId || d < 8.0f || toMeet < 0.5f)
            w = std::min(1.0f, w + m_frameDt * 4.0f);
        else if (d > 12.0f && toMeet > 1.0f)
            w = std::max(0.0f, w - m_frameDt * 4.0f);
    }
    // Client: whether the host's state of a car it simulates, at its sample
    // `ack`, or the inputs the host sent for the samples after, differ from
    // what it ran the car on (as its own car's: 3 mm, 3 cm/s, 0.0015 in the
    // matrix; any input).
    static bool nearCarDiffers(const RemoteVehicle& rv, std::uint32_t ack, const net::NearCarState& n) {
        const auto at = std::ranges::find(rv.trail, ack, &RemoteVehicle::TrailPoint::seq);
        if (at == rv.trail.end())
            return true;
        const Mat34& a = at->ics;
        const Mat34& b = n.state.matrix;
        if (a.m3.dist(b.m3) > 0.003f || at->velocity.dist(n.state.linearVelocity) > 0.03f)
            return true;
        for (const auto& [x, y] : {std::pair{a.m0, b.m0}, std::pair{a.m1, b.m1}, std::pair{a.m2, b.m2}})
            if (std::max({std::abs(x.x - y.x), std::abs(x.y - y.y), std::abs(x.z - y.z)}) > 0.0015f)
                return true;
        const auto same = [](net::CarInputFrame x, net::CarInputFrame y) {
            x.events = y.events = 0;
            return x == y;
        };
        if (!same(at->input, n.input))
            return true;
        auto next = std::next(at);
        for (std::size_t k = 0; k < n.upcoming.size() && next != rv.trail.end(); ++k, ++next)
            if (next->seq != ack + 1 + k || !(next->input == n.upcoming[k]))
                return true;
        // Ran past what the host sent on its last input: so would the host.
        for (; next != rv.trail.end(); ++next)
            if (!same(next->input, n.upcoming.empty() ? n.input : n.upcoming.back()))
                return true;
        return false;
    }
    bool predictedBody(const phys::Body* b) const {
        for (const auto& [id, rv] : m_remotes)
            if (rv.predicted && rv.sim && &rv.sim->sim().body == b)
                return true;
        // The shared traffic's cars the acknowledgement runs again with this
        // car (predictNetTraffic).
        return std::ranges::find(m_netFullBodies, b) != m_netFullBodies.end();
    }

    // The cars of players no longer in the race leave it.
    void dropRemoteCars(const std::vector<std::uint8_t>& present) {
        for (auto it = m_remotes.begin(); it != m_remotes.end();) {
            if (std::find(present.begin(), present.end(), it->first) == present.end()) {
                if (it->second.sim)
                    it->second.sim->removeFrom(*m_world);
                if (it->second.audio)
                    it->second.audio->stop();
                m_netDamage.replica().forget(game::DamageReplica::playerKey(it->first));
                m_netDamage.forgetPlayer(it->first);
                it = m_remotes.erase(it);
            } else {
                ++it;
            }
        }
    }

    // mmNetObject::Init: another player's car is a vehCar of the level, built
    // with the polygonal bound (a vpcop on vpmustang99's tuning, towing its
    // trailer except in multiplayer cruise and Cops and Robbers). On a client
    // it is drawn from the host's states, a kinematic body moving at their
    // velocity; on the host (`simulated`, OpenMM2) it is simulated from its
    // player's inputs like this machine's car.
    void loadRemoteCar(Context& ctx, std::uint8_t id, RemoteVehicle& rv, const game::NetCar& car,
                       bool simulated) {
        if (rv.sim)
            rv.sim->removeFrom(*m_world);
        if (rv.audio)
            rv.audio->stop();
        game::HostInputQueue inputs = std::move(rv.inputs);
        rv = {};
        rv.inputs = std::move(inputs);
        rv.base = car.vehicle;
        rv.color = car.color;
        rv.simulated = simulated;
        const auto mode = m_result.config.mode;
        const bool towing = mode != game::GameMode::Cruise && mode != game::GameMode::CopsAndRobbers;
        // mmNetObject::Init takes the car through mmVehList::GetVehicleInfo:
        // one this machine lacks is the default car.
        const std::string vehicle = game::netVehicle(ctx.game->catalog, rv.base);
        std::string error;
        rv.sim = game::SimVehicle::load(ctx.game->vfs, vehicle, &error, {}, true, towing);
        if (!rv.sim) {
            // Tried once per car and paint job, not again every frame.
            log::warn("race: network car {}: {}", vehicle, error);
            return;
        }
        auto& sim = rv.sim->sim();
        sim.options.weatherFriction = weatherFriction();
        sim.setPolygonalBound(true); // vehCar::Init(..., true) in mmNetObject::Init
        if (simulated) {
            // As this machine's car (loadVehicle): mmPlayer::Update's input
            // overrides, the player's damage, its own random stream.
            sim.options.player = true;
            sim.ownRandom = true;
            sim.randomState = 1;
            rv.driver.attach(*rv.sim);
            sim.onImpactCallback = [this, id](const phys::CarImpact& impact) { hostCarImpact(id, impact); };
        } else {
            sim.body.kinematic = true;
            sim.body.resetCollider();
            rv.sim->addTo(*m_world);
            orderPlayerCars();
        }
        rv.renderer = std::make_unique<game::VehicleRenderer>(ctx.device(), *m_textures, *m_models,
                                                              rv.sim->model(), rv.color);
        setupVehicleRenderer(ctx, *rv.renderer);
        if (const auto* trailer = rv.sim->trailerModel()) {
            rv.trailer = std::make_unique<game::VehicleRenderer>(ctx.device(), *m_textures, *m_models,
                                                                 *trailer, rv.color, "TRAILER", "TWHL");
            setupVehicleRenderer(ctx, *rv.trailer);
        }
        // mmNetObject::Init -> vehCar::Init -> vehSiren::vehSiren.
        game::VehicleRenderer::setLightGlowScales(0.2f, 0.6f);
        // vehCar::Init gives the network car its vehCarAudioContainer
        // (engine, tyres, horn; a police car's siren), which
        // mmNetObject::PositionUpdate drives from the packets.
        const auto* info = ctx.game->catalog.vehicle(vehicle);
        rv.audio = loadAiCarAudio(ctx, vehicle, info && (info->flags & game::VehicleInfo::kFlagCop));
        // ... and its vehCarDamage (vehCar::Init): the host's own collisions
        // of it, or (on a client) the host's record of them replayed.
        rv.fx = loadVehicleFx(ctx, vehicle, rv.sim->model(), *rv.renderer);
    }

    // --- OpenMM2: the players' cars simulated by the host (game/net/PlayerCars) -------------

    // A client's simulation runs a little faster or slower to keep the host
    // supplied with its inputs (reconcileNetCar).
    float physicsDt(Context& ctx, float dt) const {
        return multiplayer(ctx) && !ctx.netGame->isHost() ? dt * m_netDilation : dt;
    }

    // Host: the clients' inputs, and their cars, before the frame's samples.
    void updateHostCars(Context& ctx, float dt) {
        const std::uint8_t self = ctx.netGame->localId();
        for (auto& r : ctx.netGame->takePlayerInputs())
            if (r.player != self && !m_netLeft.contains(r.player))
                m_remotes[r.player].inputs.receive(r.msg);
        const double lagMs = static_cast<double>(m_world->remainderAfter(dt)) * 1000.0;
        m_netStateTime = static_cast<std::uint32_t>(std::max(0.0, ctx.netGame->frameTime() - lagMs));
        m_remoteDrawn.clear(); // updateDrawnPoses, after the samples
        const bool damage = !m_session || m_session->playerDamageEnabled();
        std::vector<std::uint8_t> present;
        for (const auto& p : ctx.netGame->players()) {
            if (p.id == self || m_netLeft.contains(p.id) || !ctx.netGame->playerLoaded(p.id))
                continue;
            present.push_back(p.id);
            RemoteVehicle& rv = m_remotes[p.id];
            const game::NetCar car = ctx.netGame->playerCar(p.id);
            if (!rv.simulated || rv.base != car.vehicle || rv.color != car.color)
                loadRemoteCar(ctx, p.id, rv, car, true);
            if (!rv.sim)
                continue;
            if (!rv.placed) {
                // Its player's first command puts it where that machine
                // started it.
                const auto first = rv.inputs.ready() ? rv.inputs.placement() : std::nullopt;
                if (!first || !netCommandAllowed(rv, *first))
                    continue;
                game::NetCarDriver::command(*rv.sim, *first);
                rv.lastMoveSeq = first->seq;
                rv.sim->addTo(*m_world);
                orderPlayerCars();
                rv.placed = true;
                log::info("race: player {}'s car starts at ({:.1f}, {:.1f}, {:.1f})", p.id, first->position.x,
                          first->position.y, first->position.z);
            }
            auto& sim = rv.sim->sim();
            sim.body.declare(3, 0x1b); // mmNetObject::Update
            if (auto* trailer = rv.sim->trailer())
                trailer->body.declare(3, 0x1b);
            sim.setWaterLevel(waterLevelAt(sim.modelMatrix().m3));
            sim.damage.enabled = damage; // EnableRacers / DisableRacers
        }
        dropRemoteCars(present);
    }

    // Host: whether a client's command may reset its car (game::ResetRules,
    // on this machine's simulation of it).
    bool netCommandAllowed(const RemoteVehicle& rv, const net::CarCommand& c) const {
        static const bool debug = std::getenv("OPENMM2_DEBUG_RESPAWN_MS") != nullptr;
        game::ResetRules r;
        r.placed = rv.placed && rv.sim;
        r.lastMoveSeq = rv.lastMoveSeq;
        r.wrecked = rv.sim && rv.sim->sim().damage.maxDamaged();
        r.debug = debug;
        r.city = m_city->psdl.bounds;
        return r.allows(c);
    }

    // What the water's handler does to a car (NetCarDriver::setWaterHandler):
    // in a race RespawnAt the last checkpoint it cleared
    // (mmGameMulti::HitWaterHandler; `checkpoint` its index), elsewhere
    // Reset at its reset position (mmGame::HitWaterHandler, mmMultiCR's).
    net::CarCommand waterHandler(std::optional<int> checkpoint) const {
        if (!checkpoint || !m_session || m_session->setup().checkpoints.empty())
            return {0, net::CarCommandKind::Reset, {}, 0.0f};
        const auto& cps = m_session->setup().checkpoints;
        const int last = static_cast<int>(cps.size()) - 1;
        const auto k = static_cast<std::size_t>(std::clamp(*checkpoint, 0, last));
        const Mat34 at = game::session::spawnAt(cps[k]);
        return {0, net::CarCommandKind::RespawnAt, at.m3, phys::resetRotationOf(at)};
    }
    // This machine's car's handler: where its session puts it back.
    net::CarCommand ownWaterHandler() const {
        if (!m_session)
            return {0, net::CarCommandKind::Reset, {}, 0.0f};
        const std::optional<Mat34> at = m_session->netRespawnPoint();
        if (!at)
            return {0, net::CarCommandKind::Reset, {}, 0.0f};
        return {0, net::CarCommandKind::RespawnAt, at->m3, phys::resetRotationOf(*at)};
    }
    // This machine's car was put back by the water or a fall at the start of
    // its sample `sample` (the host's decision for its own car, a client's
    // prediction of the host's): the trace, Cops and Robbers' handler on the
    // host, and the rest once the frame's samples are done.
    void ownWaterReset(std::uint32_t sample) {
        if (m_traceNet)
            game::traceWaterReset(m_traceNet->traceFile(), m_traceNet->frameTime(), m_traceNet->localId(),
                                  sample, m_player->sim().body.ics.matrix.m3);
        if (m_traceNet && m_traceNet->isHost())
            m_crWaterHandled = m_cr != nullptr; // mmMultiCR::HitWaterHandler drops the gold
        m_ownWaterReset = true;
    }
    // mmPlayer::Reset's presentation: the car's effects and dents, the
    // camera; the session's water message may show again.
    void presentOwnWaterReset() {
        if (!std::exchange(m_ownWaterReset, false))
            return;
        if (m_vehicleFx)
            m_vehicleFx->reset(); // vehCar::Reset
        clearVehicleDamage();
        m_cams.reset(cameraTarget());
        if (m_session)
            m_session->netWaterReset();
    }
    bool m_ownWaterReset = false;

    // Host: a simulated player's car's impact (vehCarDamage::ApplyImpact):
    // its sound, sparks and damage, which everyone is sent.
    void hostCarImpact(std::uint8_t id, const phys::CarImpact& impact) {
        const auto it = m_remotes.find(id);
        if (it == m_remotes.end() || !it->second.sim)
            return;
        RemoteVehicle& rv = it->second;
        if (impact.sound && rv.impacts.size() < 16)
            rv.impacts.push_back({impact.soundStrength, impact.audioId, impact.position});
        if (rv.fx)
            rv.fx->impact(impact, rv.sim->sim());
        if (impact.damaging)
            m_netDamage.player(id).impact(m_netStateTime, game::damageImpactOf(impact, rv.sim->sim()));
        if (impact.damaging)
            if (auto* trace = m_netProps.trace()) // OPENMM2_NET_TRACE
                trace->impact(m_netStateTime, std::format("player{}", id), impactCause(impact), impact.value,
                              rv.sim->sim().damage.currentDamage);
        // mmMultiCR::ImpactCallback for this car: a damaging hit from another
        // player's car.
        if (m_cr && impact.damaging && impact.otherBody) {
            std::optional<std::uint8_t> other;
            if (m_player && impact.otherBody == &m_player->sim().body && m_traceNet)
                other = m_traceNet->localId();
            for (const auto& [oid, orv] : m_remotes)
                if (orv.sim && &orv.sim->sim().body == impact.otherBody)
                    other = oid;
            if (other && *other != id)
                m_crImpacts.push_back({id, *other, impact.total});
        }
        traceNetImpact(id, impact);
    }

    // OPENMM2_NET_TRACE: a collision between two players' cars.
    void traceNetImpact(std::uint8_t self, const phys::CarImpact& impact) {
        if (!impact.otherBody || !m_traceNet || !m_traceNet->tracing())
            return;
        std::optional<std::uint8_t> other;
        if (m_player && impact.otherBody == &m_player->sim().body)
            other = m_traceNet->localId();
        for (const auto& [id, rv] : m_remotes)
            if (rv.sim && &rv.sim->sim().body == impact.otherBody)
                other = id;
        if (other && *other != self)
            m_traceNet->traceImpact(self, *other, impact.position, impact.total,
                                    m_world ? static_cast<double>(m_world->remainder()) * 1000.0 : 0.0);
    }

    // The sample hooks (phys::World::setSampleHooks), every machine of a
    // network race: each player's car takes its input for the sample.
    void beforeNetSample() {
        if (!m_player || !m_traceNet)
            return;
        net::CarInputFrame in = m_netInput;
        in.events = std::exchange(m_netKeys, 0); // the keys go with the first sample after them
        if (m_traceNet->isHost()) {
            // OpenMM2: the water and the fall on every car the host
            // simulates are its own decision (protocol 14).
            m_netDriver.setWaterHandler(ownWaterHandler());
            const std::uint32_t ownResets = m_netDriver.waterResets();
            if (m_netDriver.apply(*m_player, in))
                clearVehicleDamage(); // mmPlayer::ResetDamage
            if (m_netDriver.waterResets() != ownResets)
                ownWaterReset(m_netOwnSample + 1);
            for (auto& [id, rv] : m_remotes) {
                if (!rv.simulated || !rv.placed || !rv.sim)
                    continue;
                auto next = rv.inputs.next();
                if (!next)
                    continue;
                for (const auto& c : next->commands) {
                    if (c.kind == net::CarCommandKind::ResetTo && c.seq == rv.lastMoveSeq)
                        continue; // the placement, carried out already
                    if (!netCommandAllowed(rv, c)) {
                        static const bool verbose = std::getenv("OPENMM2_DEBUG_NETCARS") != nullptr;
                        if (verbose)
                            log::info("netcars: refused player {}'s {} at sample {}", id,
                                      net::carCommandName(c.kind), c.seq);
                        ++rv.refused;
                        continue;
                    }
                    game::NetCarDriver::command(*rv.sim, c);
                    if (c.kind != net::CarCommandKind::ClearDamage)
                        rv.lastMoveSeq = c.seq;
                    ++rv.resets;
                    hostCarReset(id, rv);
                }
                // Nobody goes before the start, whatever its input says.
                if (m_netHeld)
                    next->frame.flags |= net::kInputHeld;
                hostGoldInput(id, next->frame);
                // mmGameMulti::HitWaterHandler (a race: the last checkpoint
                // the referee counted it clearing) or mmGame's / mmMultiCR's.
                rv.driver.setWaterHandler(waterHandler(m_netRules.respawnIndex(id)));
                const std::uint32_t waterResets = rv.driver.waterResets();
                if (rv.driver.apply(*rv.sim, next->frame))
                    hostCarReset(id, rv);
                if (rv.driver.waterResets() != waterResets) {
                    m_crResets.insert(id); // mmMultiCR::HitWaterHandler / DropThruCityHandler
                    ++rv.resets;
                    hostCarReset(id, rv);
                    game::traceWaterReset(m_traceNet->traceFile(), m_traceNet->frameTime(), id, next->seq,
                                          rv.sim->sim().body.ics.matrix.m3);
                }
                rv.input = next->frame;
            }
            return;
        }
        // A client: the other players' cars it simulates take the input the
        // host last applied to them.
        for (auto& [id, rv] : m_remotes)
            if (rv.predicted && rv.sim) {
                rv.lastInput = rv.input;
                if (!rv.inputsAhead.empty()) {
                    rv.lastInput = rv.inputsAhead.front();
                    rv.inputsAhead.pop_front();
                }
                rv.driver.apply(*rv.sim, rv.lastInput);
            }
        // Where the bodies around its car (the other players' cars, the
        // police, knocked traffic cars and props) stood for this sample: the
        // samples run again meet them there.
        m_bodyHistory.push_back({m_prediction.nextSeq(), bodyPoses(), {}});
        if (m_trafficClient && m_trafficBodies && netTrafficReplay() != NetTrafficReplay::Frame)
            m_bodyHistory.back().rails =
                m_trafficBodies->railPoses(m_player->sim().body.ics.matrix.m3, 60.0f);
        while (m_bodyHistory.size() > 240)
            m_bodyHistory.pop_front();
        // Its car's first sample puts it where this machine started it, on
        // the host too.
        if (m_prediction.nextSeq() == 1)
            m_prediction.command(*m_player, {0, net::CarCommandKind::ResetTo, m_player->sim().resetPos(),
                                             m_player->sim().resetRotation});
        // The water and the fall as the host will decide them (predicted;
        // its states confirm or correct them).
        m_netDriver.setWaterHandler(ownWaterHandler());
        const std::uint32_t waterResets = m_netDriver.waterResets();
        const std::uint32_t sample = m_prediction.nextSeq();
        m_prediction.beginSample(*m_player, m_netDriver, in);
        if (m_netDriver.waterResets() != waterResets)
            ownWaterReset(sample);
    }
    void afterNetSample() {
        if (!m_player || !m_traceNet)
            return;
        if (m_traceNet->isHost()) {
            netRulesSample(); // OpenMM2: the host's referee sees every car after every sample
            return;
        }
        // Development aid: OPENMM2_DEBUG_NETCARS_NOISE=<fraction> nudges a
        // client's car's momentum by about that fraction each sample, as a
        // machine built by another compiler might round differently.
        static const float noise = [] {
            const char* v = std::getenv("OPENMM2_DEBUG_NETCARS_NOISE");
            return v ? static_cast<float>(str::parseDouble(v).value_or(0.0)) : 0.0f;
        }();
        if (noise > 0.0f) {
            m_noiseState = m_noiseState * 1664525u + 1013904223u;
            const float k = 1.0f + noise * (static_cast<float>(m_noiseState >> 8) / 8388608.0f - 1.0f);
            auto& ics = m_player->sim().body.ics;
            ics.linearMomentum = ics.linearMomentum * k;
            ics.linearVelocity = ics.linearMomentum * ics.invMass;
        }
        m_prediction.endSample(*m_player, m_netDriver);
        recordNearCars(m_prediction.nextSeq() - 1);
    }
    std::uint32_t m_noiseState = 1;

    // Client: where the cars it simulates stand after its sample `seq`.
    void recordNearCars(std::uint32_t seq) {
        for (auto& [id, rv] : m_remotes) {
            if (!rv.predicted || !rv.sim)
                continue;
            while (!rv.trail.empty() && rv.trail.back().seq >= seq)
                rv.trail.pop_back(); // run again: the newer place
            const auto& sim = rv.sim->sim();
            rv.trail.push_back(
                {seq, sim.modelMatrix(), sim.body.ics.matrix, sim.body.ics.linearVelocity, rv.lastInput});
            while (rv.trail.size() > 180)
                rv.trail.pop_front();
        }
    }
    // Client: a simulated car's place at (fractional) sample `t`, from its
    // trail.
    static std::optional<Mat34> trailAt(const RemoteVehicle& rv, double t) {
        if (rv.trail.size() < 2 || t < static_cast<double>(rv.trail.front().seq))
            return std::nullopt;
        for (std::size_t i = 1; i < rv.trail.size(); ++i) {
            const auto& a = rv.trail[i - 1];
            const auto& b = rv.trail[i];
            if (static_cast<double>(b.seq) < t)
                continue;
            if (b.seq != a.seq + 1)
                return std::nullopt;
            return game::blendTransform(a.model, b.model, static_cast<float>(t - static_cast<double>(a.seq)));
        }
        return std::nullopt;
    }

    // Host: a simulated player's car was reset or its damage cleared
    // (vehCar::ClearDamage): its damage starts again on every machine.
    void hostCarReset(std::uint8_t id, RemoteVehicle& rv) {
        if (rv.fx)
            rv.fx->reset();
        if (rv.renderer)
            rv.renderer->resetDamage();
        m_netDamage.player(id).reset(m_netStateTime);
    }

    // Client: how far this machine's car runs ahead of the host's simulation
    // of it (OpenMM2's shared traffic is shown that far ahead): the host's
    // state after sample `ack` is at session time `time`, and this machine ran
    // that sample at the time its newest sample's end since then gives less
    // the samples in between. Smoothed (the host's queue of inputs moves it
    // by a sample or two).
    void updateNetCarLead(std::uint32_t ack, std::uint32_t time) {
        if (ack == 0)
            return;
        const auto anchor =
            std::ranges::find_if(m_netSampleTimes, [ack](const auto& e) { return e.first >= ack; });
        if (anchor == m_netSampleTimes.end())
            return;
        const double step = static_cast<double>(phys::kFixedSampleStep) * 1000.0 / m_netDilation;
        const double ran = anchor->second - static_cast<double>(anchor->first - ack) * step;
        const double lead = std::clamp(static_cast<double>(time) - ran, 0.0, 1000.0);
        m_netCarLead = m_netCarLead ? *m_netCarLead + (lead - *m_netCarLead) * 0.1 : lead;
    }

    // Client: where the samples run again meet the shared traffic's cars on
    // their rails: where each sample met them (History), or where the host
    // had them at that sample's time (Predicted: TrafficClient::poseAt, from
    // the newest messages), or where this frame put them (Frame).
    // OPENMM2_NET_TRAFFIC_REPLAY=predicted|frame picks the others, for
    // comparison.
    enum class NetTrafficReplay { History, Predicted, Frame };
    static NetTrafficReplay netTrafficReplay() {
        static const NetTrafficReplay mode = [] {
            const char* v = std::getenv("OPENMM2_NET_TRAFFIC_REPLAY");
            const std::string_view s = v ? v : "";
            return s == "frame" ? NetTrafficReplay::Frame
                                : s == "predicted" ? NetTrafficReplay::Predicted : NetTrafficReplay::History;
        }();
        return mode;
    }

    // Client: the session time sample `seq` of this machine's car ended at
    // (m_netSampleTimes keeps each frame's last).
    std::optional<double> netSampleTime(std::uint32_t seq) const {
        const auto e =
            std::ranges::find_if(m_netSampleTimes, [seq](const auto& x) { return x.first >= seq; });
        if (e == m_netSampleTimes.end())
            return std::nullopt;
        const double step = static_cast<double>(phys::kFixedSampleStep) * 1000.0 / m_netDilation;
        return e->second - static_cast<double>(e->first - seq) * step;
    }

    // Client: the host's states of this machine's car since the last frame:
    // the newest corrects the prediction when they differ; how many of its
    // inputs the host had in hand sets how fast the samples run.
    void reconcileNetCar(Context& ctx) {
        if (!multiplayer(ctx) || ctx.netGame->isHost() || !m_player || !m_world)
            return;
        const auto updates = ctx.netGame->takeOwnCarStates();
        const net::Session::OwnCarUpdate* newest = nullptr;
        std::uint32_t ack = 0;
        for (const auto& u : updates) {
            m_netWaiting.emplace_back(u.arrival, u.waiting);
            if (u.hasOwn && u.ack > ack) {
                newest = &u;
                ack = u.ack;
            }
            updateNetCarLead(u.ack, u.time);
        }
        // The host ran this car's samples past this machine's (it stalled, or
        // its inputs were lost for a while): count those as run.
        if (ack != 0 && ack >= m_prediction.nextSeq()) {
            const double rtt = ctx.netGame->peerStats(net::kHostPlayerId).rttMs;
            const auto trip = static_cast<std::uint32_t>(
                std::ceil(rtt / (static_cast<double>(phys::kFixedSampleStep) * 1000.0))) + 1;
            if (const std::uint32_t n = m_prediction.skipTo(ack, trip); n > 0)
                log::info("netcars: the host ran {} samples of this car this machine had not; skipped to {}",
                          n, m_prediction.nextSeq());
        }
        while (m_netWaiting.size() > 1 && m_netWaiting.back().first - m_netWaiting.front().first > 1000.0)
            m_netWaiting.pop_front(); // a second of reports
        // The host keeps a sample of this car's inputs in hand at the least
        // (HostInputQueue): below that the samples run 3% faster (the inputs
        // reach it sooner), above three 2% slower.
        if (!m_netWaiting.empty()) {
            const std::int32_t least = std::ranges::min_element(m_netWaiting, {}, [](const auto& w) {
                                           return w.second;
                                       })->second;
            const float target = least < 1 ? 1.03f : (least > 3 ? 0.98f : 1.0f);
            m_netDilation += (target - m_netDilation) * std::min(1.0f, m_frameDt * 2.0f);
        }
        if (!newest)
            return;
        // How far ahead of the host this car runs: what OPENMM2_NET_OTHERS=ahead
        // places the others by (the samples not yet acknowledged, less the
        // way back).
        {
            const std::uint32_t sent = m_prediction.nextSeq() - 1;
            const double unacked = static_cast<double>(sent - std::min(ack, sent));
            const double lead = unacked * static_cast<double>(phys::kFixedSampleStep) * 1000.0 -
                                static_cast<double>(ctx.netGame->peerStats(net::kHostPlayerId).rttMs) * 0.5;
            m_netLead = m_netLead < 0.0 ? lead : m_netLead + (lead - m_netLead) * 0.1;
        }
        const game::VehiclePose before = drawnPose(game::Drawn::Player, 0, *m_player);
        // Where the other players' cars are drawn now: the drawing blends
        // from there to where the host's states put them.
        std::map<std::uint8_t, Mat34> othersBefore;
        std::set<std::uint8_t> predictedBefore;
        for (const auto& [id, rv] : m_remotes) {
            if (!rv.sim || rv.simulated)
                continue;
            if (const auto drawn = remoteDrawnBase(id, rv))
                othersBefore[id] = rv.blend.apply(*drawn);
            if (rv.predicted)
                predictedBefore.insert(id);
        }
        std::vector<game::CarPrediction::Companion> companions;
        predictNearCars(newest->nearIds, newest->near, ack, companions);
        predictNetTraffic(ctx, newest->time, companions);
        m_netProps.fullCompanions(newest->time, ctx.netGame->frameTime(), companions, m_netFullBodies,
                                  m_player->sim().body,
                                  [this, ack](const phys::Body* b) -> std::optional<std::pair<Mat34, Vec3>> {
                                      // As each sample began: the one after the acknowledged one.
                                      for (const auto& h : m_bodyHistory)
                                          if (h.seq == ack + 1)
                                              for (const auto& p : h.poses)
                                                  if (p.body == b)
                                                      return std::pair{p.ics, p.velocity};
                                      return std::nullopt;
                                  });
        m_prediction.pusherKey = [this, &ctx](std::uint8_t id) -> const void* {
            if (id == ctx.netGame->localId())
                return m_player->sim().body.collider.key();
            const auto it = m_remotes.find(id);
            if (it == m_remotes.end() || !it->second.sim)
                return nullptr;
            return it->second.sim->sim().body.collider.key();
        };
        // The shared traffic's cars on their rails around this car: the
        // samples run again meet each where that sample met it (or, the
        // experiment, where the host had it at that sample's time); after,
        // where this frame put them (netTrafficReplay).
        const NetTrafficReplay railReplay = netTrafficReplay();
        std::vector<game::TrafficBodies::RailPose> railsNow;
        if (m_trafficClient && m_trafficBodies && railReplay != NetTrafficReplay::Frame)
            railsNow = m_trafficBodies->railPoses(m_player->sim().body.ics.matrix.m3, 60.0f);
        // The bodies the samples run again may move, as they stand now: the
        // ones a sample met, and the ones around the car now (a sample that
        // did not meet one, say a piece the host's state brought later, runs
        // without it: it waits far below the city).
        std::vector<BodyPose> now;
        const auto keep = [&](phys::Body* b) {
            if (std::ranges::none_of(now, [&](const BodyPose& q) { return q.body == b; }) &&
                m_world->contains(b) && !predictedBody(b))
                now.push_back(poseOf(*b));
        };
        for (const auto& h : m_bodyHistory)
            if (h.seq > ack)
                for (const auto& p : h.poses)
                    keep(p.body);
        for (const auto& p : bodyPoses())
            keep(p.body);
        const auto replayStart = std::chrono::steady_clock::now();
        m_netReplaying = true;
        const auto c = m_prediction.acknowledge(
            *m_player, m_netDriver, *m_world, ack, newest->own,
            [this] {
                // What the drawing blends from: the cars before their last
                // sample.
                const std::uint32_t resets = m_player->sim().resets;
                m_drawnPhys.record(game::drawnKey(game::Drawn::Player, 0), m_player->pose(), resets);
                if (m_player->trailer())
                    m_drawnPhys.record(game::drawnKey(game::Drawn::PlayerTrailer, 0), m_player->trailerPose(),
                                       resets);
                for (const auto& [id, rv] : m_remotes)
                    if (rv.predicted && rv.sim)
                        m_drawnPhys.record(game::drawnKey(game::Drawn::RemoteCar, id), rv.sim->pose(),
                                           rv.sim->sim().resets);
                recordNetCopSteps(true);
            },
            [&](std::uint32_t seq) {
                for (const auto& h : m_bodyHistory) {
                    if (h.seq != seq)
                        continue;
                    placeBodies(h.poses);
                    for (const auto& p : now) {
                        const auto met = [&](const BodyPose& q) { return q.body == p.body; };
                        if (std::ranges::none_of(h.poses, met))
                            parkBody(p);
                    }
                    if (!railsNow.empty() && railReplay == NetTrafficReplay::History)
                        m_trafficBodies->placeRailCars(h.rails);
                }
                if (railsNow.empty() || railReplay != NetTrafficReplay::Predicted)
                    return;
                const auto t = netSampleTime(seq);
                if (!t)
                    return;
                const double at = *t + m_netCarLead.value_or(0.0);
                std::vector<game::TrafficBodies::RailPose> then;
                for (const auto& r : railsNow)
                    if (const auto p = m_trafficClient->poseAt(r.id, at))
                        then.push_back({r.id, p->transform});
                m_trafficBodies->placeRailCars(then);
            },
            companions, [this](std::uint32_t seq) { recordNearCars(seq); });
        placeBodies(now);
        if (!railsNow.empty())
            m_trafficBodies->placeRailCars(railsNow);
        // The near cars' inputs for the samples to come: the ones the host
        // held beyond those just run again, then the last again.
        if (c.rebased)
            for (const auto& k : companions)
                for (auto& [id, rv] : m_remotes)
                    if (rv.sim.get() == k.car) {
                        for (auto& t : rv.trail)
                            if (t.seq > ack && t.seq <= ack + static_cast<std::uint32_t>(c.replayed)) {
                                const std::size_t i = t.seq - ack - 1;
                                t.input = i < k.upcoming.size() ? k.upcoming[i]
                                          : k.upcoming.empty()   ? k.input
                                                                 : k.upcoming.back();
                            } else if (t.seq == ack) {
                                t.input = k.input;
                            }
                        rv.inputsAhead.clear();
                        for (std::size_t i = static_cast<std::size_t>(c.replayed); i < k.upcoming.size(); ++i)
                            rv.inputsAhead.push_back(k.upcoming[i]);
                        if (!k.upcoming.empty())
                            rv.input = k.upcoming.back();
                    }
        m_netReplaying = false;
        afterNetTrafficReplay(newest->time);
        m_netProps.afterReplay(newest->time);
        // The other players' cars: what the host's states moved the ones
        // simulated here by, and a switch between simulating one and placing
        // it at its states, drawn away (a switch more slowly).
        for (auto& [id, rv] : m_remotes) {
            const auto from = othersBefore.find(id);
            const auto to = remoteDrawnBase(id, rv);
            const bool switched = rv.predicted != predictedBefore.contains(id);
            if (from == othersBefore.end() || !to || (!rv.predicted && !switched))
                continue;
            rv.blend.halfLife = switched ? 0.15f : 0.08f;
            rv.blend.snapDistance = 20.0f;
            rv.blend.add(from->second, *to);
        }
        const double replayMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - replayStart).count();
        m_netReplayMs += replayMs;
        m_netReplayWorstMs = std::max(m_netReplayWorstMs, replayMs);
        // Run again with the other cars, this one moves only where they meet.
        if (!c.corrected && c.moved.mag() <= 0.003f)
            return;
        const game::VehiclePose after = drawnPose(game::Drawn::Player, 0, *m_player);
        m_correction.add(m_correction.apply(before.body), after.body);
        m_pose = m_player->pose();
        m_trailerPose = m_player->trailerPose();
        ctx.netGame->traceCorrection(ack, c.replayed, c.moved, c.velocityError,
                                     m_correction.offset() == 0.0f);
        static const bool verbose = std::getenv("OPENMM2_DEBUG_NETCARS") != nullptr;
        if (verbose)
            log::info("netcars: correction at sample {} ({} run again): position {:.4f} m, "
                      "velocity {:.4f} m/s, rotation {:.5f}{}{}{}{}; moved {:.4f} m",
                      ack, c.replayed, c.positionError, c.velocityError, c.rotationError,
                      c.damage ? ", damage" : "", c.held ? ", held" : "", c.gear ? ", gear" : "",
                      c.corrected ? "" : ", only the other cars' states", c.moved.mag());
    }

    // Client: the bodies within reach of its car as they stand, and put
    // back there (the samples run again, reconcileNetCar, hold them still).
    // A body that has left the world since is skipped (compared by address
    // against the world's, never read).
    struct BodyPose {
        phys::Body* body = nullptr;
        Mat34 ics, bound;
        Vec3 velocity, spin, kinematicVelocity, kinematicSpin;
    };
    static BodyPose poseOf(const phys::Body& b) {
        return {const_cast<phys::Body*>(&b), b.ics.matrix, b.boundMatrix, b.ics.linearVelocity,
                b.ics.angularVelocity, b.kinematicVelocity, b.kinematicSpin};
    }
    std::vector<BodyPose> bodyPoses() const {
        std::vector<BodyPose> out;
        if (!m_world || !m_player)
            return out;
        std::vector<phys::Body*> near;
        m_world->bodiesNear(m_player->sim().body.ics.matrix.m3, 40.0f, near);
        const phys::Trailer* trailer = m_player->trailer();
        for (phys::Body* b : near)
            if (b != &m_player->sim().body && (!trailer || b != &trailer->body) && !predictedBody(b))
                out.push_back(poseOf(*b));
        return out;
    }
    // A body a sample run again did not meet, out of its way (1 km below).
    void parkBody(const BodyPose& p) {
        phys::Body& b = *p.body;
        b.ics.matrix = p.ics;
        b.ics.matrix.m3.y -= 1000.0f;
        b.boundMatrix = p.bound;
        b.boundMatrix.m3.y -= 1000.0f;
        b.ics.linearVelocity = b.ics.angularVelocity = b.kinematicVelocity = b.kinematicSpin = {};
    }
    void placeBodies(const std::vector<BodyPose>& poses) {
        for (const auto& p : poses) {
            if (!m_world->contains(p.body) || predictedBody(p.body))
                continue;
            phys::Body& b = *p.body;
            b.ics.matrix = p.ics;
            b.boundMatrix = p.bound;
            b.ics.linearVelocity = p.velocity;
            b.ics.angularVelocity = p.spin;
            b.kinematicVelocity = p.kinematicVelocity;
            b.kinematicSpin = p.kinematicSpin;
        }
    }

    // After the frame's samples: the host sends each client its states, a
    // client the inputs the host has not acknowledged.
    void afterNetSamples(Context& ctx) {
        if (!multiplayer(ctx) || !m_player)
            return;
        const std::uint64_t now = net::monotonicMs();
        if (ctx.netGame->isHost()) {
            collectHostCars(ctx);
            if (m_frameSteps > 0 && now - m_netCarsSentAt >= netCarStatesIntervalMs()) {
                sendNetCars(ctx, now);
                sendNetTrafficFull(ctx); // OpenMM2: the shared traffic near each client, in full
                // OpenMM2: the pieces round each client's car, in full.
                for (const auto& [id, rv] : m_remotes)
                    if (rv.simulated && rv.placed && rv.sim)
                        m_netProps.sendFull(*ctx.netGame, id, m_netStateTime, rv.sim->sim().body);
            }
        } else if (m_frameSteps > 0) {
            if (const auto msg = m_prediction.message()) {
                m_netInputBytes += ctx.netGame->sendPlayerInput(*msg);
                ++m_netInputsSent;
            }
            // The session time this machine's car's newest sample ended at,
            // for the lead over the host (reconcileNetCar).
            const double lag = m_world ? static_cast<double>(m_world->remainder()) * 1000.0 : 0.0;
            m_netSampleTimes.emplace_back(m_prediction.nextSeq() - 1, ctx.netGame->frameTime() - lag);
            while (m_netSampleTimes.size() > 240)
                m_netSampleTimes.pop_front();
        }
        const double age = m_world ? static_cast<double>(m_world->remainder()) * 1000.0 : 0.0;
        ctx.netGame->traceFrame(m_pose.body, m_player->sim().body.ics.frameVelocity, age, m_remoteCars);
        logNetCars(ctx, now);
    }

    // Host: the simulated players' cars for the rules, the HUD and the
    // drawing, as the frame's samples left them.
    void collectHostCars(Context& ctx) {
        m_remoteCars.clear();
        for (const auto& p : ctx.netGame->players()) {
            const auto it = m_remotes.find(p.id);
            if (it == m_remotes.end() || !it->second.simulated || !it->second.placed || !it->second.sim)
                continue;
            const RemoteVehicle& rv = it->second;
            const net::VehicleSnapshot s = game::carSnapshot(*rv.sim, rv.input);
            game::NetRemoteCar rc;
            rc.id = p.id;
            rc.name = p.name;
            rc.car = ctx.netGame->playerCar(p.id);
            rc.time = m_netStateTime;
            rc.transform = rv.sim->sim().modelMatrix();
            rc.velocity = s.linearVelocity;
            rc.angularVelocity = s.angularVelocity;
            rc.controls = s.controls;
            rc.damage = s.damage;
            rc.flags = s.flags;
            rc.hasState = true;
            m_remoteCars.push_back(std::move(rc));
        }
    }

    // Host: how often each client gets its CarStates: after every frame that
    // ran a sample, up to 60 a second (OPENMM2_NET_STATE_HZ=<n> for fewer,
    // the comparison; protocol 9 sent 20).
    static std::uint64_t netCarStatesIntervalMs() {
        static const std::uint64_t ms = [] {
            const char* v = std::getenv("OPENMM2_NET_STATE_HZ");
            const double hz = v ? std::clamp(str::parseDouble(v).value_or(60.0), 1.0, 60.0) : 60.0;
            return hz >= 60.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(1000.0 / hz);
        }();
        return ms;
    }

    // Host: the other players' cars sent to a client in full
    // (net::NearCarState): those within kNearEnter metres of its car, kept
    // until kNearLeave, the nearest net::kMaxNearCars; not one towing a
    // trailer (its trailer's state does not travel).
    static constexpr float kNearEnter = 60.0f, kNearLeave = 70.0f;

    // Host: each client's CarStates: the last input applied to its car and
    // the car's state after it, every other player's car, and the ones near
    // it in full.
    void sendNetCars(Context& ctx, std::uint64_t now) {
        m_netCarsSentAt = now;
        std::vector<std::pair<std::uint8_t, net::VehicleSnapshot>> cars;
        cars.emplace_back(ctx.netGame->localId(), game::carSnapshot(*m_player, m_netInput));
        struct Full {
            std::uint8_t id = 0;
            const game::SimVehicle* car = nullptr;
            net::CarInputFrame input;
            std::vector<net::CarInputFrame> upcoming;
        };
        std::vector<Full> full;
        // The players' cars by their colliders (the states' pushers).
        const game::PusherOf pusherOf = [this, &ctx](const void* key) -> std::uint8_t {
            if (key == m_player->sim().body.collider.key())
                return ctx.netGame->localId();
            for (const auto& [id, rv] : m_remotes)
                if (rv.sim && key == rv.sim->sim().body.collider.key())
                    return id;
            return net::kPusherOther;
        };
        if (!m_player->trailer())
            full.push_back({ctx.netGame->localId(), m_player.get(), m_netInput, {}});
        for (const auto& [id, rv] : m_remotes)
            if (rv.simulated && rv.placed && rv.sim) {
                cars.emplace_back(id, game::carSnapshot(*rv.sim, rv.input));
                if (!rv.sim->trailer())
                    full.push_back({id, rv.sim.get(), rv.input, rv.inputs.upcoming(net::kMaxUpcomingInputs)});
            }
        for (auto& [id, rv] : m_remotes) {
            if (!rv.simulated || !rv.sim)
                continue;
            net::CarStatesMsg msg;
            msg.time = m_netStateTime;
            msg.ack = rv.inputs.lastApplied();
            msg.waiting = rv.inputs.takeLeastWaiting();
            msg.hasOwn = rv.placed && msg.ack != 0;
            if (msg.hasOwn)
                msg.own = game::ownCarState(*rv.sim, rv.resets, &rv.driver, pusherOf);
            for (const auto& c : cars)
                if (c.first != id)
                    msg.cars.push_back(c);
            if (rv.placed) {
                const Vec3 at = rv.sim->sim().body.ics.matrix.m3;
                std::vector<std::pair<float, const Full*>> near;
                for (const auto& f : full) {
                    if (f.id == id)
                        continue;
                    const float d = std::sqrt(f.car->sim().body.ics.matrix.m3.dist2(at));
                    if (d < (rv.nearIds.contains(f.id) ? kNearLeave : kNearEnter))
                        near.emplace_back(d, &f);
                }
                std::ranges::stable_sort(near, {}, &std::pair<float, const Full*>::first); // ties: list order
                if (near.size() > net::kMaxNearCars)
                    near.resize(net::kMaxNearCars);
                rv.nearIds.clear();
                for (const auto& [d, f] : near) {
                    rv.nearIds.insert(f->id);
                    msg.nearIds.push_back(f->id);
                }
                // In full as many as fit one packet, starting with the next
                // in turn (all of them while they fit).
                for (std::size_t k = 0; k < near.size(); ++k) {
                    const Full* f = near[(rv.nearTurn + k) % near.size()].second;
                    net::NearCarState n;
                    n.id = f->id;
                    n.state = game::ownCarState(*f->car, 0, nullptr, pusherOf);
                    n.input = f->input;
                    n.input.events = 0;
                    n.upcoming = f->upcoming;
                    msg.near.push_back(std::move(n));
                    if (net::encodeMessage(msg).size() > net::kMaxCarStatesBytes) {
                        msg.near.pop_back();
                        break;
                    }
                }
                if (!near.empty())
                    rv.nearTurn = (rv.nearTurn + std::max<std::size_t>(1, msg.near.size())) % near.size();
            }
            m_netStatesBytes += ctx.netGame->sendCarStates(id, msg);
            ++m_netStatesSent;
        }
    }

    // Every 10 s with OPENMM2_DEBUG_NETCARS: what the players' cars cost and
    // how the prediction fares.
    void logNetCars(Context& ctx, std::uint64_t now) {
        static const bool verbose = std::getenv("OPENMM2_DEBUG_NETCARS") != nullptr;
        if (m_netStatsAt == 0)
            m_netStatsAt = now;
        if (now - m_netStatsAt < 10000)
            return;
        const double seconds = static_cast<double>(now - m_netStatsAt) / 1000.0;
        m_netStatsAt = now;
        if (verbose && ctx.netGame->isHost()) {
            std::string queues;
            for (const auto& [id, rv] : m_remotes)
                if (rv.simulated)
                    queues += std::format(" player {}: {} inputs missed, {} dropped to catch up, {} resets "
                                          "refused;",
                                          id, rv.inputs.missed(), rv.inputs.skipped(), rv.refused);
            log::info("netcars: host sent {} states ({:.0f} B/s);{}", m_netStatesSent,
                      static_cast<double>(m_netStatesBytes) / seconds, queues);
        } else if (verbose) {
            const auto& st = m_prediction.stats();
            log::info("netcars: client sent {} inputs ({:.0f} B/s); {} acks, {} corrections, {} samples "
                      "replayed ({:.1f} ms a second, at most {:.2f} ms a frame), {} too old; "
                      "running at {:.3f}",
                      m_netInputsSent, static_cast<double>(m_netInputBytes) / seconds, st.acks,
                      st.corrections, st.replayedSamples, m_netReplayMs / seconds, m_netReplayWorstMs,
                      st.unreplayable, m_netDilation);
            m_netReplayMs = m_netReplayWorstMs = 0.0;
        }
        m_netStatesBytes = m_netStatesSent = m_netInputBytes = m_netInputsSent = 0;
        // The rules' messages (game/net/NetRules): sent by the host, taken by a client.
        if (verbose && m_netRules.active()) {
            const auto& r = m_netRules.stats();
            log::info("netrules: {} messages sent ({:.0f} B/s) and {} states ({:.0f} B/s), {} taken and {} "
                      "states, {} decisions, {} refused, {} malformed, {} stale, {} decisions missed",
                      r.sent - m_netRulesSent.sent,
                      static_cast<double>(r.bytes - m_netRulesSent.bytes) / seconds,
                      r.states - m_netRulesSent.states,
                      static_cast<double>(r.stateBytes - m_netRulesSent.stateBytes) / seconds, r.received,
                      r.statesTaken, r.decisions, r.refused, r.malformed, r.stale, r.gaps);
            m_netRulesSent = r;
        }
    }
    game::NetRules::Stats m_netRulesSent; // the rules' statistics at the last log

    // mmPlayer::Reset, the HitWaterHandlers and vehCar::ClearDamage on this
    // machine's car. A client's command reaches the host with its inputs and
    // happens there at the same sample.
    void netCommand(net::CarCommand c) {
        if (!m_player)
            return;
        if (m_traceNet && !m_traceNet->isHost() && m_world)
            m_prediction.command(*m_player, c);
        else
            game::NetCarDriver::command(*m_player, c);
    }

    // Client: this machine's car's damage as the host decides it (its dents
    // and lost parts; the sparks and sounds it predicted itself).
    void updateOwnNetDamage(Context& ctx) {
        if (!m_player || !m_vehicle)
            return;
        auto& sim = m_player->sim();
        game::DamageTarget t;
        t.renderer = m_vehicle.get();
        t.body = m_drawPose.body;
        t.speed = sim.speed();
        t.texelRadius = sim.damage.params.textelDamageRadius;
        const std::string base = m_player->model().baseName;
        t.eject = [&](const game::VehicleRenderer::Breakable& b, float speed) {
            // OpenMM2: the host threw it for everyone (game/net/NetProps).
            if (m_netProps.hostThrowsParts())
                return false;
            return ejectCarPart(*m_vehicle, base, b, t.body, speed, sim.body.room);
        };
        t.sound = [](const Vec3&, float, int) {};
        const double now = ctx.netGame->frameTime();
        const auto key = game::DamageReplica::playerKey(ctx.netGame->localId());
        m_netDamage.update(key, t, now, now, m_ownDamageFresh);
        m_ownDamageFresh = false;
    }

    void drawRemoteCars(Context& ctx, const game::Camera& camera) {
        if (!multiplayer(ctx))
            return;
        for (const auto& rc : m_remoteCars) {
            const auto it = m_remotes.find(rc.id);
            if (!rc.hasState || it == m_remotes.end() || !it->second.renderer || !it->second.sim)
                continue;
            RemoteVehicle& rv = it->second;
            // vehCarModel::Draw takes the wheels from the car's vehCarSim: the
            // kinematic one's wheels have run this frame's samples against
            // the ground under the placed body (suspension, roll, steering
            // from the snapshot's pedals), so they sit on the road instead of
            // hanging at their rest positions.
            // OpenMM2: the body at its sample a physics step back, the
            // simulated wheels and trailer between their last two samples.
            const auto drawn = m_remoteDrawn.find(rc.id);
            game::VehiclePose pose =
                game::placePose(drawnPose(game::Drawn::RemoteCar, rc.id, *rv.sim),
                                drawn != m_remoteDrawn.end() ? drawn->second : rc.transform);
            pose.headlights = (rc.flags & net::kVehicleHeadlights) != 0;
            pose.brakeLights = (rc.flags & net::kVehicleBrakeLights) != 0;
            pose.reverseLights = rc.controls.gear < 0;
            rv.renderer->draw(pose, camera.transform);
            if (rv.trailer)
                rv.trailer->draw(m_drawnPhys.pose(game::drawnKey(game::Drawn::RemoteTrailer, rc.id),
                                                  rv.sim->trailerPose(), rv.sim->sim().resets),
                                 camera.transform);
        }
    }

    // --- OpenMM2: the network cars' damage (game/net/DamageSync) ----------------------------

    // vehCar::ClearDamage on the player's car's model; in a network race the
    // other machines clear their copy of it too.
    void clearVehicleDamage() {
        if (m_vehicle)
            m_vehicle->resetDamage();
        if (m_result.config.multiplayer && (!m_traceNet || m_traceNet->isHost()))
            m_netDamage.own().reset(m_netStateTime);
    }

    // A car drawn from the network (another player's; the host's police car
    // on a shared-traffic client) at session time `sampleTime`: its damage
    // level as CurrentDamage (vehCarDamage::Update's smoke and the tyre
    // wobble follow it, mmNetObject::PositionUpdate), and its owner's
    // patches, parts and impacts as they become due (its whole record first
    // when `fresh`). The car takes no damage of its own here (its vehCarDamage
    // is off; MM2's simulated network car took its own collisions too).
    void updateNetCarDamage(Context& ctx, std::uint32_t key, game::VehicleRenderer& renderer,
                            game::fx::VehicleEffects* fx, phys::CarSim& sim, const std::string& vehicle,
                            const Mat34& body, const Vec3& velocity, float level, double sampleTime,
                            bool& fresh, std::vector<audio::game::ImpactInput>& sounds) {
        sim.damage.enabled = false;
        sim.damage.currentDamage = game::damageFromFraction(sim.damage.params, level);
        game::DamageTarget t;
        t.renderer = &renderer;
        t.effects = fx;
        t.body = body;
        t.speed = std::abs(velocity.dot(body.m2));
        t.texelRadius = sim.damage.params.textelDamageRadius;
        t.eject = [&](const game::VehicleRenderer::Breakable& b, float speed) {
            // OpenMM2: a client shows the parts the host throws (its ring),
            // the host throws them for everyone.
            if (m_netProps.hostThrowsParts())
                return false;
            const bool player = (key & 0x10000u) != 0;
            const auto owner = player ? net::PropOwner::Player : net::PropOwner::Catalog;
            const int ownerId = player ? static_cast<int>(key & 0xFFu) : m_trafficCatalog.find(vehicle);
            const std::uint32_t tag = netPartTag(owner, ownerId, b.part, renderer.paintjob());
            return ejectCarPart(renderer, vehicle, b, body, speed, sim.body.room, tag);
        };
        t.sound = [&](const Vec3& position, float strength, int audioId) {
            if (sounds.size() < 16) // taken by the car's audio each frame
                sounds.push_back({strength, audioId, position});
        };
        m_netDamage.update(key, t, sampleTime, ctx.netGame->frameTime(), fresh);
        fresh = false;
    }

    // The network cars' vehCarDamage::Update (smoke, fire, exhaust) and the
    // replayed impacts' sparks and shards; their wheels are not simulated
    // (no tracks or wheel particles).
    void updateNetFx(float dt) {
        game::fx::VehicleFxContext context;
        context.wheels = false;
        const std::uint64_t now = net::monotonicMs();
        const bool log = m_debugNetDamage && now - m_netFxLoggedAt >= 2000;
        if (log && m_player) {
            m_netFxLoggedAt = now;
            const auto& d = m_player->sim().damage;
            log::info("netdamage: own car damage {:.0f} of {:.0f} (med {:.0f}), level {:.3f}", d.currentDamage,
                      d.maxDamage(), d.medDamage(), d.damage);
            for (std::size_t i = 0; i < m_cops.size(); ++i) {
                const auto& c = m_cops[i].sim->sim().damage;
                const bool out = m_cops[i].driver->mode() == ai::PoliceCar::Mode::Disabled;
                if (c.currentDamage > 0.0f)
                    log::info("netdamage: own police {} damage {:.0f} of {:.0f} (med {:.0f}){}",
                              kNetPoliceId + static_cast<int>(i), c.currentDamage, c.maxDamage(),
                              c.medDamage(), out ? ", out of action" : "");
            }
        }
        // The smoke comes from the car as drawn, a step behind its body.
        auto update = [&](const char* what, int id, game::fx::VehicleEffects& fx, const game::SimVehicle& car,
                          const std::optional<Mat34>& drawn) {
            context.body.reset();
            if (drawn)
                context.body = drawnIcs(*drawn, car);
            const phys::CarSim& sim = car.sim();
            fx.update(dt, sim, context);
            if (log)
                log::info("netdamage: {} {} damage {:.0f} of {:.0f} (med {:.0f}), smoke {}, sparks {}", what, id,
                          sim.damage.currentDamage, sim.damage.maxDamage(), sim.damage.medDamage(),
                          fx.smoke().count(), fx.sparks().count());
        };
        for (auto& [id, rv] : m_remotes)
            if (rv.fx && rv.sim && !rv.simulated)
                update("player", id, *rv.fx, *rv.sim, netCarDrawn(id));
        for (auto& [id, cop] : m_netCops)
            if (cop.fx && cop.sim)
                update("police", id, *cop.fx, *cop.sim, netCopDrawn(id));
    }

    // Where a network player's car and a shared police car are drawn
    // (model matrices), when known.
    // Client: another player's car as drawn before the blend: simulated
    // here, between its last two samples; else at its interpolated state.
    std::optional<Mat34> remoteDrawnBase(std::uint8_t id, const RemoteVehicle& rv) const {
        if (rv.predicted && rv.sim) {
            // Simulated here: at this machine's car's time while the two may
            // meet (they meet where the host's do) and always for another
            // client's car, else (the host's car) at its player's present
            // (the car's place a lead back on its trail: what the host's
            // newest states put there, little predicted), blended
            // (updateDrawAhead).
            const Mat34 now = drawnPose(game::Drawn::RemoteCar, id, *rv.sim).body;
            if (rv.drawAhead >= 1.0f || !m_world || !m_netCarLead)
                return now;
            const double step = static_cast<double>(phys::kFixedSampleStep) * 1000.0;
            const double last = static_cast<double>(m_prediction.nextSeq()) - 1.0;
            const double alpha = static_cast<double>(m_world->interpolationAlpha());
            const double t = last - 1.0 + alpha - *m_netCarLead / step;
            const auto present = trailAt(rv, t);
            return present ? game::blendTransform(*present, now, rv.drawAhead) : now;
        }
        const auto it = m_remoteInterp.find(id);
        return it != m_remoteInterp.end() ? std::optional(it->second) : std::nullopt;
    }
    std::optional<Mat34> netCarDrawn(std::uint8_t id) const {
        const auto it = m_remoteDrawn.find(id);
        return it != m_remoteDrawn.end() ? std::optional(it->second) : std::nullopt;
    }
    std::optional<Mat34> netCopDrawn(int id) const {
        if (const auto it = m_netCops.find(id); it != m_netCops.end() && it->second.predicted)
            return netCopPose(id, it->second).body;
        return m_trafficClient ? m_trafficClient->transformAt(id, m_netDrawTime) : std::nullopt;
    }
    std::uint64_t m_netFxLoggedAt = 0;

    void drawNetFx(render::Device& dev, const game::Camera& camera) {
        for (auto& [id, rv] : m_remotes)
            if (rv.fx)
                rv.fx->draw(dev, *m_textures, m_cards, m_skids, camera.transform);
        for (auto& [id, cop] : m_netCops)
            if (cop.fx)
                cop.fx->draw(dev, *m_textures, m_cards, m_skids, camera.transform);
    }

    // Development aid: OPENMM2_DEBUG_RESPAWN_MS=<session ms>[,<ms>...] puts a
    // network player's car back at its reset position at those session
    // times ("+<ms>": from the race's order), as the water does in a cruise
    // (mmPlayer::Reset: its damage cleared on every machine).
    void debugRespawn(Context& ctx) {
        static const char* times = std::getenv("OPENMM2_DEBUG_RESPAWN_MS");
        if (!times || !multiplayer(ctx) || !m_player || !ctx.netGame->raceStarted())
            return;
        const auto list = str::split(times, ',');
        const auto k = static_cast<std::size_t>(m_debugRespawns);
        if (k >= list.size())
            return;
        // "+<ms>": from the race's order, as OPENMM2_DEBUG_NET_SHOT_MS.
        const bool fromOrder = list[k].starts_with('+');
        const long long at = str::parseInt(fromOrder ? list[k].substr(1) : list[k]).value_or(0) +
                             (fromOrder ? static_cast<long long>(ctx.netGame->raceOrderTime()) : 0LL);
        if (static_cast<long long>(ctx.netGame->sessionTime()) < at)
            return;
        ++m_debugRespawns;
        log::info("race: debug respawn at session t {}", ctx.netGame->sessionTime());
        netCommand({0, net::CarCommandKind::Reset, {}, 0.0f});
        if (m_vehicleFx)
            m_vehicleFx->reset();
        clearVehicleDamage();
        m_cams.reset(cameraTarget());
    }
    int m_debugRespawns = 0;
    int m_netShotsTaken = 0; // OPENMM2_DEBUG_NET_SHOT_MS's times passed

    // What this machine's cars painted and broke goes to the others, after
    // this frame's effects (this player's car; a shared-traffic host's police).
    void sendNetDamage(Context& ctx) {
        if (!multiplayer(ctx))
            return;
        const std::uint64_t now = net::monotonicMs();
        m_netDamage.send(*ctx.netGame, now);
        m_netDamage.logStats(now);
    }

    // --- OpenMM2: the host's props (game/net/NetProps) ---------------------------------------

    // This machine's own car (or its trailer): on a client the only thing
    // that may set a prop moving (the host decides what the others do).
    bool ownCarBody(const phys::Instance* other) const {
        if (!m_player || !other)
            return false;
        if (other == &m_player->sim().body)
            return true;
        const phys::Trailer* trailer = m_player->trailer();
        return trailer && other == &trailer->body;
    }

    // A client: what may set its props moving (the host decides what the
    // others do): its own car, and the cars it simulates along with it,
    // whose knocks it predicts as its own: the other players' near it
    // (predictNearCars), and the shared police and knocked traffic cars the
    // host sends it in full or its car knocked loose (predictNetTraffic).
    bool propMover(const phys::Instance* other) const {
        if (!other)
            return false;
        if (ownCarBody(other))
            return true;
        for (const auto& [id, rv] : m_remotes)
            if (rv.predicted && rv.sim && other == &rv.sim->sim().body)
                return true;
        for (const auto& [id, c] : m_netCops)
            if (c.predicted && c.sim && other == &c.sim->sim().body)
                return true;
        if (m_netTrafficCars && m_trafficBodies) {
            const int car = m_trafficBodies->carOfBody(other);
            return car >= 0 && m_netTrafficCars->simulated(car);
        }
        return false;
    }

    // A client: which player's car set a prop moving (-1: none of them).
    int propMoverCar(const phys::Instance* other) const {
        if (!other)
            return -1;
        if (ownCarBody(other))
            return m_netLocalId;
        for (const auto& [id, rv] : m_remotes)
            if (rv.predicted && rv.sim && other == &rv.sim->sim().body)
                return id;
        return -1;
    }
    // A client: where the host had player `id`'s car, and when (its states
    // as this machine shows them).
    std::optional<std::pair<double, Vec3>> hostCarState(int id) const {
        for (const auto& rc : m_remoteCars)
            if (rc.id == id && rc.hasState)
                return std::pair{rc.time, rc.transform.m3};
        return std::nullopt;
    }

    // Host: where a client's car is (what its props messages carry first).
    std::optional<Vec3> netCarAt(std::uint8_t id) const {
        const auto it = m_remotes.find(id);
        if (it == m_remotes.end() || !it->second.simulated || !it->second.placed || !it->second.sim)
            return std::nullopt;
        return it->second.sim->sim().body.ics.matrix.m3;
    }

    // What set a prop moving, for OPENMM2_NET_TRACE's props lines.
    std::string propToucher(const phys::Instance* by) const {
        if (!by)
            return "world";
        if (ownCarBody(by))
            return "own";
        for (const auto& [id, rv] : m_remotes) {
            const phys::Trailer* trailer = rv.sim ? rv.sim->trailer() : nullptr;
            if (rv.sim && (by == &rv.sim->sim().body || (trailer && by == &trailer->body)))
                return std::format("player{}", id);
        }
        for (const auto& c : m_cops)
            if (c.sim && by == &c.sim->sim().body)
                return "police";
        for (const auto& [id, c] : m_netCops)
            if (c.sim && by == &c.sim->sim().body)
                return "police";
        if (m_bangers && m_bangers->isActiveBody(by))
            return "prop";
        return "traffic"; // the traffic's bodies and rail cars (or anything else)
    }

    // What a car's damaging impact was against, for OPENMM2_NET_TRACE: a
    // player's car by its id, else as propToucher names it.
    std::string impactCause(const phys::CarImpact& impact) const {
        if (m_player && impact.otherBody == &m_player->sim().body)
            return std::format("player{}", m_netLocalId);
        return propToucher(impact.otherBody);
    }

    // The session time the simulation will have reached after this frame's
    // fixed steps (before them), or has reached (after them).
    double netSimTime(Context& ctx, float dt, bool afterSteps) const {
        const float left = !m_world ? 0.0f : afterSteps ? m_world->remainder() : m_world->remainderAfter(dt);
        return ctx.netGame->frameTime() - static_cast<double>(left) * 1000.0;
    }

    // A car part's owner as the props name it (game::carPartTag): this
    // machine's car, another player's, or a shared police car.
    std::uint32_t netPartTag(net::PropOwner owner, int ownerId, const std::string& part, int paint) const {
        const int index = game::damagePartIndex(part);
        if (!m_result.config.multiplayer || index < 0 || ownerId < 0)
            return 0;
        return game::carPartTag(owner, ownerId, index, paint);
    }

    // Client: before the physics steps, the host's props.
    void updateNetProps(Context& ctx, float dt) {
        if (!multiplayer(ctx) || !m_bangers)
            return;
        m_netProps.beforeStep(*ctx.netGame, m_netEvents, netSimTime(ctx, dt, false),
                              [this, &ctx](const net::PropDescriptor& d) { return netCarPart(ctx, d); });
    }

    // The banger a thrown part of another machine's car is drawn as.
    std::optional<game::bangers::BangerSet::MirrorSpec> netCarPart(Context& ctx,
                                                                   const net::PropDescriptor& d) {
        std::string vehicle;
        if (d.owner == net::PropOwner::Player) {
            if (d.ownerId == ctx.netGame->localId() && m_player)
                vehicle = m_player->model().baseName;
            else if (const auto it = m_remotes.find(d.ownerId); it != m_remotes.end() && it->second.sim)
                vehicle = it->second.sim->model().baseName;
            else
                vehicle = game::netVehicle(ctx.game->catalog, ctx.netGame->playerCar(d.ownerId).vehicle);
        } else if (const std::string* model = m_trafficCatalog.name(d.ownerId)) {
            vehicle = *model;
        }
        const std::string part = game::damagePartName(d.part, d.paint);
        if (vehicle.empty() || part.empty() || !m_bangerData)
            return std::nullopt;
        game::bangers::BangerSet::MirrorSpec spec;
        spec.data = m_bangerData->find(vehicle + "_" + str::lower(part));
        if (!spec.data)
            return std::nullopt;
        spec.model = vehicle;
        spec.mesh = part;
        spec.paint = d.paint;
        spec.tag = game::carPartTag(d.owner, d.ownerId, d.part, d.paint);
        return spec;
    }

    // After the props' update: the host's props to the others; a client's
    // predictions; the trace.
    void sendNetProps(Context& ctx) {
        if (!multiplayer(ctx) || !m_bangers)
            return;
        const double now = netSimTime(ctx, 0.0f, true);
        m_netProps.afterStep(*ctx.netGame, now, net::monotonicMs(),
                             [this](const phys::Instance* by) { return propToucher(by); });
        auto* trace = m_netProps.trace();
        if (!trace || !m_netProps.traced())
            return;
        // Each car's damage as this machine shows it: its level (0..1
        // between MedDamage and MaxDamage) and its dents (the host's
        // records, which it sends; a client's copies of them).
        const std::uint8_t self = ctx.netGame->localId();
        auto dentsOf = [&](std::uint8_t id) {
            if (!ctx.netGame->isHost())
                return m_netDamage.replica().recordSize(game::DamageReplica::playerKey(id));
            return static_cast<std::size_t>(id == self ? m_netDamage.own().recorded()
                                                       : m_netDamage.player(id).recorded());
        };
        if (m_player)
            trace->car(std::format("player{}", self), m_player->sim().damage.damage,
                       static_cast<float>(dentsOf(self)));
        if (ctx.netGame->isHost()) {
            for (const auto& [id, rv] : m_remotes)
                if (rv.simulated && rv.placed && rv.sim)
                    trace->car(std::format("player{}", id), rv.sim->sim().damage.damage,
                               static_cast<float>(dentsOf(id)));
        } else {
            for (const auto& rc : m_remoteCars)
                if (rc.hasState)
                    trace->car(std::format("player{}", rc.id), rc.damage, static_cast<float>(dentsOf(rc.id)));
        }
    }

    double m_debugInputTime = 0.0; // OPENMM2_DEBUG_INPUT's phases

    void updatePlayer(Context& ctx, float dt) {
        if (!m_player)
            return;
        phys::PedalInput pedals;
        // mmReplayManager::Update reads mmInput::GetThrottle / GetBrakes /
        // GetSteering(playerFilterSteering) / GetHandBrake (the pedal swap
        // is ArcadeControls'); the steering filters use the parameters
        // mmPlayer::Update set from the last frame's speed. It records them
        // in every frame the game runs, the menu up or not.
        if (!m_flyCamera) {
            pedals.accelerator = m_gameInput.throttle();
            pedals.brake = m_gameInput.brakes();
            pedals.handbrake = m_gameInput.handBrake();
            pedals.steering = m_gameInput.steering(dt);
        }
        // Development aid: constant pedal input "accel,brake,steer,handbrake"
        // (the steering through the game pad's filter); several separated by
        // '/' take turns, each for OPENMM2_DEBUG_INPUT_MS (2000) of game time.
        if (const char* dbg = std::getenv("OPENMM2_DEBUG_INPUT")) {
            const auto phases = str::split(dbg, '/');
            const char* ms = std::getenv("OPENMM2_DEBUG_INPUT_MS");
            const auto phaseMs = static_cast<double>(std::max(1LL, str::parseInt(ms ? ms : "").value_or(2000)));
            if (!multiplayer(ctx) || ctx.netGame->raceStarted()) // a network race's from its start
                m_debugInputTime += dt;
            const auto phase = static_cast<std::size_t>(m_debugInputTime * 1000.0 / phaseMs);
            const auto parts = str::split(phases[phase % phases.size()], ',');
            auto f = [&](std::size_t i) {
                return i < parts.size() ? static_cast<float>(str::parseDouble(parts[i]).value_or(0.0)) : 0.0f;
            };
            pedals.accelerator = f(0);
            pedals.brake = f(1);
            pedals.steering = m_gameInput.filterAxis(clampf(f(2), -1.0f, 1.0f), dt);
            pedals.handbrake = f(3);
        }
        if (m_autopilot && m_player) {
            const bool go = (!multiplayer(ctx) || ctx.netGame->raceStarted()) &&
                            !(m_session && m_session->playerHeld());
            const auto c = m_autopilot->drive(dt, m_player->sim(), !go);
            pedals.accelerator = c.reverse ? 0.0f : c.throttle;
            pedals.brake = c.reverse ? c.throttle : c.brake;
            pedals.steering = c.steering;
            pedals.handbrake = 0.0f;
        }
        // ... and records them, and the car is driven with the recorded
        // values (bytes).
        pedals = controls::replayQuantize(pedals);
        // mmGame::Update with the menu up (mmPopup enabled, the game not
        // paused: a network game, a lost race, the chat line): the game's
        // keys are off, but mmGame::UpdateSteeringBrakes still gives the car
        // the recorded inputs for every controller but the mouse (inputDevice
        // 0), whose car keeps the inputs it had.
        if (m_popup != Popup::None && m_gameInput.controller() == controls::Controller::Mouse)
            pedals = m_carPedals;
        m_carPedals = pedals;
        m_steerApplied = pedals.steering; // mmPlayer::SetSteering: +0x2264
        m_gameInput.setSpeed(m_player->sim().speed());
        // Countdown: the car is held until "Go!" (and during wreck
        // penalties, and after a wreck or a multiplayer finish), and until
        // the shared start time in multiplayer.
        // mmPlayer +0x2258: after the other endings the car brakes with the
        // wheel turned full left (CarSim applies it for the player); after
        // the water it is left alone.
        const bool over = (m_session && m_session->playerHold() == game::session::PlayerHold::FinishBrake) ||
                          m_crFinished;
        const bool held = (m_session && m_session->playerHeld()) || (multiplayer(ctx) && m_netHeld);
        if (multiplayer(ctx)) {
            // OpenMM2: in a network race the car takes this input once a
            // sample (game::NetCarDriver, which applies the rest of this
            // function there), as the host takes it for this car.
            m_netInput = game::inputFrame(pedals);
            m_netInput.flags = static_cast<std::uint8_t>(
                (held ? net::kInputHeld : 0) | (over ? net::kInputFinished : 0) |
                (m_netAutomatic ? net::kInputAutomatic : 0) |
                (m_controlOptions.autoReverse ? net::kInputAutoReverse : 0) |
                (hornDown(ctx) ? net::kInputHorn : 0) | (carLights() ? net::kInputHeadlights : 0) |
                (m_regen ? net::kInputRegen : 0));
            m_netInput.extraMass = m_netGold;
            m_netInput.throttleCap =
                static_cast<std::uint8_t>(std::clamp(std::lround(m_throttleCap * 255.0f), 0L, 255L));
            if (m_player->sim().trans.getCurrentGear() > 0)
                pedals.accelerator = std::clamp(pedals.accelerator, 0.0f, m_throttleCap);
            if (over)
                pedals = {};
            else if (held)
                pedals.brake = 1.0f;
            m_lastPedals = pedals;
            updateForceFeedback(ctx, dt, false);
            return;
        }
        m_player->sim().raceFinished = over;
        if (over) {
            pedals = {};
            m_player->drive(pedals);
        } else if (held) {
            // (mmMultiRoam says "Go!" and lets the car go two updates after
            // its own load, with no shared start: NetRaceStart never holds
            // a cruise.)
            m_player->hold(pedals); // vehCar::SetDrivable(0, 1)
            pedals.brake = 1.0f;
        } else {
            m_player->drive(pedals);
        }
        m_lastPedals = pedals;
        updateForceFeedback(ctx, dt, false);
        // Falling out of the city is the session's rule
        // (mmGame::DropThruCityHandler below y = -50); this OpenMM2 safety net
        // catches only cities whose geometry lies far below that.
        if (m_player->sim().modelMatrix().m3.y < std::min(-50.0f, m_city->psdl.bounds.min.y) - 30.0f)
            m_player->reset();
    }

    // Force feedback (mmPlayer::Update -> UpdateFF while mmInput::DoingFF;
    // paused: ResetFF) on the race's joystick (app/ForceFeedback).
    void updateForceFeedback(Context& ctx, float dt, bool paused) {
        m_ff.setDevice(ctx.input.forceFeedback(m_gameInput.controller() == controls::Controller::GamePad));
        if (m_player)
            m_ff.update(controls::ffCarState(m_player->sim()), dt, paused);
    }

    // The horn as mmGame::UpdateHorn last set it (+0x274, the slot's held
    // bit), not in the free camera.
    bool hornDown(Context&) const { return !m_flyCamera && m_hornHeld; }

    // mmGame::UpdateGameInput: the discrete in-race keys, handled while the
    // game is paused too.
    void updateGameInput(Context& ctx) {
        using controls::Action;
        // mmGame::UpdateHorn opens UpdateGameInput: with the menu up neither
        // runs and the horn stays as it was (playing on in a running game).
        m_hornHeld = m_gameInput.held(Action::Horn);
        auto pressed = [&](Action a) { return m_gameInput.fired(a); };
        bool viewChanged = false;
        if (m_hud) {
            auto& hud = *m_hud;
            if (pressed(Action::MapToggle)) {
                m_cams.cycleMap();
                viewChanged = true;
            }
            if (pressed(Action::FullScreenMap)) {
                // In single player the full-screen map pauses the game and
                // leaving it resumes (mmReplayManager flags 0x1c / 0x1d).
                if (!multiplayer(ctx))
                    m_paused = m_cams.mapMode() != game::MapMode::FullScreen;
                m_cams.toggleFullScreenMap();
                viewChanged = true;
            }
            hud.options().mapMode = m_cams.mapMode();
            if (pressed(Action::MapZoom)) {
                hud.toggleMapZoom();
                viewChanged = true;
            }
            if (pressed(Action::RotatingMap)) {
                hud.toggleMapRotation();
                viewChanged = true;
            }
            if (pressed(Action::HudToggle)) {
                hud.toggleCluster();
                viewChanged = true;
            }
            if (pressed(Action::OpponentPosition)) {
                hud.toggleOpponentIcons();
                viewChanged = true;
            }
        }
        // A paused game takes these keys too: mmGame::Update runs while
        // asRoot is paused. (mmGame::UpdatePaused, which would take the C and
        // V keys themselves, is never called: nothing reaches the game's
        // UpdatePaused slot in build 3393.)
        if (pressed(Action::ChangeCamera))
            m_cams.toggleCamera();
        // mmViewMgr::SetViewSetting(2), input event 0x0C (Thrill Cam): the
        // XCam, orbiting the car under the keyboard (CameraInput::orbit).
        if (pressed(Action::ThrillCam))
            m_cams.toggleXCam();
        if (pressed(Action::WideAngle))
            m_cams.toggleWideAngle();
        if (pressed(Action::Dashboard))
            m_cams.toggleDashboard();
        // mmViewMgr::SetViewSetting(9), input event 0x1E: the rear-view mirror.
        if (pressed(Action::RearViewMirror)) {
            m_mirror.toggle();
            m_cams.setViewSetting(game::ViewSetting::Mirror);
        }
        if (m_player && multiplayer(ctx)) {
            // OpenMM2: the car takes them with its next sample's input
            // (game::NetCarDriver), on the host too.
            if (pressed(Action::Transmission))
                m_netAutomatic = !m_netAutomatic;
            if (pressed(Action::ShiftUp))
                m_netKeys |= net::kInputShiftUp;
            if (pressed(Action::ShiftDown))
                m_netKeys |= net::kInputShiftDown;
            if (pressed(Action::Reverse))
                m_netKeys |= net::kInputReverse;
        } else if (m_player) {
            auto& trans = m_player->sim().trans;
            auto& pedals = m_player->controls();
            if (pressed(Action::Transmission)) {
                // Automatic <-> manual; a reverse taken with the swapped
                // pedals goes back to drive first.
                if (pedals.swapThrottle)
                    trans.setDrive();
                pedals.swapThrottle = false;
                trans.automatic(!trans.isAutomatic);
            }
            if (pressed(Action::ShiftUp) && !trans.isAutomatic)
                trans.upshift();
            if (pressed(Action::ShiftDown) && !trans.isAutomatic)
                trans.downshift();
            if (pressed(Action::Reverse)) {
                // Reverse, or first gear from reverse, in either box.
                pedals.swapThrottle = false;
                trans.setCurrentGear(trans.currentGear != phys::Transmission::kReverse
                                         ? phys::Transmission::kReverse
                                         : phys::Transmission::kFirst);
            }
        }
        // mmSingleRace::UpdateGameInput: the target checkpoint.
        if (m_session && pressed(Action::NextCheckpoint))
            m_session->cycleTarget(true);
        if (m_session && pressed(Action::PrevCheckpoint))
            m_session->cycleTarget(false);
        if (viewChanged)
            saveViewSettings(ctx);
    }

    // The driver the frontend last selected (mmPlayerData), for the view
    // settings it keeps.
    void loadProfile() {
        game::ProfileStore store(game::ProfileStore::defaultDir());
        const std::string name = store.lastUsed();
        if (name.empty())
            return;
        for (auto& p : store.list())
            if (p.name == name) {
                m_profile = std::move(p);
                return;
            }
    }

    // The view settings MM2 keeps per driver (mmPlayerConfig): OpenMM2 keeps
    // them in [HUD] of its settings. Defaults: mmStatePack /
    // mmPlayerConfig::DefaultViewSettings.
    void loadViewSettings(Context& ctx) {
        auto& o = m_hud->options();
        const auto& ini = ctx.settings.ini;
        // mmHudMap::Reset applies the kept map mode (the map and the 3D
        // view's place, not the rest of SetViewSetting).
        m_cams.setMapMode(static_cast<game::MapMode>(std::clamp(ini.getInt("HUD", "MapMode", 0), 0LL, 2LL)));
        o.mapMode = m_cams.mapMode();
        o.rotatingMap = ini.getBool("HUD", "RotatingMap", o.rotatingMap);
        o.zoomedIn = ini.getBool("HUD", "MapZoomIn", o.zoomedIn);
        o.opponentIcons = ini.getBool("HUD", "OpponentIcons", o.opponentIcons);
        o.cluster = ini.getBool("HUD", "Cluster", o.cluster);
        m_hudMapBeforeFull = m_cams.mapMode();
    }

    void saveViewSettings(Context& ctx) {
        const auto& o = m_hud->options();
        auto& ini = ctx.settings.ini;
        // The full-screen map is not kept (mmPlayerConfig keeps the mode it
        // was opened from).
        const auto current = m_cams.mapMode();
        const auto mode = current == game::MapMode::FullScreen ? m_hudMapBeforeFull : current;
        if (current != game::MapMode::FullScreen)
            m_hudMapBeforeFull = current;
        ini.setInt("HUD", "MapMode", static_cast<int>(mode));
        ini.setBool("HUD", "RotatingMap", o.rotatingMap);
        ini.setBool("HUD", "MapZoomIn", o.zoomedIn);
        ini.setBool("HUD", "OpponentIcons", o.opponentIcons);
        ini.setBool("HUD", "Cluster", o.cluster);
        ctx.saveSettings();
    }

    // cityLevel::DrawRooms: no rain while the camera's room is subterranean
    // (0x0A), nor in a landmark room (0x20) when something lies over the
    // camera (dgPhysManager::Collide with flags 0x20 from 100 m above it).
    bool rainVisible(const Vec3& eye) const {
        const int flags = levelRoomFlagsAt(eye);
        if (flags & (city::LevelRoomFlag::Subterranean | city::LevelRoomFlag::Covered))
            return false;
        if ((flags & city::LevelRoomFlag::TerrainInstance) && m_world) {
            phys::RayHit hit;
            if (m_world->wheelProbe(eye + Vec3{0.0f, 100.0f, 0.0f}, eye, hit, nullptr, nullptr))
                return false;
        }
        return true;
    }

    game::CameraTarget cameraTarget() const {
        const auto& sim = m_player->sim();
        game::CameraTarget t;
        // camCarCS tracks vehCarSim's world matrix (the model origin); OpenMM2
        // follows it as drawn, between the last two physics samples.
        t.matrix = drawnPose(game::Drawn::Player, 0, *m_player).body;
        t.angularMomentum = sim.body.ics.angularMomentum; // camTrackCS::UpdateCar's spin test
        t.speed = sim.speed(); // vehCarSim: |velocity . Z|
        t.steering = sim.steering;
        t.throttle = sim.engine.throttle;
        t.handBrake = sim.handBrake;
        t.reverseGear = m_player->reversing();
        for (std::size_t i = 0; i < t.wheels.size(); ++i)
            t.wheels[i] = {sim.wheels[i].onGround, sim.wheels[i].intersection.normal};
        // mmPlayer::Update: the lvlRoomInfo flags of the room the car's model
        // is in (0x02 / 0x08 subterranean, 0x20 a terrain-bound instance).
        t.roomFlags = levelRoomFlagsAt(t.matrix.m3);
        return t;
    }

    // The original's car cameras (TrackCamCS / PovCamCS, ported from MM1).
    void updateCarCamera(Context& ctx, float dt) {
        auto& in = ctx.input;
        // OpenMM2 extras: a game pad's right stick looks around, and with the
        // keyboard or the mouse controller its North button changes the
        // camera (the joystick types bind the pad's buttons themselves).
        bool left = false, right = false, back = false, forward = false;
        const auto c = m_gameInput.controller();
        const bool padIsController =
            c == controls::Controller::Joystick || c == controls::Controller::GamePad || c == controls::Controller::Wheel;
        for (const auto& pad : in.gamepads()) {
            using platform::GamepadAxis;
            using platform::GamepadButton;
            if (!padIsController && !m_flyCamera && m_popup == Popup::None &&
                pad.pressed.test(static_cast<std::size_t>(GamepadButton::North)))
                m_cams.toggleCamera();
            const float rx = pad.axes[static_cast<std::size_t>(GamepadAxis::RightX)];
            const float ry = pad.axes[static_cast<std::size_t>(GamepadAxis::RightY)];
            left |= rx < -0.5f;
            right |= rx > 0.5f;
            back |= ry > 0.5f;
            forward |= ry < -0.5f;
        }
        game::CameraInput input;
        // mmInput::GetCamPan: the joystick's POV hat or the look buttons.
        input.camPan = m_gameInput.camPan(left, right, back, forward);
        m_camPan = input.camPan;
        if (const auto extent = ctx.device().sceneExtent(); extent.height)
            input.aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        // camPolarCS::Update reads these keys itself (the XCam and the orbit
        // camera at a multiplayer finish).
        if (!m_flyCamera && m_popup == Popup::None) {
            using platform::Key;
            input.orbit.azimuthDown = in.keyDown(Key::Delete);
            input.orbit.azimuthUp = in.keyDown(Key::PageDown);
            input.orbit.inclineDown = in.keyDown(Key::End);
            input.orbit.inclineUp = in.keyDown(Key::Home);
            input.orbit.closer = in.keyDown(Key::PageUp);
            input.orbit.farther = in.keyDown(Key::Insert);
            input.orbit.fast = in.keyDown(Key::LShift) || in.keyDown(Key::RShift);
        }
        // The cameras' floor, ceiling and wall probes (camTrackCS::MinMax,
        // Collide; mmPlayer::Update's overhead test): dgPhysManager::Collide
        // with mask 0x20, never hitting the player's car.
        const game::CameraProbe probe = [this](const Vec3& from, const Vec3& to, game::CameraHit& out) {
            phys::RayHit hit;
            if (!m_world->wheelProbe(from, to, hit, &m_player->sim().body, nullptr))
                return false;
            out = {hit.position, hit.normal, hit.t};
            return true;
        };
        m_cams.update(dt, cameraTarget(), probe, input);
        m_cams.apply(m_camera);
        // Development aid: fixed orbit "distance,height,angle" around the car.
        if (const char* orbit = std::getenv("OPENMM2_DEBUG_ORBIT")) {
            const auto parts = str::split(orbit, ',');
            if (parts.size() == 3) {
                auto f = [&](int i) { return static_cast<float>(str::parseDouble(parts[i]).value_or(0.0)); };
                const Mat34 car = m_drawPose.body;
                const Vec3 eye = car.m3 + Mat34::rotationY(f(2)).transformDir({0, f(1), f(0)});
                m_camera.transform = game::Camera::lookAt(eye, car.m3 + Vec3{0, 1.0f, 0});
            }
        }
    }

    void updateFlyCamera(Context& ctx, float dt) {
        auto& in = ctx.input;
        if (in.mouseDown(platform::MouseButton::Right)) {
            m_yaw -= in.mouseDelta().x * 0.004f;
            m_pitch = clampf(m_pitch - in.mouseDelta().y * 0.004f, -1.5f, 1.5f);
        }
        const float turn = 1.6f * dt;
        if (in.keyDown(platform::Key::Left))
            m_yaw += turn;
        if (in.keyDown(platform::Key::Right))
            m_yaw -= turn;
        if (in.keyDown(platform::Key::PageUp))
            m_pitch = clampf(m_pitch + turn, -1.5f, 1.5f);
        if (in.keyDown(platform::Key::PageDown))
            m_pitch = clampf(m_pitch - turn, -1.5f, 1.5f);
        const Mat34 rot = Mat34::rotationX(m_pitch) * Mat34::rotationY(m_yaw);
        const Vec3 forward = -rot.m2, right = rot.m0;
        float speed = in.keyDown(platform::Key::LShift) ? 120.0f : 30.0f;
        Vec3 move;
        if (in.keyDown(platform::Key::W) || in.keyDown(platform::Key::Up))
            move += forward;
        if (in.keyDown(platform::Key::S) || in.keyDown(platform::Key::Down))
            move -= forward;
        if (in.keyDown(platform::Key::D))
            move += right;
        if (in.keyDown(platform::Key::A))
            move -= right;
        if (in.keyDown(platform::Key::E))
            move += Vec3::yAxis();
        if (in.keyDown(platform::Key::Q))
            move -= Vec3::yAxis();
        m_position += move * (speed * dt);
        m_camera.transform = rot;
        m_camera.transform.m3 = m_position;
    }

    void drawDebugUi() {
        const auto& s = m_cityRenderer->stats();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("World", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.1f fps", ImGui::GetIO().Framerate);
        ImGui::Text("pos %.1f %.1f %.1f  room %d", m_position.x, m_position.y, m_position.z, s.cameraRoom);
        ImGui::Text("rooms %d  instances %d  draws %d", s.roomsDrawn, s.instancesDrawn, s.drawCalls);
        if (m_aiRenderer) {
            const auto& a = m_aiRenderer->stats();
            ImGui::Text("traffic %zu (%d drawn)  peds %zu (%d)  signals %d", m_ai->cars().size(), a.cars,
                        m_ai->peds().size(), a.peds, a.signals);
            const ai::AmbientCar* nearest = nullptr;
            for (const auto& c : m_ai->cars())
                if (!nearest || c.transform.m3.dist2(m_camera.position()) < nearest->transform.m3.dist2(m_camera.position()))
                    nearest = &c;
            if (nearest) {
                ImGui::Text("nearest car %s at %.1f %.1f %.1f (%.1f m/s)", nearest->model.c_str(), nearest->transform.m3.x,
                            nearest->transform.m3.y, nearest->transform.m3.z, nearest->speed);
                if (std::getenv("OPENMM2_DEBUG_NEAREST"))
                    for (const auto& p : m_ai->peds())
                        log::info("ped {} {} at {:.1f} {:.1f} {:.1f} state {} anim {} frame {:.1f}", p.typeName, p.variant,
                                  p.transform.m3.x, p.transform.m3.y, p.transform.m3.z, p.state, p.animFile, p.frame);
                if (std::getenv("OPENMM2_DEBUG_NEAREST_CARS"))
                    for (const auto& c : m_ai->cars()) {
                        phys::RayHit hit;
                        const bool ground = m_world->probe(c.transform.m3 + Vec3{0, 30, 0}, c.transform.m3 - Vec3{0, 30, 0}, hit);
                        log::info("car {} at {:.1f} {:.1f} {:.1f}  ground below/above: {:.1f}", c.model, c.transform.m3.x,
                                  c.transform.m3.y, c.transform.m3.z, ground ? hit.position.y : -999.0f);
                    }
            }
        }
        if (m_player) {
            const auto& sim = m_player->sim();
            ImGui::Text("%.1f mph  gear %d  %.0f rpm  damage %.0f%%", sim.speedMph(), sim.trans.getCurrentGear(),
                        sim.engine.rpm, sim.damage.damage * 100.0f);
        }
        int tod = static_cast<int>(m_result.config.timeOfDay), weather = static_cast<int>(m_result.config.weather);
        bool changed = ImGui::Combo("Time", &tod, "Morning\0Noon\0Evening\0Night\0");
        changed |= ImGui::Combo("Weather", &weather, "Clear\0Cloudy\0Fog\0Rain\0");
        if (changed) {
            m_result.config.timeOfDay = static_cast<game::TimeOfDay>(tod);
            m_result.config.weather = static_cast<game::Weather>(weather);
            applyEnvironment();
        }
        ImGui::Checkbox("PVS", &m_detail.usePvs);
        if (ImGui::SliderInt("Object detail", &m_objectDetail, 0, 3))
            m_detail.objects = game::ObjectDetail::forLevel(m_objectDetail);
        ImGui::TextDisabled("Keys as in Options > Controls; F2 free camera, F3 this panel");
        ImGui::End();
    }

    ui::TextureCache m_ui;
    ui::TextRenderer m_text;
    float m_lookMessage = 0.0f; // seconds left of the F9 look message
    std::string m_loadingImage;
    int m_loadStep = 0;    // the next part of the loading (loadStep)
    int m_loadPercent = 0; // the loading bar's value (ProgressCB)
    std::chrono::steady_clock::time_point m_loadStart;
    game::RaceResult m_result;
    State m_state = State::ShowLoading;
    double m_time = 0.0;
    float m_frameDt = 0.0f;

    std::unique_ptr<city::CityData> m_city;
    std::unique_ptr<game::TextureLibrary> m_textures;
    std::unique_ptr<game::ModelLibrary> m_models;
    std::unique_ptr<game::CityRenderer> m_cityRenderer;
    game::Environment m_env;
    game::DetailSettings m_detail;
    game::EnvironmentOptions m_envOptions;
    int m_objectDetail = 3; // the Object Detail option, 0-3
    game::Camera m_camera;
    std::unique_ptr<game::CityLevel> m_cityLevel;
    std::unique_ptr<phys::World> m_world;
    int m_frameSteps = 0; // the physics samples this frame ran
    std::unique_ptr<DrawTrace> m_drawTrace = DrawTrace::fromEnvironment(); // OPENMM2_DEBUG_DRAW_TRACE
    std::unique_ptr<game::SimVehicle> m_player;
    std::unique_ptr<game::VehicleRenderer> m_vehicle;
    std::unique_ptr<game::VehicleRenderer> m_trailer;
    game::VehiclePose m_pose;
    game::VehiclePose m_trailerPose;
    // OpenMM2 presentation (game/Interpolation.h): the physics' and the AI's
    // objects as their last steps began, and the player's car and trailer as
    // drawn this frame.
    game::StepHistory m_drawnPhys, m_drawnAi;
    game::VehiclePose m_drawPose, m_drawTrailerPose;
    bool m_flyCamera = std::getenv("OPENMM2_DEBUG_FLY") != nullptr;
    bool m_showDebugOnly = std::getenv("OPENMM2_DEBUG_NOHUD") != nullptr;
    bool m_showDebug = std::getenv("OPENMM2_DEBUG_HUD") != nullptr;
    // The player's controls: the [Controls] options and the chosen
    // controller's bindings read each frame (mmInput, app/GameInput).
    controls::GameInput m_gameInput;
    controls::ForceFeedback m_ff; // mmPlayer::UpdateFF and the effects
    float m_steerApplied = 0.0f;  // the recorded steering (mmPlayer +0x2264)
    controls::Options m_controlOptions;
    // The game is paused (asRoot): the full-screen map or the popup in
    // single player.
    bool m_paused = false;
    // mmReplayManager's reset flag (+0x19): the race starts over at the
    // start of the next frame (applyRestart).
    bool m_restartPending = false;
    // What mmGame::UpdateSteeringBrakes last gave the car (the recorded
    // inputs); with the menu up and the mouse controller the car keeps them.
    phys::PedalInput m_carPedals;
    // The in-race popup (mmPopup). Options: one of the pages
    // frontend::PopupOptions builds (m_popupPage).
    Popup m_popup = Popup::None;
    frontend::PopupPage m_popupPage = frontend::PopupPage::Options;
    std::unique_ptr<frontend::PopupOptions> m_popupOptions;
    std::unique_ptr<frontend::PopupSounds> m_popupSounds;
    std::optional<frontend::PopupScript> m_popupScript = frontend::PopupScript::fromEnvironment();
    std::unique_ptr<ui::Menu> m_popupMenu;
    std::vector<std::unique_ptr<ui::Menu>> m_popupGraveyard;
    ui::NavReader m_nav;
    bool m_popupPaused = false;
    float m_camPan = 0.0f; // mmInput::GetCamPan, kept at mmPlayer +0x1D6C
    bool m_hornHeld = false; // mmGame +0x274: the horn as UpdateHorn last set it
    game::session::MapMode m_hudMapBeforeFull = game::session::MapMode::Off;
    game::PlayerCameras m_cams;
    // The rear-view mirror's camera (camera-props); drawMirror draws it. Its
    // on/off switch and the profile flag are the session's to wire.
    game::RearViewMirror m_mirror;
    std::optional<game::Profile> m_profile; // the driver, for the view settings and rewards
    std::string m_chatText;                  // PUChat's text field
    std::unique_ptr<game::session::CopsAndRobbers> m_cr; // mmMultiCR's rules
    std::vector<game::session::CopsAndRobbers::Impact> m_crImpacts; // the local car's hits on network cars
    int m_crSelf = -1;
    game::session::CrTeam m_crMyTeam = game::session::CrTeam::Robber;
    std::uint32_t m_crRng = 1;  // the places' intersection draws
    float m_crEnd = -1.0f;      // UpdateLimit's wait before the results
    double m_crClock = 0.0;     // seconds of the game since the shared start
    bool m_crFinished = false;
    bool m_crWaterHandled = false;        // the water / fall handler fired this frame
    std::set<std::uint8_t> m_crPlayers;   // the players last frame (who left)
    bool m_regen = false;       // mmPlayer::EnableRegen
    float m_throttleCap = 1.0f; // mmGame +0x40c
    std::uint64_t m_chatSeen = 0;            // NetChatLine::serial of the next line to post
    bool m_textInput = false;                // SDL text input on for the chat line
    std::optional<game::Progress> m_progress; // the reward rules (loaded at the first finish)
    const vfs::Vfs* m_vfs = nullptr;
    std::unique_ptr<ai::World> m_ai;

    // Race rules, opponents and HUD (src/game/session).
    std::unique_ptr<game::session::Session> m_session;
    std::unique_ptr<game::session::Hud> m_hud;
    // MM2's positioned-sound slots (Aud3DObjectManager); declared before every
    // sound that uses it so it outlives them.
    audio::game::Object3DManager m_audioSlots;
    std::vector<std::unique_ptr<AmbientAudio>> m_ambientAudio; // by ambient car id
    std::map<std::string, std::optional<audio::game::CreatureVoiceDef>> m_voiceDefs;
    std::string m_voiceCategory;
    int m_voiceFileNum = -1;
    std::vector<game::TrafficImpact> m_trafficImpacts;
    // The BangerSet instance of each traffic light (ai::World::signals()).
    std::vector<std::optional<std::size_t>> m_signalProps;
    struct Opponent {
        std::size_t sessionIndex = 0; // in Session::opponents() (cars that fail to load are skipped)
        Mat34 spawn;                  // its start (the car's reset place keeps the settled one)
        std::unique_ptr<game::SimVehicle> sim;
        std::unique_ptr<game::VehicleRenderer> renderer;
        std::unique_ptr<ai::Opponent> driver;
        std::unique_ptr<audio::game::OpponentCarAudio> audio;
        std::unique_ptr<game::fx::VehicleEffects> fx;
        // Impact sounds since the last audio update (CarSim's impact events).
        std::shared_ptr<std::vector<audio::game::ImpactInput>> impacts =
            std::make_shared<std::vector<audio::game::ImpactInput>>();
    };
    std::vector<Opponent> m_opponents;
    struct Cop {
        std::unique_ptr<game::SimVehicle> sim;
        std::unique_ptr<game::VehicleRenderer> renderer;
        ai::PoliceCar* driver = nullptr; // owned by m_police
        std::string vehicle;             // its [Police] car (the shared traffic's model)
        int livery = 0;                  // its paint job
        int resets = 0;                  // the shared traffic's generation
        std::unique_ptr<audio::game::OpponentCarAudio> audio;
        std::unique_ptr<game::fx::VehicleEffects> fx;
        std::shared_ptr<std::vector<audio::game::ImpactInput>> impacts =
            std::make_shared<std::vector<audio::game::ImpactInput>>();
        float sirenAngle = 0.0f; // vehSiren::Update: 2.5 pi rad/s while on
        // A shared-traffic host: its damage for the clients (m_netDamage).
        game::DamageRecorder* damage = nullptr;
        // The controls its driver set last (throttle, brake, steering,
        // handbrake), and their sum over the frames since the last full
        // state sent (updateAiDrivers, sendNetTrafficFull: their average
        // travels).
        std::array<float, 4> controls{};
        std::array<float, 4> controlSum{};
        int controlFrames = 0;
    };
    std::unique_ptr<ai::PoliceSquad> m_police;
    std::vector<Cop> m_cops;
    int m_lastImpacts = 0;
    game::session::PlayerState m_playerState;
    int m_vehicleImpacts = 0, m_objectImpacts = 0;
    bool m_resultsShown = false;

    // Props that can be knocked over, and particle effects.
    std::unique_ptr<game::bangers::BangerDataLibrary> m_bangerData;
    std::unique_ptr<game::bangers::BangerSet> m_bangers;
    game::bangers::RoadDecals m_roadDecals;
    std::optional<game::fx::SparkLut> m_sparkColors;
    game::fx::Rand m_ejectRand{0xB4EAu};
    // The gizmos (src/game/world).
    std::unique_ptr<game::world::Gizmos> m_gizmos;
    std::unique_ptr<game::world::CableCars> m_cableCars;
    // MM2's global irand / frand stream (gRandSeed) through the race's
    // set-up, in mmGame::Init's order (loadWorldObjects, loadAi,
    // loadEffects; docs/parity/round3/random-streams.md), then the AI
    // world's (ai::Settings::random), which aiMap::Reset sets back to 1.
    ai::Random m_random{1u};
    std::uint32_t m_racerInitState = 1;  // the stream at the racers' vehCar::Init
    std::uint32_t m_policeInitState = 1; // the stream at the police cars' vehCar::Init
    game::fx::EffectLibrary m_effects;
    std::unique_ptr<game::fx::VehicleEffects> m_vehicleFx;
    std::unique_ptr<game::fx::Weather> m_weather;
    game::fx::ParticleRenderer m_cards;
    game::fx::SkidRenderer m_skids;

    // Multiplayer: other players' cars, drawn from the interpolated snapshots.
    struct RemoteVehicle {
        std::string base;
        int color = -1;
        std::unique_ptr<game::SimVehicle> sim; // kinematic body (and its trailer)
        std::unique_ptr<game::VehicleRenderer> renderer, trailer;
        std::unique_ptr<audio::game::OpponentCarAudio> audio;
        // OpenMM2: its damage as its owner's machine shows it (smoke from
        // the snapshots' level, sparks and shards of the replayed impacts).
        std::unique_ptr<game::fx::VehicleEffects> fx;
        std::vector<audio::game::ImpactInput> impacts; // replayed impact sounds this frame
        bool fresh = true; // its damage record not shown yet
        // Host (OpenMM2, game/net/PlayerCars): the car simulated from its
        // player's inputs, placed where the player's first command puts it.
        bool simulated = false;
        bool placed = false;
        game::HostInputQueue inputs;
        game::NetCarDriver driver;
        net::CarInputFrame input;       // the last one applied
        std::uint32_t resets = 0;       // the commands carried out
        std::uint32_t lastMoveSeq = 0;  // the sample of the last command that moved it
        std::set<std::uint8_t> nearIds; // the other cars sent to its player in full
        std::size_t nearTurn = 0;       // which of them goes in full first in the next state
        std::uint64_t refused = 0;      // its commands the host refused
        // Client (OpenMM2): a car near this machine's that the host sends in
        // full (net::NearCarState) is simulated here along with this
        // machine's car instead of placed at its interpolated states
        // (reconcileNetCar, predictNearCars), on the input the host last
        // applied to it (`input`); each new state, and the change between
        // the two ways of placing it, is blended away in the drawing.
        bool predicted = false;
        game::CorrectionBlend blend;
        std::deque<net::CarInputFrame> inputsAhead; // the host's inputs for its next samples
        // Where it stood after each recent sample of this machine's car (its
        // model matrix by sample number), and how far its drawing is from its
        // player's present (0) toward this machine's car's time (1):
        // remoteDrawnBase.
        struct TrailPoint {
            std::uint32_t seq = 0;
            Mat34 model, ics;
            Vec3 velocity;
            net::CarInputFrame input; // what it ran this sample on
        };
        std::deque<TrailPoint> trail;
        net::CarInputFrame lastInput; // the input of the sample running (beforeNetSample)
        float drawAhead = 0.0f;
    };
    std::map<std::uint8_t, RemoteVehicle> m_remotes;
    // OpenMM2: the network cars' damage (game/net/DamageSync): this player's
    // car's and, on a shared-traffic host, the police's, sent; the other
    // players' and the host's police, received. m_netStateTime is the
    // session time the simulation's state belongs to this frame.
    game::NetDamage m_netDamage;
    std::uint32_t m_netStateTime = 0;
    // A shared-traffic client: the host's police and knocked cars in full
    // (receiveNetTrafficFull) by the host sample they belong to; when each
    // car's came last; the ones the acknowledgement being run puts in
    // (predictNetTraffic).
    static constexpr double kNetFullStaleMs = 250.0; // five states
    static constexpr std::uint64_t kNetCopDrawnKey = 1000; // its drawing's key: this + its id
    std::map<std::uint32_t, std::vector<net::TrafficFullCar>> m_trafficFull;
    struct TrafficFullSeen {
        double at = 0.0; // session ms
        unsigned generation = 0;
        net::AmbientKind kind = net::AmbientKind::Traffic;
    };
    std::map<int, TrafficFullSeen> m_trafficFullSeen;
    std::vector<const phys::Body*> m_netFullBodies;
    game::NetProps m_netProps; // OpenMM2: the host's props (game/net/NetProps)
    int m_netLocalId = -1;     // this player's id in a network race
    std::vector<game::NetRemoteCar> m_remoteCars; // this frame's sample (updateRemoteCars)
    std::map<std::uint8_t, Mat34> m_remoteDrawn;  // and where they are drawn
    std::map<std::uint8_t, Mat34> m_remoteInterp; // client: where their interpolated states put them
    std::map<std::uint8_t, Mat34> m_remoteStepBodies; // their bodies in the last sample (recordStepPoses)
    std::vector<game::NetGameEvent> m_netEvents; // this frame's game events (takeNetEvents)
    std::set<std::uint8_t> m_netLeft;            // players who quit this race
    std::map<std::uint8_t, std::string> m_netPlayers; // the players last frame (who left)
    bool m_netPlayersKnown = false;
    std::uint32_t m_netRace = 0; // NetGame::raceNumber() of this race
    game::NetGame* m_traceNet = nullptr; // the race's session (the sample hooks, OPENMM2_NET_TRACE)
    // OpenMM2: the host simulates every player's car (game/net/PlayerCars,
    // docs/multiplayer.md "Players' cars"). This machine's car takes its
    // input once a sample, as every machine applies it; a client runs its
    // own car ahead on it and the host's states correct it.
    net::CarInputFrame m_netInput;   // this frame's input for this machine's car
    std::uint8_t m_netKeys = 0;      // gearbox keys pressed since the last sample
    bool m_netAutomatic = true;      // the gearbox switch (TRANSMISSION)
    std::uint16_t m_netGold = 0;     // Cops and Robbers: the gold's mass on this car (kg)
    game::NetCarDriver m_netDriver;  // this machine's car
    game::CarPrediction m_prediction;   // client
    game::CorrectionBlend m_correction; // client: what corrections moved, drawn away
    bool m_netReplaying = false;        // client: samples run again (no sounds or effects)
    // Client: its samples' numbers and the session times they ended at
    // (newest last), and how far its car runs ahead of the host's (ms).
    std::deque<std::pair<std::uint32_t, double>> m_netSampleTimes;
    std::optional<double> m_netCarLead;
    bool m_ownDamageFresh = true;       // client: its car's damage record not shown yet
    float m_netDilation = 1.0f;         // client: the rate its samples run at
    double m_netLead = -1.0;            // client: how far (ms) its car runs ahead of the host
    double m_netReplayMs = 0.0, m_netReplayWorstMs = 0.0; // client: what its corrections cost
    struct BodyHistory {
        std::uint32_t seq = 0;
        std::vector<BodyPose> poses;
        std::vector<game::TrafficBodies::RailPose> rails; // the shared traffic's cars on their rails
    };
    std::deque<BodyHistory> m_bodyHistory; // client: the bodies around its car at each recent sample
    // Client: the host's latest counts of its inputs in hand, with their
    // arrival (session ms).
    std::deque<std::pair<double, std::int32_t>> m_netWaiting;
    std::uint64_t m_netCarsSentAt = 0;     // host: the last CarStates
    std::uint64_t m_netStatsAt = 0;        // the last log of the statistics
    std::uint64_t m_netStatesBytes = 0, m_netStatesSent = 0; // host: sent since then
    std::uint64_t m_netInputBytes = 0, m_netInputsSent = 0;  // client: sent since then
    std::optional<game::NetRaceStart> m_netStart; // from the end of the loading (updateNetStart)
    bool m_netHeld = true;                        // its car hold
    std::optional<float> m_netToGo;               // its countdown (Session::setNetStart)
    // OpenMM2: the rules the host decides (game/net/NetRules, docs/multiplayer.md "Rules").
    game::NetRules m_netRules;
    std::uint32_t m_netOwnSample = 0;                // host: its own car's samples
    std::set<std::uint8_t> m_crResets;               // host: cars the water handlers reset this frame
    std::unique_ptr<game::NetAutopilot> m_autopilot; // OPENMM2_DEBUG_AUTOPILOT
    int m_tracedPlace = 0, m_tracedRacers = 0;       // OPENMM2_NET_TRACE's last RE line
    std::vector<std::pair<int, int>> m_tracedScores; // and CS line
    bool multiplayer(Context& ctx) const { return m_result.config.multiplayer && ctx.netGame; }
    std::unique_ptr<game::AiRenderer> m_aiRenderer;
    // A shared-traffic client's received cars as the bodies' traffic
    // (outlives m_trafficBodies, which holds it).
    std::unique_ptr<game::NetTrafficCars> m_netTrafficCars;
    std::unique_ptr<game::TrafficBodies> m_trafficBodies;

    // OpenMM2's shared traffic of a network cruise (see netTraffic()).
    game::TrafficCatalog m_trafficCatalog;
    std::optional<game::TrafficHost> m_trafficHost;     // host: each client's cars
    std::optional<game::TrafficClient> m_trafficClient; // client: the received cars
    std::vector<ai::World::OtherPlayer> m_netTrafficPlayers; // host: the other players for the traffic
    std::vector<ai::TrackedCar> m_netTrackedPlayers;         // host: and for the police
    std::unordered_set<int> m_hornLatch;                     // host: horns since the last message
    std::uint64_t m_trafficSentAt = 0, m_trafficStatsAt = 0, m_trafficLoggedAt = 0;
    struct TrafficSent {
        std::uint64_t bytes = 0, messages = 0, cars = 0;
        std::uint64_t damageBits = 0, wheels = 0; // protocol 4's share (see sendNetTraffic)
        std::uint64_t fullBytes = 0, fullCars = 0; // protocol 12: police and bodies in full
    };
    std::map<std::uint8_t, TrafficSent> m_trafficSent; // host: per client, since the last log
    std::map<std::uint8_t, std::set<int>> m_trafficFullSent; // host: per client, the cars sent in full last
    std::vector<ai::AmbientCar> m_netCars;              // client: the received traffic this frame
    std::vector<ai::AmbientCar> m_netCarsDrawn;         // client: and as drawn, at m_netDrawTime
    double m_netDrawTime = 0.0;
    std::unordered_map<int, float> m_netTireRotation;   // client: their wheels' turn
    std::unique_ptr<game::TrafficProxies> m_trafficProxies; // client: their physics
    std::unordered_set<int> m_netAccidentNodes, m_netAccidentPaths; // client: off-rail cars' components
    std::unordered_map<int, int> m_netAccidentRooms;
    std::map<int, NetCop> m_netCops; // client: the host's police
    double m_trafficRenderTime = 0.0; // client: the session time the shared cars are drawn at
    std::unordered_map<int, game::AiRenderer::PhysicalCar> m_netPhysical; // client: knocked cars with bodies
    std::multimap<std::string, NetCop> m_spareNetCops; // client: police cars no longer shown, by model
    std::set<std::string> m_badNetCopModels;           // client: police models that failed to load
    static constexpr std::size_t kMaxNetCops = 32;
    bool m_trafficCatalogWarned = false;
    // Development aid: OPENMM2_DEBUG_NETTRAFFIC logs the shared cars near
    // each client twice a second, on the host and on the client.
    bool m_debugNetTraffic = std::getenv("OPENMM2_DEBUG_NETTRAFFIC") != nullptr;
    // OPENMM2_DEBUG_NETDAMAGE logs every damage event sent and received.
    bool m_debugNetDamage = std::getenv("OPENMM2_DEBUG_NETDAMAGE") != nullptr;
    std::unordered_set<int> m_debugKnocked;
    std::uint32_t m_traceAiSteps = 0; // OPENMM2_NET_TRACE: the AI step last traced (host)
    game::RailMotionTracker m_railMotion; // host: the rail cars' acceleration and curvature
    game::StillBodies m_stillBodies;      // host: the knocked cars' bodies standing still

    // Sound: the player's car, city ambience and rain (src/audio/game).
    std::unique_ptr<audio::SoundBank> m_bank;
    audio::game::PlayerCarAudio m_carAudio;
    audio::game::CityAmbience m_ambience;
    audio::game::RainAudio m_rain;
    audio::game::PedestrianAudio m_pedAudio;
    std::vector<audio::game::PedestrianSoundInput> m_pedSounds;
    audio::game::Announcer m_announcer;
    audio::game::AudioManager m_audioManager; // AudManager::Update
    bool m_announcerOk = false;
    bool m_carAudioOk = false;
    bool m_tunnel = false; // the audio's tunnel flag (mmPlayer::Update, audio flag 0x80)
    bool m_ambienceStopped = false; // MMDMusicManager +0x53: the ambience segment stopped underground
    audio::Mixer* m_ctxMixer = nullptr;
    std::vector<audio::game::ImpactInput> m_impacts;
    std::map<std::string, audio::game::SoundSlot> m_gameSounds; // the session's sounds by name
    phys::PedalInput m_lastPedals;
    std::unique_ptr<audio::MusicDirector> m_musicDirector;
    bool m_musicFinished = false, m_musicResults = false;
    Vec3 m_position;
    float m_yaw = 0.0f, m_pitch = 0.0f;
};

} // namespace

std::unique_ptr<Screen> makeRaceScreen(Context& ctx, const game::RaceConfig& config) {
    log::info("race: mode {} in {}, vehicle {}", static_cast<int>(config.mode), config.city, config.vehicle);
    return std::make_unique<RaceScreen>(ctx, config);
}

} // namespace mm2::app
