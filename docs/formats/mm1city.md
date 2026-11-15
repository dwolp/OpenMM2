# MM1 cities (Chicago)

Status: work in progress, for bringing MM1's Chicago into the OpenMM2
Enhanced fork. MM2 never reads any of this. Readers live beside their MM2
counterparts and are named `Mm1*`; this page collects what is known so far.

Sources: Open1560 (GPL-3.0): its C++ where rewritten, else `code/midtown/game.asm`.
Data: the user's own MM1 disc; nothing here is kept in git.

MM1 builds a city very differently from MM2. There is no PSDL: streets,
ground and landmarks are prebuilt meshes per cell, and the buildings are
"facades", flat fronts stretched along the street edges.

## Files

| File | Read by | What |
|---|---|---|
| `city/<city>.cells` | `asRenderWeb::LoadCells` | text: cell count, max id, then `id,cullFlags[,roomFlags,tagCount,tags...]` |
| `city/<city>.ptl` | `asRenderWeb::LoadPortals` | portals between cells (`PtlPortal`, 0x24 bytes) |
| `city/<city>.ext` | | text: x min, z min, x max, z max of the city (Chicago: −1428 −1480 1301 1420) |
| `city/<city>.bng` | `mmCullCity::LoadBangers` | props and other placed objects; see below |
| `city/<city>.fcd` | `mmCullCity::LoadFacades` | facades and buildings; see below |
| `city/<city>.gizmo` | | text: drawbridges (`DrawBridgeN` blocks of `mesh,flag,x0,y0,z0,x1,y1,z1`) |
| `city/<city>/chicago.bai` | | AI road network; not MM2's `CAI1` format |
| `city/<city>/anim_*.csv` | | El train, boats, planes: point lists |
| `bnd/<city>_hitid.bnd`, `bnd/<city>city/` | `asRenderWeb::LoadHitId`, `LoadRoomBounds` | collision (MM1 `2DNB` bounds) |
| `bms/<city>city/cull<N>_*.bms` | `mmCellRenderer` | cell meshes in world coordinates (`bms.md`) |
| `bms/<city>lm/cull<N>_*.bms` | | landmark cells (ids below `CITY_MESH_START`) |
| `race/<city>/` | | race data, laid out nearly as MM2's (`race.md`) |

Chicago has 978 cells. The `_h`/`_m`/`_l` meshes are the streets; `_h2`,
`_m2`, `_l2` and `_a2` hold more (the lake, the elevated Loop, landmarks).

## `.bng` and `.fcd`

Both are a u32 count, then fixed records each followed by a NUL-terminated
mesh name (at most 63 characters). Every record goes to
`mmCullCity::AddInstance(cell, name, 0, flags, &v0, &v1, &v2, f)`.
Verified: Chicago's `.bng` (5,941 records) and `.fcd` (3,389) parse to their
last byte.

```
.bng record, 0x1C bytes:  u16 cell, u16 flags, Vec3 position, Vec3 ahead     (v2 = null, f = 0)
.fcd record, 0x2C bytes:  u16 cell, u16 flags, Vec3 start, Vec3 end, Vec3 sides, f32 width
```

`ahead` is a second point, not always 1 m away: 85 of Chicago's records
are other distances (the Chinatown gate's is 11 m along the gate, trailers'
16 m) and 18 are above or below the position. Only its horizontal direction
matters.

Flags (`INST_INIT_FLAG_*`): 0x1 static, 0x2 shear, 0x4 building, 0x8 left,
0x10 right, 0x20 top, 0x40 glow, 0x200 upper, 0x400 back, 0x800 terrain.
`AddInstance` picks the class: not static → a banger (prop); static + shear →
`mmShearInstance` (`mmShearInstance::Init`: x along end − start scaled by
|end − start| / width, keeping the slope; y up; z = (−u.z, 0, u.x); the origin
exactly at start, where MatrixFromPoints lowers it to the lower end; meshes
the model's own H/M/L/VL, else FACADE, else BLDG; `city::mm1ShearMatrix`.
Chicago's ramp walls, freeway walls, rails and tunnel walls are shear pieces;
a descending one sat up to 7.5 m too low as a facade); static + building → `mmBuildingInstance`; other static →
`mmFacadeInstance`.

### Facades

`mmFacadeInstance::InitFacade` loads the groups `FACADE` (else `BLDG`),
`LEFT`, `RIGHT`, `GRND`, `TOP` and `BACK` from `bms/<name>/`. The `FACADE`
mesh is a front in its local x/y plane, x from 0 to `width`, with the other
groups 30 m deep toward −z (`dfbldg04`). It is placed with
`MatrixFromPoints(start, end, width)`:

