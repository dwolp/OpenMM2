# Car cameras

`src/game/Cam*.{h,cpp}` reimplements Midtown Madness 2's car cameras. The
behaviour is ported from MM2's own code: MM2Recomp's disassembly of the
unprotected build 3393, used as documentation only (see CLAUDE.md). The
first version of this port followed Midtown Madness 1 build 1560 (Open1560,
`code/midtown/game.asm`); MM2 kept the class structure but renamed the
classes (`TrackCamCS` became `camTrackCS`) and changed much of the
behaviour, so the code now follows MM2 throughout. Evidence below names the
MM2 function.

## Classes

| MM2 | OpenMM2 | File |
|---|---|---|
| `camBaseCS`, `camAppCS`, `camCarCS` | `CarCamera` | `CamCar.*` |
| `camTrackCS` (chase) | `TrackCamera` | `CamTrack.*` |
| `camPovCS` (hood / dashboard) | `PovCamera` | `CamPov.*` |
| `camPreCS` (pre-race) | `PreCamera` | `CamRace.*` |
| `camPointCS` (post-race, water) | `PointCamera` | `CamRace.*` |
| `camPolarCS` (multiplayer finish line, XCams) | `PolarCamera` | `CamRace.*` |
| `camViewCS` + `camTransitionCS` | `CameraView` | `CamView.*` |
| `mmPlayer`, `mmViewMgr::SetViewSetting`, `mmGame::UpdateGameInput` (camera parts) | `PlayerCameras` | `CamPlayer.*` |
| `Matrix34` / `Vector3` helpers | `cam::` functions | `CamMath.*` |
| tune file fields (`FileIO`) and constructor defaults | `TrackCamParams`, `PovCamParams` | `CamParams.*` |

Every camera keeps a goal matrix (`matrix_`) and the matrix actually
rendered (`camera_`). Each update the camera computes the goal, then
`camAppCS::ApproachIt` moves `camera_` towards it. Position moves per axis
with `DApproach` at `AppXZPos` / `AppYPos`. Orientation moves per Euler
angle ("zxy" order) at `AppRot` / `AppXRot`, eased in within `AppPosMin` /
`AppRotMin`, and optionally low-pass filtered (`AppAppOn`, `AppApp`).
`UpdateMaxDist` keeps it between `MinDist` and `MaxDist` of `TrackTo`.

## Using it

```cpp
game::PlayerCameras cams;
cams.load(vfs, "vpbug");                 // tune/camera/vpbug_near.camtrackcs, ...
cams.setVehicleFlags(info.flags);        // tune/vpbug.info Flags
game::CameraTarget target;               // car matrix, speed, inputs, wheels
cams.reset(target);                      // after placing the car
cams.startPreRace();                     // race modes: before the first update
// every update (refresh target first):
game::CameraInput input;
input.camPan = game::cameraPanFor(lookLeft, lookRight, lookBack, lookForward);
input.aspect = width / height;           // 3D viewport
cams.update(dt, target, probe, input);   // probe: nearest segment hit (world collision)
cams.apply(camera);                      // game::Camera for the renderer
cams.toggleCamera();                     // "Change Camera"
cams.toggleDashboard();                  // "Dashboard"
cams.startPostRace();                    // the race is over
cams.startWaterCam();                    // the car went into the water
cams.display();                          // draw body / hide it / draw the dash model
```

* **Car matrix.** `CameraTarget::matrix` is the car's frame (`camCarCS::Init`
  points the cameras at the vehicle simulation's matrix). `TrackTo` and
  `Offset` are in that frame; it faces -m2. RaceScreen passes the rigid
  body's matrix. MM2 points the cameras at `vehCarSim`'s world matrix (set by
  `vehCarSim::SetWorldMatrix`), and `mmPlayer::SetCamInterest(0)` points them
  at the inertial matrix; whether those differ by the centre of gravity
  offset depends on the physics port.
