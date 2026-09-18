# Damage & combat

Part of the [protocol index](../protocol.md). Server→client SEND, ch0, `msgType=8`.

## 0x35 (53) — body damage application  ✅ length fixed = 34, semantics CONFIRMED
```
u32   tag = 0x35
u32   vehicleId
u32   bodyIndex       which body of the vehicle (0..N-1)
float localPos[3]     hit point on that body (metres, local frame)
float magnitude       impact/damage value: 1000.0 small hits, 5000.0 big hit
float value2          scales with magnitude (0.5 small / 2.5 big — same 5× ratio)
u16   flags           (0 or 256)
```
One 0x35 **per body damage application** — NOT per voxel (a body has far more voxels than the 11–38
records per hit); the client computes which voxels inside the body's hit region are destroyed. Confirmed
on `session_20260915_005445_704_damage` vs the server's `[Damage]` addon log (vid **74** in all 280 records,
body indices exactly **0..9** = the vehicle's ≤10 bodies).

**Multi-damage quirk:** certain parts make one body take damage repeatedly, so a single hit can emit the
**same (bodyIndex, localPos, magnitude) record many times**. The big hit's 38 records were only ~10
distinct hit points — body 5 appeared 13×, body 0 11×, all byte-identical. That inflation (not a bigger
blast radius) is why the big hit had 38 records vs 11–19 for the small ones.

**Fire spread emits NO damage record.** In `session_20260915_010023_871_fire` the big hit fired 0x35 once
(38 records) and then fire burned for ~27 s with **no further 0x35** and no new tag — fire is simulated
client-side. The only wire artifacts after the hit were a 0x4D object spawn (casing/debris, ~1.5 s
later) and an unrelated periodic 0x2E batch.

## Combat tags — length known, meaning TBD
| tag | length | notes |
|-----|--------|-------|
| 0x27 39 | fixed 12 | combat |
| 0x19 25 | fixed 54 | |
| 0x30 48 | fixed 36 | list entry |
| 0x34 52 | fixed 49 | |
| 0x28 40 | u32@8==1 ? 12 : 46 | **inventory slot GIVE** (respawn burst): tag · u32 charId · u32 slot · u32 itemId · f32 charge · u32 chargeMax · u8 · i32 -1 · 17 B zero. Followed by 0x4D (new item object) + 0x26 per slot. (session_20260917_002237) |
| 0x26 38 | fixed 35 | **inventory slot state** (short form): tag · u32 charId · u32 slot · u32 itemId · f32 charge · u32 chargeMax · u8 0 · i32 -1 · 5 B zero · u8 1. Respawn: slots 2..6 with item=0 then slot 10; combat (002757): slot 1 item 27 charge 400/400. VERIFIED boundary in 3 captures |
| 0x67 103 | fixed 25 | **CHARACTER VITALS** broadcast: tag · u32 charId · u8 1 · f32 hp · u32 (36000 full, drops while dying) · f32 100 · f32 (5 / 15). Exact mirror of client 0x39 — damage is **client-authoritative**: the burning client reports its own hp every tick (864× in a 10 s burn) and the server rebroadcasts. |
| 0x64 100 | 20 + len | **CHARACTER DEATH** broadcast (server reply to client 0x37, 1 tick after hp reaches 0): tag · u32 charId · u32 46 · u32 21 · u32 payloadLen · payload. Payload = component-value entries `u8 1 · u32 width · value[width] · u8 0 · u32 idx` (same encoding as 0x2E vehicle pushes). 70 B in the fire capture. recordCount counts it as 2 (compound) — the earlier "0x64(28)+0x06(42)" split was wrong; 0x06 is the *player-join* character dump, unrelated |
| 0x65 101 | fixed 13 | **HEAL applied** (first-aid kit, item 11): tag · u32 charId · f32 amount (50.0) · u8 0. Mirror of client 0x38; emitted with the 0x29 `primary` mirror. hp then rises client-side over ~0.5 s (0x39/0x67 stream). VERIFIED (session_20260917_002831) |
| 0x63 99 | fixed 30 | **RESPAWN POSITION**: tag · u16 1 · double[3] world pos. Sent once at the end of the respawn burst (server reply to client 0x36) |
| 0x61 97 | fixed 116 | character appearance (ARGB palette) — moved to ui-chat.md (seat exit) |
| 0x2E 46 | 12 + u32@8 | per-vehicle data push (bulk state; 17 B typical, seen 14 KB). NOT damage — fires occasionally |

