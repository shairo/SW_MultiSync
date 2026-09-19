# How the hook works

Where `swhook.dll` attaches to `server64.exe` and why that single seam is enough for capture, edit
and inject. Purpose and relay design: [目的と設計.md](目的と設計.md). Wire format:
[protocol.md](protocol.md) / [protocol/transport.md](protocol/transport.md).

## Background

This project is the successor to `../sw-serverhook`, which hooked `ws2_32.dll`'s `sendto` to
reverse the dedicated-server wire protocol. As of **v1.15.23 (2026-09) "The Multiplayer API
Update"** the game moved multiplayer off the original Steam networking onto the current Steam
networking API. Verified against the shipped `server64.exe`: it uses
**`ISteamNetworkingMessages` (interface version string `SteamNetworkingMessages002`)** — not
`ISteamNetworkingSockets`, and not plain `sendto` for P2P payloads. The old sendto hook no longer
sees game packets.

## The one seam

Everything — capture, edit, inject extra server→client records — goes through a single method:

    ISteamNetworkingMessages::SendMessageToUser(
        const SteamNetworkingIdentity &identityRemote,
        const void *pubData, uint32 cubData,
        int nSendFlags, int nRemoteChannel)

vtable index **0** (first method in `isteamnetworkingmessages.h` declaration order). x64 calling
convention: `rcx=this, rdx=&identityRemote, r8=pubData, r9=cubData`, stack: `nSendFlags,
nRemoteChannel`.

- **Capture** = read `pubData/cubData` in the hook (written to `.swcap`, see
  [tools/swcap/FORMAT.md](tools/swcap/FORMAT.md)).
- **Edit** = build a modified copy of the buffer and pass that to the original (the caller's buffer
  is never touched).
- **Inject** = append records to the message that is going out this tick (we never call
  `SendMessageToUser` for a message of our own — see 目的と設計.md §4).

The server calls it **once per recipient**, so one hook sees every peer's stream. This is what makes
cross-peer relay possible in-process.

Receive side (if ever needed) is `ReceiveMessagesOnChannel` at vtable index 1; nothing uses it.

## Getting the interface pointer

`server64.exe` resolves the interface via `SteamInternal_FindOrCreateGameServerInterface(hUser,
"SteamNetworkingMessages002")` (both symbols are present in the exe). The injected DLL makes the
SAME call: `FindOrCreate` returns the game's existing singleton, so we get the identical vtable.
This must run after the game's `SteamInternal_GameServer_Init`; we gate on
`SteamGameServer_GetHSteamUser()` returning nonzero (polled on a worker thread).

Then `vtable[0]` is swapped in place (`VirtualProtect` the vtable page, write our thunk). One
instance, so a vtable swap is enough — no inline/trampoline library. The original pointer is saved
and called through.

No Steamworks SDK headers are vendored: `src/hook/steam_min.h` declares only the few types/offsets we
touch.

## Injection / unload

- `inject.exe` / the GUI / `swctl inject` load the DLL with `LoadLibrary` via `CreateRemoteThread`
  (`src/inject/injector.h`), requesting only the process rights that needs.
- The DLL is single-instance per process (named mutex). The IPC listener comes up before the Steam
  wait, so the control plane can be tested by loading the DLL into any process.
- `swctl unload` (IPC `unload`) restores the vtable, waits for in-flight hook calls, joins the
  IPC/worker threads, closes files and the mutex, then `FreeLibraryAndExitThread`s from its own
  thread. Dev loop without a server restart: `swctl unload` → `build.bat` → `swctl inject`.
