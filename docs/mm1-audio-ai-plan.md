# Chicago: MM1's sounds and racer AI (plan)

Goal (maintainer): everything exactly like Midtown Madness 1. Source: Open1560
`code/midtown/game.asm` plus its C++ (`mmcar/*audio*`, `mmai/*`, `mmaudio/*`); data in
`CORE.AR` (`tune/*.mm*caraudio`) and `AUDIO.AR` (`aud/aud22/*.22k.wav`, mounted as
`mm1/aud`, indexed by `SoundBank` as `mm1:<name>`).

## Sounds

| Area | MM1 class | State |
|---|---|---|
| Skids (clear/rain), volume | mmSurfaceAudio::UpdateSkid* | done (snow: unported) |
| Banger impacts (glass, signs...) | mmImpactAudio::PlayBanger | done |
| Car-vs-wall, car-vs-car impacts | mmImpactAudio::Play/PlayWall/PlayCar | done (ids 0..2 wall: soft/med/huge by strength 2/5/15 m/s; id 9 PlayCar is empty in MM1; strength = impulse / mass, inferred) |
| Player engine, clutch, horn | mmPlayerCarAudio, mmEngineAudio | done (tune/<car>.mmplayercaraudio; clutch between reverse and first only) |
| Opponent / network engines | mmOpponentCarAudio, mmEngineAudioOpponent | done (constant max volume x attenuation, pitch clamp(rpm x mult)); police siren, ambient traffic, truck reverse beeper: todo |
| Surface roll, suspension, tyre wobble | mmSurfaceAudio::UpdateSurface/Suspension/TireWobble | done for clear/rain (grass roll, cobble thumps, Suspension3, TireWobble1); snow (UpdateSurfaceSnow/Skid) todo; surface index mapping inferred |
| Sirens, explosion | mmPoliceCarAudio | done (slow/fast fluctuators on timers and squared distance, 4 loops, min volume 0.85); cop voices todo |
| Weather and ambience (rain, thunder, planes, bridge, fountain) | mmAudioManager and friends | todo |
| UI, race tags, voices, vehicle-select voice lines | various | todo |

## Racer AI

MM1 racers (aiVehicleOpponent) run three goals in priority order: aiGoalStop (full
brake, momentum x 0.95), aiGoalBackup (reverse toward the road centre) and
aiGoalFollowWayPts. Open1560 has C++ for Update, Context, Stop, Backup and
aiVehicleOpponent::Init/Update; the rest is asm in `game.asm` (line numbers at the time
of writing):

| Function | asm line | notes |
|---|---|---|
| PlanRoute | 128585 | next waypoint intersections/roads |
| LocateWayPtFromRoad / FromInt | 128769 / 129352 | |
| DetRdSegBetweenInts | 129454 | |
| DetectCollision | 129540 | blocked ranges on the road |
| AddToBlockedRange | 130735 | |
| AvoidCollision | 130892 | |
| CalcSpeed | 131401 | throttle/brake from the bends ahead; ~1170 lines |
| SolveTargetPoint | 132578 | |
| DetectOpponentCollision / AvoidOpponentCollision | 128046 / 128198 | |
| aiStuck Update / Pegged | 134581 / 134543 | |
| aiMap::DetermineOppMapComponent | 61731 | which road/vertex the car is on |
| aiRailSet (CalcRailPosition, CalcTurnIntersection, SolveTurnType...) | 101575.. | turn geometry |

Structure of aiGoalFollowWayPts (Open1560 `mmai/aiGoalFollowWayPts.h`): Brakes,
Throttle, Steering, DistToSide, Offset, TargetPtOffset, MaxThrottle, NearObstacles[12][2],
FarObstacles[12][2], WayPtIds, WayPtIdx, CurLap, State, DamageState...

