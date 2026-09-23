# Entity lifecycle (spawn / load / unload / despawn / destroy)

Part of the [protocol index](../protocol.md). Server→client SEND, ch0, `msgType=8`.

Two id spaces behave differently: **vehicles** and **objects** (NPCs & world objects — casings,
dropped items, coal, debris — share one id space, distinct from vehicles). **Ids are never reused or
reassigned:** a reloaded entity keeps its id; a fresh spawn gets a new incrementing id.

## Vehicle lifecycle  ✅ CONFIRMED (`session_20260913_221632_497_lifecycle`, 6 F8-marked events)

| event | wire signature |
|-------|----------------|
| **create** (new vehicle) | **0x96(+0x37)** create marker, then **0x2D + 0x2F + 0x2B**, then 0x81 sync begins |
| **load** (enter client range; on create AND reload) | **0x2D + 0x2F + 0x2B**, then 0x81 begins |
| **unload** (move out of range) | **0x2C alone** (no 0x38); 0x81 stops; then the SAME id **reloads** (0x2D/0x2F/0x2B, 0x81 resumes) |
| **despawn** (workbench recover / command) | **0x2C + 0x38** (adjacent, same id) |
| **destroy** (attack) | **0x2C alone** (no 0x38) + **0x4D** spawning a new wreck entity |

**0x2C (44) is the universal "remove this vehicle id from the client"** — fires on unload, despawn AND
destroy (`tag + u32 vehicleId`, 8 B). **0x38 (56) is despawn-only** (`tag + u32 vehicleId`, 8 B; never on
unload or destroy). **0x4D wreck is destroy-only.** Telling them apart: `0x2C+0x38`→despawn; `0x2C` then
the same id's 0x81 **resumes**→unload/reload; `0x2C` then a **0x4D** new-entity→destroy. Match was
tick-for-tick — e.g. id 18's two 0x2C (8236, 10371) landed exactly on its two 0x81 gaps.

### Create/placement records (`swcap solve`, ×6 each — VERIFY on more data)
```
0x96 (150) len 13  spawn create : tag + u32 vehId + 5 B  — emitted as a PAIR (two 13-B: id then 0)
0x37 ( 55) len 54  placement    : tag + u32 vehId + double[3] pos + 12 B + f32=1.0 + 6 B
0x2B ( 43) len 90+nA+nB placement-2 : tag + double[3] pos + f32[4] quat + u16 nA + strA (spawn-location name, e.g. `hangar_edit`, empty for addon spawns) + u16 nB + strB (display name, e.g. `BLUE` for an addon flag) + double[3] + **u32 groupId** (spawn-group id = id of the group's first vehicle: a 3-body spawn 35/36/37 carries 34 in all three; session_20260919_115624) + u32 vehId + u32 + 4 B flags + u16 0 (VARIABLE; always followed by 0x62 (12 B: u32 20000 · u32 5). recordCount-verified on a 3-vehicle warp, session_20260917_005339)
0x2D ( 45) len 17  load state   : push family, 12 + u32 payloadLen@8
0x2F ( 47) var     load state   : push family, 12 + u32@8 (zlib transform/state)
```
0x37 and 0x2B carry the **same spawn-point double[3] position**; 0x2B repeats the id twice at the tail
(like 0x38). Full minimal-vehicle lifecycle: `0x96 → 0x37 → (0x2D + 0x2F) → 0x2B → 0x81 … → 0x2C(+0x38)`.

### Vehicle definition handshake — placement is a REQUEST, the state waits for the client  ✅ (`session_20260913_214246_776_freeze`, 4 peers, 25 loads each)
The load is a round trip, and the server does not push the vehicle's state until the client says it
has built the vehicle:

```
server → client   0x2B placement (+0x62)            "vehicle V exists here"   (ch0, msgType 8)
client → server   0x1B  u32 V   (tag+3 · u32, 8 B)  "send me V's definition" — one per vehicle, same tick
server → client   ch1 msgType=12                    the definition (see below), ~30 ms later, reliable
client → server   0x29  u32 V   (8 B)               "V is loaded"  — 60–150 ms later for a small vehicle,
                                                    1.3 s for a 186 KB one (the client stalls while it builds it)
server → client   0x2D + 0x2F (+0x30 …)             load state, ~16 ms after the 0x29 — NOT before
```
Every peer gets the same 0x2B for every vehicle the server loads — the load set is global (the idle
peer 28 km away requested and acked all 25 vehicles too), not per-recipient range. The server holds
the 0x2D/0x2F back per recipient until that recipient's 0x29 arrives; a client that never acks
(the frozen one, see `tools/swcap/README.md`) simply never receives the state for those ids.

