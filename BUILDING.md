# Building GestureSwitch

GestureSwitch is plain C (C11) for Windows x64, built with GCC from MinGW-w64. There are no third-party libraries:
it uses only the Windows API (kernel32, user32, and the XInput headers).

## Requirements

- **Windows 10 or 11, x64.**
- **GCC for MinGW-w64, x86_64, UCRT runtime.** The release was built with WinLibs GCC 16.2.0
  (MinGW-W64 x86_64-ucrt-posix-seh, r1), from https://winlibs.com or with
  `winget install BrechtSanders.WinLibs.POSIX.UCRT`. `gcc` must be on the PATH (build.bat also looks in WinGet's
  WinLibs folder).
- **Windows PowerShell 5.1** (included with Windows): the build checks and translates the menu file with it.

## Build

From this folder, in a Command Prompt:

```
build.bat
```

This makes:

- `build\GestureSwitch.dll`: the mod, loaded by the f2g DLL Mod Loader (crloader) from
  `crmods\GestureSwitch\`. This is the file in the release.
- `build\hid.dll`: the same mod as a standalone `hid.dll` proxy (not in the release).
- `build\gestureswitch.menu.json`: the Mod Settings Menu page, with its translations merged in.

To make the release zip as well:

```
build.bat package
```

which writes `build\GestureSwitch-<version>.zip` (the DLL, the menu page, README.md and CREDITS.md).

`build.bat deploy` copies the build into a game folder; it uses the `GAME` path at the top of build.bat and is
only for development.

## What the build does

1. Checks `menu\gestureswitch.menu.json` is strict JSON (`menu\check_menu.ps1`).
2. Merges the translations (`menu\*.tsv`, one file per language) into `build\gestureswitch.menu.json`
   (`menu\translate.ps1`) and checks the result the same way.
3. Compiles the sources in `src\` with:
   `gcc -std=c11 -O2 -Wall -Wextra -shared -static -s -Wl,--exclude-all-symbols -o build\GestureSwitch.dll src\*.c`
   (all of `src\*.c` except `proxy_hid.c`, which only goes into the standalone `hid.dll`).

## What the mod does at runtime

It runs inside the game process only. It finds a few game functions by byte patterns (`src\scan.c`), redirects the
game's calls to them to its own functions (`src\hook.c`), and replaces some import-table entries (XInput, Raw Input)
to read and adjust controller, keyboard and mouse input. It reads and writes only its own settings files
(`GestureSwitch.ini`, the Mod Settings Menu's `ModMenuConfig\gestureswitch.ini`), the game's keybind file
(`%LOCALAPPDATA%\Remedy\CONTROLResonant\input.json`, read only) and its log. It has no network code.
