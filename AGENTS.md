# AGENTS.md

Context for coding agents. Keep short.

## What this is

A fresh, deliberately-minimal successor to `../sw-serverhook`. That project hooked
`ws2_32.dll`'s `sendto` to reverse the Stormworks dedicated-server wire protocol. As of
**v1.15.23 (2026-09) "The Multiplayer API Update"** the game moved multiplayer off the
8-year-old original Steam networking onto the **current Steam networking API**. Verified against
the shipped `server64.exe`: it now uses **`ISteamNetworkingMessages` (interface version string
`SteamNetworkingMessages002`)** — NOT `ISteamNetworkingSockets`, and NOT plain `sendto` for P2P
payloads anymore. So the old sendto hook no longer sees game packets.

## The one seam we hook

Everything the user wants — capture, edit, and inject extra server→client packets — goes through
a single method:

    ISteamNetworkingMessages::SendMessageToUser(
        const SteamNetworkingIdentity &identityRemote,
        const void *pubData, uint32 cubData,
        int nSendFlags, int nRemoteChannel)

vtable index **0** (first method after the compiler-generated layout; see
`isteamnetworkingmessages.h` declaration order). x64 calling convention:
`rcx=this, rdx=&identityRemote, r8=pubData, r9=cubData`, stack: `nSendFlags, nRemoteChannel`.

- **Capture** = read pubData/cubData in the hook.
- **Edit** = mutate the buffer (or swap the pointer) before calling the original.
- **Inject** = call SendMessageToUser ourselves on the same interface pointer.

Receive side (if ever needed) is `ReceiveMessagesOnChannel` at vtable index 1, but the goal is
server→client SEND, so we only need index 0.

## How we get the interface pointer

`server64.exe` resolves it via `SteamInternal_FindOrCreateGameServerInterface(hUser,
"SteamNetworkingMessages002")` (these symbols are present in the exe). We do the SAME call from
our injected DLL: `FindOrCreate` returns the game's existing singleton, so we get the identical
vtable. Must run AFTER the game's `SteamInternal_GameServer_Init`; we gate on
`SteamGameServer_GetHSteamUser()` returning nonzero (poll on a worker thread).

Then we swap `vtable[0]` in place (VirtualProtect the vtable page, write our thunk). One
instance, so vtable swap is enough — no inline/trampoline lib needed. Original is saved and
called through.

## Toolchain (no external deps)