* **Inputs.** `angularMomentum` is the body's world angular momentum
  (`camTrackCS::UpdateCar` reads `vehCarSim +0x60`, the inertial body's
  angular momentum): above 1500 kg m^2/s while the car has been off the
  ground for 0.1 s, the chase cameras stop following its heading and keep
  their offset (`UpdateTrack`), so a car tumbling through the air does not
  drag the view round. `speed` is `vehCarSim`'s speed, |m2 . velocity| (it picks
  `AppXZPos`); `steering`, `throttle` and `handBrake` are the car's inputs;
  `reverseGear` is the transmission in reverse (gear 0); `wheels` give each
  wheel's contact flag and the ground normal of its last probe hit
  (`vehWheel`), which the hill pitch follows. `roomFlags` are the
  `lvlRoomInfo` flags of the car's room (see below; 0 by default).
* **Timing.** `dt` is the update delta (`datTimeManager::Seconds`). MM2
  updated the cameras once per rendered frame; the cameras work with any
  step.
* **Probe.** `CameraProbe(from, to, hit)` returns the nearest hit on the
  segment, with the hit's fraction along it. It is used for the floor and
  ceiling clamp (`MinMax`) and for keeping the camera out of walls
  (`Collide`), and for the big vehicles' overhead test. MM2's cameras use
  `dgPhysManager::Collide` with mask 0x20, ignoring the player's car (the
  city's collision polygons and the rooms' instances flagged 0x20):
  RaceScreen passes `phys::World::wheelProbe`, the same query the wheels
  use, with the player's body as the one never hit. Without a probe the
  cameras still work, without collision.
* **FOV.** `CameraFOV` is the *vertical* field of view in degrees:
  `gfxViewport::Perspective(fov, aspect, near, far)` takes the tangent of
  half of it for the height and multiplies by the window's aspect ratio for
  the width. A 70 degree camera therefore spans about 86 degrees across a
  4:3 screen. `Camera::horizontalFov` is the horizontal FOV on a 4:3 screen,
  so `apply()` converts (`cam::horizontalFov4x3`); with
  `render::computeProjection`'s Hor+ mode the vertical FOV is kept on any
  screen, as in the original.

## Camera files and views

`mmPlayer::Init` loads per car:

| View | Class | File |
|---|---|---|
| near | camTrackCS | `tune/camera/<car>_near.camtrackcs` |
| far | camTrackCS | `<car>_far.camtrackcs` |
| ind | camTrackCS | `<car>_ind.camtrackcs` (only vpbug and vpbus have it) |
| pov | camPovCS | `<car>.campovcs` (hood) |
| dash | camPovCS | `<car>_dash.campovcs` |
| pre | camPreCS | none: `camPreCS::Init` does not load, constructor defaults |
| point | camPointCS | none |
| polar | camPolarCS | none (`mmPlayer::Init` gives it no name) |
| xcam 0, 1 | camPolarCS | none; the second follows the car's heading (AzimuthLock), 38.1 m away, 0.321 rad up |

A missing file leaves the camera at the constructor defaults, and its
`AfterLoad` does not run (`asNode::Load` only calls it after a successful
load). vpvwcup's far camera ships as `vpvwcup__far.camtrackcs` (typo), so
the original used the defaults for it: no wall collision (`CollideType 0`),
near plane 1.0. `<car>_pov.campovcs` (cab, coop, coop2k) is not loaded by
MM2. On a screen narrower than 1.3:1 `mmPlayer::Init` scales the dashboard
camera's `Offset.z` by 0.7352941.

## Switching views

* **Change Camera** (`mmViewMgr::SetViewSetting(0)`, `mmPlayer::SetCamera`):
  cycles `CarCams` near -> pov -> far with `camViewCS::NewCam(cam, 3, 0.8)`,
  a 0.8 s ease-in-out blend. From the dashboard it returns to the cycled
  camera without advancing. It also turns the dashboard off.
* **Dashboard** (`SetViewSetting(6)`): the dashboard camera cuts in at once
  (`camViewCS::SetCam`); switching it off blends to the cycled camera (mode
  3, 0.8 s). Not available in wide-angle mode, before or after the race.
  After a reset with the dashboard on, `mmPlayer::Reset` puts the view on
  the dashboard camera but leaves the camera group at the cycled cameras
  with the point-of-view index: switching the dashboard off then does not
  move the view until the camera is changed. PlayerCameras keeps this.