## Combat tags — re-examined 2026-09-17 (long-range `005757_200` now 100 %, near `002757` 99.9 %)
Most of the old "undefined" list turned out to be either **mis-attributed by an earlier wrong length**
(0x2B, 0x2A) or plain fixed-size records that only needed a boundary. All lengths below are boundary-
verified in the long-range capture; meanings are still candidates unless stated.

| tag | len | layout / reading |
|---|---|---|
| 0x92 146 | 44 | `double[3] pos · f32[4]` — 145× in the long-range fight, ~1:1 with 0x76. Candidate: projectile / shell spawn (pos + velocity/orientation) |
| 0x76 118 | 32 | `double[3] pos · f32` — recordCount-verified. Candidate: projectile end / impact point |
| 0x31 49 | 20 | `u32 vehId · i32 x · i32 y · i32 z` — runs per vehicle: **voxel damage list** (vehicle damage was ON in these captures) |
| 0x9D 157 | 67 | `u32 id · u32 1 · double[3] pos · 7 × f32 (0,1,0,0,0,0,1) · u8 1 · u16 0xFFFF` — runs; object/debris placement candidate |
| 0x7B 123 | 12+n1+n2 | **addon notification** (`server.notify`): `u16 title · u16 subtitle · u32 type` ("Game Start" / "20:00 left.") |
| 0x4C 76, 0x8F 143, 0x3D 61 | 8 | `u32 id` — emitted as a 0x4C/0x8F/0x3D/0x3B quartet per incrementing id: map-object remove family (with 0x3B) |
| 0xA9 169 | 32 | `i32 tileX · i32 tileZ · u32 100 · u32 0 · f32 500 · f32 200 · u32 0` — a real record after all (reference.md said otherwise) |
| 0x74 116 / 0x75 117 | 40 / 36 | `double[3] pos · (u32 1 · f32 0.5 · f32 1)` / `double[3] pos · f32 · f32 1` — pair, once per event |
| 0x2A 42 | **14** | `u32 charId · f32 hp · u8 · u8` — CHARACTER HP SET. The old "tooltip 24+len" reading was 0x2A(14) + 0x95 map-object(10+n); this also explains the "recordCount counts 0x2A as 2" note in lifecycle.md |
| 0x03 3 | 11+n | **player joined**: `u16 peer_id · u16 n · name · u16 0 · u8 1` (near capture, lobby) |
| 0x8D 141 | 16+n1+n2 | peer-addressed message: `u16 peer_id · 6 B · u16 text · u16 sender` ("Authed …" / "[Server]") |
| 0x91 145 / 0x5F 95 / 0x16 22 | 28 / 10 / 11 | `u16 peer_id · …` small per-peer records (lobby) |
| 0xB8 184 / 0xB9 185 | 52 / 48 | `u32 vehId · u32 · i32 · u32 · -1 · vehId · u32 · 1 · 1 · …` runs — vehicle component/body state family |

Reminder: player damage was **disabled** in both captures, so none of these is a character-hit record;
0x31 (voxel list) + 0x35 (body damage) are the vehicle-damage path.

## Death / respawn sequence (`session_20260917_002237_770`, fire burn, solo) — 100 % walked

| t | client → server (msgType 3) | server → all (msgType 8) |
|---|---|---|
| burn | 0x39 vitals every tick, hp 100 → 0 (u32 field stays 36000) | 0x67 mirror every tick |
| hp = 0 | **0x37** (4 B, tag only) = "I died" | 0x64 (death, 70 B), 1 tick later |
| +4 s | **0x36** (4 B, tag only) = respawn request | one 2.3 KB message: 5 × [0x28 slot give · 0x4D item object · 0x26 slot state] for slots 2..6 (items 15, 6, 8, 17, 11) · 0x26 slot 10 · **0x63 respawn pos**, then 0x09, 6 × 0x29 `equip` mirrors, 0x50 for each new item object |
| +60 ms | 6 × 0x52 `equip` slot 0, 0x50 select 0, 0x39 hp=100 (last f32 now 15.0), **0x2E** (4 B, tag only) = respawn done | 0x29 ×6 |
| +0.7 s | seat enter burst: 0x64 keyMask 0 + 8 × 0x65 axis + 0x66 + 0x01 (73 B) | 0xA6 + 0xA7 |

Note: the earlier combat captures (002757 / 005757_200) were recorded with player damage DISABLED (vehicle damage only), so the undefined tags there may be unrelated to character hits.
The player's health is never computed by the server: no server record precedes the client's 0x39 drop.
