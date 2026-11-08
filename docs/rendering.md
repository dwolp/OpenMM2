# World rendering

How OpenMM2 draws the city, cars and sky (`src/game/CityRenderer`,
`VehicleRenderer`, `TextureLibrary`, `ModelLibrary`, `MeshDraw`), and the
evidence for each choice. "MM2 (`Function`)" means ported from the original's
code (build 3393); "verified" means checked against the retail data (usually
by rendering it and looking); "inferred" means a reasoned guess that should
be compared with the original game running. OpenMM2 keeps its own renderer
(Vulkan/OpenGL, any resolution); what is ported are the original's
decisions: levels of detail, distances, textures, states and order.

## Textures

| Topic | Behaviour | Evidence |
|---|---|---|
| Orientation | `.tex` rows are uploaded in file order (bottom row first) and the game's UVs are used unchanged; Targa, BMP and JPEG pictures are stored top row first, as MM2's loaders store them | verified: road markings, facades, car bodies and signs appear upright; MM2 (`gfxLoadTexImage`, `gfxLoadTargaImage`, `gfxLoadBmpImage`) |
| Lookup | `texture/<name>.tex`, then `.tga`, then `.bmp` (a file that fails to load falls through), then `jpg/<name>.jpg` | MM2 (`gfxLoadImageAll`) |
| Wrap | the `.tex` flag word is the texture environment: 0x1 clamps U, 0x10000 clamps V, everything else repeats (car paint 0x10001 clamps both ways, skies and fences 0x10000 clamp V, the road textures 0x18006 clamp V, facades 0x00002 repeat). Targa textures repeat. Street geometry uses its textures' modes like everything else | MM2 (`gfxRenderState::DoFlush`; 0x2, 0x4 and 0x8000 are not read by the renderer) |
| Mipmaps | a `.tex` brings its own levels, as many as the file has (a partial chain stays partial, one level means no mipmaps); a square Targa gets a full chain averaged 2 × 2, a non-square one none; textures asked for without mipmaps (rain particles) get the top level only | MM2 (`gfxGetTexture`, `gfxLoadTexImage`, `gfxLoadTargaImage`, `gfxImage::GenerateMipmaps`) |
| Animated textures | a texture that does not exist but has frames `texture/<name>-0001.tex`, `-0002`, ... (at most 128) plays them at `tune/<name>.movie`'s `rate N` frames per second (30 without the file); PSDL names ending in `-0NNN` are looked up by their base name | MM2 (`gfxGetTextureMovie`, `gfxTextureMovie::Update`, `lvlSDL::LoadBinary`); verified for s_ocean, s_thames, s_water, s_pond |
| Texture Quality | while the city loads, textures (the streets', the city objects' and their xrefs', the sky's) are limited to 32 << quality pixels (32, 64, 128, 256; default quality 2): their top mip levels are dropped, or a single level halved keeping every other texel of every other row, until both sides fit; cars, pedestrians, traffic, effects and the HUD load without a limit. OpenMM2 loads on first use, so CityRenderer names the city's textures while the limit is set and they keep it (also across variant reloads) | MM2 (`cityLevel::Load` sets `gfxTexReduceSize` and restores it after `LoadPathSet`; `gfxDefaultPrepareImage`, `gfxImage::Halve`). Props placed from pathsets are not covered (inferred: MM2 loads them in that window) |
| Material colours | float-colour PKG materials are snapped as they load (by the PKG reader): below 0.05 to 0, above 0.95 to 1, otherwise down to a 32nd; byte-colour ones are used as stored | MM2 (`modShader::Load`) |
| Variants | in game every texture load goes through the variant handler: in rain `<name>_fa` when it exists (82 wet roads and decals), at night (time 3 only) `<name>_ni` when it exists (lit windows, shop fronts, lamps); at night every texture that is not a `_ni` one is darkened, each channel halved on every mip level (cars, sky and `_fa` textures included) | MM2 (`InstallTextureVariantHandler`: the `gfxLoadImage`/`gfxPrepareImage` wrappers) |
| Untextured materials | at night the diffuse colour of materials that name no texture is halved | MM2 (`modShader::Load`) |
| Alpha | a material blends (and so alpha tests) when its diffuse alpha is not 1 or its texture's format has alpha (PA8, P8A8, PA4, ARGB1555, RGBA8888, 32-bit Targa), whatever the texels hold; street textures with alpha are drawn after all the opaque street geometry. The object passes test GREATER 100; shadows, glows, particles and effects run with the default test (alpha not 0) | MM2 (`modStatic::Draw`, `gfxTexture::Create`, `vglEndBatch`, `cityLevel::DrawRooms` render states; ALPHABLENDENABLE and ALPHATESTENABLE share a byte) |

## Environment

