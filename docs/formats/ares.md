# MM1 archives (`.AR`, "AngelRes")

Reader: `src/vfs/AresArchive.{h,cpp}`. Tool: `mm2tool ls <mm1 core.ar>`, `mm2tool cat`, `mm2tool extract`.

Midtown Madness 1 packs its data (`CORE.AR`, `AUDIO.AR`, `UI.AR`) in this
format. MM2 replaced it with DAVE (`dave.md`) and never reads it. OpenMM2
reads it only so MM1 content (Chicago) can be converted for the fork.
`mm2tool` picks the reader by the magic, since both formats use `.ar`.

Source: Open1560 `code/midtown/stream/vfsystem.{h,cpp}` (GPL-3.0,
`VirtualFileSystem`). Verified: the retail MM1 `CORE.AR` (13,872 files) lists
completely, with every node passing `ValidateNodes`' checks.

## Layout (little-endian)

```
char[4]  "ARES"
u32      nodeCount
u32      rootCount            nodes [0, rootCount) are the top directory
u32      namesSize
node[nodeCount]               12 bytes each
char     names[namesSize]     NUL-terminated strings, indexed by offset
```

Node:

```
u32  f0    file: data offset in the archive; directory: index of its first child
u32  f4    bits 0-22: file size / directory child count
           bits 23-31: offset of the extension string (0 = none)
u32  f8    bit 0: directory
           bits 1-13: name number
           bits 14-31: offset of the name string
```

A node's name is its name string with each `\1` replaced by the name number in
decimal, then `.` and the extension string if there is one. One string thus
names a numbered series (`CULL\1_H` → `CULL1000_H`). Names are stored in
uppercase, and each directory's children are sorted by expanded name
(`VirtualFileSystem::Lookup` binary-searches them). Files are stored
uncompressed. Sizes are 23 bits, so no file exceeds 8 MiB.