* **Look around** (`mmGame::UpdateGameInput`): only while the view's camera
  is the hood or dashboard camera, `camPovCS+0x144` is set to CamPan x 2 pi;
  the view turns about its up axis. The chase cameras do not look around.
  `mmInput::GetCamPan`: left 0.25, back 0.5, right 0.75, forward 0,
  back+left 0.375, back+right 0.625, forward+left 0.125, forward+right
  0.875; back wins over forward and left over right; an analog "Camera Pan"
  axis overrides the buttons.
* **Pre-race** (`mmPlayer::SetPreRaceCam`, from the modes' `Reset`: every
  single-player mode but cruise): only between a reset and the first update.
  The view cuts to `camPreCS` (22 m away, 1.1 rad above the horizon, 2 m
  higher, behind the car as `MakeActive` found it) and blends to the
  selected camera over 3.5 s; the point-of-view camera is reached through the
  near camera, then near -> pov (mode 3, 0.5 s), and the dashboard through
  near -> pov (mode 1, 0.3 s) -> dash (mode 2, 0.3 s) (`mmPlayer::Update`).
  Camera changes are ignored meanwhile.
* **Post-race** (`mmPlayer::SetPostRaceCam`, when the race is over): the far
  camera is updated once and a `camPointCS` is placed 3.5 m above it, with
  MaxDist 25; the view blends to it (mode 3, 0.8 s). The point camera looks
  at the car and narrows its FOV from 60 to 25 degrees as the car gets from
  7.5 m (0.3 x MaxDist) to 25 m away. Camera changes are ignored until the
  next reset.
* **Multiplayer finish** (`mmPlayer::SetMPPostCam`, from
  `mmGameMulti::SetFinishCam` when a multiplayer checkpoint race or circuit
  is over; multiplayer blitz uses the post-race camera above): the far camera
  is updated once and the view blends (mode 3, 0.8 s) to a `camPolarCS`
  orbiting the finish line (the last waypoint, the first in a circuit),
  2.5 m up, at azimuth (heading + 180) x -pi / 180 from the waypoint's
  heading in degrees, 21.5 m away and 0.34 rad above the horizon, or 15.5 m
  away and level while the car is in a room with flag 0x02 or 0x08
  (`PlayerCameras::startMultiplayerPostRace`). `camPolarCS::Update` reads
  the keyboard every update (`CameraInput::orbit`): Delete / Page Down turn
  it, End / Home tilt it, Page Up / Insert zoom it, at 0.3 x PolarDelta
  (2) rad/s and 2 x PolarDelta m/s, or with either Shift key PolarDelta
  rad/s and 5 x PolarDelta m/s; the distance stays within 0.5 .. 200 m and
  the incline within +-pi. Camera changes are ignored until the next reset.
