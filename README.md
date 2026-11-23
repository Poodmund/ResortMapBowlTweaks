# Resort Map Bowl Tweaks

**Version 1.0.0 — Ski-E-O Resort Map Bowl Tweaks mod**

Resort Map Bowl Tweaks changes how bowls are shown in the game's Resort Map view. It replaces their outline-based map strokes with calculated downhill centrelines and provides options for line and name-label presentation. It does not modify the resort's world geometry, save data, or trail definitions.

<img width="2560" height="480" alt="image" src="https://github.com/user-attachments/assets/a67850c6-9e96-4511-9047-e8affb8cd3fc" />

Screenshot showing various configurations: `Dashed | Inverted`, `Hidden | Default`, and `Solid | Box`

## Features

- Calculate a downhill centreline from each bowl's three-dimensional boundary geometry. The line follows the midpoints of elevation-slice intersections; results are approximate for complex or strongly concave bowls.
- Show bowl centrelines as dashed lines, solid lines, or hide their map strokes.
- Style bowl name labels with dark/light outlines or a bordered rectangular plate.
- Identify bowl-label widgets using native map geometry indexes and live widget associations, without relying on displayed names. Exact bowl-name matching is available as an alternate mode.
- Leave label placement and visibility decisions to the game's Resort Map renderer. A bowl without a displayed game label will not gain a new label.

## Requirements and compatibility

**Build compatibility requirements**
- Ski-E-O on Windows (64-bit), with the game executable named 'skieogame.exe'.
- A compatible **RE-UE4SS** native C++ mod environment, including **MinHook**, for compiling this source.
- A configured CMake project exposing the 'BowlMapDiagnostic' target (or an equivalent project configured to build this source).

The mod searches the running executable's code sections for five unique native-function signatures and checks eleven machine-instruction layout invariants before installing hooks. It also checks selected native record data at runtime. These precautions reduce dependence on fixed function addresses, but they do not guarantee compatibility with a future game update. The mod can deliberately decline to install hooks if code signatures or layout evidence differ. Internal Unreal and game structure layouts still require a compatible build.

**Tested game build:** CL8239

## Build from source

This is a source-only archive. It does not contain the game executable, the RE-UE4SS SDK, the complete CMake project, or a prebuilt mod DLL.

1. Copy 'src/dllmain.cpp' into 'BowlMapDiagnostic/dllmain.cpp' in a configured RE-UE4SS C++ mod project.
2. From the project root, build with the project's normal Windows toolchain. For the project layout used during development:

   ```powershell
   cmake --build build --config Game__Shipping__Win64 --target BowlMapDiagnostic -- /p:VcpkgEnabled=false
   ```

3. Install the resulting mod DLL as 'mods/ResortMapBowlTweaks/dlls/main.dll' under the Ski-E-O game directory.
4. Install 'config/ResortMapBowlTweaks.ini' at 'mods/ResortMapBowlTweaks/ResortMapBowlTweaks.ini'.
5. Restart the game to load the DLL and configuration.

The exact output name and build directory can vary with your CMake setup; rename the compiled DLL to 'main.dll' if necessary.

## Configuration

Edit 'ResortMapBowlTweaks.ini' and restart the game. The shipped defaults are **'lines = dashed'** and **'trail_name_text = default'**.

| Option | Values | Default | Effect |
| --- | --- | --- | --- |
| `lines` | `dashed`, `solid`, `hidden` | `dashed` | Bowl-centreline stroke mode. |
| `dash_length` | 3–30 | `7` | Ink-segment length in dashed mode. |
| `gap_length` | 4–60 | `15` | Gap length in dashed mode. |
| `trail_name_text` | `default`, `inverted`, `box` | `default` | Bowl name-label style. |
| `light_color` | `#RRGGBB` | `#EFE7D4` | Light label colour. |
| `dark_color` | `#RRGGBB` | `#26262B` | Dark label colour. |
| `id_label_style` | `true`, `false` | `true` | Native index/widget matching when true; exact name matching when false. |
| `debug` | `true`, `false` | `false` | Enable diagnostic logging. |

The text styles are:

- **default:** dark letters with a light glyph outline.
- **inverted:** light letters with a dark glyph outline.
- **box:** dark letters on a light rectangular plate, edged in dark colour.

Colours are sRGB hex values. In 'hidden' line mode, bowl strokes are suppressed after the game has used their geometry to place the labels.

## Troubleshooting

To inspect a compatibility or styling problem, set 'debug = true', restart the game, and open the Resort Map. The log is written to:

'mods/ResortMapBowlTweaks/logs/ResortMapBowlTweaks.log'

When signatures and layout evidence are valid, the log should include 'Resolved ... at RVA=' entries, a 'LAYOUT native instruction evidence passed checks' message, and enabled-hook messages. With native-ID styling active, 'IDSTYLE pass=...' lines report candidate labels, matched live widgets, applied styles, and any failures. Counts vary with visible map labels, zoom, and map state.

If native signature resolution or layout validation fails, the mod leaves its hooks uninstalled. If the game crashes or the map does not render correctly, remove the mod DLL and restart the game, then investigate compatibility with that executable build. Do not assume that passing the startup checks proves all internal layouts are unchanged.

## Distribution

This source archive includes only the mod implementation, configuration template, and documentation. It contains no game binaries, SDK files, or third-party libraries.

## License

Resort Map Bowl Tweaks is released under the Zero-Clause BSD Licence (0BSD).

You are free to use, modify, redistribute, incorporate or otherwise reuse this project's original code for any purpose, including commercial purposes, without seeking permission or providing attribution.

This software is provided as is, without warranty of any kind. The author provides no guarantee of compatibility with future Ski-E-O versions and undertakes no obligation to maintain, update or support the project.

See the LICENSE file for the full licence terms.
