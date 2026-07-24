<h1 align="center">OBS Selective Fullscreen</h1>

<p align="center">
  <b>An OBS Studio source for Windows that captures a whole monitor — but only shows the apps you choose.</b><br>
  Everything else is true transparency. No chroma key, no cropping, no scene juggling.
</p>

---

## What it does

**OBS Selective Fullscreen** adds a new capture source whose canvas is exactly the size of one of your monitors. Instead of showing the whole desktop, it composites **only the windows belonging to applications you select** (by executable name, e.g. `notepad.exe`) — at their real on-screen positions, with their real z-order, moving and resizing live exactly as they do on your desktop. Every pixel not covered by a selected window is fully transparent.

```
        Your desktop                      This source's output
┌───────────────────────────┐      ┌───────────────────────────┐
│ ┌────────┐  ┌───────────┐ │      │ ┌────────┐                │
│ │ Notepad│  │  Discord  │ │      │ │ Notepad│    (alpha)     │
│ └────────┘  └─────┬─────┘ │  →   │ └────────┘                │
│    ┌──────────────┴──┐    │      │    ┌─────────────────┐    │
│    │      Game       │    │      │    │      Game       │    │
│    └─────────────────┘    │      │    └─────────────────┘    │
└───────────────────────────┘      └───────────────────────────┘
     (selected: notepad.exe, game.exe — Discord isn't shown)
```

Each selected window is captured **individually** with [`Windows.Graphics.Capture`](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture), so a selected window stays fully visible in the capture even while an unselected window covers it on the real desktop — overlapping windows you didn't select simply don't exist in the output.

### Feature highlights

- 🖥️ **Monitor-sized canvas** with a fully transparent background (real alpha channel)
- 🎯 **Select apps, not windows** — every window of a selected process is captured, including ones the app opens later; closed windows disappear automatically
- 🔎 **Optional window-title filter** per entry (`chrome.exe|My Stream`) — capture just the one browser window you mean, not all of them
- 🪟 **Faithful desktop behavior** — position, size, movement, and stacking order mirror your desktop 1:1, pixel-exact (invisible resize borders are cropped away)
- 🖱️ **Cursor capture** that only appears while the cursor is over a selected window
- 🟡 **No yellow capture border** on Windows 11
- 🧊 Windows spanning multiple monitors are clipped cleanly at the canvas edge
- ⏸️ **Zero cost while hidden** — capture sessions and window scanning shut down automatically when the source isn't visible in any scene, preview, or projector

## Compatibility

