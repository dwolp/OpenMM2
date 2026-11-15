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
* MM1's files hold only FrontLeft and BackLeft wheels; the right ones copy
  them. `_opp` files (racers) lack RubberSpringLat/RubberDampLat (inferred: the long
  values stand in).
* The tuning keys are listed in `phys/vehicle/Mm1CarParams.h`; the parser reads
  all 50 retail files (`mm2tool mm1carsim <mm1 source>`).

## Status

- [x] Tuning parser (`parseMm1CarParams`, `mm2tool mm1carsim`, tests).
- [ ] mmCarSim::Update flow and the drift/spin assist.
- [ ] mmWheel (suspension, rubber, tyre forces, Realism blend).
- [ ] mmEngine, mmTransmission, mmDrivetrain.
- [ ] VehGyro, mmForce, asAero.
- [ ] MM1 cars in Chicago: model, wheels, car list, player, racers, cops.
- [ ] Verification against the maintainer's memory of MM1 (no executable to
      compare with: SafeDisc).
