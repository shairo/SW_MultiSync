# Reference: envelope, walker, tools, scenario mapping

Part of the [protocol index](../protocol.md). Server→client SEND, ch0, `msgType=8` unless noted.

## Message envelope (msgType=8)
```
+0  u32 const1     = 1
+4  u32 msgType    = 8
+8  u32 tick       monotonic frame counter (world tick)
+12 u32 subType    = 0x10
+16 u32 recordCount   (counts compound records 0x2A/0x96/0x38 as 2 — see lifecycle.md)
+20      records[recordCount]
```
Each record starts with a `u32 tag`. To walk a batch you must know each tag's length rule.
"% fully walked" (records consume the body exactly, no overrun/leftover) is the KPI in `swcap validate`.
Length rules live in **`tools/swcap/Records.cs`** (`Records.Decode`) — the single source of truth.

## Multiplayer
Server calls `SendMessageToUser` once per recipient, so a capture interleaves N per-peer streams keyed by
destination SteamID. **Always split by `--peer <SteamID>`** — merged views cause mis-reads (e.g. a
vehicle's dETA looks huge only because distant-observer records are mixed in). See
`tools/swcap/README.md` for the analysis playbook.

## Periodic / always-on tags — length known, meaning TBD
| tag | length | notes |
|-----|--------|-------|
| 0x05 5   | fixed 34 | periodic; u32 id (=0x3E) + 26 B mostly zero |
| 0x07 7   | fixed 71 | seat-entry snapshot broadcast: tag · u16 peer_id · zeros · f32 yaw · f32 pitch · zeros — mirror of client 0x01 (client.md) |
| 0x09 9   | fixed 4  | bare tag / flag |
| 0x62 98  | fixed 12 | vehicle load companion: tag · u32 20000 · u32 5 — one per 0x2B placement (lifecycle.md) |
| 0x94 148 | fixed 8  | tag + u32 vehId; HIGH freq with MULTIPLE vehicles |
| 0x47 71  | 16 + u32@12 | tag + i32 tileX + i32 tileZ + u32 payloadLen + payload — the tile's dynamic state, sent after the client acks a 0x45 tile load (see below) |
| 0xA6 166 | fixed 22 | seat KEY STATE broadcast: tag + u16 peer_id + u32 keyMask + 12 B zero (mirror of client 0x64, see client.md) |
| 0xA7 167 | fixed 11 | seat AXIS broadcast: tag · u16 peer_id · u32 axisIdx 0..7 · u8 value — mirror of client 0x65 (runs of 8 at seat enter) |
| 0xA8 168 | fixed 22 | player LOOK broadcast: tag · u16 peer_id · 8 B zero · f32 yaw · f32 pitch — mirror of client 0x66, ~every 16 ticks while the view moves |

## Tile / world-grid streaming — 0x45 / 0x46 / 0x47 / 0x49 / 0x4A  ✅ length, ✅ meaning (updated 2026-09-22)
**Server-driven tile set (session_20260913_214246_776_freeze, 4 peers, 63 s):** the 0x45/0x46/0x49
stream is **byte-identical to every peer** (195 records each) — it is the SERVER's loaded-tile set,
broadcast, not per-recipient. The set is the union of a **radius-2 ring (5×5, 1 km tiles,
index = floor((pos+500)/1000))** around every player's vehicle, re-evaluated ~1–2 s behind the
server-side position. Per tile:
```
0x45 (x,z,0)  server: LOAD tile        →  client 0x28 (x,z) ack, 60–120 ms later
                                      →  server 0x47 (x,z, len, payload): the tile's dynamic state
                                         (len 0 = sea, 5 = one entry `00 A0 00 00 00`, up to ~1 KB —
                                         the same `u8 1·u32 width·value·u8 0·u32 idx` list as 0x2E)
0x46 (x,z)    server: UNLOAD tile      (no answer)
0x49 (x,z)    with 0x45: the tile was never loaded/revealed before in this session (18 of 88)
```
88 × 0x45 ↔ 88 × client 0x28 ↔ 88 × 0x47, fully paired even on the frozen client (it keeps acking
tile loads while its simulation is stopped — the ack comes from the network thread). A tile a
player ENTERS is normally already in the set (loaded when it came within 2 tiles); entering it
sends nothing tile-specific to that client.

Earlier (solo, 2026-09-17) reading, still valid for the fog case:
**Fog of war (session_20260917_012608):** as the player moves, the *client* sends 0x28 (`i32 tileX ·
i32 tileZ`) for every tile whose fog it cleared (an L-shaped ring edge on each tile crossing); the server
answers each with **0x49 (x,z) + 0x45 (x,z,u8 0)**. So 0x49/0x45 = "fog revealed" broadcast, client
driven. 0x46 comes ~150 ms later for a different (outer) ring with no client trigger, followed by 0x47
data runs — that part is the loading/streaming ring.
**Tile purchase (012747 / 013059):** client 0x19 (`i32 x · i32 z · u8 0`) → **0x4A (12 B: x,z) + 0x62**;
"buy all" = client 0x1A (tag only) → 30 × (0x4A + 0x62) in one message (recordCount 61).
**"Clear all fog" (013020)** sends no tile records at all — only the settings bool array (0x48 → 0x88,
index 1 flips to 1), see settings.md.

Original notes:
Fixed-size **grid cells** streamed in interleaved back-to-back runs as the player moves across the world's
1 km tile grid. Each cell starts with its own tag, so fixed lengths let the walker consume a whole run:
| tag | length | layout |
|-----|--------|--------|
| 0x46 70 | fixed 12 | `tag + u32 tileX + i32 tileZ` |
| 0x49 73 | fixed 12 | `tag + u32 tileX + i32 tileZ` |
| 0x45 69 | fixed 13 | `tag + u32 tileX + i32 tileZ + u8 flag` |
| 0x4A 74 | fixed 12 | `tag + i32 tileX + i32 tileZ` — TILE PURCHASED, always + 0x62 |
**`index` = tile X, `value` = tile Z** — the 1 km tile's grid coordinates (small signed ints). Verified
because the cells always bracket the player's own tile: at player tile (11, −9) the burst covered
X∈{10..14}, Z∈{−10..−6}, and on another flight (player tile X=5) the indices were 5,6,7,8. Within a burst
one axis is held while the other scans (an L-shaped ring edge = the row+column newly entering the loaded
ring on a crossing).

**Pairing:** **0x49 and 0x45 come as a pair carrying the *same* (X,Z)** (0x49 first, then 0x45 with its
extra 1-B flag) — read as "reference tile (0x49) + tile state/flag (0x45)". **0x46** is emitted on its own.

Confirmed on the long flight `session_20260915_222504` (100 % walked): a burst fires **once per ~1 km tile
crossing** (≈ 285 ticks/km; bursts lag each crossing by a few ticks), ~10–14 cells per crossing. Whole
flight (~135 s, ~35 crossings) cell counts: **0x46 = 181, 0x45 = 181, 0x49 = 133**. This is the client's
**loaded-tile ring bookkeeping**, not the tiles' contents — per-tile entities (vehicles / objects) stream
separately via the load family (0x2B/0x2D/0x2F + 0x4D), and per-tile state (tree/door/oil) is not in these
cells. Previously mis-filed as combat tags.

## Tree felling — 0x3F / 0xAD  ✅ length, ✅ meaning (0x3F) / △ (0xAD)
Knocking down a field tree. Emitted as a pair: **0x3F** (the felled event) then **0xAD** ~6 ticks later
(fall physics).
```
0x3F (63) fixed 26:  tag + u32=0 + u32 tileX + i32 tileZ + u32 treeIndex + u32 type + u16
0xAD (173) fixed 20: tag + u32 + float[3]   (fall direction / impulse)
```
`tileX,tileZ` are the same 1 km tile grid coords as the streaming cells above; `treeIndex` is the tree's
id within that tile. Confirmed on `session_20260915_223959` (5 trees knocked down by a vehicle, all in tile
(7,−10), treeIndex 341/317/1182/391/393). So **per-tile tree state IS synced**, keyed by (tile, treeIndex).

## Not a record / unverified
- **0x00 (0)** — NOT a record. Was the zero tail of the 328-B 0x50 mis-split by the old fixed-76 rule;
  gone once 0x50 became variable.
- **0x100 (256), 0xA9 (169)** — likely walker-desync garbage after an undecoded tag. Unverified; confirm
  they are real records before decoding.

## Scenario → tag mapping (2026-09-12 captures S0–S9)
Isolated by capturing one action per file and diffing against **S0 (idle, no vehicle)**:
- **S0 idle**: 05, 07, 09, 29, 2D, 2E, 4D, 81, 8E, A6, A7, A8 (periodic/always-on).
- **any vehicle present**: 0x1B(27).
- **spawn**: 0x4D(77) + 0x47(71) + 0x29(41) at the spawn tick.
- **equip (S4)**: 0x29(41) + 0x20(32).
- **buttons/switches (S6)**: 0x14(20), 0x15(21)+0x61(97), 0x55(85), 0x1A(26).
- **seat/interaction (S3)**: 0x14(20), 0x15(21), 0x61(97).
- **damage/death/respawn (S9)**: 0x67(103), 0x09(9), 0x55(85), 0x50(80), 0x26(38), 0x28(40), (0x88 / 0x8B were misfiled here — they are game-settings broadcasts, see settings.md).

KPI: scenario captures walk 99.1–100 %, combat file 99.5 %, 0 overruns. Technique: many "variable" tags
were a fixed short record `solve` had merged with a following unknown; once the neighbour decoded, the
true fixed length showed (0x19, 0x2C, 0x30, 0x34, 0x3B, and the 0x2D/0x2F push family).

## Tools
- `swcap validate <file>` — walk all records; % fully walked, per-tag OK/overrun/unknown, and which tag
  stops each incomplete walk (the next rule worth decoding).
- `swcap walk <file> --seq N | --tag N` — record breakdown of one message + hex of the undecoded tail.
- `swcap lens --tag N` — ground-truth length from messages where N is the LAST record.
- `swcap solve --tag N` — for every message blocked at N, finds the end offset that lets the rest walk to
  the body end (works mid-batch). Prints a length histogram + u32-length-field hint. Confirmed 0x4D's
  `12 + u32@8` and the spawn cluster (0x96/0x37/0x2B) lengths.
- `--peer <SteamID>` on validate/census/when/msgs/frames — isolate one recipient.
- Full analysis playbook: `tools/swcap/README.md`.
