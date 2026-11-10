# OpenMM2 — enhanced fork

A fork of [OpenMM2](https://github.com/lowlevelmetal/OpenMM2), the open source
(GPL-3.0) reimplementation of **Midtown Madness 2** (Angel Studios /
Microsoft, 2000), with a modern graphics overhaul and a long list of
smoothness fixes on top.

Everything here is optional. The **Original** look is the game as it was in
2000; the enhancements are switched on from the game's own menus, and F9
flips between them and the original during a race. The game's simulation
(handling, AI, traffic, rules) is untouched: the changes are to how it is
drawn.

> You need your own copy of Midtown Madness 2. Neither this fork nor OpenMM2
> contains any of the game's content; they load it from your CD, a disc
> image or an existing installation, and never touch the original
> executable's copy protection. Installing, building and the rest of the
> project's documentation are in the
> [upstream README](https://github.com/lowlevelmetal/OpenMM2#readme).
>
> Not affiliated with or endorsed by Microsoft, Rockstar Games or Angel
> Studios. "Midtown Madness" is a trademark of its respective owner.

## Getting started

- **Options** (the first page you reach from the main menu) has the OpenMM2
  panel: pick a look — **Original**, **Enhanced**, **Enhanced RT** (ray
  traced) or **Custom** — then anti-aliasing, render scale, display and
  camera options. Each option shows what it does and a recommendation.
- **Customise the look** opens the Tweaks page, where every effect can be set
  on its own.
- In a race: **F9** switches between your look and the original, **F11**
  shows a frame rate counter.

Recommended: **Enhanced RT** on a GPU with ray tracing (Vulkan), otherwise
**Enhanced**; **TAA** anti-aliasing, at 75% render scale on 1440p screens and
above.

## Looks and settings

- **Look presets:** Original, Enhanced, Enhanced RT and Custom. Enhanced RT is
  Enhanced with the sun shadows and reflections ray traced; on hardware
  without ray tracing it falls back to Enhanced.
- **OpenMM2 panel** on the Options page: the look, anti-aliasing, render
  scale, VSync, UI scale, field of view and the free camera, with
  descriptions and recommendations. Display changes apply at once.
- **Tweaks page:** every effect with simple levels (Off / Subtle / Strong and
  the like), plus colour sliders (brightness, contrast, saturation, warmth).
- **F9** compares your look with the original in a race; **F11** toggles a
  frame rate and frame time counter.

## Lighting

- **Car lighting:** per-pixel lighting on the cars with highlights, and a
  glossy **clear coat** on the paint that mirrors the sky and catches sharp
  sun glints.
- **Live reflections:** the player's car reflects the real city around it
  (a live reflection probe), or with ray tracing, exactly what is there.
- **Sun shadows:** cascaded shadow maps out across the city, with soft,
  contact-hardening edges (sharp at the tyres, soft far from the caster),
  long shadows fading towards their tips, and no popping as you drive or
  turn. On the High level, four larger cascades.
- **Ambient occlusion:** soft contact shading under cars, along kerbs and in
  corners.
- **Night lighting:** headlights, sirens and street lamps really light the
  road, walls and cars after dark. Halogen-warm headlight beams with a soft
  top and a dipped-beam shape that follows hills; sodium-orange street
  lamps; up to 48 lights at once, fading in and out of the budget without
  popping.
- **Glowing windows:** at night the city's lit windows and shop fronts glow
  and catch the bloom (and their reflections in the wet streets and the bay).
- **Cloud shadows:** big soft shadows of passing clouds drift over the city
  on clear and partly cloudy days, cast along the sun's direction.
- **Distance haze:** a soft haze over faraway buildings and hills, brighter
  towards the sun.

## Ray tracing (Vulkan)

- **Ray traced sun shadows**, including from trees and fences (alpha-tested
  geometry), with soft penumbras.
- **Ray traced reflections** on the car paint and on wet roads: the real city,
  its traffic, the sky, the night's lamps and glowing windows, lit by the
  scene lights.
- **Ray traced shadows from the night lights:** lamp posts, cars and walls
  cast shadows in the headlights and street lamps.
- **Ray traced water** that mirrors the skyline and the bridges.
- Built for speed: rays shared across pixel quads, half-rate wet-road rays,
  the city captured in parts over several frames, a static/dynamic split of
  the scene.

## Surfaces and water

- **Reflective water:** the bay, lagoons and ponds get moving deep-water
  waves, a Fresnel mirror (of the sky, or with ray tracing the city) and the
  sun's glint, with no shimmer in the distance.
- **Wet roads:** reflections, puddles and rain ripples on the streets (in rain
  or always), rougher and weaker in the distance and correct on hills.
- **Grass:** real swaying blades of grass on the parks and lawns, with
  lusher and drier patches (off in the presets: costly in big parks).
- **Street detail:** roads, kerbs, lane markings and dividers drawn at full
  detail however far away, so the highway no longer pops in ahead.

## Image and effects

- **Anti-aliasing:** FXAA, or **TAA** (temporal anti-aliasing) that smooths
  fences and trees too, with **upscaling** from a lower render scale that
  stays sharp in motion.
- **Colour grading** (Vivid, Warm, Cinematic) with sliders, **sharpening** and
  **vignette**.
- **Bloom** around lamps, headlights and the bright sky.
- **Sun shafts** streaming past buildings and trees, and the sun's glare.
- **Motion blur** at speed, easing in with the camera's speed, with the car
  itself kept sharp.
- **Heat shimmer** over hot roads on a clear noon, with a mirage of the sky on
  the far road.

## Smoother: less pop-in, fewer hitches

- **Level-of-detail crossfades:** buildings, props and cars blend between
  their detail levels instead of switching, and props, traffic and
  pedestrians fade out before their draw distance instead of vanishing.
- **Pedestrians** blend between their full model and the distant stick figure.
- **Traffic fades:** cars fade in where the traffic places them and fade out
  where it takes them away, instead of appearing and vanishing in view.
- **No popping shadows:** shadows are cast from everything around, including
  walls the camera faces away from and buildings round corners.
- **Hitch fixes:** the ray traced city is rebuilt in parts over several frames
  (it was a 45–50 ms stall every 150 m), traffic sounds are preloaded and set
  up a few at a time, and shadow and probe views are only redrawn when due.
- **Performance work** for ultrawide screens: large speed-ups in parks and
  rain, cheaper grass and wet roads, fewer and lighter ray tracing builds.

## Controls and camera

- **Free camera:** swing the chase camera round the car with the right stick,
  or the mouse with its right button held; it eases back behind the car.
- **Gamepad:** Start opens and closes the race menu (like Esc), and the left
  stick moves the menu pointer.

## Fixes along the way

- Headlight beams no longer cut off in a straight line on hills, nor
  flicker on and off with ray tracing (the car's own body was shadowing its
  beams); street lamps no longer shade their own bulbs.
- No speckled shimmer on walls under the street lamps.
- Wet reflections on hills and near the camera no longer jump or show mirror
  walls; buildings missing from reflections are back.
- The car's shadow no longer warps under motion blur when turning; shadows no
  longer pop at intersections.
- Trees no longer fade in and out a short way off.
- Filters kept off the HUD map; the sky dome kept out of the shadows.

## Developer tools

- `OPENMM2_DEBUG_TIMING=1` logs the CPU time of each frame stage once a
  second.
- `OPENMM2_DEBUG_HITCHES=1` logs every frame much slower than usual, with its
  stages and the slow work in it (textures, models, pipelines, the ray
  scene, GPU waits).
- `OPENMM2_DEBUG_SHOT_FRAMES=<first>-<last>` saves a range of frames as
  screenshots (with `--screenshot`), to look at flicker frame by frame.
- `OPENMM2_DEBUG_FREELOOK=<yaw>,<rise>` holds the free camera at an angle,
  for screenshots.

The rendering details of every feature — how it works, its constants and
what it costs — are in [docs/rendering.md](docs/rendering.md).

## Licence

GPL-3.0, as OpenMM2. See [LICENSE](LICENSE).
