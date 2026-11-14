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

**Distance LOD (`tools/rectest/test_dist.cpp`, session_20260919_143912_627: 5 peers, 64k 0x81, both
directions 100 % walked):** pairing each 0x81's `dETA = time − tick` with the recipient's distance to
body0 (recipient position = that peer's latest client 0x2F world pos) gives a clean, speed-independent
rule:

| dist [m] | dETA p50 | | dist [m] | dETA p50 |
|---|---|---|---|---|
| 0–110 | 5 | | 500–750 | 56 |
| 120–140 | 7 | | 750–1000 | 85 |
| 200–220 | 15 | | 1000–1500 | 125 |
| 280–300 | 23 | | 2000–3000 | 225 |
| 400–500 | 40 | | ≥5000 | 720 (cap) |

```
dETA ≈ clamp(0.1·d − 6, 5, 720)      ticks   (1 tick per 10 m beyond ~110 m; 12 s ceiling)
```
Fit residual over 200–3000 m: p10/p50/p90 = −1.8 / +0.5 / +13 ticks (the +13 tail is the approach to the
720 cap). Speed does not enter (dETA/d median 0.088 for >50 m/s vs 0.090 for <5 m/s). The interval is
decided per send from the distance *at that send*, so a vehicle closing fast keeps its long promise
until the next send — the motivation for the relay's distance-aware target interval.

**Promise-keeping:** the real gap exceeded the previous record's dETA by >2 ticks in only 376 / 63 459
consecutive pairs (0.6 %), all with dETA = 5. 325 of them are vehicles at rest (body0 moved < 5 cm — the
server evidently skips the send when nothing changed); the rest cluster in two short bursts where the
server tick itself ran ~25 % slow (171 ms real for 8 ticks). No correlation with the recipient or the
vehicle crossing a 1 km tile boundary (0 of 376 with boundaries at ±500 m; 6 with boundaries at 0 m vs a
6 % base rate).

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

## 0x68 / 0x69 / 0x6A / 0x84 — wildlife fish  ✅ lengths fixed, meaning ✅ (matched against the save)
Decoded 2026-11-08 from `session_20261108_045606_169` (1 peer, addon missile combat; these four were 25–33 % of
walk failures). Lengths pinned with `swcap lens` (last-record cases) and `swcap solve`; all fixed. The same rules
also lift the 2026-09 `--legacy` captures from 76–93 % to 99.8–100 % walked, so they are not new in the update.
```
0x84 (52): u32 tag · u32 uid · u32 world tick (= message tick) · f64 x,y,z · f32 quat[4] (|q| = 1)      fish pose
0x68 (52): u32 tag · u32 fish_type · u32 0 · f64 x,y,z · f32[3] heading (x,0,z) · f32 scale (1.0)        fish spawn
0x6A (38): u32 tag · u32 uid · f32[3] (0 in all samples) · f32[3] target_heading (x,0,z) · u16 0 ·
           u16 jump_timer (180) · u16 out_of_water_timer (2000)                                        fish state
0x69  (8): u32 tag · u32 uid                                                                             fish remove
```
Meaning (2026-11-14). The save keeps the same objects as `<wildlife fish_uid="N"><fish><f fish_type uid
state_ticks jump_timer out_of_water_timer scale [turn_factor]><transform/><target_heading x z/><flee_heading/></f>`
(`working_server/scene.xml`: 24 fish, uids 813..867, next uid 868). Against the capture:

| capture | save `<f>` |
|---|---|
| ids 789..834, ~30 at a time, consecutive new ids (833, 834) | uids 813..867, `fish_uid` = next uid counter |
| x 13950..14150, z −25860..−26070, y −3..−9 (mostly −5.0) | same area, y −4.95..−5.08 |
| 0x84 moves 15–68 m per ~3400 ticks (0.3–1 m/s) | — (fish swim slowly) |
| 0x6A unit vector, y = 0, constant per uid; equals the direction 0x84 moves (uid 827: (0,0,1) and +68 m in z; uid 808: (−0.84,0,−0.55) vs moved (−13,−9)) | `target_heading x z` |
| 0x6A u16 180 / 2000 in every sample | `jump_timer="180" out_of_water_timer="2000"` on all 24 fish |
| 0x68 first u32 = 15, 37 | `fish_type` values 2..42 include 15 and 37 |
| 0x68 last f32 = 1.0; f32[3] before it = (−0.9987, −0, 0.0515), horizontal unit | `scale="1"`; initial heading |

- Spawn/remove keep the count: `0x69 uid 815` (tick 32299) → `0x68 fish_type 15 @ (14125,−3,−25987)` (32300) →
  first `0x84 uid 833` at the same position (32301); again `0x69 821` → `0x68 type 37` → `0x84 uid 834`. 0x68 carries
  no uid; the new uid appears in the next 0x84.
- 0x84 spacing is a distance LOD like 0x81's: uids 808/827 every ~11 ticks, the rest 260–1370. 0x6A every 24 ticks.
- The two misses: the capture (11-08 04:56) and the save (11-14) are different sessions, so values are not compared
  uid by uid; the first f32[3] of 0x6A (always 0) and its u16 0 are unknown (flee_heading / turn_factor?).
  `hebi3_mm_1108` (saved 11-08 23:36) has no fish.
- Still unknown on the legacy captures (< 0.2 % of messages): 0x6B, 0x6C, 0x82, 0x56 — maybe more wildlife.
