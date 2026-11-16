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