| Requirement | Supported |
|---|---|
| OS | Windows 10 2004 (build 19041) or later; **Windows 11 recommended** |
| OBS Studio | 31.x (built against 31.1.1; the source API used is stable across 30+) |
| Renderer | Direct3D 11 (OBS's default on Windows; OpenGL is not supported) |
| Architecture | x64 |

### Known limitations

- **Windows 10:** the OS draws a yellow border around each captured window. Removing it requires an API that only exists on Windows 11.
- **Elevated apps:** windows of processes running as administrator can only be captured if OBS itself runs elevated.
- **DRM-protected windows** (`SetWindowDisplayAffinity`) capture black — same as every capture tool.
- **UWP apps** are hosted by `ApplicationFrameHost.exe`; select that executable to capture them.
- Captures are capped at 32 simultaneous windows.

## Installation

Copy the plugin into OBS's plugin search path and restart OBS:

```
C:\ProgramData\obs-studio\plugins\
└── obs-selective-fullscreen\
    ├── bin\64bit\obs-selective-fullscreen.dll
    └── data\locale\en-US.ini
```

(Alternatively, drop the DLL into `obs-plugins\64bit\` and the locale file into `data\obs-plugins\obs-selective-fullscreen\locale\` inside your OBS installation folder.)

Verify it loaded via **Help → Log Files → View Current Log** — you should see:

```
[obs-selective-fullscreen] plugin loaded successfully (version 0.1.0)
```

## Usage & configuration

Add **Sources → + → Selective Fullscreen**, then configure:

| Option | Default | Description |
|---|---|---|
| **Monitor** | Primary | The display to mirror. The source's dimensions are exactly this monitor's resolution, and only windows intersecting it are drawn. Monitors are identified by stable device IDs, so the selection survives display re-arrangement. |
| **Applications** | *(empty)* | Editable list of executable names, one per entry (e.g. `notepad.exe`). Matching is case-insensitive. Entries persist even while the app isn't running — its windows appear the moment it starts. Append `\|text` to an entry to only capture windows whose **title** contains the text (case-insensitive), e.g. `chrome.exe\|My Stream` — handy for picking one browser window out of many. |
| **Add running application** | — | Convenience dropdown listing every executable that currently owns a visible window; picking one appends it to the list above. |
| **Match full executable path** | Off | When enabled, entries must be full paths (`C:\Tools\MyApp\app.exe`) instead of file names — useful to distinguish two programs with the same executable name. Entries containing `\` are always compared as full paths. |
| **Capture cursor** | On | Includes the mouse cursor while it hovers a selected window. |
| **Pause capture while the source is hidden** | On | Shuts down all capture sessions and window scanning while the source isn't shown anywhere (any scene, studio-mode preview, or projector counts as shown), freeing GPU/CPU. Re-showing restarts captures within a frame or two — turn this off if you hard-cut between scenes and the brief re-capture blank bothers you. |
| **Fit layout to canvas (keep window pixel size)** | Off | Normally the source is monitor-sized, so on a smaller canvas OBS scales everything down and windows shrink with it. Enable this to instead report the source at the **target/canvas size** and remap each window's desktop position **proportionally** (a window at a desktop edge lands at the matching canvas edge) while keeping its **native pixel size** — so windows stay readable. Reveals the three options below. |
| **Shrink windows larger than the canvas to fit** | On | Only applies in the mode above. **On:** a window bigger than the canvas scales down (aspect-preserved) just enough to fit — nothing is ever cut off. **Off:** the window keeps native pixels and is clipped at the canvas edges. Windows that already fit are unaffected either way. |
| **Target width / height** | 0 (auto) | Only applies in the mode above. The canvas size to map windows into. `0` means **auto** — match OBS's base (canvas) resolution, recomputed live if you change it. Set explicit values if this source only occupies part of your scene. |

**Tip:** to check the transparency, place a Color Source behind this source in your scene — every area not covered by a selected window should show it.

## Building from source

The project is based on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate); the build is fully self-contained.

### Dependencies

- **Visual Studio 2022** (or newer Build Tools) with the *Desktop development with C++* workload
- **CMake ≥ 3.28**
- **Windows SDK ≥ 10.0.22621**
- Internet access on first configure — CMake automatically downloads and builds the remaining dependencies pinned in [`buildspec.json`](buildspec.json):
  - OBS Studio 31.1.1 sources (libobs is built locally and linked against)
  - Pre-built [obs-deps](https://github.com/obsproject/obs-deps)
  - Pre-built Qt6 (required by the template's tooling, not linked by this plugin)

The plugin itself links only Windows system libraries on top of libobs: `d3d11`, `dxgi`, `dwmapi`, and `windowsapp` (C++/WinRT).

### Build steps

```powershell
git clone https://github.com/bogenpirat/obs-selective-fullscreen.git
cd obs-selective-fullscreen

cmake --preset windows-x64          # first run downloads deps + builds libobs (a few minutes)
cmake --build --preset windows-x64  # RelWithDebInfo by default
```

Artifacts land in `build_x64\RelWithDebInfo\` and a ready-to-copy layout in `build_x64\rundir\RelWithDebInfo\`. Install as described [above](#installation).

> **Note for VS 2026 Build Tools:** the stock preset pins the VS 2022 generator. To build with VS 2026 instead, create a local (git-ignored) `CMakeUserPresets.json` next to `CMakePresets.json`:
>
> ```json
> {
>   "version": 8,
>   "configurePresets": [
>     {
>       "name": "windows-x64-local",
>       "inherits": ["windows-x64"],
>       "generator": "Visual Studio 18 2026",
>       "architecture": "x64,version=10.0.26100",
>       "environment": { "VCToolsVersion": "14.44.35207" }
>     }
>   ],
>   "buildPresets": [
>     { "name": "windows-x64-local", "configurePreset": "windows-x64-local", "configuration": "RelWithDebInfo" }
>   ]
> }
> ```
>
> and use `--preset windows-x64-local` for both commands. The `VCToolsVersion` environment override is only needed if your default MSVC toolset install is incomplete — set it to a version present under `VC\Tools\MSVC\`, or omit it.

### Releases & CI

The GitHub Actions workflows inherited from obs-plugintemplate build and package the plugin (zip/installer) on push and tags — see the template's [wiki](https://github.com/obsproject/obs-plugintemplate/wiki) for the release flow.

## How it works

1. A lightweight tracker enumerates top-level windows every frame (`EnumWindows`, which yields z-order), resolves their owning process image (`QueryFullProcessImageNameW`, cached), and keeps the set matching your selection — including any per-entry title filter. A window whose title stops matching (e.g. a browser tab switch) vanishes from the canvas immediately, but its capture session is kept warm for a few seconds so it reappears instantly if the title switches back. All of this idles completely while the source isn't visible anywhere (see the pause option).
2. Each matched window gets its own `Windows.Graphics.Capture` session on OBS's D3D11 device; frames are copied straight into OBS textures — windows are captured whole even when occluded or moved off-screen.
3. On render, the textures are drawn bottom-to-top at each window's DWM frame position (`DWMWA_EXTENDED_FRAME_BOUNDS`), premultiplied-alpha-blended onto the transparent monitor-sized canvas, cropped by the invisible resize borders and clipped to the monitor.

## License

[GPL-2.0-or-later](LICENSE)

## Acknowledgements

- [obsproject/obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) — project scaffolding
- OBS Studio's `libobs-winrt` — reference for the WGC ↔ D3D11 ↔ libobs interop
