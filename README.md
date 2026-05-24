# IdeaCatcher

A tray-resident corner overlay for capturing fleeting ideas on Windows. Press a hotkey, type, press Enter, get back to what you were doing.

## Features

- **Always available** — lives in the system tray, summons via a global hotkey (default `Ctrl + Shift + Space`)
- **Borderless transparent overlay** — docked to any screen corner, real per-pixel transparency via DirectComposition
- **Tag prefix** — type `[EDIT] cut the intro` and the tag becomes a labeled pill on the card
- **Cards stack from the bottom** — newest sits right above the typing field and pushes older ones up
- **Persistent** — ideas and settings saved to `%APPDATA%\IdeaCatcher\`
- **Cheap** — ~50 MB RAM, ~5 % of one core when visible, near zero when hidden

## Build

Requires:
- Windows 10 or 11 (DirectComposition + Win10+ APIs)
- [MSYS2](https://www.msys2.org) with the UCRT64 toolchain (`pacman -S mingw-w64-ucrt-x86_64-gcc`)
- PowerShell 5+ (built into Windows)

The build script expects `g++` at `C:\msys64\ucrt64\bin\g++.exe`. If yours is elsewhere, edit `$gpp` at the top of `build.ps1`.

```powershell
.\build.ps1
```

Or via the `.bat` wrapper:

```cmd
build.bat
```

Output: `IdeaCatcher.exe`, ~4.7 MB, statically linked (no runtime DLL dependencies beyond Windows itself).

## Run

1. Double-click `IdeaCatcher.exe`. The tray icon appears; the overlay opens immediately on first launch.
2. Press the global hotkey to toggle it after that.
3. Type an idea — optionally prefix `[TAG]` to label it. Enter submits, Esc hides.
4. Right-click the tray icon → **Settings** to change the hotkey, choose a corner, or pick a size preset.

To auto-launch on login, drop a shortcut to `IdeaCatcher.exe` into the folder `shell:startup` (paste that path into Explorer's address bar).

## Data

| File | Location | Purpose |
|------|----------|---------|
| `ideas.json` | `%APPDATA%\IdeaCatcher\` | Your captured ideas |
| `settings.json` | `%APPDATA%\IdeaCatcher\` | Hotkey, corner, size |

Both are plain JSON — edit by hand if you need to. The app reads them on startup and rewrites on every change.

## Stack

Single C++17 translation unit (`src/main.cpp`) built against:

- **Win32** — window, tray icon (`Shell_NotifyIconW`), global hotkey (`RegisterHotKey`)
- **DirectX 11** — rendering
- **DirectComposition** — per-pixel alpha. HWND swap chains can't carry alpha to the desktop compositor; only a DComp visual can.
- **Dear ImGui** (vendored at `third_party/imgui`) — the UI
- **nlohmann/json** (vendored at `third_party/json/json.hpp`) — persistence

## License

MIT. ImGui and nlohmann/json are vendored under their own MIT licenses — see their respective `LICENSE.txt` files inside `third_party/`.
