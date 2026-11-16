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
  missing). OPEN QUESTION: taken literally, a 0.35 Beetle on our MM1 physics stays in first gear
  at 22 mph (net torque ~0 just under the 6300 rpm upshift; engine and transmission match the
  asm). MM1 does not play like that, so something is still missing; until found, the values stay
  mapped onto MM2's range (`city::mm1MaxThrottleForMm2`).

Still MM2's: route planning (MM2 enumerates routes; MM1's PlanRoute follows the waypoint roads),
DetectCollision / AvoidCollision (MM1's blocked ranges), opponent avoidance (MM1: throttle 0.9
and a side offset), aiStuck details.