```
x = unit(end - start) * (|end - start| / width)
y = (0, 1, 0)
z = unit(end - start) cross y
origin = start, with y lowered to end.y if that is lower
```

The facade keeps less than that. mmFacadeInstance is an mmStaticInstance,
which is an mmYInstance, and `mmYInstance::FromMatrix` stores only the
origin, x's x and z, and `invsqrtf_fast(x.x² + x.z²)`. `ToMatrix` rebuilds
x = (x.x, 0, x.z), y = +Y, z = (−x.z, 0, x.x) times that inverse. The slope
of end − start is lost (`city::mm1FacadeMatrix`). This matters for the
three `tfskyway` walkways: each has end 7.5 m below start, and its mesh
(x −24.5..24.5, y −6..−1) hangs level 9–14 m up. With the full matrix it
tilts until one end nearly reaches the street.

`sides.x` and `sides.y` go to the left and right side quads
(`mmFacadeQuad(mesh, sides.x | sides.y, sides.z)`). Inferred: they are the
heights of the neighbouring buildings the side walls are clipped against,
−start.y when there is none (`EnableFacadeSideClipping`). Supported by the
data, though mmFacadeQuad's code is not ported: at the L-shaped tunnel
downtown (−59, 290 to −20, 240), dfskys03's left side has 26, the height of
the tunnel building beside it (tunnel01's facade is 26 m tall). Drawn whole,
that side runs 26 m back from the street through the tunnel. OpenMM2 cuts
each side quad off below start.y + that value (Mm1CityRenderer), which
clears the tunnel and leaves the skyline unchanged.

The side quads are single quads (left.bms / right.bms, 4 positions, 0 up to
the building's height). Missing levels of detail: MM1's GetMeshSetSet fills
none (H, M, L and VL are loaded as named, and Draw draws exactly the level
asked for). Yet tunnel02, which has only L and VL, is the tunnel walls seen
up close, so the port still uses the next lower level for a missing one.

### Buildings

`mmBuildingInstance::Init(name, a, b, c)` loads `FACADE` and `GRND` and
uses the rows `b − a`, (0, 1, 0), `c − a` (unnormalised) with origin `a`.

## Textures

Readers: `asset::decodeDds` (`src/asset/Image.cpp`) and `asset::Mm1TexSheet`
(`src/asset/Mm1TexSheet.cpp`). Tools: `mm2tool mm1tex`, `mm2tool mm1texcheck`.

MM1 keeps every texture twice. The hardware renderer searches `tex16a/`
(ARGB4444, 156 files), then `tex16o/` (RGB565, 1,668), then `tex16/`
(Open1560 `glpipe.cpp`). `texp/` (1,825 files) holds 8-bit palette indices
for the software renderer, with the palettes in separate `texp/nbr*.lut`
files; OpenMM2 does not use it. All are uncompressed DDS with a full mip
chain, stored top row first. Verified: all 1,824 `tex16*` files decode, and
the digit texture `G_3` comes out upright.

`mtl/global.tsh` is the texture sheet: a CSV with a header line and the
columns `name, neighborhood, h, m, l, flags, alternate, sibling, xres, yres,
hexdaycolor, hexnightcolor` (726 entries). The flag letters are
`agiTexProp`'s: `w` snowable, `g` alpha glow, `l` lightmap, `s` shadow,
`t` transparent, `k` chroma key, `n` not lit, `d` dull or damaged, `u` `v`
`c` `U` `V` clamping, `e` road/floor/ceiling, `m` always modulate, `p`
always perspective-correct.

A mesh's texture name becomes a file name as `GetPackedTexture` does:

1. Weather: in rain `<NAME>_FALL`, in snow `<NAME>_WIN`, when the sheet
   has that entry (`mmCullCity::Init` sets `TextureSuffix`). Eleven
   textures have both, the roads and grass among them; rain turns the
   grass to autumn leaves. Note that `_WIN` elsewhere usually means
   "window" (`07_WIN`).
2. Variation: `agiTexSheet::Lookup` follows the `sibling` chain as many
   steps as the mesh's variation number, stopping at its end
   (`G_1` → `G_2` → `G_3`). `GetVariationCount` is the chain's length.
3. Night: at sunset and night (`TEXSHEET.SetUseAlternate`) `RemapName`
   swaps in the `alternate` column, when there is one. 563 entries have
   one, all named `<NAME>_N`: the night textures, with lit windows.

All 726 names and 563 alternates have files.

## In the game

Point the game at an MM1 disc image, disc copy, installation or `CORE.AR`
with `[GameData] MM1Source=` in `openmm2.ini`, or the `OPENMM2_MM1_SOURCE`
environment variable (`vfs::mountMm1Source` mounts `CORE.AR` under `mm1/`,
below everything else). Then `openmm2 --quickstart chicago` drives into it.

MM1's cities appear in the menus' city lists (`city::listCities`: every
`mm1/city/<map>.cells`, named after the map, with `CityInfo::mm1` set and no
races yet, so only cruise is offered). The race menu shows MM1's own map of
the city, `mm1/bmp16/<map>640.jpg` from `UI.AR` (mounted beside `CORE.AR`);
MM1 stores those pictures upside down, so the UI flips `mm1/` images.

`city::loadCity` takes the MM1 route when a city has no `city/<map>.psdl` but
has `mm1/city/<map>.cells` (`CityData::mm1`; the PSDL keeps only bounds and
sphere, from `.ext`). `CityRenderer` hands its drawing to
`game::Mm1CityRenderer`, which draws the cells and facades through the same
mesh and texture paths as MM2's city, so the fork's graphics apply. MM1
textures go through `TextureLibrary` as `mm1:<name>`, resolved by the texture
sheet, uploaded unflipped (verified in game: upright, and the night
alternates light the windows).

Collision (`game::Mm1Level`, through `CityLevel`): rooms are cells, found
from the hit-id footprints; each cell collides with its own bound; neighbours
come from the portals. MM1's bounds hold no building fronts (only ground,
kerbs and barriers, nearly all under 10 m), so the walls are generated:
every footprint edge with no footprint 0.3 m beyond it, at about its height,
becomes a wall 40 m up (inferred, until MM1's own 2D edge test is ported).
Verified in game: the car drives the streets and stops against the
buildings. MM1's physics materials map to MM2's by name.

Polygons are collected from a 16 m grid over every cell, not through the
portals: MM1's polygons are listed under one cell but cross cell borders.

A wall is left out where MM1's bounds have ground beyond the edge, from 0.5 m
below it to 6 m above (buildings stand on no ground polygons), or where a
drawbridge leaf's deck lies there. Without this, the river's footprints
(5 m down) walled off the roads at both ends of every drawbridge.

Each edge is tested in pieces of at most 2 m, and only the runs of pieces
with nothing beyond them become walls (inferred). One footprint edge can be
long: the road along Soldier Field's west side is one 355 m edge (x 378.3,
z 650..1006) with the stadium beyond its middle but lawns beyond both ends.
Tested at its midpoint alone, it walled off the lawns too.

Holes in the bounds: a few leave out a polygon under ground the cell meshes
draw. For example, cell 999's bound skips one sidewalk quad (x −77..−67, z
511..516), and nothing collides under the lawn east of cell 40's (−60, 470),
which only cell 40's A2 mesh draws. A car fell through the first; the
second was walled off. OpenMM2 makes every upward, non-water triangle of
a cell's H, A, H2 and A2 meshes into ground when it has no ground under
its middle but ground at its own height (±0.3 m) under a corner. That
filled 563 triangles; roofs, with no ground beside them, stay open.

