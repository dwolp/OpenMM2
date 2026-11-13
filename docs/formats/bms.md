# MM1 meshes (`bms/<object>/<group>_<lod>.bms`)

Parser: `src/asset/Bms.{h,cpp}`. Tools: `mm2tool bmsinfo <core.ar> <path>`,
`mm2tool bmscheck <core.ar> [glob]`, `mm2tool mm1map` (see `mm1city.md`).

Midtown Madness 1's mesh format (`agiMeshSet`). MM2 replaced it with PKG
(`pkg.md`) and never reads it. OpenMM2 reads it for the Chicago port and
converts each mesh to a `PkgMesh` (`bmsToPkgMesh`) so the MM2 renderer can
draw it.

Source: Open1560 `code/midtown/agiworld/meshload.cpp` (`agiMeshSet::BinaryLoad`),
`meshrend.cpp` (the quad split) and `packnorm.cpp` (the normal table); the
magic check is in `game.asm` before the `BinaryLoad` call. Verified: all
7,513 meshes in the retail MM1 `CORE.AR` parse with every index in range
(111,222 triangles).

## Layout (little-endian)

```
u32     magic "3HSM" (0x4D534833)
u8[12]  not used by a plain load; non-zero in 1,209 retail files (meaning unknown)
u32     vertexCount
u32     adjunctCount          per-corner records; at most 65,536 (u16 indices)
u32     surfaceCount
u32     indexCount            always surfaceCount * 4
f32     radius, radiusSqr, boxRadius
u8      textureCount
u8      flags                 0x01 uv, 0x02 normals, 0x04 colours, 0x08 offset, 0x10 planes
u16     padding
u32     cache size            recomputed by MM1
texture[textureCount]         agiTexParameters, 0x30 bytes:
    char  name[32]
    u8    flags               0x01 alpha, 0x02 wrap u, 0x04 wrap v, 0x08 keep loaded,
                              0x10 no mipmaps, 0x40 chroma key, 0x80 second
    u8    lod, maxLod, padding
    u32   props
    f32   ?                   Open1560: "Snowable ? 4.0f : 2.0f"
    u32   colour
Vec3    positions[vertexCount]
Vec3    boundingBox[8]        only when vertexCount >= 16
u8      normals[adjunctCount] flag 0x02: index into a fixed table of 198 unit vectors
Vec2    uvs[adjunctCount]     flag 0x01
u32     colours[adjunctCount] flag 0x04; taken to be D3DCOLOR (ARGB), inferred
u16     adjunctPosition[adjunctCount]
Vec4    planes[surfaceCount]  flag 0x10
u8      surfaceTexture[surfaceCount]   1-based; 0 = untextured
u16     surfaceCorners[surfaceCount][4]  adjunct indices; corner 3 == 0 marks a triangle
```

A quad is drawn as corners (1, 2, 3) and (1, 3, 0).

Flag combinations in the retail files: 0x03 (3,757 meshes), 0x13 (1,789),
0x1B (760), 0x17 (576), 0x1F (478), 0x07 (111), 0x1D (20), 0x11 (11),
0x0B (10), 0x01 (1). What the offset flag (0x08) changes is not yet known.

## Naming

`mmInstance::GetMeshSetSet` asks for a group's levels as `<GROUP>_H`, `_M`,
`_L` and `_VL`, with plain `H` .. `VL` as a fallback (some objects, such as
`freewaywall04`, have only those). Some objects have only the low levels
(`tunnel02`: `FACADE_L`, `FACADE_VL`). The exact fill rules for missing
levels are still to be ported.

City cells are named `bms/<city>city/cull<N>_<lod>.bms` (and
`bms/<city>lm/` for landmark cells; `asRenderWeb::LoadCells`). Their
vertices are in world coordinates.
