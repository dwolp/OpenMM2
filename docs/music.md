# Music

Midtown Madness 2's soundtrack is interactive DirectMusic: style-based
segments (`.sgt`) that pick patterns from styles (`.sty`), played with
instruments from DLS collections (`.dls`). All of it is in `aud/dmusic/` in
`MM2AUD.AR`. OpenMM2 plays it with [GothicKit/dmusic](https://github.com/GothicKit/dmusic)
(MIT), a DirectMusic reimplementation, with a few fixes for MM2's data (see
[dmusic changes](#dmusic-changes)).

Code: `src/audio/Music.{h,cpp}` (tables, loader, engine, threaded player),
`src/audio/MusicDirector.{h,cpp}` (MM2's rules for when the music changes),
`src/audio/MusicMotif.{h,c}` (motif support, built into the dmusic library),
`src/audio/SynthReverb.{h,cpp}` (the synth port's reverb), the dmusic
section of `cmake/Dependencies.cmake`. Tools: `mm2tool musicinfo <source>`
and `mm2tool music <source> ...`.

## What the game data says

`aud/dmusic/csv_files/` drives the music:

| File | Content |
|------|---------|
| `singlerace.csv` | one row per race song: Start, Return, Idle Race, Idle Cop, Cop chase, Pause, Race results segments, plus the "Big Air" motif (style, motif name, band) |
| `singleroam.csv` | cruise songs: Start, Return (named *Restart*), Idle, Cop Chase, idle cop, Pause, Big Air motif |
| `ui.csv` | the menu segment (`UI`) |
| `londonambience.csv`, `sfambience.csv` | the city ambience segment ("SFX segment") |

Retail content (US disc, build 3390):

| Table | Songs |
|-------|-------|
| race | Enemy, Bullet, Duck (and Cover), London, PimpHand |
| cruise | SunRoof, TacoLoco, LeTigre, BackOff |
| other | `UI` (menu, Pause style), `LondonAmbience`, `SFAMbience`, `UndergrounAmbience` (sic; London Underground) |

Every song uses the Pause segment for the pause menu, one of two results
segments (VanillaNice style) and the `BigAir` motif of `GrooverStyle.sty`
played with that style's `BigAir` band. Not referenced by any table:
`GrooverStart/Return/Idle/Cops`, `NightRoamStart`, `Vanilla1/2`,
`LondonTrans1`, `useforsomething`, and `BigAir.sgt`. That last one is a
sequence-track version of the motif, which dmusic cannot play.

Observations from the files (all **verified** by parsing them):

* "Start" segments open with an intro and then loop from a loop point back
  into the body (e.g. `LondonStart` loops ticks 75264-222720).
* "Return"/"Restart" segments are the same songs starting mid-way
  (`play_start` > 0, e.g. `DuckReturn` at tick 61440). They let the game
  resume a song after idle or cop music without restarting it.
* Each state segment selects its own patterns through a groove level in its
  command track (London: Start 1-11, Idle 20, Cops 30, Idle cops 31).
* No soundtrack segment has a chord track. Everything plays against the
  DirectMusic default chord (see below).
* Some band entries reference General MIDI instruments from the system's
  `gm.dls`. They are DirectMusic Producer's placeholders for unused channels:
  in the race, cruise, menu, pause, results and ambience segments, no note is
  ever played on such a channel (checked by tracing every note-on). So the
  soundtrack is complete without `gm.dls`. If `$OPENMM2_GM_DLS` (or, on
  Windows, `%WINDIR%\System32\drivers\gm.dls`) exists it is used anyway.

## Behaviour

`audio::MusicEngine` holds three DirectMusic performances: the soundtrack, the
motif layer and the city ambience. `audio::MusicPlayer` runs the engine on a
worker thread that renders ahead into two ring buffers, music and ambience,
which the mixer pulls as `StreamSource`s. Segment files are loaded on a
separate loader thread, so the renderer never waits for disk or DLS parsing.
Commands are applied in the order they were issued.

| Call | Effect |
|------|--------|
| `playMenu()` | menu segment, immediately |
| `startRace(song, cruise, play)` | preloads the song's segments, then plays its Start segment immediately (or, with `play = false`, only selects the song for a `MusicDirector`); `song < 0` picks a random song |
| `setState(state, timing)` | that state's segment on the next beat or measure, or immediately (`MusicTiming`); `Start` / `Return` name the song's two racing segments |
| `setState(Racing)` | Start the first time, Return afterwards (tools) |
| `setState(state)` | without a timing: menus, pause and results immediately, the rest on the next measure |
| `triggerBigAir()` | the motif from the next beat of the soundtrack, played twice, following its tempo and chord |
| `setAmbience("london" / "sf" / "underground" / "")` | city ambience on its own stream |
| `setStereo(stereo)` | STEREO FX (not mono), taken by the next `playMenu` / `startRace` (see [the synth port](#the-synth-port-reverb-and-channels)) |
| `stop()` | silence |

The original's "Music/City Volume" option controlled the DirectMusic buffer,
which plays either the soundtrack or the city ambience segment, so the
ambience stream goes to `Bus::Ambient` with the music slider. The slider maps
to an Angel volume log(200 v) / log(200) (`DMusicWaveBuffer::SetVolume`; the
mixer applies it, see `docs/audio.md`). In MM2 the audio options make "Music"
and "Ambient" exclusive (`AudioOptions::ToggleMusic` / `ToggleAmbient`): with
music on, the city's DirectMusic ambience segment and its 3D ambient emitters
are not loaded (`mmGameMusicData::Load`, `mmPlayer::Init`). OpenMM2 plays
both. MM2 also stops the ambience segment underground and starts it again
outside (`MMDMusicManager::UpdateAmbientSFX`, audio flag 0x80: the player's
car in a room flagged underground); the race does the same with
`setAmbience("")` and the city's name. The music itself has no tunnel echo
(`MMDMusicManager::EchoOn` is never called). `setAmbience("underground")` plays
`UndergrounAmbience.sgt`, which MM2 never uses (a tool option).

### When the music changes (`MusicDirector`)

Ported from MM2 (`mmGame::StartMusic`, `UpdateDMusic`,
`MMDMusicManager::UpdateMusic`, `MatchMusicToPlayerSpeed`, `mmPopup`'s
`PlayPauseMusic` / `PlayReturnMusic` / `ShowResults`, the race modes'
`StopSegment` calls). The game calls `MusicDirector::update` every unpaused
frame with the player's speed, the number of cops pursuing the player
(`vehPoliceCarAudio::GetNumCopsPursuingPlayer`) and the car's airborne flag
(`vehCarAudio::IsAirBorne`, see `docs/audio.md`), and passes the director's
commands to `MusicPlayer`.

| Rule | Evidence |
|------|----------|
| The song is a row of `singlerace.csv` / `singleroam.csv` drawn with a generator seeded by the clock's second (`audio/AngelRandom.h`); every line after the header counts, so a blank line is a song without segments | MM2 (`mmGameMusicData::RandomizeNumber`, `GetNumDMusicChoiceGroups`, `mmSingleRaceMusicData::LoadMusic`, `LoadMusicSegments`) |
| The Start segment begins 1.25 s into the game, on the next beat | MM2 (`mmGame::StartMusic`) |
| Idle: speed at or below 5 m/s for 5 s; the switch waits for the next measure. The idle timer starts expired, so in cruise (no countdown) a stationary player gets the idle segment at once; the race modes hold the idle logic from the music start until "Go!", which also resets the timer | MM2 (`MatchMusicToPlayerSpeed`, both constants 5.0; flag +0x50 set by `StartMusic` for races, cleared by `mmSingleCircuit` / `mmSingleBlitz` / `mmMultiRace` at the start) |
| Leaving idle (above 5 m/s): Idle → Return, IdleCops → CopChase, on the next measure | MM2 (`MatchMusicToPlayerSpeed`) |
| Stopping during a chase gives IdleCops in races; cruise has no idle-cop segment (the cruise table's column is loaded but its index never set), so the chase music keeps playing | MM2 (`mmSingleRoamMusicData::LoadMusic` leaves +0x30 at -1) |
| Cop chase: when the pursuing-cop count goes from 0 to exactly 1; Return when it goes from 1 to 0, both on the next beat. Other changes (0 → 2, 2 → 0) switch nothing | MM2 (`UpdateMusic`) |
| Big Air: when the car becomes airborne (once per jump); the motif plays as a secondary segment with one repeat (twice) from the next beat (flags DMUS_SEGF_SECONDARY, GRID and BEAT; the beat is assumed to win); a jump while it still plays adds nothing | MM2 (`UpdateMusic`, `DMusicObject::PlayMotif`, flags 0x880 \| 0x1000, `SegmentWrapper::Play` with SetRepeats(1), nothing while playing) |
| Pause: the Pause segment on the next beat; resuming restarts the previous segment (whatever it was) from its beginning | MM2 (`mmPopup::PlayPauseMusic`, `PlayReturnMusic`) |
| The last stretch: the cop chase segment, with an END embellishment on the next beat (not rendered by dmusic), when a checkpoint race has only the finish left, when a circuit's final lap starts and when that lap's last checkpoint is cleared (`MusicDirector::finalStretch`, on the session's FinalCheckpoint / FinalLap events); nothing if it already plays. A cop chase ending later returns as usual | MM2 (`mmWaypoints::Update`: SegmentSwitch(+0x24, DMUS_COMMANDT_END, DMUS_COMPOSEF_BEAT), in music mode) |
| Finish: the race modes stop the music at once (`StopSegment(0)`); the music logic keeps running, so standing still for 5 s brings in the idle segment; the results popup starts the results segment on the next beat (with an END embellishment, not rendered by dmusic) | MM2 (`mmSingleRace` / `mmSingleCircuit` / `mmSingleBlitz` `StopSegment`, `mmPopup::ShowResults`) |
| Wrecked: the race modes end the music with a composed ending on the next beat (`StopSegment(1)`: AutoTransition to nothing, DMUS_COMMANDT_END, DMUS_COMPOSEF_BEAT); OpenMM2 stops on the next beat (`MusicDirector::damagedOut`) | MM2 (`mmSingleRace::UpdateGame`, `DMusicObject::StopSegment`); the ending **inferred** |
| A segment switch to the segment already playing does nothing | MM2 (`DMusicObject::SegmentSwitch`) |
| Restarting the race (the popup's Restart): StartMusic again at once, so the Start segment comes back and the race modes hold the idle logic until "Go!" (`MusicDirector::restart`). If Start is still the current segment MM2 replays it from its beginning; OpenMM2 lets it play on | MM2 (`mmGame::Reset`, `mmGame::StartMusic`); the replay is a **deviation** |

Notes on the port:

* MM2's measure switches are composer transitions (`AutoTransition` with the
  groove command and `DMUS_COMPOSEF_MEASURE`). dmusic's composed fill
  transitions are silent for styles without fill patterns, so the new
  segment simply starts on the measure.
* Music variation is random per play, as with DirectMusic's `rand()`-based
  pattern and variation choice. `$OPENMM2_MUSIC_SEED` fixes the seed for
  reproducible renders.

## The synth port: reverb and channels

Every DirectMusic sound MM2 makes goes through a port on the software synth
named "Microsoft Synthesizer" (`DMusicObject::FindMSSoftWareSynth`), opened by
`DMusicObject::InitPort` (from `MMDMusicManager::Init` through
`DMusicManager::Init` and `DMusicObject::Init`). Each phase of the game makes
its own music manager and so its own port: the menus
(`mmInterface::PlayUIMusic`, or `AudioOptions::ToggleMusic` when the music is
turned on), a race's music (`mmSingleRaceMusicData` /
`mmSingleRoamMusicData::LoadMusic`) or, with music off, its city ambience
(`mmGameMusicData::LoadAmbientSFX`); `EndPhase` deletes it. The Big Air motif
is a secondary segment on the soundtrack's performance, so it shares the
soundtrack's port. The port's `DMUS_PORTPARAMS`:

| Parameter | MM2 (`DMusicObject::InitPort`) | OpenMM2 |
|-----------|--------------------------------|---------|
| channel groups | 2 (32 pchannels) | dmusic's channels |
| voices | 41 (`MMDMusicManager::Init`), when the port's caps give a limit | no limit (TinySoundFont adds voices as needed) |
| audio channels | `AudManagerBase::IsStereo()` + 1: one with STEREO FX set to mono | stereo, or both channels (L + R) / 2 when mono |
| sample rate | `AudManager::GetSamplesPerSecond`: 22050 Hz (its constructor's value; nothing changes it) | the mixer's rate (48 kHz); the reverb runs near 22050 Hz |
| effects | `DMUS_EFFECT_REVERB` when the object's reverb flag (+0x18, set to 1 by the constructor) is on and the port's caps report the effect, which the Microsoft Synthesizer's do | `SynthReverb` on the music and on the ambience |

**Mono.** `AudioOptions::SetStereoFX` maps STEREO FX 0 (mono) to a clear
stereo flag and 1 (stereo) and 2 (surround) to a set one
(`AudManagerBase::SetStereoFlag`), so only mono gives a one-channel port.
`MusicPlayer::setStereo` follows the setting and the engine takes it when the
menus or a race start their music (`playMenu`, `startRace`), as MM2 reads it
when it opens the port: a change in the options is heard from the next phase.
The mono port played through a mono DirectSound buffer, which sounds the same
in both speakers; how the synth folds its panned voices into one channel is
not known, so OpenMM2 uses (L + R) / 2 (**inferred**).

**Reverb parameters.** MM2 never sets the reverb's parameters: there is no
`IKsControl` property call on its port (and the audio manager's EAX and
reverb calls, `AudManagerBase::SetReverbFlags` among them, are empty). So the
synth's defaults apply, `DMUS_WAVES_REVERB_PARAMS` as the DirectX 7
documentation gives them
([Microsoft Learn: DMUS_WAVES_REVERB_PARAMS](https://learn.microsoft.com/en-us/previous-versions/dd551316(v=vs.85)),
header `dmusicc.h`):

| Member | Default | Meaning |
|--------|---------|---------|
| `fInGain` | 0 dB | input gain |
| `fReverbMix` | -10 dB | 0 dB is 100% reverb with no direct sound, lower is drier; "the coefficients are calculated so that the overall output level stays approximately constant, regardless of the amount of reverb mix" |
| `fReverbTime` | 1000 ms | decay time |
| `fHighFreqRTRatio` | 0.001 | the high frequencies' decay time over `fReverbTime` (1000 ms and 0.1 would be 100 ms) |

The same page says the reverb is Waves' TrueVerb, licensed to Microsoft as
"SimpleVerb", and that from DirectX 8 music reverb is a DMO (the Waves Reverb
DMO, `DSFXWavesReverb`, with the same four parameters but a default mix of
0 dB). Which of the two a DirectX 7 style port gets on a newer DirectX
runtime, and with which mix, is not known; OpenMM2 uses the DirectX 7
values MM2 was written against (**inferred**). Wine's dmsynth turns its
FluidSynth reverb on for `DMUS_EFFECT_REVERB` and sends every channel to it
at the General MIDI default level (CC91 = 40, about -10 dB), which comes to
much the same amount of reverb.

**Reverb algorithm** (`audio/SynthReverb.cpp`, **inferred**: the Waves
algorithm is proprietary). The structure follows OpenMPT's emulation of the
Waves Reverb DMO (`soundlib/plugins/dmo/WavesReverb.cpp`, BSD-3-Clause):

* four comb delays of 45 ms × 2^(n/4) (45, 53.5, 63.6, 75.7 ms), in two
  pairs; each pair is cross-coupled by a two-channel allpass lattice
  (coefficient 0.618, the inverse golden ratio) that takes 11.5% of the
  pair's delays. Left feeds combs 0 and 3, right combs 1 and 2 (comb 2
  inverted); the output is combs 0 + 2 (left) and 1 + 3 (right), so a mono
  input comes out decorrelated;
* each loop's gain makes the tail fall 60 dB in `fReverbTime`; a two-tap
  low-pass in each loop takes the highest frequencies down in
  `fReverbTime × fHighFreqRTRatio` (with 0.001 it averages two samples, a
  zero at the reverb's Nyquist frequency);
* `fReverbMix` is the reverb's share of the power: direct sound ×
  √(1 − 10^(mix/10)), reverb input × √(10^(mix/10)) × a normalisation of the
  loops' energy; both × `fInGain`. At the defaults the direct sound is at
  0.949 (-0.46 dB).

The loop filter acts at the reverb's own Nyquist frequency, and MM2's synth
port ran at 22050 Hz, so the reverb runs at the output rate divided by the
whole number nearest to output / 22050 (24 kHz at 48 kHz, 22050 Hz at
44.1 kHz): the input is averaged over those frames and the reverb's output
interpolated linearly between its samples. Its loop values are flushed to
zero below 1e-20 (no denormals; a tail ends in exact silence), and input that
is not a finite number is fed as silence.

Measured from an impulse at 48 kHz (`tests/audio/test_synth_reverb.cpp`
measures the same way; T30 from the Schroeder integral per band): 1.00 s at
250 Hz, 0.92 s at 2 kHz, 0.75 s at 4 kHz, 0.46 s at 8 kHz; the first
reflection after 32 ms; left and right uncorrelated.

Effect on the music (30 s renders with `OPENMM2_MUSIC_SEED=1`, `mm2tool
music --wav` with and without `--dry`): the correlation of the two channels
drops from 0.90-0.995 to 0.74-0.90, the side (L − R) to mid (L + R) energy
from -12.6..-26.0 dB to -8.3..-12.5 dB, with the RMS level up by at most
0.7 dB. Samples over full scale stay rare: none in nine of the eleven
renders, 2 → 0 in cruise 1 and 1 → 23 (of 2.9 million) in cruise 3:

| Render | Correlation dry → reverb | Side/mid dry → reverb |
|--------|--------------------------|-----------------------|
| race 0 (Enemy) | 0.898 → 0.744 | -12.6 → -8.3 dB |
| race 1 (Bullet) | 0.973 → 0.900 | -18.4 → -12.5 dB |
| race 2 (Duck) | 0.899 → 0.776 | -12.7 → -9.0 dB |
| race 3 (London) | 0.935 → 0.779 | -14.6 → -8.9 dB |
| race 4 (PimpHand) | 0.995 → 0.852 | -26.0 → -10.9 dB |
| cruise 0 (SunRoof) | 0.988 → 0.868 | -22.3 → -11.4 dB |
| cruise 1 (TacoLoco) | 0.935 → 0.829 | -14.6 → -10.3 dB |
| cruise 2 (LeTigre) | 0.942 → 0.809 | -15.2 → -9.8 dB |
| cruise 3 (BackOff) | 0.928 → 0.820 | -14.3 → -9.6 dB |
| menu (UI) | 0.980 → 0.816 | -20.0 → -9.9 dB |
| SF ambience | 0.961 → 0.832 | -16.9 → -10.3 dB |

The reverb costs about 0.05% of a core per stream.

## dmusic changes

The upstream sources are patched at configure time. Each patch is checked, so
a future pin bump fails loudly if one no longer applies:

1. **Missing collections** (`Band.c`): an instrument whose DLS collection
   cannot be found (`gm.dls`) is skipped instead of failing the band and with
   it the whole segment.
2. **Empty collections** (`Band.c`): instrument lookup underflowed for an
   empty collection.
3. **Parts without variations** (`Performance.c`): a part with no valid
   variations (in `Pause.sty`) caused a division by zero; it is skipped.
4. **Late style** (`Performance.c`): a segment whose style track starts after
   its first groove command (`DuckCops.sgt`) uses that style from the start,
   instead of crashing on a NULL style.
5. **Mid-way starts** (`Performance.c`): when a segment starts mid-way
   (`play_start` > 0, all Return/Restart segments and two cop segments), the
   style, band, tempo and groove in effect at the start point apply. Upstream
   discarded them, which left those segments silent or playing with the
   previous segment's style.
6. **Motif support** (`Performance.c`): `DmPattern_generateMessages` is
   exported for `MusicMotif.c`.

Two more fixes live in `MusicMotif.c`:

* **Default chord.** Performances start with DirectMusic's default chord, C2
  major (root 12, pattern `0x91`, scale `0xab5ab5`). dmusic started from an
  all-zero chord, which, with no chord tracks in MM2, put every
  chord-relative note an octave too low; bass notes even came out negative.
  The evidence: with the default chord, the drum music values map exactly
  onto the key ranges of the MM2 DLS drum instruments. For example the
  SunRoof hi-hats `0x3010`/`0x310f`/`0x320f` land on keys 38/39/42 and the
  kick `0x3300`/`0x4000` on 47/48; dmusic's zero chord gave 26/27/30 and 35/36.
  That previously silenced the first seconds of `SunroofStart`.
* **Motifs.** DirectMusic plays a motif as a secondary segment; dmusic has
  none. `OpenMM2_DmPerformance_playPattern` plays a named pattern of a style
  with a named band of that style on the motif performance.

## Known differences from the original

* **Synthesizer.** dmusic converts DLS instruments to TinySoundFont.
  Articulations that SoundFont cannot express (some scaled DLS connection
  blocks) are dropped. Timbre and envelopes are close but not identical. The
  synth renders at the mixer's rate (48 kHz) where MM2's port ran at
  22050 Hz, and has no 41-voice limit (MM2's port would steal voices past
  it). The Microsoft Synthesizer's reverb is approximated after the synth
  (above); MM2 asked for no chorus (`DMUS_EFFECT_CHORUS` is not set).
* **Level.** The synth output is scaled by 0.5, which keeps every song below
  full scale (peaks of -0.6 dBFS or lower across all states in 8 s renders;
  over 30 s a handful of samples reach it, see the reverb's measurements).
  The original's absolute music level relative to effects is not known.
* **Pattern selection.** dmusic follows DirectX 7-style random selection and
  never picks one-measure groove patterns (a rule added upstream for Gothic).
  For MM2 that skips `LondonIntro` and `VanillaIntro` at groove 1, so the
  longer pattern at that level is always used.
* **Unsupported content.** Sequence tracks (`BigAir.sgt`) and DirectMusic
  Producer chunks dmusic does not read (`mtfs` motif settings) are ignored.

## Verification

* `mm2tool musicinfo` loads all 70 segments referenced by the tables (and
  the tests load every `.sgt` in `aud/dmusic`).
* Every song × state (race: racing, idle, idle-cops, cop-chase, paused,
  results; cruise: the same without results) rendered for 8 s: no clipped
  samples, no silent seconds, per-second RMS between about -37 and -12 dBFS.
* A full state sequence (racing → idle → cop chase → racing → paused →
  racing → results) renders without gaps.
* The Big Air motif adds sound only from the beat after the trigger (it plays
  twice); the render before it is bit-identical with a fixed seed.
* Rendering runs at about 300× real time (release build); the player keeps
  about 85 ms (4096 frames at 48 kHz) buffered, which is also the worst-case
  latency of a command before the musical boundary it waits for.
* `tests/audio/test_music.cpp` covers table parsing, loading, audible output,
  the motif, silence after stop, the default-chord fix, streaming through
  the mixer and the `MusicDirector` rules.
* `tests/audio/test_synth_reverb.cpp` covers the reverb: the documented
  defaults and dry gain, the internal rate, the decay time at 250 Hz within
  10% at 48, 44.1 and 22.05 kHz (and for 0.5 s and 2 s), the highs decaying
  faster (and not with a ratio of 0.999), decorrelated channels, the direct
  sound unchanged with the effect off or the driest mix, `fInGain`, block
  sizes, silence (and a tail ending in exact zeros, no denormals), no NaN from
  bad input or extreme parameters; with game data, a wider race song and a
  mono engine and player.
* `mm2tool music ... --dry` renders without the reverb (bit-identical to
  OpenMM2 0.4.2's renders), `--mono` as with STEREO FX mono; every render
  prints the channels' correlation and side/mid ratio.
* A General MIDI fallback for systems without `gm.dls` (FluidR3_GM.sf2 and
  the like): dmusic reads only DLS collections, so it would take a dmusic
  patch to load SoundFont presets for the GM references; since no
  soundtrack note plays on a GM instrument (above), it is not done (open).

## Midtown Madness 1's music (Chicago)

MM1 played its music from the disc's CD audio tracks. A raw (2352-byte sector) disc image keeps them after the
data track (the ISO volume's last sector); the retail US image has 15 tracks of about 1.5 to 3 minutes. OpenMM2
Enhanced reads them straight from the image (`audio::Mm1CdMusic`, nothing is copied out of it): a background scan
finds the tracks by their silence (the image has no table of contents; a second of near-silence separates them),
and a Chicago race with MUSIC on plays one at random, then the next ones in turn, instead of MM2's songs. The
race's end stops it (as MM2's finish). Which track MM1 chose when is not decoded (inferred). A disc image without
audio tracks (a data-only ISO or a folder) keeps MM2's music in Chicago. `OPENMM2_DEBUG_*` none; the log lines
`mm1 music: ...` show the scan and each track.