MSVC BuildTools x64 (`cl.exe` under
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\...`). `build.bat` calls
`vcvars64.bat` then compiles. No Steamworks SDK headers vendored — we declare only the few
types/offsets we touch (kept in `steam_min.h`), because the real headers aren't needed to swap
one vtable slot and log a buffer.

## Layout

- `src/hook/` — the injected DLL (`swhook.dll`): DllMain, interface-pointer acquisition, vtable
  swap, SendMessageToUser thunk, logging.
- `src/inject/` — a tiny CLI injector (`inject.exe`): opens `server64.exe`, LoadLibrary our DLL
  via CreateRemoteThread. Kept separate and dumb.
- `build.bat` — builds both.

## Status

PoC / observation-first. First milestone: hook installs and logs every send (size, flags,
channel, first bytes, remote SteamID). Edit + inject come after the log confirms the seam.

## Wire format (decoded live, 2026-09-12, v1.15.23)

Header is 24 bytes, then body. Verified across 6937 captured packets:

    +0  u32  laneOrFlag   0 for a self-contained single-frame message (all gameplay type=8).
                          For BULK transfers it takes values 0..3 — a lane/stream id, NOT a
                          simple head/continuation flag (see "Bulk" below). Do not assume 0/1.
    +4  u32  bodyLen      = cubData - 8 for single-frame messages (holds for every gameplay pkt)
    +8  u32  =1           protocol constant
    +12 u32  msgType      6=version, 7=?, 8=game-update, 16=file-transfer, ...
    +16 u32  tick         monotonic frame/sequence counter
    +20 u32  subType      always 0x10 for msgType 8
    +24 u32  recordCount  number of records that follow
    +28..    records[]

Channels: ch0 = state/version/tiles, ch1 = reliable addon/mod file push
(`/data/components`, `/mod.png`, `/mod.xml`, `/data/definitions`), ch15 = 1-byte control
(session open=0x01 / close=0x02).

### type=8 record taxonomy (corrected 2026-09-12 via `swcap rec`)

The `@20` u32 at the record start is a **record-KIND tag, not a per-object id** (earlier guess was
wrong). Each type=8 message is a batch of `recordCount` records; most ticks send recordCount=0
(empty heartbeat, body 20). Kinds seen so far:

| @20 kind | shape | content |
|----------|-------|---------|
| 0x8E 142 | recs=1 | HUD text widgets: `SPD\n56.16km/h`, `tickrate`→`TPS:60(100%)...` (u16-len + ascii label) |
| 0x05   5 | recs=2, ~every 16 ticks, 68 B | periodic (clock/environment?) |
| 0x29  41 | recs=5 | `equip` + floats |
| 0x4D  77 | recs=7, ~1178 B | vehicle physics: many float32; carries an object id sub-field (`3E 0A 00 00` = 0x0A3E) |
| 0x81 129 | recs=3, 129 B | physics-ish floats |

So the real per-object identity lives INSIDE certain records (e.g. 0x0A3E in the 0x4D vehicle
record), while @20 says which category of update it is. Record-length rules per kind are still
undecoded — needed before recordCount>1 batches can be split. Easiest/safest edit target: 0x8E HUD
text. Real state: 0x4D vehicle physics floats. Tools: `swcap rec` (list records-bearing messages),
`swcap track --id N` (follow one kind over time; float decode only valid for matching layouts).

**ch0 msgType=8 is the live gameplay stream** (2911/6937 pkts). Record batch: each record is
`{u32 id, fields, optional [u16 len + ascii label], floats}`. Seen: id 0x8E carries HUD text
widgets — `"tickrate"` → `"TPS:60(100%)..."` overlay, `"SPD\n56.16km/h"` speedometer. 28-byte
type=8 = empty heartbeat (recordCount=0) every tick. The 523272-byte ch0 packets are map-tile
bulk transfer (fragmented; only the first fragment has a header) — noise for gameplay RE.

Bulk-transfer 523272-byte fragments break a naive msgType read (continuation bytes look like
random types); filter them out by size before parsing.

## Fragmentation status (corrected 2026-09-12)

**Every gameplay msgType=8 message is a single frame** (body 20..5013 B, well under Steam's
~512 KB message cap), so for the gameplay stream reassembly is a no-op and trivially correct.

**Fragmentation ownership — CONFIRMED (2026-09-12, `swcap frag`).** The game does NOT split
gameplay type=8: across light and heavy-vehicle captures, type=8 stayed single-frame up to
16,792 B with 0 game-level fragments; ch1 mod files go 180,053 B in one frame too. So the game
hands large messages to Steam in ONE SendMessageToUser call and **Steam does the wire-level
fragmentation** up to its cap `k_cbMaxSteamNetworkingSocketsMessageSizeSend = 524288` (512 KB).
The ONLY game-level splitting seen is the tile transfer (type=9, 2.5 MB > 512 KB), which the game
hand-chunks at 523272 B/frame and multiplexes across lanes (+0 = 1,2,3,4).

**Design rule for edit/insert (phase 4):** build the whole logical message and send it with a
single SendMessageToUser call; as long as it stays under ~512 KB (normal gameplay always does),
Steam fragments it — do NOT reimplement the game's lane-multiplex splitting. Re-fragmentation is
only ever needed above the 512 KB cap, which gameplay doesn't reach.

**Multi-frame BULK transfers are NOT yet decoded.** The map-tile transfer (type=9) and large mod
files send ~523272-byte frames whose +0 field cycles through 0,1,2,3 (~1030 each in the sample) —
a lane/stream id, not a 0/1 head/continuation flag. An earlier guess (flag=0 head + flag=1
continuation, "2519136 = 4×523264+…") was WRONG and silently dropped ~3095 frames. The C# tool now
refuses to guess: `Message.Reassemble` emits only genuine single-frame messages as Complete; every
other frame passes through as `Bulk=true` (unmerged, full raw bytes kept) so nothing is dropped and
the bulk scheme can be studied on its own later. Bulk decoding is deferred — it's asset download,
not gameplay, and not on the path to editing server→client game state.

## Capture container (.swcap) + bulk thinning

Binary, one file per session. Header `SWCAP` + version byte. Per event: `u8 dir | u64 seq |
u64 tickMs | u64 steamId | i32 ch | i32 flags | u32 origCub | u32 storedCub | bytes[storedCub]`
(v1 had no `storedCub`; the reader supports both). Metadata overhead is ~41 B/event — negligible
vs payload, so a .swcap ≈ the true application-layer payload handed to Steam (pre-transport; Steam
likely compresses on the wire, so actual network bytes are fewer).

**Bulk thinning** (DLL, `kBulkThreshold=65536`, `kBulkKeepHead=256`): frames larger than 64 KB are
stored header-only (first 256 B: transport header + msg header + any filename prefix), payload
dropped, but `origCub` still records the true size. Gameplay msgType=8 (≤~15 KB) is never thinned;
map tiles (523272 B) and big mod files are. This turns multi-GB tile-sync sessions into small
capture files while keeping every gameplay frame intact and all sizing accurate. Thinned frames
read back as `Frame.Truncated` and are always treated as `Bulk` (never single-frame complete).

## Tooling: tools/swcap (C# / net10.0 CLI)

Offline analyzer for `.swcap` capture files. `Container.cs` reads the DLL's binary format;
`Message.cs` reassembles (single-frame only, per above); `Program.cs` commands:
`stats` (dir/channel/type breakdown), `frames` (raw pre-reassembly), `msgs` (logical messages +
header fields), `hex --seq N [--body]` (hex dump a frame or its body). Run:
`dotnet tools/swcap/bin/Release/net10.0/swcap.dll <cmd> <file> ...`. GUI viewer + live-verify pipe
come later, on top of this library.
