# Midtown Madness 1 car physics (OpenMM2 Enhanced)

Goal (2026-11-15, the maintainer's choice): Chicago drives with MM1's own car
physics and MM1's own cars (the player's, the racers', the police's), so it
plays like the original. MM2's collision core (`phys::World`, `InertialCS`,
bounds) is the same Angel lineage and stays; the car model on top of it is
replaced. Source of truth: Open1560's `game.asm` (symbol-named MASM), ported
to readable C++ in the same operation order with 32-bit float maths. Anything
inferred is marked in the code and here.

## Why a port, not a retune

* MM1 tuning (`tune/<car>.mmcarsim`) is physical: spring and damper rates
  (`Spring 66300`, `Damping 6200`), gear ratios, rubber springs. MM2's
  (`tune/vehicle/<car>.vehcarsim`) is abstracted (`SuspensionExtent`,
  `SuspensionFactor`, `TireDispLimitLong`), so numbers do not carry over.
* MM1 adds a drift and spin assist (`DriftThreshold`, `SpinThreshold`,
  `Front/BackDriftFricMultiplier`, `SpinFromMin`...) that mmCarSim::Update
  runs every sample and that sets the wheels' friction multipliers. It is a
  large part of how MM1 feels.
* MM1's racers drive the same car with `Realism` 0 (1 while touching the
  player): mmWheel::Update blends two force terms by it. See
  `docs/formats/mm1city.md` ("MM1's racer AI").

## Architecture

Keep `phys::CarSim` and its public parts (`engine`, `trans`, `wheels`,
`drivetrains`, `gyro`...): audio, skid marks, AI, the net code and the HUD read
them. An MM1 car gets `CarSimParams::mm1` set and its components run MM1's
formulas instead of MM2's:

| MM1 class (asm lines in Open1560 `game.asm`) | Our part |
|---|---|
| `mmCarSim::Update` (137079-137700), `Init` (135716), `ReInit` (136185), `Reset` (136960), `UpdateDamage` (137865) | `CarSim::beforeIntegrate/afterIntegrate`, `init`, `reset` |
| `mmWheel::Update` (157567, 1245 lines), `ComputeDwtdw` (723 lines), `ComputeConstants`, `Init` | `Wheel` |
| `mmEngine::Update` (168), `CalcTorqueAtFullThrottle`, `CalcTorqueAtZeroThrottle` | `Engine` |
| `mmTransmission::Update` (83) | `Transmission` |
| `mmDrivetrain::Update` (489) | `Drivetrain` |
| `VehGyro::Update` (153), `mmForce::Update` (167), `asAero::Update` (426), `mmStuck::Update` (226) | `Gyro`, `Aero`, `Stuck` |

About 5,000 asm lines in all. Collision (`asBound::Impact`, `asInertialCS`) is
shared with MM2 and not ported.

## Findings so far

* `mmCarSim` layout (Open1560 `mmcar/carsim.h`): ICS, bounds, `mmWheel`
  FrontLeft/FrontRight/BackLeft/BackRight, `mmEngine`, `mmTransmission`,
  `mmDrivetrain` x3, `VehGyro`, `mmForce`; `Realism` is a pointer (+0x17F0):
  the player's points at `MMSTATE.PhysicsRealism` (0.75 default, 0.25 on the
  easy setting), an AI car's at a private variable.
* mmCarSim layout (offsets from Open1560's headers and the asm; asNode is
  0x20 bytes, asOverSample 0x30, so the ICS starts at 0x50 and its Matrix.m3
  is at 0x94): ICS.Mass 0xE4, ICS.FrameVelocity 0x134 (Speed = |it|, 0x1830),
  ICS.Gravity 0x1A4 (y 0x1A8), BodyCG 0x308, Dimensions 0x320 (x width, z
  length), RedistHeight 0x860, RedistLongRatio 0x864, Realism* 0x17F0, Brakes
  0x1804, HandBrake 0x1808, Steering 0x180C, Speed 0x1830, wheels FL 0x948, FR
  0xBB0, BL 0xE18, BR 0x1080 (0x268 apart), Engine/Trans from 0x16B0/0x16FC.
  Assist state: SpinState 0x1F34, SpinStartTimer 0x1F38, SpinSight 0x1F3C,
  SpinEndTimer 0x1F40, previous steering 0x1F30.
