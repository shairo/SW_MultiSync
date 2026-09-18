# Position / physics sync

Part of the [protocol index](../protocol.md). Server→client SEND, ch0, `msgType=8`.

## 0x81 (129) — vehicle body position/attitude sync  ✅ length + time semantics CONFIRMED
**The relay's primary edit/insert target.**
```
u32 tag = 0x81
u32 vehicleId
u32 time        = FUTURE world-tick ETA: the tick by which the client should reach this pose
u16 bodyCount
Body[bodyCount]:
    u8 type
    type 0: (no change since last sync)          data = 0
    type 1: float[4] rotation + double[3] pos     data = 16 + 24 = 40   (first body is always type 1)
    type 2: float[4] rotation + float[3]  pos     data = 16 + 12 = 28   (relative to first body)
length = 4 + 4 + 4 + 2 + Σ (1 + typeDataLen)
```
Header verified on driven vehicle id=16 (bodyCount=13, body0 type=1, rotation quat + double pos ~4104).

**`time` semantics (`swcap eta`):** `delta = record.time − envelope.tick` is always ≥ 0 (a genuine
future ETA). **Near vehicle: delta = +5 for all records** (sync interval = 5 ticks, `time = tick + 5`).
**Distant: delta 5 … ~687**, matching the per-(peer,vehicle) sync spacing → `time = tick + interval`.
Tick rate ≈ **62.5/s**. Distant deltas of ~300 ticks ≈ 5 s = the distant-vehicle lag the relay fixes.

**Relay time-rewrite:** when forwarding a fresher 0x81 into recipient B's stream, set
`time = B_message.tick + desiredInterval` (e.g. 5) so B interpolates over a short future window.

## 0x39 (57) — vehicle low-frequency position sync  ✅ length fixed = 36
```
u32 tag = 0x39
u32 vehicleId
double[3] position   (24)
float azimuth        (4)
```
Matches old `Record_57`. Used for distant/low-rate vehicles.

## 0x1B (27) — set NUMBER signal on a vehicle part  ✅ length fixed = 34, meaning CONFIRMED
Written when an addon (or logic) sets a **numeric value** on a vehicle component. Shares a header family
with the bool counterpart [0x1A](ui-chat.md):
```
u32 tag = 0x1B
u16 = 0xFFFF
u16 groupId       per-vehicle/part-group id (NOT constant)
...
i32 partIndex @ record +14
float value @ record +26      (rest of the 34 B is zero tail)
length = 34
```
Confirmed on `session_20260915_213813` (2 numbers → 2 records/tick, values ramp 42→48 / 91→97, wrap at
0–99) and `_214459` (1 number → 1 record/tick, 100 % walked). This also explains its old "vehicle
high-frequency" reputation: it was the biggest walk blocker in the driving capture (13.6 % until decoded)
because component signals change every tick while driving — the records are per-signal value updates, not
a position stream.

## 0x1D (29) — object position/physics sync  ✅ length fixed = 34, meaning partial
The **object counterpart of 0x1B** (new tag). **Fixed 34 B**, one record per object. The "variable"
solve lengths (68/102/136) were just **runs of 1–4 back-to-back 0x1D** (one per object) that solve had
merged — a single-body coal object has no variable field. Confirmed on `session_20260915_012132_375_coal`:
with 0x1D=34, all 511 records close cleanly (0 bad), run-lengths {1:298, 2:71, 3:17, 4:5}. Field layout
(signed int deltas + a float = movement) not fully reversed.

## 0xB5 (181) — vehicle transform / teleport (setVehiclePos)  ✅ length fixed = 136, meaning CONFIRMED
```
u32 tag = 0xB5
u32 vehicleId
double[16]   4x4 transform matrix, column-major (diagonal 1.0 = identity rotation)
             translation X,Y,Z = matrix indices 12,13,14 -> offsets 104/112/120
length = 136
```
Emitted per tick while an addon moves a vehicle (`server.setVehiclePos`-style). Confirmed on
`session_20260915_211421` (addon nudged veh25 along X, then Y, then Z, ~1 s each): the translation
triplet isolates cleanly to X-only, then Y-only, then Z-only. Values oscillate ±0.2–0.3 around 0 and do
not accumulate → they are **per-tick position deltas**, not absolute world coords. 453 records, 0 bad.

## 0x5E (94) — PLAYER teleport: map fast-travel  ✅ length fixed = 29, meaning CONFIRMED
```
u32 tag = 0x5E
double X, Y, Z    world position of the fast-travel destination
u8   = 1
length = 29
```
Emitted when the player fast-travels from the map screen. Confirmed on `session_20260915_215536` (2 map
fast-travels; Y = 0 = sea level). Also fires a bare `0x09` flag right after. The **addon** teleport path is
a different tag (0x55).

## 0x55 (85) — PLAYER teleport: addon (server.setPlayerPos)  ✅ length fixed = 31, meaning CONFIRMED
```
u32 tag = 0x55
u16 = 2
double X, Y, Z    world position
u8   = 0
length = 31
```
Emitted when an addon teleports the player. Confirmed on `session_20260915_215536` (2 addon teleports to
the team flag, Y = 18.5 = flag altitude). Accompanied by an addon-authored `0x01` system message
(`"Teleported to your team flag: BLUE"`, sender `[Matchmaker]`) — that log is the addon's own, not part of
the teleport record. `0x09` fires on every teleport (map + addon = 4×) as a generic position-reset flag.

## 0x50 (80) — NPC / object state (position + HP)  ✅ length variable, meaning CONFIRMED
```
u32 tag = 0x50
u32 id                (NPCs & world objects share this id space)
u32 payloadLen        length = 12 + payloadLen
... payload ...
```
Seen len **76** (payload 64, the common single form, 3333× across captures) and **328** (payload 316 =
124-B header + **3 × 64-B entries**, each a body: `float[3] pos` + HP `float ≈ 100`). One entry per body;
NPCs & objects both use it. The old fixed-76 guess mis-split the 328 form's zero tail into a phantom
**tag 0x00** (not a real record — resolved by the variable rule).