Where the footprints find no cell (such a filled hole), the room is the
cell of the ground under the position, so a car there keeps simulating.
All of this is inferred; whether MM1's own collision covers these spots
is unknown.

Drawbridges (`city/<city>.gizmo`, `mmBridgeMgr::Init` and
`mmBridgeSet::ReadEntry`): blocks named on a line of their own and closed
by the same name, each line `<tab><model>,<ignored>,x0,y0,z0,x1,y1,z1`.
The leaves (`tpdrawbridge04`/`06`, 40 m wide, reaching 32.5 m toward -Z)
hinge at the first point about the axis toward the second; Chicago's nine
bridges have a leaf on each bank of a 65 m gap. Inferred from the data
(`mmAnimDOF::Init` is unported): x along the hinge, y up, z = x cross y.
The leaves draw with their `RAIL` group and collide with their own bound
(`bnd/<model>_bnd.bnd`, in the leaf's frame).

### Opening and closing

After `mmBridgeMgr`, `mmBridgeSet` and `mmAnimDOF` (game.asm), in
`game::world::Mm1Bridges`. Each set is the gizmo block's six parts: a leaf,
two crossing gates, the other leaf, two gates.

- **Part frame** (`mmAnimDOF::Init`): X = unit(p1 - p0) on the ground, Y up,
  Z = X x Y, at p0 plus the `PIVOT` point of `dlp/<model>.dlp` turned by it
  (the gates' hinge, 0.92 m up; the leaves have none). Meshes are modelled
  about the pivot. A `tpdrawbridge*` part turns about X, anything else (the
  `tpcrossgate*` gates) about Z, and a gate also places a `tpcrossbase` post
  (an unhit banger at p0 facing p1).
- **Part motion** (`mmAnimDOF::Update`, `SetSwitch`, `SetState`): state 0 off,
  1 turning on, 2 on, 3 turning off. A turning part waits its delay, then
  moves its progress t by Delta per second, clamped to 0..1, ending on or
  off. The angle is `off + (on - off) * (cos((t + 1) pi) + 1) / 2`.
- **Set cycle** (`mmBridgeSet::Update`): down (0) counts DownInterval, up (1)
  counts UpInterval; once one is exceeded every part switches the other way.
  The set is 2 while any part turns, else the switch's value. A triggered
  set (3) waits for the player within sqrt(TriggerDist2) of TriggerPos
  (`mmAnimTrigger::CheckObjectDistance`, the player is the only tracked
  object) or TriggerTimeout, then switches up.
- **Tuning** (`mmBridgeMgr::Init`): the manager is named "roam", "race<N>",
  "circuit<N>", "blitz<N>" or "copsnrobbers" ("m" appended in a network
  game) and loads `tune/<name>.mmbridgemgr` (Chicago has default, roam,
  race6 and multi; `drawbridgeN.mmbridgeset` files are not read, inferred).
  Before the file: network games set every set up for good (InitialPos 1,
  UpInterval infinite), cruise too (roam's file then cycles 10 s up, 20 s
  down, starting up); races trigger each set at the midpoint of its two
  leaves' hinges within four times their distance and open to 0.1 pi.
  Defaults: BridgeDelta 0.2, BridgeOnGoal 0.15 pi, BridgeOnDelay 0.9,
  GateDelta 0.6, GateOffGoal -pi/2 (upright), GateOnGoal 0 (across the
  road), GateOffDelay 4.3; sets InitialPos 0, DownInterval 10, UpInterval 1.

The leaves and gates are MM2 `GizmoBody`s listed in every cell they cover,
an instance source of the level. Verified in game: the gates close, the
leaves rise, and the car drives up a raised leaf. The sets sound with
MM2's "drawbridge" object (`BridgeAudio`, active while a leaf turns, between
the two hinges), whose samples are MM1's own `bridgemove` and `bridgebell`
(MM2's data kept them; MM1's mmBridgeAudMgr's mixing is not ported). Not ported: the gates
breaking.

Traffic (`aiPath::HasBridge`): `mmBridgeSet::Init` flags the paths its
leaf's portal cell lists, and MM1's ambient traffic (`aiMap` link choice,
`aiGoalRandomDrive`) and pedestrians never take a flagged path, whatever the
bridge's state. OpenMM2 flags the paths whose centre line passes over a
leaf's deck (`city::markMm1BridgePaths`, inferred: the portal web's lists are
unported), 18 in Chicago (one road per bridge), and closes both sides of
those roads to ambient traffic and pedestrians (MM2's road types 1 and 2).
`mm2tool mm1cityinfo` lists them.

`dlp/*.dlp` (DLP7, big endian; `asset::parseDlp`): u32 group, patch and
vertex counts; groups (u8 name length, name, u32 vertex and patch counts,
u16 indices); patches (u16 SRes, TRes, flags, ROpts, material, texture,
physics; SRes x TRes vertices of u16 id, normal, uv, u32 colour; a name as a
u32 length and bytes, inferred from Chicago's files); then the vertices.

Not yet: MM1's racer physics (Realism 0) and grid, portal culling, MM1's exact levels of
detail and the facades' side clipping.

## Cops and Robbers

MM1's `race/<city>/copswaypoints.csv` ("Cops and Robbers waypoint set for
c39", then `x,y,z,...` rows; 19 places in Chicago) is MM2's
`multicopwaypoints.csv` under another name, the pool the bank, gold and
hideout are drawn from (`game::session::loadCrLocationsIn` tries both in the
city's race folder). Verified: a Chicago Cops and Robbers game starts with
the gold on MM1's places, and two machines play a Chicago cruise together
(the props' catalogues match, the host's traffic is shared). In network
games the drawbridges stay up (MM1's rule; see the drawbridges).

## Animated objects (`city/<city>/anim*.csv`)

`anim.csv` lists files by name; each is one `x,y,z` point per line
(`mmAnimSpline::LoadFile`). `mmAnimSpline::Init` starts an object by the
file's name: `anim_eltrain.csv` the El train, `anim_plane.csv` the
airliner, `anim_boat.csv` a boat (`InitBoat` returns 0 in the release: no
boats). `anim_sailboat.csv` (a path at 105 m) matches no name and is unused.
Ported in `game::world::Mm1Anim`, drawn as moving models by the MM1
renderer:

- **El train** (`mmAnimTrain`, `mmAnimTrainCar`): six `r_L_train` cars on a
  closed loop. Segment i is a Hermite curve (`TrainHermite`, the basis of
  the traffic rails) from point i to i + 1, its tangents unit(p[i+1] −
  p[i−1]) and unit(p[i+2] − p[i]) times the segment's length, in x, y and z.
  The speed is the loop's length / 65 (`SetPathSpeed`): Chicago's 2.4 km
  loop at 37 m/s. A car's progress t runs at speed × dt / segment length and
  carries over into the next non-empty segment. The cars start one length
  apart behind the first point (`GetStart`, `SetStart`): a car is its DLP's
  H box along z + 1 m (10 m without one), spaced by that less 1 m. Each
  faces along the curve's derivative: z = −unit(dir), x = Y × z, y = z × x.
  It sounds with MM1's `thel` at the fourth car (heard within
  sqrt(200000) m, `s_fAudioMaxDistance`).
- **Airliner** (`mmAnimPlane`): `vaboeing` on a uniform cubic B-spline
  (`CatmullBasis`, despite the name: −1/6 1/2 −1/2 1/6 …) through the
  points, segment i from p[i−1] (p[n−2] for the first), p[i], p[i+1],
  p[(i+2) mod (n−1)]; t clamped to 0..1. It laps in 60 s (Chicago's 8.7 km
  circuit at 145 m/s), starts on Meigs Field's runway, pitches along the
  curve's derivative and flies 10 m above it (`PlaneFudge`). It sounds with
  MM1's `airplane` (heard within sqrt(2000000) m).

MM2's data still has these MM1 samples (`aud/aud22/amb3d/thel.22k.wav`,
`airplane`, `bridgemove`, `bridgebell`); OpenMM2 plays them through MM2's
ambient sound objects (`AmbientObject::loadDefinition`, a definition in the
`aud/ambient` CSV layout). MM1's own `audio.ar` holds the same files.

Not ported: the UFO and airliner swaps (`mmAnimSpline::UFOSwap`), and
collisions (MM1's animated objects are not hit by the cars).

## Lighting and sky

`tune/lighting.csv` (one table for every MM1 city): a header line, then a
row per time of day (0 morning, 1 noon, 2 sunset, 3 night) and weather (0
clear, 1 fog, 2 rain, 3 snow) with the sun's and two fill lights' heading,
pitch and colour (0..1), the ambient (0..1), the fog's end and colour
(0..255), and the shadow's alpha and colour. `city::parseMm1Lighting` reads
it and `loadCity` makes MM2's 16 lighting and fog tables of it (MM2's cloudy
takes MM1's clear). MM2 folds snow into rain; an MM1 city keeps its snow rows
apart (`CityData::snowLighting` / `snowFog`), which `makeEnvironment` uses in
snow along with the `sky_<t>s` sky and the sheet's `_WIN` textures
(`TextureLibrary::setSnow`). MM1's headings and pitches
point toward the light, the pitch up from the horizon (the sun rises at
heading 0, toward +x and the lake, and sets at pi); MM2's are the way the
light travels, so the heading turns by pi and the pitch changes sign.
Verified in game: morning light on the facades facing east, sunset on those
facing west. MM1's fog starts at the camera.

The sky (`mmSky`, Open1560 `mmEnvSetup`) is `bms/mmsky.bms`, a dome drawn
around the camera, with the texture `sky_<time><weather>`: times `m`, `n`
(noon), `s`, `d` (dark), weathers `c`, `f`, `r`, `s`. `mmEnvSetup` also
names a reflection map (`refl_*`) and a cloud shadow map (`shadmap_*`, the
same names as MM2's) per look, and a fog end and sky colour that are not
used yet.

## Races

MM1's races use MM2's files, which grew from them. `UI.AR` holds
`tune/<city>.cinfo` in MM2's format (Chicago: 10 blitzes, 10 circuits, 12
checkpoint races, with their names), so `city::listCities` reads it for an
MM1 city; the race files are found under `mm1/race/<dir>/`
(`city::raceFolder`). The tables (`mm{blitz,circuit,race}data.csv`) have
MM2's columns. The waypoint files have no header line and heading and radius
0 in every row, which MM2 already reads as 15 m and as facing the next
waypoint (MM1 itself counts a checkpoint when the car enters its cell,
`mmWaypoints::GenerateHitRooms`). The races' `.aimap` opponents and their
`opp<n><mode><race>.c<n>` lines have MM2's columns too. The race menu shows
MM1's race cards, `bmp16/<city>{bl,ch,ci}<n>.jpg` (numbered from 1).

Verified in game: a blitz runs with MM1's checkpoints and time limit, and
checkpoint races and circuits start with MM1's six opponents, who follow
their lines. MM1's lines are sparse (16 points hundreds of metres apart),
but MM2's racers route between waypoint intersections over the road network
anyway (ai::Course). An [Opponent]'s one number is MM1's MaxThrottle
(0.30-0.95 across Chicago's races; 71 lines have none), relative to MM1's
racers, which drive with Realism 0 (aiGoalFollowWayPts; unported): as MM2's
MaxThrottle the low ones crawled at 16 mph. `city::adaptMm1AiMapConfig`
maps MM1's range linearly onto MM2's racers' (inferred: 0.30 -> 0.85,
1.0 -> 1.0, every car keeping its place in the order); the racers then run
at 50-100 mph. A few of MM1's lines end in an eight-column row (the
heading is missing); `parseOpponentPath` takes rows with just a position,
which otherwise lost circuits 1 and 9 all their racers. The race
.aimaps' [Speed Limit] is converted from miles per hour as the cruise's.
The lines' first rows store start headings in no convention of MM2's (none
of eight candidates fits Chicago's 223 lines; most are 0, MM1's "unset",
which MM2 reads as facing -z), so an MM1 racer faces along the road it
stands on (the nearest lane, within 12 m and 4 m in height), the way of the
two towards the race's first checkpoint; the course's own first stretch
runs straight to its first waypoint and can cut across a bend or lead back.
Not the player's way: the waypoint files' start rows store real headings in
MM2's convention but for "Soldier Sneaker" (race 3), whose 0 MM2 keeps (it
turns zero headings from the second waypoint on), so the player starts
facing backwards there; by players' accounts MM1 does the same, with the
racers facing the race. Verified: in all 22 MM1 checkpoint races and
circuits the racers face their first checkpoint as the player does (within
about 20 degrees, more where it lies round a corner), and in Soldier Sneaker
they face it while the player faces away. MM2 maps each line row to the
intersection of its room; MM1's rows are points along roads, whose rooms
hold none, and the nearest intersection could lie behind: a row on a road
now takes the end of its road nearer the next row
(`Opponent::routeFromPath`; MM2 itself stops the game there).

## Props

`game::loadMm1Props` turns every `.bng` record into an MM2 banger
(`bangers::BangerSet`): Chicago's 5,941 props from 80 models, every one with
data and meshes. MM1's `tune/<model>.mmbangerdata` is the format MM2's
`dgBangerData` grew from: the same fields, plus a `NodeName`, an `Offset`, a
single `GlowOffset`, a `PartNames` list, and object headers with a class
name and address (`mmBangerData :05e1f360 {`, `BirthRule asBirthRule
:05e1f3ec {`). `bangers::parseMm1BangerData` strips those and reads the rest
with MM2's parser. MM1's props, like MM2's, are modelled around their centre
of gravity (the street light's mesh spans +-2.94 by +-6.32 about its CG), so
the meshes (`bms/<model>/{h,m,l,vl}.bms`) become models as they are. The
breakable parts (`PartNames`: meshes `bms/<model>/<part>_<lod>.bms`, data
`tune/<model>_<part>.mmbangerdata`) become MM2's `BREAKnn` groups and
`<model>_breakNN` entries. A record's matrix is
`mmUnhitBangerInstance::Init`'s (Open1560): X horizontally toward the
`ahead` point, Y up, Z = (-x.z, 0, x.x), only the yaw kept. (Not "faces
-z": that was a 90 degree error that symmetrical props, meters and trees,
hid; the Chinatown gate, phone boxes and others showed it.) Flag 0x40 marks
a prop with a glow mesh (the street lights).

Verified in game: the props stand along the streets and are knocked loose
by a car at speed (a parking meter at 40 mph). MM2's rule holds them like
walls until the impulse that would stop the car exceeds `ImpulseLimit2`:
a meter's 844,501 takes about 2 mph of a 1,000 kg car, a street light's
9.6e7 about 22 mph.

MM1's mmBangerData has no CollisionPrim: a prop collides with its own bound,
`bnd/<model>_bnd.bnd` (binary 2DNB, about the CG like the meshes; the first
polygon is a degenerate one), and a box of Size only without one (the break
parts). Its CollisionType is not MM2's either (mmBangerActiveManager::Update):
1 updated by the active, 2 glass, 4 props (a temporary mover colliding with
the terrain and the movers), 6 walls (a permanent mover);
`parseMm1BangerData` maps them to MM2's 0x2, 0x2, 0x10 and 0x40. Read as
MM2's, 4 was "the city only" and a Size box spanned the traffic lights' and
street lights' arms down to the ground. `mm2tool mm1bnd` prints a bound.

The traffic lights are not in the `.bng`: they come with
the road network (below).

## Road network (`city/<city>/<city>.bai`)

Reader: `city::parseMm1Bai`; conversion to MM2's network: `city::mm1ToAiMap`
(`src/city/Mm1AiMap.cpp`). After Open1560's `aiMap`, `aiPath` and
`aiIntersection::ReadBinary` (game.asm) and its class layouts. Verified:
Chicago's file (900 paths, 242 intersections, 1,752 cells) parses to its last
byte, and the converted network passes `validateAiMap`.

```
u16 intersectionCount, pathCount
path[pathCount]:
    i16 id, vertexCount, numLanes, numSidewalks, stopLightIndex
    i16 intersectionType              the sink's control: 1 a light, 0 a stop sign, 3 none
    i16 alwaysBlocked, pedBlocked, divided, flat, bridge, alley
    f32 length, speedLimit (m/s: 15, or 28 on Lake Shore Drive)
    char stopLight[32]                tplttrafc, tplttrafcdual, tpsstop (a stop sign);
                                      uncontrolled ends keep tplttrafc
    i32 oncomingPath, edgeIndex, pathIndex
    f32 subSectionOffsets[rows * vertexCount], centerOffsets[vertexCount]
    i32 sink, source                  intersections at the last and first vertices
    Vec3 laneVertices[rows][vertexCount]    rows = numLanes + numSidewalks, lanes first
    Vec3 center[vc], xAxis[vc], normal[vc], zAxis[vc], subSectionDirs[vc]   subSectionDirs: unit tangents
    Vec3 boundaries[2][vc]            the right side's outer edge, then its curb
    Vec3 leftBoundary[vc]             the far side's curb
    Vec3 stopLightPos[2]              a light's pole and a point along its arm (0 without one)
    f32 laneWidths[5], laneLengths[10]
intersection[intersectionCount]:
    i16 id; Vec3 position
    i16 numSinks; i32 sinkPaths[]; i16 numSources; i32 sourcePaths[]
    i32 paths[numSinks + numSources]; f32 directions[...]   sorted by direction
u32 cellCount; then two lists per cell: u32 n, i32 pathIds[n]
```

MM1's paths run one way, in pairs (`oncomingPath`; all 900 of Chicago's
pair up). Rows run from the source to the sink; their first and last points
sit at the intersections' centres. The conversion makes an MM2 path of each
pair: the first path's lanes are its right side, its partner's (reversed
into the first's order) its left; the collapsed end points are dropped, so
lanes end at the intersection's edge as MM2's do; the far curb is
`leftBoundary`, on the first path's own sections. Entry rules come from
`intersectionType` (not the name: Chicago's uncontrolled ends are named
tplttrafc, which made 791 lights of its 353); divided and alley flags carry over; MM1's 28 m/s roads
are marked freeways (inferred). Each intersection's room is its cell
(`city::Mm1CellLocator`, the footprints).

### Traffic lights

MM1's `aiTrafficLightSet` ctor gives each path of type 1 at an intersection a
light (`aiTrafficLightInstance::Init(stopLight, stopLightPos[0],
stopLightPos[1])`), an unhit banger of that model: X along `pos[1] - pos[0]`
on the ground, Y up, at `pos[0] + R * CG`. That is MM2's own placement, so
the conversion puts the points in MM2's `trafficLightPos`/`trafficLightAxis`
and the model in `AiPathEnd::trafficLightModel` (an OpenMM2 field), and
MM2's light cycle runs them. MM1's `Draw` adds one of six glow meshes,
`GLOW{RED,YELLOW,GREEN}{DAY,NIGHT}` (`bms/<model>/glow*.bms`), modelled
about the CG like the body, so drawn at the instance's matrix; the night
glows only at night (time of day 3; MM2's from the evening), and there is
no pedestrian signal. FXLTGLOW is white with the falloff in its alpha, so the
glows are added by alpha (SRCALPHA/ONE); ONE/ONE drew flat discs. `game::Mm1ModelLoader` loads them, `ai::Signal::mm1`
marks them. Verified in game: the poles stand on the corners with their arms
over the road, and the glows change through the cycle.

MM1's cars do not run straight between a lane's vertices:
`aiRailSet::CalcRailPosition` follows a cubic Hermite curve in XZ
(`CalcXZPosition`, the basis 2 −2 1 1 / −3 3 −2 −1 / 0 0 1 0 / 1 0 0 0 set
up in `aiRailSet`'s constructor) from vertex i − 1 to i, its tangents the
path's SubSectionDirs at both vertices times the segment's length
(`SubSectionDir`, `SubSectionLength`). MM2's traffic runs straight between
points, so the conversion adds points along those curves to every row of a
path (lanes, sidewalk, curbs, centre, frames interpolated) at most 5 m apart
(`city::densifyMm1Path`; the spacing is OpenMM2's), the partner path split
the same way mirrored; the segments to the intersections' centres, which
the conversion drops, are left alone. Some of Chicago's paths have vertices
140 m apart.

`mm2tool mm1lanecheck` reports traffic lanes over grass, dirt or water: the
lanes that cross Meigs Field's grass and apron and the park drives are all
on MM1 roads flagged always blocked (no ambient traffic in MM1 either).

MM2's traffic, pedestrians and police run on it in game
(`ai::World::setRoomFinder` gives the AI the cells). MM1's
`race/<city>/roam.aimap` is MM2's format: its police posts (`vpcop x y z
heading 0`) lack MM2's optional behaviour numbers, which keep their
defaults; verified in game, a cop leaves its post with its siren on and
gives chase. Its `[Speed Limit]` of 30 is taken to be miles per hour
(inferred).

## Collision files

`bnd/<city>city/bound<N>.bnd` (each cell's; landmark cells in `<city>lm/`,
Chicago's cells 60, 24 and 174 in `dl60_bnd/`, `dl24_bnd/`) and
`bnd/<city>_hitid.bnd` (the footprints, a polygon set per cell) are
`mmBoundTemplate` binaries (`asset::parseMm1Bound`):

```
char[4] "2DNB"
Vec3    offset (zero in Chicago)
u32     xDim, yDim, zDim            the lookup grid (5 x 1 x 5 .. 300 x 1 x 300)
Vec3    centre; f32 radius, radiusSqr; Vec3 boxMin, boxMax
u32     vertexCount, polygonCount, hotVerts1, hotVerts2, edgeCount (0 in Chicago)
f32     xScale, zScale; u32 ?; f32 heightScale; u32 ?    grid table sizes
Vec3    vertices[vertexCount]       at 0x70, world space
polygon[polygonCount]               mmPolygon, 0x4C bytes:
    u16 cell; u8 material (1-based in physics.db, 0 none); u8 flags (0x4 quad)
    i16 vertices[4]; Vec3 edgePlanes[4]; Vec3 normal; f32 d   (normal . p + d = 0)
...                                 the grid tables (not read)
```

Verified: all 976 of Chicago's bound files parse; the 30,035 polygons' indices
are in range, their normals unit, and all but two lie on their planes.
Most wind counter-clockwise about their normals, as MM2's collector expects;
the rest are turned round on load.

`mtl/physics.db` is big-endian: a u32 count (98), then 88-byte records, a
32-byte name and 14 floats (inferred: friction, elasticity, ...). The names
confirm the 1-based indexing: cell 1000's polygons use 73, 74 and 75,
`RINTER`, `RWALK` and `SDWLK`, the textures of its mesh.

## Verification

`mm2tool mm1map <core.ar> "<cell globs>" out.png --fcd city/chicago.fcd`
draws the cells and every facade from above. The cells fill exactly the
`.ext` rectangle, and the facades line the blocks with their roofs
inward. 3,382 of 3,389 facade records find meshes; the other 7
(`cpbanr_red`, `cpbanr_blu`) have no folder in `CORE.AR`.
