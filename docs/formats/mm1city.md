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

`city::loadCity` takes the MM1 route when a city has no `city/<map>.psdl` but
has `mm1/city/<map>.cells` (`CityData::mm1`; the PSDL keeps only bounds and
sphere, from `.ext`). `CityRenderer` hands its drawing to
`game::Mm1CityRenderer`, which draws the cells and facades through the same
mesh and texture paths as MM2's city, so the fork's graphics apply. MM1
textures go through `TextureLibrary` as `mm1:<name>`, resolved by the texture
sheet, uploaded unflipped (verified in game: upright, and the night
alternates light the windows).

Not yet: collision (the car has no ground), traffic and AI (MM1's `.bai` is
another format), races (none listed), lighting and sky (Chicago has no
`.ltNN`/`.sky`; the defaults apply), portal culling, MM1's exact levels of
detail, the facades' side clipping, shear instances (placed as facades; two
pieces show misplaced), snow textures, and the frontend's city list.

## Verification

`mm2tool mm1map <core.ar> "<cell globs>" out.png --fcd city/chicago.fcd`
draws the cells and every facade from above. The cells fill exactly the
`.ext` rectangle, and the facades line the blocks with their roofs
inward. 3,382 of 3,389 facade records find meshes; the other 7
(`cpbanr_red`, `cpbanr_blu`) have no folder in `CORE.AR`.
