# Parity audit: OpenMM2-only

Audited against MM2Recomp (midtown2.exe build 3393) on 2026-10-07.

This area holds the 112 files classified O (OpenMM2-only): the platform
layer, the render backends and helpers, the network transport, game-source
detection, the video decoders, `src/core` helpers, the app shell and the
tools. Every file was checked for game behaviour: read in full, except the
render backends, the Indeo decoder and the network transport, which are API
plumbing, a translation of FFmpeg's decoder and the ENet/UPnP layer, and were
searched for anything the game would see (render-state mappings, colour
space, constants, timing, settings). Eight groups carry game behaviour: the
frame time step and the main loop's handling of an inactive window, the
audio options' effect on the mixer, the order in which archives override
each other, the camera projection, mipmap generation, joystick axis scaling
and the number parsing the data loaders use. Those functions were audited
against MM2 like the P files and their files are reclassified M below.

Summary: 34 functions; verified 6, fixed 8, deviation 5, inferred 4, open 1,
openmm2 10.

## Frame timing and the main loop

`src/app/App.cpp`, `src/platform/Clock.*`, `src/platform/Platform.*`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `FrameClock::tick` | `datTimeManager::Update`, `datTimeManager::RealTime` (called with 0 by `MainPhase`) | fixed | The game runs in real time: each frame's Seconds is the measured frame time held to ClampMin 0.0001 s .. ClampMax 0.1 s. OpenMM2 clamped to 0.25 s with no minimum, so a hitch between 0.1 and 0.25 s advanced the AI, timers, camera and physics by more than the original allows. Now 0.0001 .. 0.1 s (`kMinFrameSeconds`, `kMaxFrameSeconds`). The very first frame of the process (MM2's FirstFrame, which uses the sample step, i.e. ClampMin) is not reproduced; it only affects the first frame of the intro. Test: `FrameClockParity.ClampsToMM2FrameLimits`. |
| `app::run`: frame loop while another application is active (`freezesWhenInactive`) | `gfxPipeline::gfxWindowProc` (WM_ACTIVATEAPP sets the inactive event flag), `gfxPipeline::Manage` (blocks in GetMessage while inactive), `GameLoop` | fixed | MM2 stands still while inactive: no time step, no update, no drawing, and its DirectSound buffers (created without global or sticky focus, `audSound::CreateSoundBufferFromFile`) are muted by DirectSound. OpenMM2 kept simulating and playing sound while alt-tabbed. Now the loop waits for events without ticking the clock and pauses the audio device; the first frame after reactivation measures the whole pause, which the clock holds to 0.1 s (as MM2's next datTimeManager::Update does). The first-run setup screen (no game data) keeps running. |
| `app::run`: inactive window in multiplayer and automation runs | the lost-focus callback `mmGameMulti::Init` installs (`gfxPipeline_SetLostCallback`) | deviation | In a multiplayer race MM2 closes the network session (`asNetwork::CloseSession`) when it loses focus and tells the game manager to leave (inferred from the call; its meaning is not traced further). OpenMM2 keeps running and connected (freezing would stall the other players). `--frames` and `OPENMM2_FRONTEND_SCRIPT` runs also keep running, since they may never have focus. |
| `app::run`: per-frame order | `GameLoop` | verified | MM2: time step, input poll, window messages, events, audio manager, `asRoot::Update`, draw. OpenMM2: events and input, time step, screen update, draw; the audio mixer runs on its own thread. Same order for everything the game reads. |
| `app::run`: whether the intro plays | start-up code before `MainPhase` (`-nomovie`, `inWindow`), `ebolaPlayMovie` | deviation | MM2 plays `logos.avi` on every start unless run with `-nomovie` (OpenMM2: `--skip-intro` or `[Game] SkipIntro`) or in a window (`-window`). OpenMM2 draws the movie itself and plays it in every window mode. The comment that claimed any key skips it was wrong (see Missing). |
| `app::run`: `--quickstart`, `--frames`, `--screenshot`, `OPENMM2_DEBUG_VEHICLE` | | openmm2 | Development and automation aids. |
| `FrameLimiter` | | openmm2 | Optional frame cap; MM2 has none (the 0.0001 s minimum step aside). It only sleeps and does not change the time step. |
| `platform::waitForEvents` | `GetMessageA` in `gfxPipeline::Manage` | openmm2 | SDL helper for the inactive wait. |

## Input

`src/platform/Input.*`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `Input::updateDevices`: axis scaling | `mmJoystick::inputPrepareDevice`, `mmJoystick::Poll`, `mmJaxis::SetRange`, `mmJaxis::Normalize`, `mmJoystick::GetAxis` | verified | MM2 asks DirectInput for -2000..2000 on X, Y, Z and Rz and maps linearly to -1..1; OpenMM2 maps SDL's -32768..32767 linearly (clamped). Splitting an axis into its two halves (`GetAxis` codes 0x11..0x14) is the session area's input mapping. |
| dead zone | `mmJoystick::SetDeadZone`, `mmInput::Init`, `mmInput::SetDeadZone` | open | MM2 sets DirectInput's dead zone on the X and Y axes to mmInput +0x1b4 × 10000 (0.1 by default, set in `mmInput::mmInput`; the Options dead-zone slider), applied by DirectInput before the game reads the axis. OpenMM2 applies no dead zone at the device layer; `RaceScreen` uses a fixed 0.15 threshold on the left stick without rescaling and ignores the stored `DeadZone` option. Belongs to the session and frontend areas (the input mapping lives in `RaceScreen`). |
| `Input::handleEvent`: key down/up edges, auto-repeat ignored | `ioKeyboard::Update` | inferred | MM2 reads DirectInput keyboard state, which has no auto-repeat; OpenMM2 ignores repeat events. |
| `Input::handleEvent`: keys released on focus loss | DirectInput devices are unacquired when the window is inactive | inferred | Windows behaviour. |
| menu gamepad | `NavReader::read` (MM2's menus read only the keyboard and mouse) | openmm2 | The D-pad steps the focus, A / Start accept, B / Back go back, RB is Tab. The left stick moves the pointer (`app::frontend::stickPointer`, `Input::moveMouse`, which warps the system cursor too): a 0.2 dead zone, speed squared up to 1.2 screen heights a second; the hovered item takes the focus as under the mouse, so A presses it. |
| device enumeration, key names, rumble | `ioJoystick::EnumDeviceProc` | openmm2 | MM2 special-cases a few 2000-era pads by name; SDL's gamepad mappings replace that. The rumble functions are unused (see Missing). |

## Audio options

`src/app/Context.cpp`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `Context::applyAudioSettings`: SOUND FX toggle | `AudioOptions::SetAudioState`, `AudioOptions::SetSoundFX`, the wave mute in AudManager (linker-folded under the name `AudManager::GetMixerPtr`), `audManager::SetVolAllSounds`, `AudSoundBase::AudSoundBase`, `AudSpeech::AudSpeech` | fixed | SOUND FX off sets the volume of every AudSoundBase sound to zero, the same class of sounds `AudManager::AssignWaveVolume` scales. Commentary is played by AudSpeech through AudStream, an AudSoundBase, so it falls silent too; OpenMM2 muted only effects and engines. Now the voice bus needs both SOUND FX and COMMENTARY. Test: `AudioOptionsParity.SoundFxToggleSilencesCommentaryToo`. Not reproduced: with SOUND FX off at start-up MM2 never initialises the wave audio (`InitAudioManager` gets the flag), so switching it on has no effect until the next start; OpenMM2 applies it at once. |
| `Context::applyAudioSettings`: volumes, balance, master | `AudioOptions::SetSFXVolume`, `AudioOptions::SetMusicVolume`, `AudioOptions::SetBalance`, `AudManager::AssignWaveVolume`, `DMusicManager::SetVolume` | verified | Same buses as MM2 (wave volume on effects, engines and voices; music volume on DirectMusic; balance on both). The mixer now applies MM2's `AudManager::AssignWaveVolume` curve to the slider values itself (audio audit); the master volume is an OpenMM2 extra. STEREO FX reaches the mixer (`Mixer::setStereo`: mono centres every voice, surround plays as stereo, `AudioOptions::SetStereoFX`). |
| `Context::applyAudioSettings`: `Bus::Ambient` | `mmAmbientAudio::Update`, `MMDMusicManager::UpdateAmbientSFX` | fixed | The bus carried both the wave ambient sounds (rain, thunder, the city's ambient loops: AudSoundBase sounds, so MM2 scales them with SOUND FX VOLUME and silences them with SOUND FX off) and the DirectMusic ambience segment (MUSIC volume). The audio audit moved the wave sounds to `Bus::Effects`; `Bus::Ambient` now carries only the segment, at the MUSIC volume while CITY SOUNDS is on. The separate, unexposed `[Audio] Ambient` volume is gone (the MUSIC slider wrote it with the music volume). |
| `Context::music`, `shutdownMusic`, `saveSettings`, `loadGameData` | | openmm2 | Glue. |

## Game data mounting

`src/vfs/GameSource.*`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `listArchives`, `probeImage`: archive order (`sortLikeMM2`) | `zipMultiAutoInit` | fixed | MM2 finds every `*.ar` in its folder, upper-cases the paths and sorts them with strcmp. OpenMM2 put the four retail archives first in a fixed order and any others after them in lower-case order. Now one list in upper-case byte order. Test: `GameSourceParity.ArchivesAreSearchedInUpperCaseNameOrder`. |
| `mountGameSource`: which archive wins | `zipMultiAutoInit`, `zipFile::zipFile` (pushes onto the front of the archive list), `zipFile::zipOpen` (searches from the front) | fixed | The archives are created from the last in sorted order to the first, so the search runs in sorted order and the first archive holding a file wins. OpenMM2 let add-on archives override the retail ones and later names override earlier ones; an add-on named after `MM2TEX.AR` now only adds files, one named before `MM2AUD.AR` overrides. The retail archives share no paths (checked against the disc), so retail data is unaffected. Test: `GameSourceParity.FirstArchiveWinsAndLooseFilesAreIgnored`. |
| `mountGameSource`: loose files | `zipFile::Init` (replaces `Stream`'s default open methods with the archive methods) | fixed | Once an archive is open, game data is only ever read from archives. OpenMM2 mounted the installation folder above every archive, so loose files overrode archive content. Removed. |
| `mountGameSource`: required archives | | openmm2 | Validation of the source. |
| `probeGameSource`, `openSourceFile`, `suggestGameSources`, `GameSource::describe` | | openmm2 | Disc, image and install detection. |

## String helpers

`src/core/StringUtil.*`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `str::parseDouble` (then cast to float) | `datAsciiTokenizer::GetFloat` (`atof`, then float) | verified | Same value for a well-formed number (both parse to double and round to float). Stricter: a token with trailing characters fails where atof reads its numeric prefix. Each loader in the formats and city areas decides how to handle that. |
| `str::parseInt` | `datAsciiTokenizer::GetInt` (`atoi`) | deviation | Accepts `0x` hex and rejects trailing text ("12.5", "4 State") where atoi reads the leading digits. Loaders that need atoi's behaviour do it themselves (`ui::MenuLayout`, `game::session::RaceSetup`). |
| `str::iequals`, `str::lower`, `str::upper` | `_stricmp`, `_strupr` (C locale) | verified | ASCII-only case folding in both. |
| `trim`, `split`, `normalizeVirtualPath`, `toPath`, `fromPath`, `parseBool` | | openmm2 | |

## Rendering helpers

`src/render/Projection.*`, `src/render/ImageUtil.*`, the backends.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `computeProjection` (`FovMode::HorPlus`, the default) | `gfxViewport::Perspective`, `gfxViewport::SetWindow`, `asCamera::SetView` | verified | MM2's camera FOV is vertical: y is scaled by 1 / tan(fov / 2) and x by that over the viewport's width / height, so wider viewports see more at the sides. OpenMM2 carries the FOV as its 4:3 horizontal equivalent (`cam::horizontalFov4x3`) and HorPlus converts it back, giving the same projection up to float rounding. The header comment claimed Angel cameras use a horizontal FOV; corrected. |
| `computeProjection` (`VertMinus`, `Stretch`, `maxAspect`) | | deviation | OpenMM2 display options for wide screens. |
| `computeUiLayout`, `scaledExtent` | | deviation | Any-resolution UI scaling and render scale (OpenMM2 extras); `Fit` and `Stretch` equal the original on a 4:3 screen. |
| `render::downsample`, `buildMipChain` | `gfxImage::GenerateMipmaps` (called by `gfxLoadJPEGImage`, `gfxLoadTargaImage`, `gfxLoadBmpImage` when mipmaps are asked for) | fixed | MM2 averages each 2x2 block per channel as the sum shifted right by two (truncated); OpenMM2 rounded ((sum + 2) / 4), making every generated level up to one step brighter per level. Now truncated. Which textures get generated mipmaps at all is `game::TextureLibrary`'s decision (rendering-fx area). Test: `ImageUtil.DownsampleAverages`. |
| Blend, depth and sampler mapping in `GlDevice` and `VulkanDevice` | render states as defined by `render/Types.h` (rendering-fx area) | openmm2 | Checked that both backends implement every `BlendMode` and `Filter` the same way and as `Types.h` documents them, and that output is gamma-space (UNORM, no sRGB conversion) as on MM2's Direct3D. |

## Video

`src/video/*`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `Movie::decodeNext`: empty and null frames repeat the last picture | `ebolaPlayMovie` (MCIWnd, Video for Windows) | inferred | Playback was done by Windows and the system's Indeo codec, not by `midtown2.exe`. |
| `Indeo5Decoder`, `AviFile` | the system's Indeo 5 codec and AVI reader | inferred | Bit-identical to FFmpeg's decoder (docs/video.md). |

## Network

`src/net/*`.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `SessionSettings` defaults, `SnapshotBuffer` interpolation | DirectPlay transport, `mmNetObject` | openmm2 | The transport carries no rules: `game::NetGame::toSessionSettings` (session area) fills every setting from the race configuration, and remote cars' rules live in `game/net`. Notes for the session area: the protocol allows 16 players and `NetGame` clamps to 2..16 where MM2 allowed 8; traffic and pedestrian densities cross the wire as whole percent. |

## Players' cars of a network race

Added 2026-10-09 on the maintainer's decision (the host is the authority).
MM2 runs every network car on every machine as a `vehCar` driven on its
last pedals and pulled toward its packets (`mmNetObject::PositionUpdate`,
`Predict`), each machine with its own collisions (docs/multiplayer.md,
"Players' cars"; docs/review/multiplayer-desync-cars.md). These rows are not
counted in the summary above.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `game::NetCarDriver`, `HostInputQueue`, `CarPrediction`, `CorrectionBlend`, `net::PlayerInputMsg`, `CarStatesMsg` | `mmNetObject`, `mmGameMulti::SendPosition` | deviation | The host simulates every player's car from its inputs; a client predicts its own and is corrected by the host's states. |
| `CarPrediction`'s companions, `net::NearCarState` | `mmNetObject::Predict` (a network car driven on its last pedals) | deviation | A client simulates the other players' cars near its own from the host's full states, the inputs the host last applied and those it holds for their next samples (protocol 17), put to the host's state at every one that differs from what it ran them on (MM2 pulled the car toward its packet along a path). |
| The players' cars in player order among the world's movers (`RaceScreen::orderPlayerCars`) | `dgPhysManager::DeclareMover` (the order of the declarations) | deviation | So that two players' cars collide in the same order on every machine. |
| `game::ResetRules` | `mmPlayer::Reset` from `mmGameMulti::HitWaterHandler` and `DropThruCityHandler` | openmm2 | The host carries out a client's reset only where those rules would. |

## Shared traffic of a network cruise

Added 2026-10-09 on the maintainer's decision (2026-10-09: the traffic and
police are shared, host-authoritative, a host option in the lobby, on by
default). MM2's network cruise has neither: `mmGameMulti::Init` zeroes the
traffic, cop and opponent densities and the cable cars, and each machine runs
its own pedestrians. OpenMM2 keeps that exactly with the option off. With it
on, the host runs the cruise's traffic and police for every player and the
clients show them (docs/multiplayer.md, "Shared traffic"). These rows are not
counted in the summary above.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `RaceScreen::loadAi`, `spawnPolice` in a network cruise | `mmGameMulti::Init` (no traffic, police, racers or rail cars) | deviation | The host keeps its traffic density and the cruise's police posts (`RaceSetup`: the posts of the cruise's `.aimap` at the cop density, as single-player cruise); a client runs neither. Cops and Robbers and the network races are unchanged; the cable cars stay off. |
| `ai::Traffic::step` with several players, `World::setOtherPlayers` | `aiMap::Update`, `aiMap::AddPlayer`, `aiMap::RemovePlayer`, `aiPath::AddAmbPlayer` / `RemAmbPlayer`, `aiGoalRandomDrive::Update`, `aiGoalRegainRail::Update`, `aiGoalAvoidPlayer` (+0xe6), the pose solver's player loop | deviation | MM2's own player list, which a single-player game fills with one player and its network games with only the local one. OpenMM2 lists the other players' cars on the host, in 16 slots where aiMap has four. A player gone from the list gives up its roads from the room they were populated for (`RemovePlayer` asks the room of its car's place; inferred equivalent). The pedestrians keep the local player only. The single-player step is unchanged (the opponent sweep's 516/517 and 508/517 hold). |
| `ai::PoliceCar` chasing the other players | `aiPoliceOfficer::DetectPerpetrator` (walks the player list), `aiMap::Player(0)` | deviation | The other players are players to the police (detected, chased, apprehended by MM2's rules); the rules that read player 0 (the reversing check) read the local player, and only the local car's hits mark what it hits. |
| `ai::World::advanceLightsTo` | `aiTrafficLightSet::Update` | openmm2 | A client's light sets run to the host's step count (they are deterministic from `aiMap::Reset`), so the received traffic stops at the lights it sees. |
| `game::TrafficHost`, `TrafficClient`, `TrafficCatalog`, `TrafficProxies`, `net::AmbientStateMsg` | | openmm2 | Replication: interest by distance with hysteresis, a packet budget (the client's cars kept at the edge), complete messages, generations for recycled slots, the client's car meeting the received cars off their rails as moving kinematic instances. The client's hit reports (`TrafficHitEvent`) went with protocol 6: the host simulates every player's car and sees the hits itself. |
| `game::TrafficPrediction` (`RailMotionTracker`, `predictRailCar`, `predictBody`), `TrafficClient`'s prediction and corrections | `mmNetObject::PositionUpdate` (a network car predicted forward from its last packet) | openmm2 | Added 2026-10-09 (protocol 6, docs/review/multiplayer-desync-traffic.md): the host stamps the traffic with its AI step's time and sends a rail car's acceleration, curvature and speed over the ground; a client shows every car at its own car's time on the host's clock (its lead over the host's simulation of it added), a rail car predicted along an arc, a body along its velocity and yaw rate, the drawing blending corrections away. MM2's network cruise has no traffic; MM2 predicts its network cars forward, which this does for the host's. |
| `game::NetTrafficCars`, `TrafficBodies::Source` | `aiVehicleInstance::AttachEntity`, `aiVehicleAmbient::Impact`, `aiVehicleActive::Detach` / `PostUpdate` (the hand-over between the AI and the bodies) | openmm2 | Added 2026-10-09: `TrafficBodies` takes its traffic through an interface (the AI's for the host and single player: the same calls in the same order); a shared-traffic client gives it the received rail cars, which its own car (and a car it knocked) knocks loose at once with the host's physics; the local body leads until it rests or is 3 m from the host's car, then the host's messages (at once when the host did not knock the car). Protocol 12: a car standing still off its rail without a body on the host stands as a rail car at rest, which the client's car knocks loose as the host's does; in a replay a rail car meets the replayed cars as the body it would take (`RailCar::heldInertia`). |
| `net::TrafficFullMsg`, `RaceScreen::sendNetTrafficFull` / `predictNetTraffic`, `TrafficBodies::bodyState` / `setBodyState`, `CarPrediction::Companion` without a player | `aiPoliceOfficer::Update` (the police's controls), `aiVehicleActive` (a knocked car's body) | openmm2 | Added 2026-10-09 (protocol 12, docs/review/multiplayer-desync-traffic.md): the host sends each client, with its car's state, the police within 60 m with the controls their drivers set (averaged since the last state) and the knocked cars within 50 m in full, one datagram a state; the client simulates them with its own car from those states at each acknowledgement (as the players' cars near it), so its car meets them with their mass where the host's does; a police car runs on those controls until the next state. The host's police draw their wheels' bump numbers from their own streams. Only cars the host's last step ran go (MM2 leaves an undeclared police car or a body beyond its 32 movers where it is). A simulated police car knocks the traffic it hits loose. Without full states (farther, or none for 250 ms) they are predicted and kinematic as before. |
| `phys::Instance::kinematicMotion`, `Body::kinematicMoves` | | openmm2 | A kinematic instance (network car) that reports its motion strikes as a car moving at it (MM2 has no kinematic instances); off by default, on for the network cars of a shared-traffic cruise and the received traffic and police. |
| The host settings' SHARED TRAFFIC panel (`HostSettingsPage`) | `HostRaceMenu` (no traffic or cop settings) | deviation | On / off and the single-player cruise's traffic and cop density sliders (defaults 0.5 and 1, `RaceMenuBase::SetStateRace`), drawn by OpenMM2 in the upper right panel cruise leaves empty. |
| A client's pedestrians and the received cars | `aiPedestrian::UpcomingAccident` / `Accident` (`aiObstacle::InAccident` in the road's section lists) | inferred | The host's section lists do not travel: a received car off its rail counts for its whole road or intersection (`MapView::mapComponent` of its place). |
| A client's police cars' damage (`game::DamageReplica`, `VehicleEffects`, `AmbientEntity::damage`) | `vehCarDamage::Update`, `ApplyImpact`, `fxTexelDamage::ApplyDamage`, `vehBreakableMgr::Eject`, `vehCarModel::EjectOneshot` on the host's cars | openmm2 | Added 2026-10-09 (protocol 4): the host's police damage level (10 bits) drives the received car's smoke as `vehCarDamage::Update` does, and its dents, broken parts, sparks, shards and impact sounds are replayed from the host's records (docs/multiplayer.md, "Damage"); the wreck (out of action) as before, with its black smoke and the explosion sound. Before, a client saw the wreck flag and the explosion only. |
| A client's knocked traffic cars' wheels (`AmbientEntity::wheels`, `game::trafficWheelMatrices`) | `aiVehicleInstance::Draw` with an `aiVehicleActive` (`vehWheelCheap`'s drawing matrices) | openmm2 | Added 2026-10-09 (protocol 4): a car with a body on the host sends its four wheels' drawing offsets; the client draws it on them and lays its shadow as a physical car's. Before, it was drawn on its rail wheels. |

## Props of a network race

Added 2026-10-09 on the maintainer's decision (the host is the authority for
everything). MM2 sends nothing about props: every machine knocks its own
props, parked cars, traffic lights and thrown car parts with its own
simulation of every car (`mmNetObject::Update` declares each network car a
type-3 mover; no message of `mmGameMulti::GameMessageCB` or the modes'
`GameMessage` names a prop). What a network game has is MM2's and unchanged:
no parked cars in a network cruise or Cops and Robbers, no ferries, no cable
cars (`mmGame::InitGizmos`, `mmGameMulti::Init`). These rows are not counted
in the summary above.

| OpenMM2 | MM2 | Verdict | Notes |
| --- | --- | --- | --- |
| `game::PropHost`, `PropClient`, `NetProps`, `net::PropStateMsg`, `PropKnocksEvent` | (none) | openmm2 | The host simulates the props for everyone and sends the knocks (reliable, all of them to a machine that has loaded the race) and its ring of knocked-over props (`dgBangerManager`'s 40 slots); a client shows them a playout delay in the past, predicts its own car's knocks and hands them over (docs/multiplayer.md, "Props"). Since protocol 16 each client gets its own message: every occupied slot, the states near its car (150 m) first within 1100 bytes, the others less often; a client also predicts the knocks of the other players' cars it simulates (`NearCarState`), and undoes a missed one as soon as the host's messages (and, for another player's car, the host's state of it) show it missed. Since protocol 18 the host also sends the pieces round a client's car in full (`net::PropFullMsg`, `PropHost::buildFull`, `PropClient::fullPieces`, `BangerSet::bodyState`, `setBodyState`), which the client runs with its car when its samples run again (with the bound where the sample's collisions saw each piece and its hardest pusher, `BangerSet::pusherOf`, `phys::World::cityKey`, `staticKey`, `order`). |
| `BangerSet::setReplica`, `phys::Instance::acceptsContact` | `dgPhysManager::CollideInstances` | deviation | A client's props take contacts from its own car and the cars it simulates along with it (the other players' near ones, `NearCarState`; the shared police and knocked traffic cars, `TrafficFull`), and its own simulated props one another (no placed prop: the host's knocks bring the chain knocks; since protocol 18 a host's piece it simulates from the host's full states does knock placed props, as the host's does); MM2's take them from every mover. |
| `BangerSet::setRingGrowth` (network races) | `dgBangerManager::GetBanger` (the ring of 40, `dgBangerManager::Init(40)`) | deviation | When the ring is about to wrap onto a prop that still moves or was handed out less than 10 s ago, it grows by 40 slots instead, up to 40 a player (at least 80, at most 256); MM2 wraps and the oldest knocked-over prop disappears, in a pile-up while still flying. Off in single player. Test: `PropSync.APileUpGrowsTheRing`. |
| `BangerSet::breakPlaced`, `restoreStanding`, `showMirror`, `hideMirror` | `dgUnhitBangerInstance::Impact`, `Reset`, `dgHitBangerInstance` | openmm2 | A client's placed prop leaves its room as Impact makes it leave (without a body), stands again as Reset makes it stand (a prediction the host did not confirm), and the host's pieces are hit instances of their own. |
| `BangerSet::attachEntity` on a mirror | `dgBangerActive::Attach` | deviation | MM2 starts an active at rest (a placed or resting prop is); a client's mirror of a host piece in flight starts from the host's motion. |
| `phys::World::collideHeld` with `Instance::heldInertia`, `World::beginReplay`, `BangerSet::replayGhost` | (none: MM2 does not replay) | openmm2 | A client's replayed car meets a prop with the body a real hit gives it (`dgBangerActive::Attach`'s), which moves through the replay only as the hits push it (no gravity or city: inferred stand-in for its flight); a prop the car broke loose in the last second stands again for the replayed samples up to the one that broke it loose, so the replay takes the same knock, and the later ones meet its pieces where they were (`World::replayTime`), a simulated body moved on over the sample by its velocity, as a real sample moves it before the collisions. |
| Thrown car parts (`ejectPart`'s tag, `game::carPartTag`) | `vehBreakableMgr::Eject` on every machine's simulation | deviation | The host throws them for everyone; a client takes another car's parts off without a throw and throws its own car's at once (handed over to the host's). |

## Files

| File | Class | Reason |
| --- | --- | --- |
| `src/app/App.cpp` | reclassified M | Frame time step, inactive-window freeze and the intro decision reproduce MM2 (above); the rest is start-up glue. |
| `src/app/App.h` | confirmed O | Declaration. |
| `src/app/CommandLine.cpp` | confirmed O | OpenMM2 options; none changes game rules (`--skip-intro` mirrors `-nomovie`). |
| `src/app/CommandLine.h` | confirmed O | |
| `src/app/Context.cpp` | reclassified M | `applyAudioSettings` reproduces the audio options' effect (above). |
| `src/app/Context.h` | confirmed O | Shared state. |
| `src/app/GameData.cpp` | confirmed O | Source checking and import. |
| `src/app/GameData.h` | confirmed O | |
| `src/app/Screens.h` | confirmed O | |
| `src/app/SetupScreen.cpp` | confirmed O | First-run setup. |
| `src/app/main.cpp` | confirmed O | Entry point, installer actions. |
| `src/audio/AudioDevice.cpp` | confirmed O | SDL output of the mixer. |
| `src/audio/AudioDevice.h` | confirmed O | |
| `src/core/File.cpp` | confirmed O | Host file access. |
| `src/core/File.h` | confirmed O | |
| `src/core/Ini.cpp` | confirmed O | OpenMM2 settings files (MM2 used the registry and binary player files). |
| `src/core/Ini.h` | confirmed O | |
| `src/core/Log.cpp` | confirmed O | |
| `src/core/Log.h` | confirmed O | |
| `src/core/Paths.cpp` | confirmed O | |
| `src/core/Paths.h` | confirmed O | |
| `src/core/StringUtil.cpp` | reclassified M | `parseDouble`, `parseInt` and the case folding stand in for MM2's atof, atoi and _stricmp in the data loaders (above). |
| `src/core/StringUtil.h` | reclassified M | |
| `src/net/AmbientState.cpp` | O | The shared cruise traffic's message (added 2026-10-09). |
| `src/net/AmbientState.h` | O | |
| `src/net/PropState.cpp` | O | The host's props on the wire (added 2026-10-09). |
| `src/net/PropState.h` | O | |
| `src/net/VehicleDamage.h` | O | A car's damage on the wire (added 2026-10-09); the rules it replays are MM2's (docs/multiplayer.md, "Damage"). |
| `src/net/BitStream.cpp` | confirmed O | Wire encoding. |
| `src/net/BitStream.h` | confirmed O | |
| `src/net/ClockSync.cpp` | confirmed O | Host clock estimate. |
| `src/net/ClockSync.h` | confirmed O | |
| `src/net/Discovery.cpp` | confirmed O | LAN discovery. |
| `src/net/Discovery.h` | confirmed O | |
| `src/net/NatPmp.cpp` | confirmed O | |
| `src/net/NatPmp.h` | confirmed O | |
| `src/net/NatPmpBackend.cpp` | confirmed O | |
| `src/net/Net.cpp` | confirmed O | |
| `src/net/Net.h` | confirmed O | |
| `src/net/PortMapper.cpp` | confirmed O | |
| `src/net/PortMapper.h` | confirmed O | |
| `src/net/Protocol.cpp` | confirmed O | |
| `src/net/Protocol.h` | confirmed O | Messages and settings carried for the game module (above). |
| `src/net/Session.cpp` | confirmed O | Lobby and relay; knows no game rules. |
| `src/net/Session.h` | confirmed O | |
| `src/net/Sha256.cpp` | confirmed O | |
| `src/net/Sha256.h` | confirmed O | |
| `src/net/Snapshot.cpp` | confirmed O | Remote-car interpolation for drawing. |
| `src/net/Snapshot.h` | confirmed O | |
| `src/net/Transport.cpp` | confirmed O | ENet wrapper. |
| `src/net/Transport.h` | confirmed O | |
| `src/net/UpnpBackend.cpp` | confirmed O | |
| `src/platform/Clock.cpp` | reclassified M | `FrameClock::tick` is MM2's frame time step (above); `FrameLimiter` is OpenMM2's. |
| `src/platform/Clock.h` | reclassified M | |
| `src/platform/Dialogs.cpp` | confirmed O | |
| `src/platform/Dialogs.h` | confirmed O | |
| `src/platform/ImGuiPlatform.cpp` | confirmed O | |
| `src/platform/ImGuiPlatform.h` | confirmed O | |
| `src/platform/Input.cpp` | reclassified M | Axis scaling reproduces `mmJaxis::Normalize`; the dead zone is open (above). |
| `src/platform/Input.h` | reclassified M | |
| `src/platform/Platform.cpp` | confirmed O | SDL set-up and events. |
| `src/platform/Platform.h` | confirmed O | |
| `src/platform/Window.cpp` | confirmed O | |
| `src/platform/Window.h` | confirmed O | |
| `src/render/Device.h` | confirmed O | Render interface. |
| `src/render/DisplaySettings.cpp` | confirmed O | OpenMM2 display options. |
| `src/render/DisplaySettings.h` | confirmed O | |
| `src/render/GpuConstants.h` | confirmed O | Constant-buffer layouts. |
| `src/render/HandleTable.h` | confirmed O | |
| `src/render/ImGuiRenderer.cpp` | confirmed O | |
| `src/render/ImGuiRenderer.h` | confirmed O | |
| `src/render/ImageUtil.cpp` | reclassified M | Mipmap generation is `gfxImage::GenerateMipmaps` (above); PNG writing is OpenMM2's. |
| `src/render/ImageUtil.h` | reclassified M | |
| `src/render/Overlay2D.cpp` | confirmed O | 2D batching; what is drawn where is the frontend's and HUD's. |
| `src/render/Overlay2D.h` | confirmed O | |
| `src/render/Projection.cpp` | reclassified M | The default projection reproduces `gfxViewport::Perspective` (above). |
| `src/render/Projection.h` | reclassified M | |
| `src/render/Renderer.cpp` | confirmed O | Backend selection. |
| `src/render/Renderer.h` | confirmed O | |
| `src/render/ShaderBlobs.h` | confirmed O | |
| `src/render/opengl/GlApi.cpp` | confirmed O | |
| `src/render/opengl/GlApi.h` | confirmed O | |
| `src/render/opengl/GlDevice.cpp` | confirmed O | Implements `Types.h`'s states (checked against the Vulkan backend). |
| `src/render/vulkan/VmaImpl.cpp` | confirmed O | |
| `src/render/vulkan/VulkanDevice.cpp` | confirmed O | Implements `Types.h`'s states. |
| `src/vfs/DirectoryFs.cpp` | confirmed O | Used by tools; no longer mounted for game data. |
| `src/vfs/DirectoryFs.h` | confirmed O | |
| `src/vfs/FileSystem.h` | confirmed O | |
| `src/vfs/GameSource.cpp` | reclassified M | Archive order and precedence reproduce `zipMultiAutoInit` and `zipFile::zipOpen` (above); source detection is OpenMM2's. |
| `src/vfs/GameSource.h` | reclassified M | |
| `src/vfs/IsoImage.cpp` | confirmed O | ISO 9660 reader. |
| `src/vfs/IsoImage.h` | confirmed O | |
| `src/video/Avi.cpp` | confirmed O | AVI demuxer (Windows' job in MM2). |
| `src/video/Avi.h` | confirmed O | |
| `src/video/Indeo5.cpp` | confirmed O | Codec (not in `midtown2.exe`). |
| `src/video/Indeo5.h` | confirmed O | |
| `src/video/IndeoTables.inc` | confirmed O | |
| `src/video/Movie.cpp` | confirmed O | Frame stepping; playback rules are `IntroScreen`'s. |
| `src/video/Movie.h` | confirmed O | |
| `tools/introplay/main.cpp` | confirmed O | Tool. |
| `tools/mm2tool/Command.h` | confirmed O | Tool. |
| `tools/mm2tool/Common.cpp` | confirmed O | Tool. |
| `tools/mm2tool/Common.h` | confirmed O | Tool. |
| `tools/mm2tool/cmd_ai.cpp` | confirmed O | Tool; runs the game's AI modules offline. |
| `tools/mm2tool/cmd_asset.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_audio.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_city.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_data.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_fx.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_gameaudio.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_music.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_ped.cpp` | confirmed O | Tool. |
| `tools/mm2tool/cmd_phys.cpp` | confirmed O | Tool; its `.mtx` and bound-box readers are stopgaps of its own, not used by the game, so its results may differ from the game's car set-up. |
| `tools/mm2tool/cmd_vfs.cpp` | confirmed O | Tool. |
| `tools/mm2tool/main.cpp` | confirmed O | Tool. |
| `tools/netprobe/main.cpp` | confirmed O | Tool. |
| `tools/rendertest/main.cpp` | confirmed O | Tool. |

## Missing

| MM2 | What it does | Status |
| --- | --- | --- |
| `mmJoystick::SetDeadZone` applied through `mmInput::Init` / `mmInput::SetDeadZone` | Joystick X/Y dead zone from the Options slider (default 0.1), with DirectInput rescaling the rest of the range | open: the device layer reads raw axes; the session area's input mapping (`RaceScreen`) should apply the stored dead zone the way DirectInput does instead of its fixed 0.15 threshold. |
| `mmPlayer::UpdateFF`, `mmPlayer::FFImpactCallback`, `mmPlayer::ResetFF`, `mmCarRoadFF`, `mmJoystick` effects (`SetShake`, `PlaySteer`, `PlayCollision`, `SetFriction`), `mmInput::SetForceFeedbackScale` | Force feedback: road feel, steering centring, impact jolts | open: `Input::rumbleGamepad` and `rumbleJoystick` exist but nothing drives them; porting the mmPlayer force-feedback updates belongs to the vehicle/session areas. |
| `ebolaPlayMovie` skip and size rules | The logo movie stops on Escape, Space or the left mouse button (checked every 250 ms), plays at its own size centred on the desktop and pauses while the game is inactive | open for the rendering-fx area: `IntroScreen` skips on any key, mouse button or pad button and scales the movie to the 640x480 space. The inactive pause now happens through the main loop. |
| `mmJaxis::Capture`, `mmJaxis::ResetCapture` | Axis capture for control binding: an axis is assigned once it moves 0.125 from where it rested | frontend-ui area (binding pages). |
| `datReplay` (`datTimeManager::Update` records or replays each frame's time) | Debug replay of frame timing | not needed: development feature. |
| `datTimeManager::FixedFrame`, `datTimeManager::SetTempOverSampling` | Fixed-frame mode and physics oversampling | phys-core area (`World::advanceOversampled`, `World::step`). |
