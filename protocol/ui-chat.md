# UI, chat, character & interaction

Part of the [protocol index](../protocol.md). Server→client SEND, ch0, `msgType=8`.

## 0x01 (1) — chat / system message  ✅ length + semantics CONFIRMED
```
u32 tag = 0x01
u16 textLen ; char[textLen]   message body
u16 nameLen ; char[nameLen]   sender name
length = 4 + 2 + textLen + 2 + nameLen
```
Rides the normal frame-sync stream (not a separate transport). Confirmed on `session_20260914_230220_chat`:
`hoge`/`fuga`/`piyopiyo` with sender **`Shairo-jp`**; **`?echo xyz`** was a server command — the raw
`?echo xyz` appears on the **Recv** side and the broadcast came back as text **`xyz`** sender
**`[Server]`**. Also carries system logs (`Connect Restored`, `[WebMap]`) and addon logs (`[Damage]`).
Low frequency. The sender-name field gives a **SteamID → player-name** mapping.

## 0x8E (142) — popup / setPopupScreen  ✅ length + semantics CONFIRMED
```
u32 tag = 0x8E
u16 nameSize ; char[nameSize] name
u16 show                (1 for setPopupScreen)
u16 textSize ; char[textSize] text
float x, y, z, renderDistance   (16)
u32 uiId ; u32 vehicleParentId ; u32 objectParentId
length = 4 + 2 + nameSize + 2 + 2 + textSize + 16 + 12
```
Confirmed against `SPD\n0.00km/h` and the `tickrate` overlay. Matches old `Record_142`. **Safest
edit-demo target** (visible on-screen text).

## 0x2A (42) — ~~vehicle tooltip text~~ → CHARACTER HP SET (14 B) + 0x95 map object  (corrected 2026-09-17)
**Correction:** what was read here as one variable "tooltip" record is two records: **0x2A = 14 B**
(`u32 charId · f32 hp · u8 · u8`, a character hp set — also seen after 0x65 heals in combat) followed by
**0x95** (`u32 objId · u16 n · label`, map-object create marker, 10+n) whose label held the addon's long
status string. Everything below is the old reading, kept for history.

### (old) 0x2A — vehicle tooltip text (look-at popup)
```
u32 tag = 0x2A
... 24-byte header (id, float=100.0 render distance, flags) ...
u16 textLen @ +22
char[textLen] text
length = 24 + textLen
```
The **tooltip a vehicle shows when a player aims their view at it** (stock game mechanism). Content/cadence
are up to whatever set it — here the server's addon parks a 511-B `"Addon Targets / [NN] -not installed-"`
status string in it every tick; that "menu" use is an addon quirk, not the record's meaning. Length-only.
Not vehicle-state data, but MUST be length-decoded or it hides everything after it (see the blind-spot
note in [lifecycle.md](lifecycle.md)).

## 0x1A (26) — set BOOL signal on a vehicle part  ✅ length fixed = 40, meaning CONFIRMED
The **on/off (logic) counterpart of 0x1B**. Written when an addon sets a boolean value on a vehicle
component (`server.setVehicle*` bool). Shares the 0x1B header family:
```
u32 tag = 0x1A
u16 = 0xFFFF
u16 groupId       per-vehicle/part-group id (0x13, 0x1C, … — NOT constant)
...
i32 partIndex @ record +14    (small negative, e.g. -8..-11)
...
u8  value @ record +26        0 = off, 1 = on   (rest of the 40 B is zero tail)
length = 40
```
Only the bools that **changed state** are emitted, so a "1-of-N rotating" pattern produces exactly
2 records/tick (one 0→1, one 1→0). Confirmed on `session_20260915_213813` (4 bools → partIndex cycles
{-8,-9,-10,-11} period 4) and `_214459` (3 bools → cycles {-9,-10,-11} period 3, 100 % walked). See the
number counterpart 0x1B in [sync.md](sync.md).

**Update (session_20260916_002012_448, 2 players):** 0x1A is also the **player interaction broadcast** —
the server-side mirror of client [0x0A](client.md) minus the name string, sent to **every peer including
the originator** within ~1 tick of the press. Unified layout (both the addon case above and the player case):
```
u32 tag = 0x1A
u16 source        0xFFFF = addon/server-set, 1 = player interaction
u32 vehId
u32 0
i32 voxelX, voxelY, voxelZ   (the "partIndex @+14" above is voxelX of an addon-driven part)
u8  value/pressed  1 = down/on, 0 = up/off   @ +26
f32[3] hit offset within the voxel (0 for addon-set)
u8  0
length = 40
```
35 client 0x0A (Toggle/Push Button, Throttle Lever, Monitor touch, Large Keypad) → 35 0x1A per peer, 1:1.
Keypad **values** (client 0x0B) are NOT broadcast as a record; they surface only via the vehicle data
push 0x2E (payload 5 kB for veh 18 ~1 s after the entry).