* mmCarSim::Update, in order (137079-137700 of game.asm; constants: 0.8, 0.5,
  0.05, 1.0):
  1. Brake friction multipliers: front = back = 1; Brakes > 0.8 sets front =
     BrakeFrontFricMultiplier, back = BrakeBackFricMultiplier; HandBrake > 0.8
     multiplies back by BrakeBackFricMultiplier again.
  2. The spin/drift assist. a = |Steering|. When a is in [SpinFromMin,
     SpinFromMax] and (the steering sign differs from SpinSight, or the start
     timer is 0): start timer = dt, SpinSight = -sign. Otherwise when a is in
     [SpinToMin, SpinToMax], the sign equals SpinSight and 0 < start timer <
     SpinStartTime: SpinState = 2 (spinning), end timer = dt. A start timer
     above SpinStartTime is zeroed; an end timer >= SpinEndTime ends the spin
     (state 0, timer 0); running timers add dt. While spinning, front/back
     multipliers *= FrontSpinFric / BackSpinFricMultiplier; else when a >=
     DriftThreshold, *= FrontDrift / BackDriftFricMultiplier.
  3. Changed multipliers go to the wheels (SetFricMultiplier on FL, FR with the
     front value, BL, BR with the back), the rear wheels' steer multiplier is
     set to 1, and Speed / SpeedMPH are recomputed.
  4. Automatic gearbox, no throttle, Speed < 0.5, Brakes <= 0.5: Brakes = 0.5
     (a held car); manual gearbox in gear 0: Brakes = max(Brakes, 0.05).
  5. Wheel inputs: FL and FR SetInputs(Steering, BrakeRatio x Brakes), BL and
     BR SetInputs(-Steering, BrakeRatio x Brakes); with HandBrake > 0 the rear
     wheels are set again with the handbrake as their brake. FR copies FL's
     tuning and BR copies BL's (CopyVars) every sample.
  6. Weight redistribution. W = -Gravity.y x Mass, F = sum of the four wheels'
     longitudinal term (+0x204), G = the sum of their lateral term (+0x200);
     U = clamp(RedistHeight / Dimensions.z x F / W + RedistLongRatio, 0, 1) is
     the rear share and R = clamp(RedistHeight / Dimensions.x x G / W + 0.5,
     0, 1) the left share; each wheel's load (+0x1E8): FL = R(1-U)W, FR =
     (1-R)(1-U)W, BL = RUW, BR = (1-R)UW.
  7. OnGround flag, UpdateDamage, impact parameters (a wrecked car: elasticity
     0, friction 2, brakes 1), then the children's Update (engine, gearbox,
     drivetrains, wheels), HasCollided cleared, audio and force feedback.
* mmWheel layout (derives from asLinearCS, 0x88 bytes; 0x268 in all; offsets
  checked against uses in mmCarSim::Update and mmWheel::Update): CarSim 0x88,
  ICS 0x8C, Flags 0x90 (bit 0: use the car's ICS, bit 2: another velocity
  source), Spring 0x94, Damping 0x98, SteeringRatio 0x9C, BrakeRatio 0xA0,
  StaticFriction 0xA4, SuspensionLimit 0xA8, SuspensionBlend 0xAC,
  RenderableSuspensionLimit 0xB0, Intersection 0xB8.., Center 0x?, Radius,
  Width, NormalLoad, Wobble 0x1C8, Rotation 0x1CC, Steering 0x1D4, Suspension
  0x1D8, CurrentLoad 0x1DC, BlendedLoad 0x1E0, OtherNormalLoad 0x1E8 (the
  load mmCarSim::Update redistributes), FullUpdated 0x1EC, OnGround 0x1F0,
  Friction 0x1F4, CurrentTireDispLat 0x1F8, CurrentTireDispLong 0x1FC,
  TireGripLat 0x200, TireGripLong 0x204, TireResistance 0x208, RotationSpeed
  0x20C, LatSlipPercent 0x210, LongSlipPercent 0x214, RubberSpring 0x218,
  RubberDamp 0x21C, OptimumSlipPercent 0x220, StaticFric 0x224, SlidingFric
  0x228, RubberSpringLat 0x22C, RubberDampLat 0x230, FricMultiplier 0x234,
  SteerMultiplier 0x238, SteeringInput 0x248, BrakingInput 0x24C, Phys 0x258.
  mmWheel::Update begins: Steering (0x1D4) = -(SteeringInput x SteeringRatio
  x SteerMultiplier); the wheel's frame comes from the car's ICS matrix (or
  the other source); Friction (0x1F4) = 1, or the surface's (agiPhysParameters:
  bump height/width/depth and friction), times WeatherFriction unless the
  room is flagged (tunnel/indoors, flags 3), then blended by
  CarFrictionHandling (0x18B8): below 1: (1-h)(1-f)+f... the same blend as
  MM2's (Wheel.cpp), so MM1's carFrictionHandling is MM2's.
