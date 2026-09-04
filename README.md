# WowRadio

In-game internet radio for World of Warcraft 3.3.5a (build 12340), built for
[Out of Bounds Radio](https://www.outofbounds.live) on private servers.

A single DLL streams an Icecast station inside the game client and exposes a
small Lua API to addons. A companion addon gives players a minimap button with
on/off, volume and the current track.

## How it fits into a client

The 3.3.5a client only loads DLLs that its executable asks for, so the game
executable needs to import `WowRadio.dll`. How that import is added is up to
the server team; it is the same kind of change private servers already make
to their client executable. A helper script in `tools/` shows one way to do
it. The DLL exports a single symbol, `WowRadio_Loaded`, for the import to
bind to; all of its work happens when it is loaded.

Nothing else is replaced or renamed. Uninstalling is deleting the DLL and
launching an executable that does not import it.

What ends up in the game folder:

| File | Purpose |
|------|---------|
| `WowRadio.dll` | The radio engine. Self-contained: BASS is embedded, the C runtime is statically linked. Depends only on kernel32, user32 and shell32. |
| `Interface\AddOns\OOBRadio\` | The addon players interact with. Optional; any addon can drive the Lua API. |

## What the DLL does

The DLL has three jobs.

1. **It gives addons a way to control the radio.** When the game builds its
   addon environment, it registers the list of functions addons are allowed to
   call. The DLL steps in at that moment and adds six `Radio_*` functions to
   the list, then lets the game carry on as normal. Because it happens every
   time the game rebuilds that environment, the functions are still there
   after a `/reload`.

2. **It lets the game accept those functions.** The game double-checks that
   any function it is about to call lives inside its own program, which would
   normally rule out anything added by a DLL. The DLL tells the game to accept
   functions from the DLL as well. This check exists to catch corrupted
   function tables, not to keep addons in line; addons cannot create such
   functions themselves, so widening it does not open anything to them.

3. **It plays the stream.** A background thread connects to the station,
   decodes the audio and plays it through Windows, using the BASS audio
   library that is packed inside the DLL, so there is nothing else to install.
   The current track name is read from the stream itself and handed to the
   addon.

The stream plays alongside the game's own sound rather than through it, so
the game's music and volume settings do not affect it on their own. An addon
can manage that, for example by turning the game's music off while the radio
is on and passing the player's volume settings to `Radio_SetVolume`. The
OOBRadio addon in this repository does exactly that, but the DLL does not
depend on it.

### Lua API

| Function | Behaviour |
|----------|-----------|
| `Radio_Play([url])` | Start the stream. URLs must match the allow-list in `main.cpp`; anything else keeps the current URL. Returns `true`. |
| `Radio_Stop()` | Stop and free the stream. |
| `Radio_SetVolume(0..1)` | Stream volume. |
| `Radio_SetBackgroundMute(bool)` | Mute while the game window is not in the foreground. |
| `Radio_GetStatus()` | Returns `state, title, url, volume, error`. State is `stopped`, `connecting`, `playing` or `error`. |
| `Radio_GetVersion()` | Version string. |

All Lua entry points run on the game thread and never block; network and BASS
calls happen on the worker thread.

## Building

Requirements: Visual Studio Build Tools (MSVC, x86 target) with CMake and
Ninja, which the Build Tools installer bundles. Python 3 with `lief` for the
exe patcher.

```
build.cmd
```

produces `build\WowRadio.dll`. The script runs `vcvars32.bat` first; adjust
the path at the top if your Build Tools live elsewhere.

## Repository layout

```
src/main.cpp            the DLL: hooks, Lua API, worker thread
src/bass_loader.*       loads the embedded bass.dll from memory
src/resources.rc        embeds third_party/bass/bass.dll
tools/patch_exe_import.py  example: adds the WowRadio.dll import to an exe
addon/OOBRadio/         the player-facing addon
third_party/bass/       BASS 2.4 (un4seen), see bass.txt for licence terms
third_party/MemoryModule/  in-memory DLL loader (MPL 2.0)
```

## Licence

MIT for this project's own code. BASS and MemoryModule keep their own licences
in `third_party/`.