* **XCam** (`mmViewMgr::SetViewSetting(2)`, input events 0x0C and 0x2F,
  `PlayerCameras::toggleXCam`): blends (mode 3, 0.8 s) to the first XCam, a
  `camPolarCS` with its defaults orbiting the car (2.5 m up, 10 m away,
  azimuth 2.5, 0.25 rad up), steered by the keyboard like the finish-line
  camera. It turns the dashboard off and remembers whether it was on (the
  dash view's activated flag); the next press goes back to the dashboard
  or to the cycled camera. "Change Camera" also leaves it; the dashboard
  key does nothing meanwhile. `mmPlayer::GetNextCycleXCamIndex` would cycle
  the two XCams with the XcamCheat flag, which midtown2.exe never sets, so
  the second XCam (heading-locked, 38.1 m away, 0.321 rad up) is
  unreachable (`setXCamCheat` reaches it).
* **Water** (`mmPlayer::Update`, while the car's splash is active): the
  point camera is placed 9 m above the view and blended to (mode 3, 0.8 s),
  once per reset.
* **Big vehicles** (`mmPlayer::Update`): with `tune/<car>.info` Flags & 0x13
  (vpbus, vpddbus, vpcentury, vpsemi), in a room with flags & 0x0A the near
  or far camera blends to the _ind camera (mode 3, 1 s), and back to the
  selected camera when out of such rooms. It also switches in rooms with
  flag 0x20 when a segment from 100 m above the last rendered camera
  position down to it hits something (`dgPhysManager::Collide`; the
  position is `gfxRenderState`'s camera, set by `camViewCS::Update`).
  vpsemi, vpcentury and vpddbus have no _ind file and use the defaults.
* **Collision margin** (`mmPlayer::Update`): the near and far cameras keep
  their near plane 0.33 m out of walls, 1.11 m in rooms with flag 0x08.
* **Wide angle** (`SetViewSetting(5)`, `mmPlayer::SetWideFOV`): the view is
  letterboxed to 66% of the height (from 18% down) and the perspective set
  to 70 degrees; transitions then leave the perspective alone.
  `CameraView::wideAngle()` reports it; RaceScreen draws the level in that
  band on black (docs/rendering.md, "Rear-view mirror" and "Wide angle").
  `camViewCS::Reset` sets the camera's own FOV whatever the mode, and
  `mmPlayer` resets the view on the first update after every reset, so
  after a reset MM2 shows the letterboxed view at the camera's FOV until a
  view setting changes. PlayerCameras keeps this.
* **Map modes** (`mmViewMgr::SetViewSetting(1)` and `(10)`): the HUD map's
  mode is a view setting kept with the camera (`PlayerCameras::mapMode`).
  The split map forces the wide view (70 degrees, the 3D view in the top
  half) and turns the dashboard off, remembering it in the dash view's
  activated flag; leaving it restores the player's wide-angle choice (the
  view settings' wide byte) and the dashboard when its camera still shows.
  The wide angle cannot change with the split or full-screen map, nor the
  dashboard with the split map. Every view key ends with SetCamera,
  SetWideFOV, SetDash and SetMapMode, so even the mirror key writes the
  dashboard flag over the remembered one. `mmPlayer::Reset` applies the
  player's wide-angle choice, not the split map's.
* **View settings** (`mmPlayerConfig::GetViewSettings` /
  `SetViewSettings`): the selected camera, the wide angle and the
  dashboard are globals that `mmPlayer::Init` and `Reset` read. They are
  saved in the driver's config when the game ends and restored when the
  next race is set up, so the view carries over from race to race
  (`PlayerCameras::viewSettings` / `setViewSettings`, before `reset`).

The meaning of the room flags is **inferred**: mm2hook calls 0x02
"Subterranean" (MM2 also turns on the tunnel echo with it) and 0x08 "Road",
but the camera uses 0x08 like "covered". RaceScreen does not pass room flags
yet, so these two switches stay off.

### Transitions

`camTransitionCS::Update` updates both cameras, counts the blend time down
and computes the blend position t = 1 - BlendGoal x (time left / blend
time) while t < BlendGoal, blending CameraFOV and CameraNear linearly and
setting the view's perspective. Then the curve of the mode is applied to t
(1: 1 - cos(t pi/2), 2: sin(t pi/2), 3: (1 - cos(t pi)) / 2), and the view
is interpolated in polar form about the point each camera looks at, at the
car's distance (`Matrix34::PolarView`). When the time is up (and BlendGoal
is 1) the view switches to the target camera, or starts a queued one with
mode 2. Asking for the camera being blended from or to reverses the blend.
`StartTransition` resets and updates the target camera; with mode 0 or no
time it goes back to the camera being left (no MM2 code does that).

The view's perspective is a state, as `gfxViewport::Perspective` was: it is
set by `mmPlayer::SetWideFOV` whenever the view setting changes (to the
selected camera's FOV), on reset, by every blending update and by the point
camera, and otherwise keeps its value. A blend therefore ends a little short
of the target camera's FOV (by up to one update's share of the blend), as
in the original.

## Free camera (OpenMM2 extra)

Not in MM2; off by default (`[Tweaks] FreeLook`, Graphics options → TWEAKS →
CAMERA, a control the Look leaves alone). After `PlayerCameras::apply`,
`RaceScreen::applyFreeLook` swings the chase cameras (display Body, not the
pre- or post-race cameras) round the car: the right stick turns it, 2.8 rad/s
round and 1.6 up and down at full tilt past a 15% dead zone (right looks
right, up looks up), and the mouse with its right button held, 0.005 and
0.004 rad per pixel, unless the mouse steers. The eye orbits a point 1.2 m
above the car, its elevation raised by up to 0.5 rad (at most 0.75 rad above the
point) or lowered by 0.25, and what the camera looked at, a camera's distance
ahead, turns with it (`Camera::lookAt`); a wall between the point and the eye
(the cameras' probe) pulls the eye in 0.4 m short of it. A second after the
last input the swing eases back (by exp(-3 t)). The point-of-view cameras keep
MM2's look left, right and back. `OPENMM2_DEBUG_FREELOOK=<yaw>,<rise>` holds a
fixed swing. Values chosen by eye.

## Rear-view mirror

`CamMirror.*` (`RearViewMirror`) ports `mmMirror`'s camera; RaceScreen's
`drawMirror` draws it (docs/rendering.md, "Rear-view mirror").

* **Data** (`mmMirror::FileIO`): `tune/<car>.mmmirror`, which only 11 cars
  ship (vp4x4, vpauditt, vpbus, vpcaddie, vpcentury, vpcop, vpdb7,
  vpddbus, vpford, vppanoz, vpsemi); the others keep the defaults. Fields:
  Position (the eye in the car's frame; `mmMirror::Init` sets 0, 1.4, -1:
  1.4 m up and 1 m ahead), Size (0.3, 0.16 of the screen), Fov (10, the
  vertical FOV in degrees), Aspect (2), NearClip (1.2; 1.2 to 5.8 in the
  files), FarClip (100).
* **Frame** (`mmMirror::Init`, `Cull`): the identity turned pi about Y
  with m0 then negated, so it looks backwards and the picture is mirrored
  left to right (determinant -1), at Position; the view is that frame times
  the car's world matrix (`Matrix34::Dot`, `worldMatrix`).
* **Inset** (`mmMirror::Reset`): `gfxViewport::SetWindow(width - w - 1, 1,
  w, h)` with w = (int)(width x Size.x) and h = (int)(height x Size.y): the
  top right corner, one pixel in, and `Perspective(Fov, Aspect, NearClip,
  FarClip)`: the aspect is fixed at 2 whatever the inset's shape.
* **On / off**: `mmViewMgr::Init` creates it and leaves it on only when the
  driver's view settings say so (`mmPlayerConfig`, the byte after the
  camera globals); `SetViewSetting(9)` (input event 0x1E) toggles it.
  `mmGameManager::Update` declares it for drawing every frame while it is on,
  whatever the camera, after the dashboard and the HUD map.

What the renderer must do (`mmMirror::Cull`): set the inset's viewport,
clear its colour (black) and depth, render from `worldMatrix(car)` with the
fixed-aspect projection, swap the cull winding (the frame is mirrored),
hide the player's car body, and draw the level as for the main view
(`lvlLevel` draw for that viewport), then restore the viewport and the cull
mode. The inset follows the screen size, so it works at any resolution.

## Port status

| Function | Status |
|---|---|
| `camAppCS::ApproachIt`, `UpdateApproach`, `UpdateMaxDist`, `DApproach` | ported, same operation order |
| `camTrackCS::Update`, `UpdateCar`, `UpdateHill`, `UpdateTrack`, `PreApproach`, `MinMax`, `Collide` (types 1 and 2), `Reset`, `AfterLoad`, constructor | ported |
| `camTrackCS::UpdateInput`, `UpdateSwing`, `MakeActive` | empty in MM2 |
| `camTrackCS::Front`, `Rear`, `SwingToRear` and the swing spline | not ported: the spline is updated every frame but nothing calls these |
| `camPovCS::UpdatePOV`, `Update`, `Reset`, `AfterLoad`, constructor | ported |
| `camPreCS`, `camPointCS` | ported |
| `camViewCS::SetCam`, `NewCam`, `Update`, `Reset` | ported (the player's view: `camViewCS+0x48` set, so SetCam leaves the perspective) |
| `camTransitionCS::Update`, `NewTransition`, `NextTransition`, `StartTransition`, `StartNextTransition`, `ReverseTransition` | ported |
| `mmPlayer::Init`, `Reset`, `Update`, `SetCamera`, `GetCamera`, `GetCurrentCameraPtr`, `IsPOV`, `SetWideFOV`, `SetPreRaceCam`, `SetPostRaceCam`, `SetMPPostCam` (camera parts) | ported |
| `mmPlayerConfig::GetViewSettings`, `SetViewSettings` (camera part) | `PlayerCameras::viewSettings` / `setViewSettings` |
| `mmViewMgr::SetViewSetting` 0, 5, 6, `mmGame::UpdateGameInput` (CamPan), `mmInput::GetCamPan` | ported |
| `Matrix34::LookAt`, `GetEulers("zxy")`, `FromEulersZXY`, `MakeRotate*`, `Dot`, `Dot3x3`, `Rotate`, `RotateFull`, `PolarView`, `Vector3::Approach`, `Angle`, `InvMag` | ported with the original association of every sum |
| `camPolarCS`, `mmViewMgr::SetViewSetting(2)`, `mmPlayer::GetNextCycleXCamIndex`, `GetCurrentXCamIndex`, `SetCamera` group 1 | ported (`PolarCamera`, `PlayerCameras::toggleXCam`): the multiplayer finish line and the XCams |
| `camAICS` (keyboard-driven free camera), `camPostCS` (only its `MakeActive` is called; it is never shown) | not ported |
| `mmMirror::Init`, `Reset`, `FileIO`, the camera part of `Cull` | ported (`RearViewMirror`); drawing it is open (rendering) |
| `mmExternalView` (HUD gauges over the chase views) | HUD, not part of the cameras |
| HUD hidden while looking around in the point-of-view cameras (`mmGame::UpdateGameInput`) | not ported (HUD) |

The math is 32-bit float, like the original's single-precision x87. It can
differ from the original in the last bit where the x87 kept a sine or cosine
at extended precision. One place knowingly differs: `getEulersZXY` clamps
the asin argument to [-1, 1], where the original could produce a NaN from a
rounding excess.

## Tune fields

| Field | Class | MM2 use | Evidence |
|---|---|---|---|
| BlendTime | Base | not read | `camBaseCS::FileIO` |
| BlendGoal | Base | fraction of a transition that is blended | `camTransitionCS::StartTransition` |
| CameraFOV | Base | vertical FOV in degrees | `gfxViewport::Perspective` |
| CameraNear | Base | near plane; forced to 0.5 (chase) and 0.1 (point of view) after loading | `camTrackCS::AfterLoad`, `camPovCS::AfterLoad` |
| CameraFar | Base | one global for all cameras (the last file loaded wins); the game's far clip overrides it | `camBaseCS::FileIO`, `mmGame::FarClipCB` |
| ApproachOn, AppAppOn, AppRot, AppXRot, AppYPos, AppXZPos, AppApp, AppRotMin, AppPosMin, LookAbove, TrackTo, MaxDist, MinDist, LookAt | App | approach | `camAppCS::UpdateApproach` |
| Offset, TrackBreak, MinMaxOn, VertOffset | Track | tracking | `camTrackCS::UpdateTrack`, `UpdateCar` |
| CollideType | Track | 1: rays from the corners of the near plane (widened by 0.33) along the view line, MaxDist long; the camera comes in to the nearest wall facing it + CameraNear - margin. 2: the MM1-style pull-in. Every MM2 file has 1 | `camTrackCS::Collide` |
| MinAppXZPos, MaxAppXZPos, MinSpeed, MaxSpeed, AppInc, AppDec | Track | AppXZPos from MaxAppXZPos at MinSpeed to MinAppXZPos at MaxSpeed, changing by AppInc / AppDec per second; 20 when reversing with ReverseOn 0 | `camTrackCS::PreApproach` |
| HillMin, HillMax, HillLerp | Track | the wheels' ground normal, low-pass filtered by HillLerp per update, is tilted from vertical by some angle along the car's heading; up to 45 degrees that maps through a quarter cosine to a pitch from 0 to -HillMax (uphill) or -HillMin (downhill), applied about the camera's horizontal axis around the target, flipping sign as the reverse view swings to the front | `camTrackCS::UpdateHill`, `UpdateTrack` |
| ReverseOn | Track | 1: after 2 s in reverse gear with throttle >= 0.05 the camera swings round to the front, to +pi (steering <= 0.1) or -pi; out of reverse it carries on round back behind. -1: always in front. The hand brake in reverse zeroes AppXZPos | `camTrackCS::UpdateCar`, `UpdateTrack` |
| RevDelay | Track | not read (the delay is a constant 2 s) | `camTrackCS::UpdateCar` |
| RevOnApp, RevOffApp | Track | swing rates to the front and back, rad/s | `camTrackCS::UpdateTrack` |
| MinHardSteer, DriftDelay, FrontRate, RearRate, FlipDelay, SteerOn, SteerMin, SteerAmt | Track | not read | `camTrackCS::FileIO` |
| Offset | Pov | eye position in the car's frame | `camPovCS::UpdatePOV` |
| ReverseOffset | Pov | eye position in the reverse mode (`camPovCS+0x10C == -1`), which no MM2 code sets: no effect in the game | `camPovCS::UpdatePOV` |
| Pitch | Pov | rotation about the view's right axis | `camPovCS::UpdatePOV` |
| POVJitterAmp | Pov | not read; MM2's point-of-view cameras have no shake | `camPovCS::FileIO` |

Constructor defaults (used when a file is missing or leaves a field out)
are those of `camBaseCS`, `camAppCS`, `camTrackCS` and `camPovCS`; e.g. a
chase camera has CollideType 0, ReverseOn 1, HillMin -0.56, HillMax 0.56,
HillLerp 0.05, and a point-of-view camera AppXRot 0.5.

## Verification

`tests/game/test_camera.cpp` (`test_game`):

* **Math helpers:** Euler round trip and agreement with `Mat34` rotations;
  `LookAt`; `MakeRotate`'s axis-aligned and general paths through `Rotate` /
  `RotateFull`; `PolarView`'s angles; `Vector3::Approach` moving one axis at
  a time; `Angle`; the vertical-to-horizontal FOV conversion.
* **Parameters:** constructor defaults and `AfterLoad`.
* **`DApproach`** steps, ramp and clamping.
* **Chase camera** with the vpbug near tuning on synthetic car paths:
  limits, no jumps, settling behind the car, `PreApproach` over the speed
  range and its AppDec limit; the reverse view's 2 s delay, throttle
  condition, swing rates, side and the way back; the hand brake in reverse;
  the hill pitch against the closed-form value for a 10 degree slope up and
  down, the HillLerp filter, and the camera dropping uphill; CamPan having
  no effect; CollideType 1 against a wall (the expected distance computed
  from the near-plane corners) with both margins; CollideType 2;
  determinism.
* **Point of view:** eye position, look back and left, the reverse mode,
  pitch, no shake.
* **View and player:** a near -> far blend without jumps with the FOV
  inside its range and the perspective kept after it; asking for the current
  camera; reversing a blend; the near -> pov -> far cycle and the body
  showing while blending; CamPan on the point-of-view camera; the dashboard
  cut and blend, and its behaviour after a reset; the pre-race sequences to
  the point-of-view camera and to the dashboard; the post-race point camera
  and its zoom; the water camera; the _ind camera for big vehicles and the
  collision margin.
* **Retail data** (with `OPENMM2_GAME_DATA`): all 96 camera files parse,
  every chase camera has CollideType 1 and near plane 0.5; vpbug's values;
  the narrow-screen dashboard; the vpvwcup typo and its defaults; all 20
  player cars' near, far, pov and dash cameras run a scripted path (with a
  slope and reversing) without NaNs and keep the car framed.
* **`Camera.PlotPaths`** (with `OPENMM2_CAMERA_PLOT=<dir>`) writes car and
  camera paths as CSV for plotting.

`tests/game/test_parity_camera_props.cpp` (`test_game`, the parity audit,
see `docs/parity/camera-props.md`): the chase camera keeping its offset over
a car spinning in the air by angular momentum (and not by angular
velocity); the _ind camera under geometry in flag 0x20 rooms; the polar
camera's defaults, keys and limits; the multiplayer finish camera in open
and covered rooms; the view settings carried into the next race; the
rear-view mirror's frame, inset and retail files.