* mmWheel::Update, tyre part (game.asm 157567-158440; dt = ARTSPTR+0x13C,
  1/dt = +0x140; Radius 0x1A8, Width 0x1AC, NormalLoad 0x1B0). It is the
  ancestor of MM2's `Wheel::update` and has the same pieces: lateral and
  longitudinal slip percent (|v_lat| / |v_fwd| and slip velocity / |v_fwd|,
  limited to +-1; both 0 for zero velocity), a friction curve, a tyre
  displacement per axis stepped by velocity x dt and limited by
  mu x load / RubberSpring(Lat), relaxation past the limit, and the force from
  the spring and damper on the displacement. Differences found so far:
  * load = max(CurrentLoad, NormalLoad);
  * friction mu(s) = Mult x SlidingFric for |s| >= UnkFriction2 (+0x240), else
    |s| x (2 x OptimumSlipPercent - |s|) x UnkFriction1 (+0x23C): a parabola
    (MM2's computeFriction differs); the longitudinal mu omits FricMultiplier
    and is scaled by the car's LongSlideMultiplier (0x18BC);
  * the displacement update blends two candidate values by `*Realism` (0x17F0
    of the car): (1 - R) x one + R x the other, at asm 158198-158215;
  * the longitudinal force term is the displacement spring plus the damper on
    its rate (RubberDamp), the lateral one uses RubberSpringLat/RubberDampLat
    (0x22C/0x230);
  * the force goes to the body with asInertialCS::ApplyForce at the contact
    point; with no contact (+0x168 == 0) the grip terms are zeroed and the
    suspension rests at -SuspensionLimit.
  UnkFriction1/2 are set by mmWheel::ComputeConstants (asm, 66 lines; not read
  yet). Next: a symbolic x87 evaluator (a script that follows one branch path
  and prints each stored value as a formula) will make the rest of the decode
  mechanical.
* Decoding aid: `tools/x87decode.py` follows one path through a function of
  game.asm and prints each store as a formula (stack slots as SSA variables,
  float constants resolved); `tools/x87names/*.txt` name the wheel and car
  fields. Example: `x87decode.py game.asm 156841 157564 --names
  tools/x87names/mmwheel.txt --regs ebx=W --take <label,...>`. It follows
  the fall-through path unless a jump label is listed in --take.
* mmWheel::ComputeDwtdw (asm 156841-157564) is the wheel's ground probe and
  suspension, not just a derivative: steer = -(SteerMultiplier x SteeringInput
  x SteeringRatio); a segment of half length (SuspensionLimit + Radius) about
  the wheel along its up axis is collided with the world
  (`mmPhysicsMGR::Collide`, a water polygon counts as a miss); on a hit,
  suspension = SuspensionLimit + 2 x Radius - (distance along the segment);
  currentLoad = Spring x suspension + Damping x (suspension rate) + NormalLoad,
  floored at 0 and multiplied by (contact normal . wheel up); blendedLoad =
  (1 - SuspensionBlend) x NormalLoad + SuspensionBlend x currentLoad. The
  rest computes the wheel's breakpoint speeds (the longitudinal optimum-slip
  points), as MM2's Wheel does. NormalLoad is the car's static weight share.
