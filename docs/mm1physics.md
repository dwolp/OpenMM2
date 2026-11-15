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
* mmCarSim::Update starts with the drift/spin state machine: it reads the
  steering (+0x180C) and the previous sample's, compares |steering change|
  against fields at +0x1F54..0x1F60, keeps timers at +0x1F38/+0x1F40 (stepped by
  `ARTSPTR.dt`), picks front and back friction multipliers
  (`SetFricMultiplier` on the wheels at +0x948, +0xBB0, +0xE18, +0x1080) and
  sets `SetSteerMultiplier(1)`. Then it sets the wheel inputs
  (`SetInputs(steering x SteeringRatio, brakes)`) and the handbrake.
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
