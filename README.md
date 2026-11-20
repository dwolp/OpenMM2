
# OpenMM2 Enhanced (Chicago edition)

A fork of **[OpenMM2](https://github.com/lowlevelmetal/OpenMM2)**, the
open-source reimplementation of Midtown Madness 2, with Midtown Madness 1's
Chicago, better graphics, ray tracing, and a settings panel you can open
during a race.

* **[Download the latest release](https://github.com/dwolp/OpenMM2/releases/latest)**
  for Windows (installer or portable ZIP) or Linux (tarball).
* You need your own copy of Midtown Madness 2, and for Chicago your own
  Midtown Madness 1 disc image. This repository contains no game files.
* For installing, building and file formats, see the
  [original README](https://github.com/lowlevelmetal/OpenMM2#readme).

## New in this release

* **Chicago is playable.** Drive MM1's city with MM2's cars, physics and
  racers: Cruise, Checkpoint, Circuit and Blitz, with MM1's own checkpoints,
  police, pedestrians, bridges and breakables.
* **MM1's commentary and music.** The announcer's pre-race, results and cop
  lines, and the CD music, read straight from your MM1 disc image.
* **World options in the F1 panel.** Traffic density, weather including snow,
  time of day and view distance, changed during the race.
* **Music and city sounds** can both be on at once.
* **Portable.** Settings, profiles and logs live in a `user` folder next to the
  exe. The MM1 and MM2 disc images can be anywhere.
  

## Everything else this fork changes

**Gameplay and controls**
* **F1 enhancements panel:** every visual option changed live in a race:
  display and render scale, anti-aliasing, lighting, reflections and shadows,
  surfaces, image effects, camera and world.
* **F9** compares your settings with the original look, **F10** shows a frame
  counter, **F11** toggles full screen, **F12** saves a screenshot.
* **Free camera:** swing the chase camera with the right stick, or the mouse
  with its right button held.
* **Photo mode (F8):** pauses the race, hides the HUD and flies the camera free from your
  view. Z and X zoom, F1 opens the effects, the weather and the time of day, F12 saves the
  picture and F8 or Esc leaves.
* **Gamepad menus:** Start opens the race menu, the left stick moves the
  pointer.
* **Disc cards** on the Options page show whether MM2 is found and let you link
  an MM1 disc image.

**Look and feel**
* **Ray tracing (Vulkan):** shadows, reflections on cars and wet roads, water
  and night lights.
* **Lighting:** per-pixel car lighting with a glossy clear coat and live
  reflections, cascaded soft sun shadows, ambient occlusion, cloud shadows and
  distance haze.
* **Night:** headlights, sirens and street lamps that light the road, and
  glowing windows.
* **Surfaces:** reflective water, wet roads with puddles and ripples, swaying
  grass.
* **Image:** FXAA or TAA with upscaling, colour grading, sharpening, bloom, sun
  shafts, motion blur and heat shimmer.
* **Weather:** heavier rain and snow, and a fog that closes in on Chicago.
* **Smoother than the original:** detail levels, traffic and pedestrians fade
  instead of popping in, roads and kerbs stay detailed in the distance, and
  traffic sounds load with the race instead of hitching.

## Building

```
cmake -S . -B build -G Ninja
cmake --build build
```

A built game is portable: `portable.txt` next to `openmm2.exe` keeps the
settings in `user\`. Delete it to use the per-user folders instead.

## Legal

OpenMM2 is an independent fan project. It is not affiliated with, endorsed by
or sponsored by Microsoft, Rockstar Games or Angel Studios. Midtown Madness is
a trademark of Microsoft Corporation, and the games, their data and artwork
are © Microsoft. This repository contains no game files and no code from the
original games. Do not commit game files or extracted assets.

## Licence

GPL-3.0, as OpenMM2. See [LICENSE](LICENSE).

<img width="2560" height="1440" alt="rain_night_split" src="https://github.com/user-attachments/assets/5d2ea855-5c58-4486-af1e-f950c1ffb19a" />
<img width="2560" height="1440" alt="evening_car_split" src="https://github.com/user-attachments/assets/eb1c4686-9411-45ac-80cc-1fd233bcd7d0" />
<img width="2560" height="1440" alt="bay_sunset_split" src="https://github.com/user-attachments/assets/abaeedf5-84d2-4047-96ee-bc3e5baea0ac" />
<img width="2560" height="1440" alt="hill_rain_split" src="https://github.com/user-attachments/assets/139f3c74-d8f5-4af5-a855-41558688c59a" />
<img width="2560" height="1440" alt="evening_street_split" src="https://github.com/user-attachments/assets/02996729-24f0-4c3f-a228-97e87045c7d7" />
<img width="2560" height="1440" alt="downtown_night_split" src="https://github.com/user-attachments/assets/2e171b2d-607e-495d-996b-92d69cd6486b" />
<img width="2560" height="1440" alt="chicago_skyline_evening" src="https://github.com/user-attachments/assets/4a71f3b5-b99a-4df4-9d24-bc4336c4fc0f" />
<img width="2560" height="1440" alt="chicago_panel_lighting" src="https://github.com/user-attachments/assets/300a9bb3-aa2c-4f23-8450-4879ac833e8f" />
