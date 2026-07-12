# reshade-pcsx2-auto-resolution

A ReShade add-on that automatically updates the `Resolution_X` and `Resolution_Y` preprocessor definitions of the [CRT-Guest-ReShade](https://github.com/HelelSingh/CRT-Guest-ReShade) shader suite to match the PS2's current native resolution, with no manual adjustment needed when PCSX2 switches between video modes.

Compatible with all CRT-Guest ports and variants. The shader filename must contain `CRT` or `NTSC` (case-insensitive).

## The problem

The CRT-Guest shaders use `Resolution_X` and `Resolution_Y` as preprocessor definitions to size their internal pipeline textures. These must match the PS2's native output resolution (e.g. 512×448 or 640×448), which games change dynamically, typically between gameplay and cutscenes or menus.

Updating them manually in the ReShade UI each time is tedious. Editing the preset `.ini` on disk causes a full shader reload with a visible flash.

## How it works

### Primary: scissor rect analysis

PCSX2 translates the PS2 GS SCISSOR register into a D3D/Vulkan scissor rectangle for each draw call. The add-on accumulates these rectangles over a rolling window of 8 frames: any rectangle whose dimensions are an integer multiple of a known PS2 native resolution is counted. The **smallest** mode that reaches the minimum count threshold is taken as the active resolution.

This correctly handles cases where PCSX2 reuses a larger render target for a smaller display mode (e.g. a 512×448 RT reused for a 512×224 display): both 512×224 and 512×448 scissors appear in the window, but the smallest qualifying result is 512×224.

Accumulating over multiple frames is necessary because some modes emit only ~1 matching scissor per frame, too sparse to qualify within a single frame but reliable over a short window.

### Fallback: render target detection

Some modes emit no detectable scissors. For those, the add-on monitors newly created render targets via `init_resource`: if a render target's dimensions are a clean integer multiple of a known PS2 native resolution, using the same factor for both axes, the native resolution is identified.

```
RT 1024×896  →  1024/512 = 2,  896/448 = 2  →  native 512×448  ✓
RT 1280×896  →  1280/640 = 2,  896/448 = 2  →  native 640×448  ✓
RT 3840×2160 →  3840/512 = 7.5 (not integer) →  ignored         ✓
```

The fallback result is used only when scissor analysis produces no qualifying mode in a given window.

## Supported native resolutions

| Resolution_X | Resolution_Y |
|---|---|
| 640 | 512 |
| 640 | 480 |
| 640 | 448 |
| 640 | 447 |
| 640 | 417 |
| 640 | 416 |
| 640 | 256 |
| 640 | 224 |
| 512 | 512 |
| 512 | 448 |
| 512 | 447 |
| 512 | 417 |
| 512 | 416 |
| 512 | 256 |
| 512 | 240 |
| 512 | 224 |
| 320 | 240 |
| 320 | 224 |
| 256 | 240 |
| 256 | 224 |

Any integer upscale factor is handled automatically.

## Requirements

- ReShade 6.7.3 or later (with add-on support enabled)
- PCSX2 (any recent build)
- At least one CRT-Guest shader loaded in your ReShade preset

## Building

**Prerequisites:** Visual Studio 2022 with C++ and CMake workloads.

1. Open the folder in Visual Studio (`File → Open → Folder`)
2. Select configuration `x64-Release`
3. Build target `AutoResolution`
4. Output: `out/build/x64-Release/AutoResolution64.addon`

## Installation

1. Download `AutoResolution64.addon`
2. Place it in the same folder as `pcsx2-qt.exe`
3. ReShade loads it automatically on startup

## Known limitations

**Screen Offsets (PCSX2 Settings → Graphics → Display)**

When this option is enabled, PCSX2 shifts scissor rectangles away from the origin, causing the add-on to ignore them. Detection falls back to render target analysis, which may produce incorrect results. Disable Screen Offsets for reliable resolution detection.

**BIOS animation and scene transitions**

During certain phases, notably the PS2 BIOS opening animation, PCSX2 can emit scissor rectangles that do not correspond to the actual display output. The add-on may briefly apply an intermediate resolution during these phases. Detection corrects itself once the main rendering begins.

## Notes

- `AspectSize_X` and `AspectSize_Y` are never touched; only `Resolution_X` and `Resolution_Y` are updated.
- Shaders not present in the active preset are silently ignored.
- The add-on has no visible UI. Detection is silent and automatic.