* Tyre force assembly (mmWheel::Update): force = currentLoad x (the car's up
  axis) + gripLat x (the wheel's lateral axis) - gripLong x (its forward
  axis), applied at the contact point (asInertialCS::ApplyForce).
  gripLong = -(dispLong x RubberSpring) - (1/dt) x (dispLong - previous dispLong)
  x RubberDamp; gripLat = -(dispLat x RubberSpringLat) - (1/dt) x rate x
  RubberDampLat, where rate = (1 - R) x |step| + R x (dispLat - previous
  dispLat) and R is the car's `*Realism` (so Realism only changes the lateral
  damping term). tireResistance = gripLong x Radius.
* mmWheel constants and small functions (decoded with tools/x87decode.py):
  * Init: Radius = |bbox height| / 2 of the wheel model, Width its depth;
    NormalLoad (0x1B0) = -(ICS.Mass x gravity) / NumWheels (`mmPhysicsMGR`
    +0x30 is the scalar gravity); RubberSpringLat / RubberDampLat default to
    RubberSpring / RubberDamp when 0.
  * ComputeConstants (rerun by SetFricMultiplier and CopyVars): s = StaticFric
    x FricMultiplier; UnkFriction1 = s / OptimumSlipPercent^2; a = 2s /
    OptimumSlipPercent; the two roots of -U1 x^2 + a x - SlidingFric x
    FricMultiplier = 0, i.e. OptimumSlipPercent +- sqrt(opt^2 - SlidingFric x
    Mult / U1); UnkFriction2 = the larger root. So mu(slip) is the parabola
    |s| (2 opt - |s|) U1 up to UnkFriction2 and the sliding value beyond.
  * SetInputs(steer, brake): SteeringInput, BrakingInput. SetFricMultiplier(m):
    FricMultiplier = m, then ComputeConstants. SetSteerMultiplier(m):
    SteerMultiplier = m. CopyVars(other) copies Spring, Damping, SteeringRatio,
    BrakeRatio, StaticFriction, SuspensionLimit, SuspensionBlend, +0x1B4/+0x1B8
    (bump height/width), RubberSpring, RubberDamp, OptimumSlipPercent,
    StaticFric, SlidingFric (not the Lat pair, nor the multipliers), then
    ComputeConstants.
  * Reset zeroes TireResistance, Wobble, Camber, Rotation, Steering,
    RotationSpeed, both displacements, Suspension, both slips and the particle
    counters.
  * Probe: a segment from (centre + (SuspensionLimit + Radius) x up) to (centre
    - (SuspensionLimit + Radius) x up); suspension = SuspensionLimit + 2 Radius
    - distance from the start, so SuspensionLimit is the droop limit (no hit:
    suspension = -SuspensionLimit) and compression is bounded only by the
    load. The wheel force is applied along the car's up axis, explicitly (no
    implicit contact stiffness as in MM2).
