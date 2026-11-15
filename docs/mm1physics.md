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