| Topic | Behaviour | Evidence |
|---|---|---|
| Lighting tables | `city/<map>.ltNN` (time × 4 + weather): key, fill1 and fill2 lights (heading, pitch, colour) and an ambient colour, read over the table's previous contents: the 16 tables live for the session, a missing file or field keeps what the table held (the constructor's: key along x 30° down, white; fills at ±120°, 22.5° down, 0.75 and 0.5 grey; ambient 0xFF101010, for the first city) | MM2 (`cityTimeWeatherLighting`, `LoadCityTimeWeatherLighting`, `datParser::Load`); `city::loadCity` keeps the tables |
| Light direction | the direction a light travels: (cos h cos p, sin p, sin h cos p); vertices take N · −L | MM2 (`cityLevel::SetLightDirection`; the old inferred convention was 90° off) |
| Light quality | the Lighting Quality option (an integer 0–3, default 3; `mmGame::SetLevelGraphics`: sm_LightQuality = trunc(gxLightQuality)) turns on the key light from quality 1, fill1 from 2, fill2 from 3; the ambient is the file's at quality 3, white at 0, and at 2 / 1 33% / 66% of the way to white from the ambient the table held before the file loaded: the 16 tables live for the whole session and the reduced levels are computed just before each .ltNN loads, so the first city of a session uses the constructor's 0xFF101010 and later ones the previously loaded city's | MM2 (`cityLevel::SetupLighting`, `LoadCityTimeWeatherLighting`, `ComputeAmbientLightLevels`); `city::loadCity` keeps the history |
| Instance lighting | Direct3D 7-style per-vertex lighting with those lights: the material's emissive colour plus (ambient + Σ lights) × the material colour (its ambient is its diffuse), clamped per vertex. Normals go through the inverse transpose of the instance's matrix and are not renormalised (MM2 never turns on NORMALIZENORMALS), so a scaled building or prop is lit in proportion to 1 / its scale along each axis (inferred from Direct3D's rules) | MM2. MM2 also tints the directional lights of each room's instances by the room colour (`SetupPerRoomLighting`); every room is white in retail (below), so this has no effect |
| Street geometry | unlit: roads, sidewalks, roofs, fans and ground take the room colour, curb faces and curb caps half of it, facades and slivers the wall light table entry (ambient + Σ max(0, n · −L) × light colour for 64 horizontal directions) shaded by the room colour | MM2 (`sdlPage16::Draw`, `GetShadedColor`, `sdlCommon::UpdateLighting`): a facade or sliver takes the table index the room's last FacadeBound attribute stores (the one retail wall without one uses entry 0, as Draw starts each room with it) |
| Room colours | `city/<map>.lmap` holds one per room, but cityLevel::Load rejects it unless its count equals the PSDL room count including room 0 (London 1340 vs 1341, SF 1125 vs 1171), so in retail every room is white | MM2 (`cityLevel::Load`) |
| Fog | `city/<map>_fog.csv` row (time × 4 + weather): linear fog colour, start = min(far − 30, start), end = min(far, end); the clear colour is the fog colour | MM2 (`lvlSky::SetupFog`, `cityLevel::DrawRooms`) |
| Far plane | the Far Clip option (100–1000 m); OpenMM2's Visibility slider maps 0–1 to that range | MM2 (`PUGraphics::FixClip`); the slider mapping is inferred |
| Sky | `city/<map>.sky` "model yOffset yScale speed" (retail `sky_dome_l 0 0.95 0.005`): the dome at (camera x, camera y × yScale + yOffset, camera z), turning about Y at `speed` rad/s, unlit, unfogged, no depth; its 16 paint jobs are time of day × weather (texture letters: a dawn, n noon, d dusk, m midnight; c clear, p partly cloudy, f fog, r rain) | MM2 (`lvlSky::AutoInit`, `Update`, `DrawHat`); paint jobs verified by viewing the sky textures |
| Light flag | evening, night or fog: car lights on (see below) | MM2 (`mmGame::InitWeather`) |
| Cloud shadows | the Cloud Shadows option (0 none, 1 low, 2 high; default high) picks the .tex flag bit that receives them (vglCloudMapEnable 0, 4, 2: low shades textures with flag 0x4, such as the roads' 0x18006, high those with 0x2, the roads and the facades' 0x00002). The map is `shadmap_day` in the morning and at noon, `shadmap_nite` in the evening and at night, made black with its alpha inverted (vglSetCloudMap). Each opaque street texture with the bit is drawn again with it at ((x + y), (y + z)) / 128 in world space, and each city object's packets with the bit and no alpha format at those coordinates of their model-space positions, alpha blended and tested above 0, fogged | MM2 (`mmGame::SetLevelGraphics`, `cityLevel::Load`, `vglSetCloudMap`, `vglEndBatch`, `cityLevel::Update`'s `vglSetOffset`, `lvlFixedAny::Draw`, `modStatic::DrawOrthoMapped`, `gfxPacket::OrthoMap`). The .tex bits 0x2 and 0x4 are read only by this |
| Lamp glows | at night only (time 3) | MM2 (`mmGame::InitWeather` → `dgBangerManager::InitGlow`), see bangers.md |