## 0x29 (41) — equip / inventory action  ✅ length + meaning CONFIRMED
```
u32 tag = 0x29
u32 playerId          (character id, e.g. 2622)
u32 slot              inventory slot index
u16 strLen ; char[strLen]   ACTION: "equip" (equip/store into a slot) | "swap" (switch held item)
u8 ; float[2]
length = 23 + strLen
```
Confirmed on `session_20260915_220559` (pick up 7 vehicle-mounted items, then store all 7 back). The 7
items map to 7 distinct slots `{0,5,11,13,27,31,72}`. Storing choreography per item is `swap slot=N`
(select/hold that item) then `equip slot=N` (put it into the vehicle) — the **"swap" action is the held-item
selection switch**. `0x20 (32 B)` rides alongside (~12×, inventory slot-state, FFFFFFFF marker + ints).

## 0x14 / 0x15 / 0x61 — seat enter / exit  ✅ length fixed, meaning CONFIRMED
Sitting in and standing up from a vehicle seat. Strictly alternating **0x14 (sit) → ~0.8 s → 0x15 (stand)**.
- **0x14 (30 B) = SIT DOWN (enter seat).** `@+4 u32 characterId` (the seated player; constant across all
  seats when it is always you) · `@+8 u16` · `@+10 u16 vehicleId`. Confirmed on `session_20260915_214907`
  (2 vehicles × 3 seats, one sit+stand each): characterId stays 2622, vehicleId flips 30→29 between the two
  vehicles. (Seat index within a vehicle not yet isolated — needs a same-vehicle 3-seat capture.)
- **0x15 (10 B) = STAND UP (exit seat)**, always paired with **0x61 (116 B)**.
- **0x61 (116 B) = character appearance** re-sent on exit (character was hidden while seated, redrawn on
  stand). Payload = ~12 **ARGB colour entries** (each 4 B, alpha byte = 0xFF — outfit/suit palette) + a few
  small config ints. **No doubles / no position** — the exit position is computed client-side, not synced
  (other players' positions ride the 0x0B/0x0C avatar stream instead). 0x61 fires only on exit (6× = exit
  count), never on sit.

## 0x3A / 0x3B — vehicle MAP label  ✅ length, ✅ meaning (0x3A) / △ (0x3B)
An **addon** setting a label to show on the **map screen** for a vehicle (an addMapLabel / show-on-map
feature). Preceded by **0x3B** (8 B = tag + u32). **Not** related to seating — it merely happened on the
same tick as the addon-seat operation in the capture below, because that addon does both.
```
u32 tag = 0x3A
64-B numeric header (u32=1, u32=2, two double=50.0 = map pos/zoom-range?, zeros)
u32 = 0xFFFFFFFF        marker
u16 len ; char[len]     str1  (e.g. "Shairo-jp")
u16 len ; char[len]     str2  = label text (e.g. "Vehicle")
... 12-B trailing (u32 vehId, ints) ...
length = 84 + len(str1) + len(str2)   (VARIABLE)
```
Confirmed on `session_20260915_215903` (2× 0x3A/0x3B, 100 B each).

**Update (session_20260917_010256, two addon flags with a 50 m map circle):** 0x3A is the generic
`server.addMapObject` record, and the "FFFFFFFF marker" is just one of its u32 fields (it was 0 here,
so the rule no longer depends on it). Full layout as seen for the flags:
```
u32 tag=0x3A · u32 positionType (1 = vehicle-parented) · u32 markerType (9)
double x · double 50.0 · double z · double 0 · double radius (50.0)
u32 0 · u32 objId (28 / 29 — same value as the flag's vehId) · u32 0
u16 n · label ("RED") · u16 n · hoverLabel ("RED")
f32 1000.0 · u32 (vehId-1 ?) · u8 r,g,b,a  (FF 00 00 FF red / 00 00 FF FF blue)
```
It is preceded by **0x95** (`tag · u32 objId · u16 n · label` = 10 + n) instead of 0x3B in this case —
0x95 / 0x3B are two "create map object" markers, the map object itself is 0x3A. Ordering in the capture:
`0x95 + 0x3A` (map circle) → 0.85 s → `0x2B` (flag vehicle placement, strA "" / strB "RED", veh id at
tail) + 0x62 → 0x2D/0x2F → 0x81. Seat mechanics themselves are the
normal 0x14 (sit) / 0x15+0x61 (exit); the `[Matchmaker] … now seated in the vehicle.` line is a separate
`0x01` addon log.

## Interaction / character — length known, meaning TBD
| tag | length | notes |
|-----|--------|-------|
| 0x20 32 | fixed 52 | equip-adjacent (S4) |
| 0x67 103 | fixed 25 | character VITALS (hp) — mirror of client 0x39, see damage-combat.md |
| 0x88 136 | fixed 37 | character (S9) |
| 0x0B 11 / 0x0C 14 | fixed 38 / 14 | **other-player avatar** pair (per remote player: 0x0B tag+u16+double[3] pos+8 B, then 0x0C). Batch = 2×playerCount |
