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

NEXT (the cause of the first-corner pile-up, user report "cars turn left way too early"):
the steering target. Ours is MM2's first route node, the end of the current road: on circuit 0
all six racers aim at (149, 394) from the start, converge across the 140 m straight and clip
the inside corner. MM1's SolveTargetPoint = CalcSpeed, then
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
