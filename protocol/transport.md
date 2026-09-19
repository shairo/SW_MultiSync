# Transport framing, channels, fragmentation

Part of the [protocol index](../protocol.md). What one `SendMessageToUser` call carries, before the
record level. Decoded live 2026-09-12 on v1.15.23 and verified across the captured sessions.

## Frame header (24 bytes, then records)

```
+0  u32  laneOrFlag   0 for a self-contained single-frame message (all gameplay type=8).
                      For BULK transfers it takes values 0..3 — a lane/stream id, NOT a
                      simple head/continuation flag (see "Bulk" below). Do not assume 0/1.
+4  u32  bodyLen      = cubData - 8 for single-frame messages (holds for every gameplay pkt)
+8  u32  =1           protocol constant
+12 u32  msgType      6=version, 7=?, 8=game-update, 9=tile transfer, 16=file-transfer, ...
+16 u32  tick         monotonic frame/sequence counter (world tick)
+20 u32  subType      always 0x10 for msgType 8
+24 u32  recordCount  number of records that follow
+28..    records[]
```

The first 8 bytes are the transport prefix; everything from `+8` on is the **message body**.
[reference.md](reference.md) describes the `msgType=8` envelope with body-relative offsets
(`+0 const1, +4 msgType, +8 tick, +12 subType, +16 recordCount, +20 records`), i.e. shifted by 8
from the table above.

Most ticks send `recordCount=0` (empty heartbeat, 28 B on the wire).

## Channels

- **ch0** — state / version / tiles, and the live gameplay stream (`msgType=8`).
- **ch1** — reliable addon/mod file push (`/data/components`, `/mod.png`, `/mod.xml`,
  `/data/definitions`).
- **ch15** — 1-byte control: session open = `0x01`, close = `0x02`.

## Fragmentation — who splits what (confirmed 2026-09-12, `swcap frag`)

**Every gameplay `msgType=8` message is a single frame** (body 20..~16 KB, well under Steam's
message cap), so reassembly for the gameplay stream is a no-op. Across light and heavy-vehicle
captures type=8 stayed single-frame up to 16,792 B with 0 game-level fragments; ch1 mod files went
180,053 B in one frame too. The game hands large messages to Steam in ONE `SendMessageToUser` call
and **Steam does the wire-level fragmentation** up to
`k_cbMaxSteamNetworkingSocketsMessageSizeSend = 524288` (512 KB).

**Design rule for edit/inject:** build the whole logical message and send it with a single
`SendMessageToUser` call; as long as it stays under ~512 KB (normal gameplay always does), Steam
fragments it. Do NOT reimplement the game's lane-multiplex splitting.

## Bulk transfers (not decoded, deliberately)

The only game-level splitting is the map-tile transfer (`msgType=9`, ~2.5 MB > 512 KB), which the
game hand-chunks at **523,272 B/frame** and multiplexes across lanes (`+0` cycles 0,1,2,3). Only the
first frame of a chunk carries a header; continuation bytes look like random `msgType` values, so
**filter frames by size before parsing**.

An earlier guess (flag=0 head + flag=1 continuation) was wrong and silently dropped ~3,000 frames.
The analyzer now refuses to guess: `Message.Reassemble` emits only genuine single-frame messages as
`Complete`; every other frame passes through as `Bulk=true` (raw bytes kept) so nothing is lost.
Bulk decoding is asset download, not gameplay, and not on the path to editing game state.