OpenMM2's racers use MM2's aiVehiclePhysics port (`src/ai/Driving*.cpp`: calcRoute,
enumRoutes, continueCheck), which MM2 rewrote after MM1, so MM1's behaviour needs its own
driver, not constant tweaks (`adaptMm1AiMapConfig` and Realism 0 / 1-on-contact only
approximate it). Plan: a separate `Mm1RacerDriver` ported function by function in the
order above (data: the converted MM1 AI map must keep MM1's own intersections and paths,
`city::Mm1Bai`), switched on for Chicago racers; verify by driving each circuit and
comparing lap times and lines with the current driver.

### Done so far (racers on MM1's physics)

- Steering: the plain heading error clamped to +-1, no handbrake (MM2: x 1.33 x 1.428 and the
  handbrake above 30 m/s); angular momentum x 0.1 while aimed within 0.1 rad and not touching
  the player (aiGoalFollowWayPts::Update).
- CalcSpeed (decoded): same corner formula and numbers as MM2 (R = r / (1 - sin((3.14 - |a|)/2)),
  v = sqrt(R x 23.76), brake = (speed - v) / (t x 23.76), threshold 0.7, yaw x 0.85; a flagged
  next road halves v and cuts the linear momentum x 0.97). MM1 brakes only for junction turns
  (the next one, or the one after a straight junction), never for bends within a road: the MM2
  driver's in-road sharp-turn braking is skipped for MM1 cars.
- aiGoalBackup: MM1's own (road-centre target via aiPath::Index, throttle |angle| 0.1..0.85,
  5 s / 3 s in gear, forward again below 2 m/s).
- MaxThrottle: MM1 uses the race file's number as the throttle (aiRaceData "%s %s %f", 1.0 when
  missing), taken literally for racers on MM1's physics. Verified in the asm: the constructor
  also stores it in mmEngine's field_60, which only aiStuck::Pegged / mmStuck::Pegged read
  (Throttle > 0.75 x it); nothing else writes Engine.Throttle. By MM1's own engine (peak torque
  P/O, zero-throttle line -0.5 T (w - 160)/(O - 160)) Amateur circuit 0's 0.30 / 0.35 Beetles
  net negative torque under the 6300 rpm upshift and stay in first gear (~22 mph). Professional
  reads `<race>.aimap_p` (aiRaceData appends "_p" when MMSTATE+0x12C is 1): circuit 0 has
  Cadillacs with no number (1.0), which matches the user's MM1 footage (racers at the
  player's pace). Racers on MM2's physics keep the mapping onto MM2's range
  (`city::mm1MaxThrottleForMm2`, inferred).
- Frontend script `pro:1` selects Professional for testing.

FIXED 2026-11-16 (the first-corner pile-up, user report "cars turn left way too early"), two causes:
1. The route window skipped the start leg. MM2's window starts on the leg after waypoint 0
   (its grids stand in that waypoint's room); MM1's grids stand on the street before the first
   corner (circuit 0: start (10, 415), first waypoint 150 m away at the corner), so all racers aimed
   straight across the block at the lane after the corner. `Opponent::routeFromPath(..., mm1)` now
   puts the intersection the grid leads out of before the first waypoint (the far end of the road
   the start lies on, or the room's intersection).
2. The target was the road end. `PhysicsDriver::mm1RailTarget` aims `clamp(0.5 s of travel, 7, 20)`
   m ahead along the road's first lane line on the driving side (inferred stand-in for MM1's
   TargetPtOffset; the speed term of DetermineOppMapComponent is not decoded), then on along the
   route's own nodes at the junction. MM2's racer-to-racer route avoidance is off for these cars.
   Circuit 0 Professional: all six hold z = 410 down the straight, take the corner at 26-40 mph,
   reach 100+ mph and keep lapping; no pile-up.
Look-ahead DECODED (2026-11-16, DetermineOppMapComponent road branch, asm 62779-63860; Open1560
constants 0.65 rad, 30, 7, 20 m): TargetPtOffset = 20 m when the next road is within 0.65 rad of
the current road's last segment. For a sharper turn: x = distance along the road to the turn's arc
start (corner less setback = cos((pi-|a|)/2) R, R = r0 / (1 - sin((pi-|a|)/2)),
r0 = clamp(laneWidth - side x DistToSide, 0.5, nextRoadWidth)); the car before the arc: x < 30 ->
max(x, 7), else 20. Inside the arc it solves the arc (asm 63374-63675: RoadDist is rewritten, and
the value is clamped to 7..20 from a term with 0.488, 23.76, speed and R); that formula is NOT
decoded, 7 m is used (inferred). Also unported from that branch: DistToSide is shifted by 4 m
towards the turn side when the car is too close to the far edge (flt 61B27C/61B280 = -4/+4).
`PhysicsDriver::mm1RailTarget` uses this, with the car's DistToSide (offset from the lane line,
clamped to the road) held for the whole race.

