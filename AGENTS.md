# AGENTS.md

Context for coding agents. Keep short; details live in the linked docs.

## What this is

A DLL injected into the Stormworks dedicated server (`server64.exe`) that hooks the one Steam
networking call every server→client packet goes through, and uses it to (1) capture the wire
protocol and (2) relay vehicle position records between peers so distant vehicles stop lagging by
seconds. Server-side only; clients are unmodified.

## Docs map

- [目的と設計.md](目的と設計.md) — purpose, client behaviour model, relay design + invariants (Japanese).
- [hooking.md](hooking.md) — the `SendMessageToUser` seam, interface-pointer acquisition, vtable swap, unload.
- [protocol.md](protocol.md) + [protocol/](protocol/) — record-level wire protocol; [protocol/transport.md](protocol/transport.md)
  for frame header / channels / fragmentation. Length rules: `tools/swcap/Records.cs` is the source of truth.
- [ipc-protocol.md](ipc-protocol.md) — control-plane (IPC) commands.
- [tools/swcap/README.md](tools/swcap/README.md) — capture analysis playbook; [tools/swcap/FORMAT.md](tools/swcap/FORMAT.md) — `.swcap` container.
- [README.md](README.md) — end-user overview; `packaging/README_*.txt` ships in the zip.

## Toolchain (no external deps)

MSVC BuildTools x64 (`cl.exe` under
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\...`). `build.bat` calls
`vcvars64.bat` then compiles. No Steamworks SDK headers vendored (`src/hook/steam_min.h` declares
the few types/offsets we touch). The analyzer `tools/swcap` is C# / net10.0
(`cd tools/swcap && dotnet run -c Release -- <cmd> <absolute path>.swcap`).

## Layout

- `src/hook/` — the injected DLL (`swhook.dll`): DllMain, interface-pointer acquisition, vtable
  swap, SendMessageToUser thunk, logging, `rec.h` (record walker, port of `Records.cs`), `relay.h`
  (relay + `Cfg` table), `stats.h` (per-peer counters), `ipc.h` (control-plane transport).
- `src/common/` — `version.h` (single version string) and `json.h` (dependency-free writer +
  flat reader), shared by the DLL and the C++ clients.
- `src/inject/` — `injector.h` (shared LoadLibrary-via-CreateRemoteThread core) and the tiny
  `inject.exe` wrapper around it.
- `src/swctl/` — `swctl.exe`, the CLI client: inject, status/peers/vehicles/watch, relay, capture,
  config. Reference implementation of `src/common/ipc_client.h`.
- `src/gui/` — `SWMultiSync.exe`, the Win32 GUI (no deps, requireAdministrator manifest): watches
  for server64.exe, auto-injects, then polls the DLL over IPC. Startup-only keys (`relay`,
  `capture`, `ipcPort`, `autoInject`) are edited on its startup tab by reading/writing swhook.ini
  directly via `relay.h`; everything else goes through IPC.
- `tools/swcap/` — offline analyzer; `tools/rectest/` — `test_rec` / `test_relay` apply `rec.h` /
  `relay.h` to real captures (walk failures must stay 0 after any relay change).
- `build.bat` — builds DLL, inject.exe, swctl.exe, SWMultiSync.exe.
- `package.bat` — build + stage `dist\SW_MultiSync_v<ver>\` + zip. Ships GUI, CLI, DLL, ini (with
  `relay=1` forced), `packaging/README_*.txt`, `ipc-protocol.md`. Version = `src/common/version.h`.
  Keep .bat files ASCII-only (cmd parses them in the OEM codepage).
- Dev loop without a server restart: `swctl unload` → `build.bat` → `swctl inject` (see hooking.md).
  Counters and relay caches reset. Dev aid only — not exposed in the GUI or the end-user README.

## Control plane (since 0.2 — no hotkeys)

All runtime control goes through **127.0.0.1:`ipcPort`** (JSON lines, see `ipc-protocol.md`):
relay on/off, capture start/stop/mark, config get/set/reload/save, stats, peers, vehicles,
version. The GUI, the CLI and any third-party tool use the same commands — never add a control
path that bypasses it. Config knobs are table-driven (`relay::kCfgFields`): adding one = a struct
member + one table row, and ini load/save + IPC pick it up automatically. The DLL exposes
cumulative counters only; clients compute rates. To test the IPC without the game, LoadLibrary the
DLL from any process: the listener comes up before the Steam wait.

## Working rules

- Relay changes: keep the invariants in 目的と設計.md §4 (append to the tick's own message, touch
  only fully-walked messages, edit a copy, world-tick arithmetic only, never change `bodyCount`,
  keep promise ETAs short). Re-run `test_relay` on the combat captures before calling it done.
- Protocol findings go in `protocol/*.md` + `Records.cs`, not here. Always analyse captures
  per recipient (`--peer <SteamID>`).