## City

| Topic | Behaviour | Evidence |
|---|---|---|
| Camera room | the PSDL room under the camera; outside every room the last one found stays | MM2 (`cityLevel::Draw`, sm_LastPvsRoom; MM2 tests ordinary rooms in XZ only) |
| Visibility | the camera room, and the rooms of its CPVS row whose bounding sphere (the perimeter's area centroid at the corners' mean height, and its farthest corner) is in view; OpenMM2 tests every room's sphere before the camera has ever been inside the city or without a PVS | MM2 (`cityLevel::Draw`, `gfxViewport::IsSphereVisible`, `sdlPage16::GetCentroid`, `ComputeBoundSphere`); the fallback is an OpenMM2 debug convenience |
| Streets | `src/city/SdlDraw` builds what `sdlPage16::Draw` sends to vgl for each room and level of detail; every visible room's primitives are gathered per texture and drawn in one batch, opaque textures then those with alpha, with the render state's default culling (clockwise faces, which OpenMM2 draws as counter-clockwise front faces like the models) | MM2 (`sdlPage16::Draw`, `vglBeginBatch`/`vglEndBatch`, `cityLevel::DrawRooms`; the default cull mode from `rglOpenPipe` and `cityLevel::Load`) |
| Street levels of detail | d = the room sphere's view depth − its radius (the camera's room: − radius): level 0 beyond 300 m, 1 beyond 100 m, 2 beyond 50 m, 3 otherwise. Road and divided road strips: level 0 one strip from outer edge to outer edge with the group's third texture over every other section, level 1 the same over every section with the edges 0.15 m lower, levels 2 and 3 the sidewalks (second texture) and the road (first texture, in two halves mirrored about the middle); only level 3 raises the curb line 0.15 m and adds the half-bright curb faces, sidewalk-strip curb faces and end caps. Dividers are drawn at levels 2 and 3 | MM2 (`cityLevel::DrawRooms`: sm_SDLVLowThresh 300, sm_SDLLowThresh 100, sm_SDLMedThresh 50, never changed; `sdlPage16::Draw`) |
| Street texture coordinates | road, rectangle and divided strips: t across (1 at the curbs, 0 at the middle for the road halves), s from `sdlPage16::ArcMap`: the distance along the strip scaled to a whole number of repeats of its average width (at most 127 over the longest section) and run back and forth; sidewalk strips planar every 4 m, fans and roofs every 8 m (less the whole repeats at the first vertex); crosswalks 0–1 across and length / width along; facades the stored repeats read unsigned (v 0 at the bottom); slivers u = round(length × density), v = (height − top) × density | MM2 (`sdlPage16::Draw`, `ArcMap`) |
| Skipped primitives | road fans, crosswalks and roofs above the camera's height; facades and slivers seen from behind (`sdlCommon::BACKFACE`); after a Texture attribute of value 0 the road, sidewalk, rectangle, crosswalk, fan and divided road attributes | MM2 (`sdlPage16::Draw`); MM2's height is the camera's plus the view matrix's third row times the near distance (ignored) |
| Tunnels | junction walls, ceilings, aprons and railings along the room's masked perimeter edges; strip tunnels' walls (straight or bulging), railings, decks, flat or arched ceilings along the next strip; at every level (formats/psdl.md) | MM2 (`sdlPage16::Draw`, `WallMap`), read from the assembly where the decompile loses operands |
| City objects | `city/<map>.inst` instances, LOD by lvlInstance::IsVisible (below) with no distance limit beyond the far plane; a missing LOD takes the next less detailed one (VL → L → M → H) and a missing VL draws nothing; PKG xrefs drawn with their parent | MM2 (`lvlInstance::IsVisible`, `GetGeomSet`); the radius is the farthest vertex from the model origin over its levels of detail (`modGetStatic`) |
| Collidable objects | a .inst flag 0x2000 object (not terrain local) is drawn through stand-ins in the neighbours of its room that its sphere (position, model radius) reaches across the perimeter, once per frame, from the first such room drawn; never from its own room, and never when it reaches none | MM2 (`lvlLevel::LoadInstances`, `lvlMultiRoomInstance::Create`, `Draw`: the stand-ins' marker is the counter `cityLevel::DrawRooms` bumps) |
| Dynamic objects | cars, traffic, pedestrians, signals and props are drawn only from a room the view lists: the object itself when the room's distance (sphere depth − radius) is at most NoDraw and it passes IsVisible, its shadow and glows when the distance is under NoDraw whatever IsVisible says. Each keeps its room by FindRoomId from its last one (a prop: the room the banger set keeps); outside every room it is not drawn (`game::RoomVisibility`) | MM2 (`cityLevel::DrawRooms`: `cityLevel_drawObjects`, `_drawShadows`, `_drawLights`; `vehCar::Update`, `aiPedestrian::Update`, `aiVehicleActive::Update`, `aiTrafficLightSet::SetFourWay`) |
| Object LOD | d = view depth − radius: H up to Med, M up to Low, L up to VLow, VL beyond; dynamic objects (cars, bangers) are not drawn deeper than NoDraw. Object Detail 0–3: Med 20/30/40/70, Low 70/90/100/130, VLow 150/175/200/200, NoDraw 200/250/300/300 m | MM2 (`cityLevel::SetObjectDetail`) |

Not ported: `gfxTexture::sm_LOD` (3 − the street level), which only limits
the mip levels Direct3D's texture manager keeps resident (`MarkHigherUse`,
`SetLOD`; a texture's limit only ever drops to the most detailed level it has
been drawn at). Not ported yet: the room flood fill used without a PVS. No
retail city model has the `refl`, `mask`, `nonrandom` or `opaque` parts
lvlFixedAny::Init looks for (it draws `mask` with the model, `refl` in
DrawRooms' reflected-parts pass, and widens the radius over them), so they
are not drawn.

## Pedestrians

| Topic | Behaviour | Evidence |
|---|---|---|
| Near | within 35 m of the camera the posed model (`modModel::Draw`, default culling), lit and fogged | MM2 (`aiPedestrianInstance::Draw`, `pedAnimationInstance::Draw`) |
| Far | beyond 35 m a stick figure from `anim/<type>.rays`: for each bone with a start width, its position raised by the bone's offset (its children see the raised position), a quad to its parent with the start and end half widths across the first row of the modelview matrix taken in the pedestrian's own space (the pedestrian's X axis in view space, MM2's view matrix negating z: the camera's right axis while pedestrian and camera turn only about Y, tilted under a pitched camera), coloured trunc(255 x diffuse) of the variant's shader the variant row names; untextured, unlit, both sides, fogged. A type without a .rays file draws nothing there | MM2 (`pedAnimation::DrawSkeleton`; the .rays reading in the pedestrian type's loader: per bone start width, end width, offset, parent and an unused byte, then one shader index per bone for each variant) |

## Wide angle

| Topic | Behaviour | Evidence |
|---|---|---|
| Letterbox | in wide-angle mode the scene is cleared to black and the level drawn in the band from trunc(0.18 x height) down, trunc(0.66 x height) tall, the full width, with the projection's aspect that of the band (MM2's perspective there is 70 degrees vertical; OpenMM2's FOV modes still apply). The HUD and the mirror are drawn over the whole screen | MM2 (`mmPlayer::SetWideFOV`: `gfxViewport::SetWindow(0, 0.18 h, w, 0.66 h)`, `Perspective(70, window aspect, near, far)`) |

## Rear-view mirror

| Topic | Behaviour | Evidence |
|---|---|---|
| Drawing | while the mirror is on (whatever the camera), after the HUD map: the inset `RearViewMirror::viewport` gives (top right, one pixel in) cleared to black colour and full depth, the level drawn from the mirror's frame times the player car's model matrix with `Perspective(Fov, Aspect 2, NearClip, FarClip)`, every draw's winding swapped (the frame is mirrored), the player's car hidden (its trailer stays). Everything the level draws is drawn: sky, streets, objects, traffic, props, cars, effects and rain | MM2 (`mmMirror::Cull`, `mmMirror::Reset`, `mmGameManager::Update`); `render::Device::setFrontFaceFlipped` swaps the winding, RaceScreen's `drawLevel` is shared with the main view |

## Cars

| Topic | Behaviour | Evidence |
|---|---|---|
| Car bodies | at the high and medium LODs each material of the high LOD body draws its clean texture (`vpbugyellow_sd` for both `vpbugyellow_sd` and `vpbugyellow_sd_dmg`), other materials as stored; the low and very low LODs draw the paint job's materials as stored. `vppanozgt` in paint job 4 draws with alpha reference 0 | MM2 (`vehCarModel::Draw`, `fxTexelDamage::Init`) |
| Texel damage | each body material with a `_dmg` pair draws a private copy of its clean texture; at the first counted impact of a frame (effects.md) every high LOD body triangle with a damage texture and a corner within TextelDamageRadius of the impact point (model space) gets one 7-row patch of `_dmg` texels copied around a random point of it (small or large at random, clipped, no wrap); a `_dmg` material whose clean texture is missing starts damaged; resetting the car repaints it. The copies are single-level textures (no mipmaps) | MM2 (`fxTexelDamage::Init`, `ApplyDamage`, `Reset`) |
| Car parts | `vehCarModel::Draw`: body; DECAL (alpha blended); BREAK0-3, BREAK01/12/23/03 and VARIANT<paint> at their pivots; at H the refl_dc reflection pass and the fenders (FNDR0/1 turned with the front wheels, offset from wheel 0's pivot + 2.5 cm, mirrored); wheels and HUB0-3 at the simulation's wheel matrices, WHL4/WHL5 at WHL2/WHL3's moved 2.2 wheel radii back along the body. VL draws the body only. A missing LOD of a part takes the next less detailed one, never a more detailed one. HLIGHT, SLIGHT, SIREN, SRN and HEADLIGHT meshes are never drawn | MM2 (`vehCarModel::Draw`, `lvlInstance::GetGeomSet`) |
| Car reflections | the high LOD body again with `texture/refl_dc` mapped from the world-space normals (u = 0.5 + 0.5 x, v = 0.5 − 0.5 y), lit by an ambient grey of ftol(power × 255) (power 0.5 for car paint), added (ONE/ONE) and fogged, "Vehicle Reflections" option | MM2 (`modStatic::DrawEnvMapped`, `modShader::BeginEnvMap`, `cityLevel::GetEnvMap`) |
| Car shadow | the high LOD SHADOW mesh turned onto the ground found 1 m above to 1 m (else 5 m) below the car, none on ground steeper than normal.y 0.7; alpha blended, depth tested, no depth writes, pulled forward | MM2 (`vehCarModel::DrawShadow`, `lvlInstance::DrawPhysics`) |
| Enhanced car lighting | off by default (`[Tweaks] EnhancedCarLighting`, Graphics options → TWEAKS → CAR LIGHTING: Original / Enhanced; `app/Tweaks.h`). The lit parts of the player's, opponents', police and traffic cars use the same Direct3D 7 formula per pixel with renormalised normals, plus a Blinn-Phong highlight from the key light of strength power × 0.8 and exponent 48 (so the paint shines and tyres and undersides, power 0, do not); the reflection pass looks the sphere map up per pixel and scales it by 0.5 + (1 − n·v)^5 (Fresnel's shape: weaker facing the camera, stronger edge on). Shadows, glows and everything else are unchanged | OpenMM2 extra, not in MM2; constants chosen by eye (`game::kHighlightScale`, `kHighlightPower`, mesh.frag) |
| Image filters | off by default (`[Tweaks]` ColorGrade, Sharpen, Fxaa, Vignette; Graphics options → TWEAKS). Applied when the scene is composited onto the window (`shaders/composite.frag`, `render::PostProcess`), so the HUD and menus are not filtered: FXAA (the simple directional form), contrast-adaptive sharpening (after AMD FidelityFX CAS's idea), contrast about mid grey, saturation about the luma, a warm or cool shift, and a vignette. COLOUR presets: Vivid (contrast 1.10, saturation 1.25), Warm (1.05, 1.10, warmth 0.6), Cinematic (1.15, 0.85, warmth 0.35); SHARPENING 0.4 / 0.8; VIGNETTE 0.25 / 0.5. BLOOM (Subtle 0.35, Strong 0.7): a chain of five half-size levels (`shaders/bloom.frag`): a soft-knee bright pass above 0.65 and 13-tap downsamples, then tent-filtered upsamples mixing 70% of the wider level with 30% of this one; the composite screen-blends level 0 × 3 × strength before the grading. Both backends keep the chain beside the scene target and draw it just before the composite; `rendertest --post` turns every filter on (with `--stress` and `--validation` it covers the chain's recreation). AMBIENT OCCLUSION (Subtle 0.6, Strong 1.0): screen-space, from the scene depth with the main camera's projection (`Device::setSceneProjection`; none while the view is letterboxed). `depthcopy.frag` turns depth into distance (t = B / (d + A); Vulkan reads the multisampled depth's first sample, OpenGL blits it to a depth texture first); `ao.frag` rebuilds positions and normals and tests 12 points of a hemisphere of radius 1.25 m, turned per pixel, with a range check and a fade from 90 to 150 m; `aoblur.frag` blurs 4×4 with depth-aware weights and darkens the scene by open^(1 + 2 × strength). Bloom and the composite then read that shaded scene. The mirror and dashboard are drawn with other projections, so their occlusion is approximate (inferred harmless: the range check drops most of it). With every filter off the composite is the plain copy (verified pixel-identical with `rendertest` on both backends) | OpenMM2 extra, not in MM2; values chosen by eye (`app::postProcessFromTweaks`) |
| Sun shadows | off by default (`[Tweaks] SunShadows`: On, a 2048 map; High, 4096). The key light (the time of day's `.ltNN` light 0) is the sun: `game::sunShadowView` puts an orthographic shadow camera along it over a 140 m square centred 38 m ahead of the camera, snapped to whole texels (no shimmer), 600 m deep. The race draws the level a second time into it before the scene (`RaceScreen::drawSunShadows`, `drawLevel` with the light's frustum, as the mirror draws it); the backends keep only draws that write depth or are alpha tested and are not additive (`Device::shadowCasterCall`: cars, buildings, props, fences and trees cast; the sky, glows, particles, decals and blob shadows do not), both faces, pushed away from the light. Every non-additive scene draw receives (`DrawFlag::ReceiveShadow`): `mesh.frag` moves the point 1-3 texels along its normal, runs a 3x3 bilinear percentage-closer filter (16 texel comparisons) and darkens the colour by strength × facing × shadow, where facing is a smoothstep of the normal against the sun (sides facing away got no sun in the game's lighting either, and grazing surfaces do not flicker); the paint highlight is masked too. Strength = 0.6 × the key light's share of the light (`game::sunShadowDarkness`; none with the sun below the horizon), so dusk and night shadows are faint. While a frame has sun shadows the cars' blob shadows (SHADOW meshes) are not drawn. At 1920x1080 on an RTX 4070 (Vulkan, vsync off, San Francisco morning): 830 fps with every tweak off, 780 with the filters, bloom and ambient occlusion, 480 with sun shadows as well (the level drawn twice) | OpenMM2 extra, not in MM2; constants chosen by eye |
| Wet roads | off by default (`[Tweaks] WetRoads`: In rain, only with the Rain weather; Always). A post pass after the ambient occlusion (`shaders/ssr.frag`) reads the scene and the distance image: surfaces whose rebuilt normal is within ~30° of up (the camera's transform gives the world's up and world positions, `Device::setSceneCamera`) get screen-space reflections, a ray marched in up to 48 growing steps and refined in 5, faded at the screen's edges and past 60-150 m, mixed by a Schlick Fresnel term × a puddle pattern (two octaves of world-space noise: puddles 1.0, wet sheen 0.35) and capped at 85%; rain ripples (noise moving with time) tilt the mirror, and wet ground is darkened to 82%. Bloom and the composite read the result | OpenMM2 extra, not in MM2; values chosen by eye |
| Car lights | glow pass, unlit (pre-lit parts: white vertices, the black material ignored), added ONE/ONE, no depth writes: TLIGHT and BLIGHT while the brake input is not zero, TLIGHT again with the light flag (evening, night or fog), RLIGHT in reverse | MM2 (`vehCarModel::DrawGlow`, `mmGame::InitWeather`) |
| Headlights | two ltLights at the headlight0/1 pivots in the HEADLIGHT0/1 colours shining forward; each draws `lt_glow` facing the camera, half size 0.2 sqrt(25 cos^3 theta) = cos^1.5 theta metres, colour 0.95 x the light's (the scales are globals: aiVehicleManager::Init sets 0.2/0.95 after the single-player cars exist, a vehSiren constructed later sets 0.2/0.6); with the light flag, or sweeping +-42.4 rad/s while the siren is on: the two directions are kept in world space, the car's forward axis without the siren and turned about Y by the frame's share of the sweep from wherever they point with it, so the sweep does not turn with the car (MM2 also turns them again for the mirror's view; OpenMM2 once a frame) | MM2 (`vehCarModel::DrawHeadlights`, `ltLight::DrawGlow`) |
| Police lights | an ltLight per SRN0-3 part (pivot, part colour) whose world-space direction turns 2.5 pi rad/s about Y, a quarter turn apart, drawn as above, and each light's lens flares: twenty `lt_flare` cards per siren (ltFlare::Random colours, sizes and places along the line through the screen's centre; the first on the light, 0.3 across, the second mirrored through the centre, 0.25), drawn when the light's intensity (25 / d^2 x cos^3, less 0.05) x 0.5 reaches 0.05 and it is within twice the half height of the centre (fading from half of it), added over the scene without depth. OpenMM2 keeps the cards square at any aspect (MM2's flare viewport is 4:3) and draws none in the mirror | MM2 (`vehSiren`, `vehSiren::Draw`, `ltLensFlare`, `ltFlare::Random`, `ltLight::ComputeIntensity`) |
| Semi trailers | `vehTrailerInstance::Draw`: the TRAILER body at its LOD with the paint job's shaders as stored (no texel damage); only at H, TLIGHT while the tow car brakes harder than 0.1, as an ordinary lit draw of the object pass (its black, fully transparent fxltglowred material leaves it invisible under the pass's alpha test; inferred from the material, as OpenMM2 draws it), and TWHL0-3 at the trailer's wheel matrices. No reflection, decal, breakables, fenders, hubs or TWHL4/5, so TWHL0/1's medium and low meshes, modelled away from their pivots, are never drawn; no glows (lvlInstance's empty DrawGlow). The shadow as Car shadow | MM2 (`vehTrailerInstance::Init`, `Draw`, `DrawShadow`) |
| Car LOD | as objects (above), culled beyond NoDraw | MM2 (`lvlInstance::IsVisible`); the radius is the body set's farthest vertex from the model origin (`modGetStatic`) |
| Traffic cars | `aiVehicleInstance::Draw`: the body at its LOD with the colour's shaders as stored (no texel damage), BREAK0-3 always at H, and only at H the reflection and the wheels (turning about their axles on the rails, no steering; no wheels below H since LAME_WHEELS is off). A car knocked off its rail (with an aiVehicleActive) draws WHL0-3 unturned at its vehWheelCheaps' drawing matrices (the pivot moved by the spring's travel along the car's up axis, Limit below it off the ground, and back by 0.2 / 0.3 of the sideways / forward tyre deflection) and WHL4/5 unturned at their pivots raised by WHL2/3's drawn height less the wheel radius. Shadow: such a car's on the ground under it (as Car shadow), otherwise at the car's matrix while upright and on the ground when upside down. Glows: TLIGHT while braking or stopped and again with the light flag; with the light flag one white spot light (the manager's) at the headlight0 pivot and its mirror image when the car has HEADLIGHT1, pulled 0.2 m towards the camera | MM2 (`aiVehicleInstance::Draw`, `DrawGlow`, `DrawShadow`, `vehWheelCheap::Update`, `aiVehicleManager::Init`) |

Parts that break off (effects.md) are no longer drawn on the car until it is
reset. Not ported yet: suspension and engine parts (SHOCK, ARM, SHAFT, AXLE,
ENGINE need the suspension matrices; no retail car has them). Traffic turn
signals (SLIGHT0/1, blinking on a frame counter) are not ported.

## Drawing between simulation steps

**MM2.** The original ran its whole game once per rendered frame with that
frame's measured time: `datTimeManager::Update` sets `Seconds` to the time
since the last frame (clamped), `mmGame::Update` and the AI (`aiMap::Update`)
use it, and `dgPhysManager::Update` splits it into equal samples of at most
1/35 s, at most three (`mmGame::Init`'s SampleStep and MaxSamples;
physics.md). Each
frame then drew the state its own update had reached, so motion never
juddered at any frame rate, but the simulation itself changed with the frame
rate.

**OpenMM2** keeps its simulation independent of the frame rate, for
accuracy (it is the original's at 60 fps) and determinism (network play,
tests, the opponent sweep): the physics runs whole 1/60 s samples
(`phys::World::advanceFixed`) and the AI whole 1/30 s steps
(`ai::World::update`), each carrying the rest of the frame's time to the next
frame. A frame can therefore run no step or several. Drawing the last step's
state made the cars, the camera and the props move in 60 Hz steps against the
frame above 60 fps (and at 60 fps whenever a frame ran 0 or 2 samples), and
the traffic and pedestrians in 30 Hz steps at any frame rate.

Instead, the drawing blends each simulated object from its state when the
last step began to its current state, by alpha = the time the simulation has
not stepped yet / its step (`game::StepHistory`, `src/game/Interpolation`).
`phys::World` and `ai::World` call a step observer at the start of every
step; the race screen's observers record what they move into one history
each, and the renderers ask the history for each object's drawn state. This
is presentation only: the physics, the AI, the rules, the audio, the random
streams and the network snapshots read the stepped state as before (tests
`StepHistory.TheSimulationIsTheSameWhateverTheFrameTimes` and
`TheTrafficIsTheSameWhateverTheFrameTimes` compare every step at 60, 90, 144
and 240 fps and with uneven frames).

| What | Drawn | Evidence |
|---|---|---|
| Blend | positions lerped, orientations slerped the short way, a scaled instance's row lengths lerped; alpha 1 gives the current state exactly | OpenMM2 |
| Player's car, trailer, opponents, police | their vehicle poses (body and simulated wheels); the wheels blended in the body's frame and spun by the difference of their accumulated turns (vehWheel's rotation), so a wheel turning more than half a revolution a sample still turns forwards | OpenMM2 |
| Shadow, lights, headlight beams, sirens, lens flares | from the drawn pose (VehicleRenderer) | OpenMM2 |
| Cameras | every car camera follows the drawn car (`CameraTarget::matrix`), so the camera, the mirror (`mmMirror`'s frame on the drawn car) and the dashboard move with it; camera cuts and resets stay instant | OpenMM2 (camCarCS tracks vehCarSim's matrix) |
| HUD | the opponent icons, the map and its player arrow follow the drawn cars (their phInertialCS matrices from the drawn model matrices) | OpenMM2 (mmIcons, mmHudMap follow the cars' matrices) |
| Traffic with a body, props an active simulates in the world | their bodies' matrices (and the traffic's cheap wheels) blended | OpenMM2 |
| Traffic on its rails, pedestrians | blended over the AI's 1/30 s steps, the rail cars' tyre turn too; pedestrians keep whole animation frames, as MM2 draws them | OpenMM2 |
| Network players' cars | the snapshot sampled one physics sample further back than the sample the physics and the rules use (the frame's session time less the playout delay less 16.7 ms: alpha x step is the remainder, so that is where the rest of the scene is drawn); the kinematic car's simulated wheels blended in its body's frame; its trailer blended | OpenMM2 |
| A client's shared traffic and police | sampled 16.7 ms before the time their physics proxies are placed at (`TrafficClient::transformAt`) | OpenMM2 |

**Never blended.** An object not recorded at the start of the last step (new
since, or not simulated in it) is drawn where it is, and so is one recorded
under another generation: a car's reset count (`CarSim::resets`, every
`vehCar::Reset` and placement: respawns, a race reset, an opponent or police
car put back, a network car's first state), a recycled traffic slot (the
slot's spawn count), a pedestrian put on a road in the last step; a race
restart clears both histories. Any move longer than 5 m or a turn of more
than 1.6 rad in one step counts as a jump too (300 m/s at 60 Hz). A traffic
car that takes a body (it is hit) changes from the AI's history to the
physics' unblended, so it moves on by up to an AI step's travel at that
moment (half a metre at 15 m/s, while the hit throws it); one that gives its
body back is at rest. Camera cuts are instant since the camera itself is not
blended.

**Effects.** The effects run MM2's per-frame updates at a fixed 60 Hz of
their own (`fx::FixedTicker`). The particle systems (the wheels' dust and
smoke, the engine and exhaust smoke, the props' debris, the rain and snow)
are drawn back along each particle's velocity by the time since their last
update would have been due (`FixedTicker::behind`): an update moves a
particle by its new velocity, so that is where it was between its last two
updates, as the rest of the scene is drawn (the rain falls at 35 m/s: 58 cm
an update). The tyre tracks, the shards and the sparks
stay as updated: a track's newest end is under the car, shards are brief,
and the sparks move in steps of at least 1/30 s in MM2 too
(`asLineSparks::Update`). What the effects are born from is the simulation's
state, up to a step ahead of the drawn car. The gizmos, cable cars and sky
already move once per frame with its time.

**Latency.** The drawn state reaches the newest simulated one only just
before the next step, and right after a step it is the one before: on average
half a step later than before, 8.3 ms for what the physics moves and 16.7 ms
for the AI's traffic and pedestrians (which are drawn 16.7 ms behind the
physics' objects). That is the usual price of drawing between steps and is
accepted. The ways to draw at the frame's own time were rejected:
extrapolating from the current state by the remainder shows cars inside
walls before each contact and snaps them back (up to 0.67 m at 40 m/s);
simulating a throwaway copy of the world for the remainder (a partial
sample) would show states the simulation never reaches, since every sample's
terms scale with its length, would draw from the shared random streams, and
doubles the physics' cost; a shorter fixed step changes the simulation away
from the original's 60 fps behaviour.

`OPENMM2_DEBUG_DRAW_TRACE=<file>` (a development aid, off unless set) writes
a line a frame: the frame, its time, the physics samples it ran, alpha, the
player's car as drawn, a point 40 m ahead of it and 6 m to its right (chosen
again once behind the camera; the segment number counts them), the car's
speed and the nearest other car as drawn, all in the camera's frame.

**Measured** with it (and a scratch script): vppanozgt flat out
in the San Francisco cruise from 20 m/s until its first contact (about 38
m/s), `[Display] VSync=off` and `FrameCap`, 640 x 360. The second difference
per frame of the drawn positions in the camera's frame (millimetres; real
bumps of the road show in both builds):

| fps | car, median / rms before | after | roadside point, median / rms before | after |
|---|---|---|---|---|
| 60 | 35 / 59 | 1.6 / 29 | 402 / 503 | 6.0 / 20 |
| 90 | 36 / 49 | 0.65 / 12 | 388 / 391 | 3.2 / 8.9 |
| 144 | 39 / 48 | 0.24 / 7.8 | 442 / 437 | 2.1 / 7.0 |
| 240 | 13 / 36 | 0.09 / 3.8 | 12 / 339 | 1.3 / 4.1 |

At a 60 fps cap 38% of the frames ran no sample or two before (the frame
time hovers about the step); the remaining rms after is the car's real
bounces on the road's bumps, which the camera follows with a lag. At 144 fps
the nearest opponent in the San Francisco checkpoint race moved by 186 / 226
mm before and 1.2 / 11 mm after; in a network cruise with shared traffic
(host and client on UDP, both flat out) the host's car as the client draws
it by 112 / 118 mm before and 1.5 / 5.6 mm after (114 / 118 and 1.3 / 5.8
mm through `netprobe relay` at 50 +- 15 ms each way, 1% loss and
reordering), the client's roadside point by 367 / 359 and 1.3 / 2.5 mm.