Still approximate: lane choice at the start (the grid's own line), the junction curve (the route's
corner node instead of CalcTurnIntersection), the in-arc look-ahead.

### Porting MM1's rail geometry natively (started 2026-11-16; "take everything from MM1's code")

Findings so far (Open1560 game.asm line numbers):
- aiRailSet::CalcCopRailPosition (102568-106184, ~3,000 lines after dropping the bounds-check
  warnings) is one function with the same blocks repeated per case. It builds the target from the
  small helpers below plus CalcXZPosition (110314) = a cubic Hermite through the entry point and
  exit point with tangents IntersectionEntryVector / IntersectionExitVector (the junction curve;
  `mm1RailCurve` in Mm1AiMap.h already implements the Hermite basis), SolveXZCurve (110766) =
  position + derivative of the stored cubic (coefficients in the rail at 0x60..0x7C).
- Ported and tested (src/city/Mm1Rail.*): aiPath::CenterLength (72362), CenterIndex (72584),
  SubSectionDir (74132), IntersectionEntryVector (74242), IntersectionExitVector (74292). The
  parser now keeps CenterOffsets (verified against Chicago's real centre lengths) and
  SubSectionOffsets.
- Still to port, in this order: CalcTurnIntersection (107302), SolveTurnType (110125),
  CalcRailLength (106800), CalcRailState (106187), CalcCopRailPosition (102568),
  DetermineOppRoadPosInfo (64235) / aiPath::CenterDistance, PredictIntersectionPath (64411),
  the intersection and no-component branches of DetermineOppMapComponent (61731), then
  PlanRoute / LocateWayPt* (128585-129540), DetectCollision / AvoidCollision (129540-131401),
  Detect/AvoidOpponentCollision (128046-128585), aiStuck. Then a native MM1 racer driver on
  `Mm1AiMap` replaces the MM2-route-based driver for MM1 racers.
- `tools/x87decode.py` follows one branch only; read the asm by hand where the code branches, and
  `strip.py`-style filtering (drop the `Warningf` bounds-check blocks) halves the reading.

Original analysis kept below. MM1's SolveTargetPoint = CalcSpeed, then
`Rail->CalcCopRailPosition(TargetPt, carPos, Rail->RoadDist + TargetPtOffset, DistToSide)`
(asm 132578): a point along the rail at the car's own lateral offset, with TargetPtOffset from
DetermineOppMapComponent (61731-64232; road branch 62779-63858 via DetermineOppRoadPosInfo
64235 / aiPath::CenterDistance; intersection branch with PredictIntersectionPath 64411):
20 m when there is no next road, else clamp(speed-based term with 0.488 and 23.76, 7, 20).
CalcCopRailPosition is 102568-106184 (turn curves via CalcTurnIntersection). These work on
MM1's one-way aiPath records (the opposite path at +0), so the port needs the MM1 path data
alongside the converted MM2 roads.

Still MM2's: route planning (MM2 enumerates routes; MM1's PlanRoute follows the waypoint roads),
DetectCollision / AvoidCollision (MM1's blocked ranges), opponent avoidance (MM1: throttle 0.9
and a side offset), aiStuck details.
