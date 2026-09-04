# Agent notes for WowRadio

In-game internet radio for WoW 3.3.5a (build 12340). One self-contained DLL
streams an Icecast station inside the client and exposes `Radio_*` functions
to addon Lua; an addon gives players a minimap button. See README.md for the
reader-facing description.

## Layout

- `src/main.cpp` — the DLL: client addresses, the two startup patches, Lua API, worker thread.
- `src/bass_loader.*` — loads the bass.dll embedded via `src/resources.rc` (MemoryModule, disk fallback).
- `tools/patch_exe_import.py` — example of adding the `WowRadio.dll` import to a game exe (needs `lief`).
- `addon/OOBRadio/` — the player-facing addon (3.3.5a Lua 5.1, interface 30300).
- `third_party/` — BASS (un4seen licence) and MemoryModule (MPL 2.0). Do not modify.

## Build and test

- `build.cmd` → `build\WowRadio.dll`. MSVC x86 via `vcvars32.bat`, CMake + Ninja from VS Build Tools.
- The DLL must stay single-file and self-contained: BASS embedded, CRT static (`MultiThreaded`),
  imports limited to kernel32 / user32 / shell32. Check with a PE import viewer after changes.
- To test: patch a copy of a 3.3.5a exe with the tool, drop `WowRadio.dll` and the addon into the
  game folder, launch the patched exe. `wowradio.log` next to the DLL should show
  `hook installed: yes`, then `Radio_* Lua functions registered` after entering the world, then
  `BASS ... initialised (in-memory)` on first play. `/reload` in game must keep the functions.
- Never commit `.exe` files or logs (`.gitignore` enforces it).

## Rules that matter

- Client addresses in `src/main.cpp` are for build 12340 only. Every memory patch verifies the
  expected bytes first and backs out on mismatch; keep that property for any new patch.
- Lua entry points run on the game thread and must not block: no BASS, no network, no I/O beyond
  the log. Hand work to the worker thread through the atomics.
- `Radio_Play` only accepts URLs on the allow-list; keep it that way.
- Addon code is Lua 5.1 for the 3.3.5a client: `self` in handlers, `#`, no modern APIs.

## Writing conventions

- Comments: light, explain why, not what. Keep the header comment in `main.cpp` accurate.
- README is for humans: functional language, no memory addresses, no server-specific instructions
  (how a server patches its exe is their choice).
- Commit messages: plain, human style.
