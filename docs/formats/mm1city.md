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

## Verification

`mm2tool mm1map <core.ar> "<cell globs>" out.png --fcd city/chicago.fcd`
draws the cells and every facade from above. The cells fill exactly the
`.ext` rectangle, and the facades line the blocks with their roofs
inward. 3,382 of 3,389 facade records find meshes; the other 7
(`cpbanr_red`, `cpbanr_blu`) have no folder in `CORE.AR`.