**ch1 msgType=12 — vehicle definition push** (single frame, flags 8 = reliable; 772 B .. 187 KB):
```
+8  u32 vehId          (body +0)
+12 u16 = 0            (body +4)
+14 u32 = ?            (body +6; 0x012C .. 0x2626, grows with the vehicle — record/part count?)
+18 u32 rawLen         (body +10; inflated size: 0x05D4 = 1492 B for the smallest, 0x0C0FCF for the 187 KB one)
+22 zlib stream (78 01) — the vehicle XML / component data, per vehicle id, no filename
```
**ch1 msgType=13** (54 B): `u16 2 · double[3] pos · u32 0x30 · u32 0x0C · u32 0 · zlib(empty)` — a placement
with an empty payload, seen once right before a client's ch1 type=14 upload (a player spawning a
vehicle: the 78 KB **RECV ch1 type=14** is the client sending its vehicle file up).
Client-side lengths (`protocol/client.md`): 0x1B and 0x29 are both `tag+3 · u32 vehId` (8 B); the old
reading of 0x1B as "u32 ×2" was two adjacent 0x1B records.

## Object lifecycle  ✅ CONFIRMED (`session_20260915_012132_375_coal`, coal spawn/collect)

| event | record |
|-------|--------|
| **spawn** | **0x4D (77)** full-state (`tag + u32 objectId + u32 payloadLen + payload`, ~323 B) — same tag as vehicle spawn-full-state |
| **despawn / collect** | **0x4F (79)** (`tag + u32 objectId`, 8 B) |
| sync while alive | **0x1D (29)** (object counterpart of vehicle 0x1B) and **0x50 (80)** (see [sync.md](sync.md)) |

**233 ids appeared in BOTH 0x4D and 0x4F**, each object living ~50 ticks (~0.8 s) spawn→collect, ids
incrementing (3885→4138). This also identifies the lone **0x4D (id 3757)** ~1.5 s after the big hit in
the fire capture as a spawned object (casing / debris), not a fire record.

## recordCount counts compound records as 2
The envelope's `recordCount` == (physical records) + n(0x2A) + n(0x96) + n(0x38) — exact on all 2633
messages of `session_20260914_232332_784_despawn`. These three are compound (header+body):
- **0x38** splits into `0x38(8) + 0x2C(8)`;
- **0x96** into two 13-B 0x96 records (id, then 0);
- **0x2A** (tooltip) is counted as 2 — because it really is two records: 0x2A (14 B, character hp set) + 0x95 (map-object label). Fixed 2026-09-17; the walker now decodes both.

In a normal (non-addon) capture with no 0x2A, recordCount matched physical exactly (0 delta on
15448/15448 msgs), so the count field itself is reliable.

> **Walker blind-spot lesson:** 0x38 and the load-time 0x81 sit AFTER 0x2A in the addon server's stream.
> Until 0x2A got a length rule, the walker stopped at 0x2A and everything after it was invisible to
> `census`/`findid`/`attick`/`lifespan`/`frozen`. If lifecycle records seem "missing", suspect an
> undecoded variable record earlier in the message hiding the tail.

## Addon warp (`session_20260917_003856_201`, `?warp`-style command ×2, vehicle 24) — 100 % walked
Each warp is literally an unload + reload of the same id, ~0.8 s apart:
`0x01 chat (command echo) + 0x2C remove veh=24` → `[0x2B placement (new pos, name "hangar_edit") + 0x62] per vehicle + 0x05 ×2` → 7 ticks later `0x2D + 0x2F` full state, then 0x81 resumes.
No dedicated teleport record for vehicles (unlike player 0x5E). `session_20260917_005339_115` (3 vehicles warped together, twice): one message holds 3 × (0x2B + 0x62), recordCount = 7 — this pinned the 0x2B tail; an earlier reading that split the last 10 B off as a "second form of 0x01" was wrong.
