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

`ahead` is 1 m in front of `position` in every Chicago record.

Flags (`INST_INIT_FLAG_*`): 0x1 static, 0x2 shear, 0x4 building, 0x8 left,
0x10 right, 0x20 top, 0x40 glow, 0x200 upper, 0x400 back, 0x800 terrain.
`AddInstance` picks the class: not static → a banger (prop); static + shear →
`mmShearInstance`; static + building → `mmBuildingInstance`; other static →
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

`sides.x` and `sides.y` go to the left and right side quads
(`mmFacadeQuad(mesh, sides.x | sides.y, sides.z)`). Inferred: they are the
heights of the neighbouring buildings the side walls are clipped against,
−start.y when there is none (`EnableFacadeSideClipping`). Not yet confirmed.

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

Drawbridges (`city/<city>.gizmo`, `mmBridgeMgr::Init` and
`mmBridgeSet::ReadEntry`): blocks named on a line of their own and closed
by the same name, each line `<tab><model>,<ignored>,x0,y0,z0,x1,y1,z1`.
The leaves (`tpdrawbridge04`/`06`, 40 m wide, reaching 32.5 m toward -Z)
hinge at the first point about the axis toward the second; Chicago's nine
bridges have a leaf on each bank of a 65 m gap. Inferred from the data
(`mmAnimDOF::Init` is unported): x along the hinge, y up, z = x cross y.
The leaves draw with their `RAIL` group and collide with their own bound
(`bnd/<model>_bnd.bnd`, in the leaf's frame), listed with the bridge cells
under them. Verified in game: the car crosses at speed. They stay down:
opening and closing (mmBridgeSet's timer and triggers) is not ported.

Not yet: MM1's racer physics (Realism 0) and grid, the traffic lights' signal states, portal culling, MM1's exact levels of
detail, the facades' side clipping, shear instances (placed as facades; two
pieces show misplaced), and snow textures.

## Lighting and sky

`tune/lighting.csv` (one table for every MM1 city): a header line, then a
row per time of day (0 morning, 1 noon, 2 sunset, 3 night) and weather (0
clear, 1 fog, 2 rain, 3 snow) with the sun's and two fill lights' heading,
pitch and colour (0..1), the ambient (0..1), the fog's end and colour
(0..255), and the shadow's alpha and colour. `city::parseMm1Lighting` reads
it and `loadCity` makes MM2's 16 lighting and fog tables of it (MM2's cloudy
takes MM1's clear; MM2 folds snow into rain). MM1's headings and pitches
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
(0.30-0.40), relative to MM1's racers, which drive with Realism 0
(aiGoalFollowWayPts; unported): as MM2's MaxThrottle the cars crawled at
16 mph. `city::adaptMm1AiMapConfig` maps it to 0.75 + 0.625 x it (inferred;
0.94-1.0, keeping MM1's order); the racers then run at 50-100 mph. The race
.aimaps' [Speed Limit] is converted from miles per hour as the cruise's.

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
`<model>_breakNN` entries. A record faces its `ahead` point (Angel objects
face -z): verified, the street lights' arms reach over the road.

Verified in game: the props stand along the streets and are knocked loose
by a car at speed (a parking meter at 40 mph). MM2's rule holds them like
walls until the impulse that would stop the car exceeds `ImpulseLimit2`:
a meter's 844,501 takes about 2 mph of a 1,000 kg car, a street light's
9.6e7 about 22 mph. The traffic lights are props for now; they do not show
the AI's signal states.

## Road network (`city/<city>/<city>.bai`)

Reader: `city::parseMm1Bai`; conversion to MM2's network: `city::mm1ToAiMap`
(`src/city/Mm1AiMap.cpp`). After Open1560's `aiMap`, `aiPath` and
`aiIntersection::ReadBinary` (game.asm) and its class layouts. Verified:
Chicago's file (900 paths, 242 intersections, 1,752 cells) parses to its last
byte, and the converted network passes `validateAiMap`.

```
u16 intersectionCount, pathCount
path[pathCount]:
    i16 id, vertexCount, numLanes, numSidewalks, stopLightIndex, intersectionType
    i16 alwaysBlocked, pedBlocked, divided, flat, bridge, alley
    f32 length, speedLimit (m/s: 15, or 28 on Lake Shore Drive)
    char stopLight[32]                tplttrafc, tplttrafcdual, tpsstop (a stop sign)
    i32 oncomingPath, edgeIndex, pathIndex
    f32 subSectionOffsets[rows * vertexCount], centerOffsets[vertexCount]
    i32 sink, source                  intersections at the last and first vertices
    Vec3 laneVertices[rows][vertexCount]    rows = numLanes + numSidewalks, lanes first
    Vec3 center[vc], xAxis[vc], normal[vc], zAxis[vc], subSectionDirs[vc]
    Vec3 boundaries[2][vc]            the right side's outer edge, then its curb
    Vec3 leftBoundary[vc]             the far side's curb
    Vec3 stopLightPos[2]; f32 laneWidths[5], laneLengths[10]
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
`leftBoundary`, on the first path's own sections. Entry rules come from the
stop light model; divided and alley flags carry over; MM1's 28 m/s roads
are marked freeways (inferred). Each intersection's room is its cell
(`city::Mm1CellLocator`, the footprints).

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