* mmEngine (asm 155464-155856; fields 0x20 MaxHorsePower, 0x24 OptRPM, 0x28
  MaxRPM, 0x2C GCL, 0x30 HPScale, 0x34 EngineMass, 0x38 MaxRotationSpeed, 0x3C
  OptRotationSpeed, 0x40 TorqueDropoff, 0x44 MaxTorque, 0x48 PullUpModifier,
  0x4C BreakDownModifier, 0x50 PrevGearRPM, 0x54 GearChangeTime, 0x58
  ChangingGear, 0x5C Throttle, 0x64 RotationSpeed, 0x68 RPM, 0x6C Horsepower,
  0x70 Torque; the car's engine is at +0x1680, its gearbox at +0x16FC).
  * Defaults: HPScale 1, GCL 0.1, PullUpModifier (sqrt 5 + 1)/2, BreakDown
    (sqrt 5 - 1)/2. Init: EngineMass = car mass x 0.0102.
  * ComputeConstants: MaxRotationSpeed = MaxRPM x 2pi/60, OptRotationSpeed =
    OptRPM x 2pi/60 (6.28319 x 0.0166667), TorqueDropoff = 1 / (MaxRot - OptRot)^2,
    MaxTorque = HPScale x MaxHorsePower x 746 / OptRot^3.
  * CalcTorqueAtFullThrottle (w = RotationSpeed, o = OptRot): w <= o:
    (PullUp x o - w)(BreakDown x o + w) MaxTorque; o < w <= MaxRot:
    (MaxRot + w - 2o)(PullUp x o - w)(BreakDown x o + w)(MaxRot - w)
    TorqueDropoff MaxTorque (the last branch, above MaxRot, not read).
    CalcTorqueAtZeroThrottle = (HPScale x HP x 746 / o) x (160 - w) x 0.5 /
    (o - 160). CalcTorque(t) = zero x (1 - t) + full x t.
  * Update: when the gearbox has just shifted (GearChanged) and no change is
    running: PrevGearRPM = RPM, GearChangeTime = GCL, ChangingGear = 1. Torque =
    CalcTorque(Throttle); with the state option MMSTATE+0x12C == 1 it is
    scaled by clamp(1.3 - car.Damage, 0, 1). Gear ratio 0 (neutral): the
    drivetrain is detached and RotationSpeed += Torque / (car mass x 0.001) x
    dt, clamped to [0, MaxRotationSpeed]; otherwise the drivetrain attaches.
    RPM = RotationSpeed x 9.5493. During a change (GearChangeTime > 0):
    Torque = 0, RPM = ((GCL - t) RPM + PrevGearRPM x t) / GCL, t -= dt; then
    ChangingGear = 0 and the gearbox's GearChanged = 0. Horsepower =
    RotationSpeed x Torque x 0.00134048.
* mmTransmission (asm 139902-140210; 0x24 IsAutomatic, 0x28 Clutch, 0x2C
  UpshiftThreshold, 0x30 DownshiftThreshold, 0x34 NumGears, 0x38 GearRatios[8],
  0x58 UpshiftRPM[8], 0x78 DownshiftRPM[8], 0x98 ManualGearRatios[8], 0xB8
  ManualNumGears, 0xBC DownshiftBias, 0xC0 GearChanged, 0xC4 InPark, 0xC8
  CurrentGear, 0xCC TimeInGear, 0xD0 GearChangeDelay). Gear 0 is reverse, 1
  neutral, 2 first (GetCurrentGear returns CurrentGear - 1). Defaults:
  automatic, NumGears 6, DownshiftBias 1.55, TimeInGear 0.4, GearChangeDelay 0
  (never set: inferred). Reset: gear 2, Clutch 1, TimeInGear 0. SetCurrentGear(g):
  an automatic box asked for g >= NumGears goes to NumGears - 1 with no flag;
  otherwise TimeInGear = 0, GearChanged = 1, CurrentGear = g. Upshift/Downshift
  work for the manual box only. CopyTrans copies GearRatios into
  ManualGearRatios and NumGears into ManualNumGears. Update (not while
  mmCar::IsDrivingDisabled): an automatic box in gear >= 2 with TimeInGear >
  GearChangeDelay: RPM > UpshiftRPM[gear] and not in the top gear and not
  just changed: up; else RPM < DownshiftRPM[gear] and gear != 2 and not just
  changed: down; else with throttle above 0.8 and DownshiftBias x
  DownshiftRPM[gear] > RPM (gear != 2, not just changed): down. TimeInGear
  += dt every sample.
* mmDrivetrain (asm 159518-160085; 0x20 car, 0x24 engine (0 when detached),
  0x28 gearbox, 0x2C wheel count (at most 4), 0x30 wheels, 0x40 and 0x44 the
  dynamic and static brake coefficients: 2 x car mass and 1.2 x that). Update:
  * brake input x = handbrake if wheel 0 has flag 2, else Brakes x BrakeRatio(0)
    + 0.002; coef = static (0x44) when wheel 0 is not turning, else dynamic;
    B = n x coef x x / Radius(0) (n wheels).
  * drive = engine Torque x gear ratio; net = drive - sum of the wheels'
    TireResistance. As MM2's: a stopped wheel: net < 0: eff = (0 < net + B ?
    0 : net + B), else max(net - B, 0); turning: b = B x sign(w0), eff = net +
    b, the brake holds when |net| <= |b|.
  * inertia = car mass x (0.02 + 0.0002 x ratio^2) with an engine attached, mass
    x 0.01 without; damping = 300 (the wheels' returned slope, always 0, plus
    300).
  * torque = -eff; each wheel's ComputeDwtdw(eff) gives a breakpoint speed
    (or the sentinels +-1e10 for a wheel in the air, +-1e11 for a crossed
    breakpoint). Up to 20 iterations: wNew = torque / (dt x 300 + inertia) x dt
    + w. A drivetrain with two wheels takes the nearer breakpoint (eff < 0: the
    smaller limit, eff >= 0: the larger), a single wheel its own. When it
    crosses (eff < 0: m < wNew; eff > 0: wNew < m): torque -= (m - w) x 300,
    w = m, that wheel's limit = sign(eff) x -1e11, repeat.
  * After the loop: a holding brake stops the wheel rather than reversing it;
    with an engine, |w| <= MaxRotationSpeed / |ratio| keeping the sign; then
    every wheel of the drivetrain gets RotationSpeed = w (no differential
    ratio, unlike MM2) and the engine's RotationSpeed = max(-ratio x w0, 0).
    The wheels' own Update runs afterwards as children of the car node.
  * The ground probe's suspension rate is limited below at -3 m/s.
