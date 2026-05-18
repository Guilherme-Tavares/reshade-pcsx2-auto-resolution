# reshade-auto-resolution

A ReShade addon that automatically updates the `Resolution_X` and `Resolution_Y` preprocessor definitions of **CRT-Guest-NTSC.fx** to match the PS2's current native resolution — no manual adjustment needed when PCSX2 switches between video modes.

## The problem

`CRT-Guest-NTSC.fx` uses `Resolution_X` and `Resolution_Y` as preprocessor definitions to size its internal NTSC pipeline textures. These must match the PS2's native output resolution (e.g. 512×448 or 640×448), which games change dynamically — typically between gameplay and cutscenes or menus.

Updating them manually in the ReShade UI each time is tedious. Editing the preset `.ini` on disk causes a full shader reload with a visible flash.

## How it works

PCSX2 renders the PS2 framebuffer to an internal render target sized at `(native_resolution × upscale_factor)`. When the game changes video mode, PCSX2 creates a new render target at the new size.

The addon listens to the `init_resource` event and checks every new render target: if its width and height are both clean integer multiples of a known PS2 native resolution — using the **same factor** for both axes — the native resolution is identified and the shader definitions are updated.

```
RT 1024×896  →  1024/512 = 2,  896/448 = 2  →  native 512×448  ✓
RT 1280×896  →  1280/640 = 2,  896/448 = 2  →  native 640×448  ✓
RT 3840×2160 →  3840/512 = 7.5 (not integer) →  ignored         ✓
```

The display resolution never satisfies the criterion because it results from aspect-ratio stretching, not an integer upscale.

## Supported native resolutions

| Resolution_X | Resolution_Y |
|---|---|
| 640 | 480 |
| 640 | 448 |
| 512 | 448 |
| 512 | 240 |
| 320 | 240 |
| 320 | 224 |
| 256 | 240 |
| 256 | 224 |

Any integer upscale factor (1×–8×) is handled automatically.

## Requirements

- ReShade 6.x (with add-on support enabled)
- PCSX2 (any recent build)
- `CRT-Guest-NTSC.fx` loaded in your ReShade preset

## Building

**Prerequisites:** Visual Studio 2022 with C++ and CMake workloads.

1. Open the folder in Visual Studio (`File → Open → Folder`)
2. Select configuration `x64-Release`
3. Build target `AutoResolution`
4. Output: `out/build/x64-Release/AutoResolution64.addon`

## Installation

Copy `AutoResolution64.addon` to the same folder as `reshade64.dll` and the PCSX2 executable. ReShade loads all `.addon` files in that directory automatically on startup.

## Notes

- `AspectSize_X` and `AspectSize_Y` are never touched — only `Resolution_X` and `Resolution_Y` are updated.
- The preset `.ini` file is never written to disk.
- The addon has no visible UI. Detection is silent and automatic.