* MM1's files hold only FrontLeft and BackLeft wheels; the right ones copy
  them. `_opp` files (racers) lack RubberSpringLat/RubberDampLat (inferred: the long
  values stand in).
* The tuning keys are listed in `phys/vehicle/Mm1CarParams.h`; the parser reads
  all 50 retail files (`mm2tool mm1carsim <mm1 source>`).

## Status

- [x] Tuning parser (`parseMm1CarParams`, `mm2tool mm1carsim`, tests).
- [x] The physics, ported into `phys::CarSim` as an MM1 mode
      (`CarSimParams::mm1`, `CarSim::isMm1()`): mmCarSim::Update with the
      drift/spin assist and the weight redistribution (`CarSimMm1.cpp`),
      mmWheel (`WheelMm1.cpp`), mmEngine and mmTransmission
      (`EngineMm1.cpp`), mmDrivetrain (`DrivetrainMm1.cpp`), mmForce, VehGyro
      and asAero (in `CarSimMm1.cpp`). Unit tests in `test_mm1_car.cpp` (a
      Beetle-sized car settles, launches, brakes, turns). NOT yet compared
      with the original in play; no tuning has been done.
- [ ] Not ported / inferred: mmStuck (MM2's vehStuck runs), the wheel's bump
      and surface particles, mmWheel's brake-lock vector (+0x16C), AWD (MM1
      probes only the first wheel), the player's steering curve (mmPlayer's
      PlayerRawSteering and SteerMultiplier), mmCar::IsDrivingDisabled (the
      game's `drivable` stands in), the right wheels sharing the left ones'
      lateral rubber numbers, GearChangeDelay 0, and network prediction state
      (`CarSimState` does not hold the MM1 state).
- [x] MM1 cars in Chicago (first cut): in an MM1 city `RaceScreen::loadMm1Car`
      builds any car that has a `mm1/tune/<name>.mmcarsim` (the player's,
      the racers' (the `_opp` tuning), the police's) through
      `SimVehicle::loadMm1`, with the model from `Mm1ModelLoader::vehicle`
      (wheel centres from the DLP template's WHLn_H groups, inferred as the
      box centre). Other cars keep MM2's. `OPENMM2_DEBUG_NO_MM1_CARS=1` keeps
      MM2's cars for comparison. Racers' Realism: 0, or 1 touching the player.
- [ ] MM1's car list for Chicago's menu (it still lists MM2's cars; a car
      without an MM1 tune runs MM2's physics); MM1's body collision bound
      (`bnd/<car>_bnd.bnd`; the body mesh box is used now); mmPlayer's
      speed-sensitive steering (tune/<car>.mmplayer: SpeedBaseLow/Hi, the
      DiscreteSteering* and filters). Without it full lock at speed is
      MM1's wheel lock (SteeringRatio, about 26 degrees), which keyboard play
      will find twitchy.
- [ ] Verification against the maintainer's memory of MM1 (no executable to
      compare with: SafeDisc).
