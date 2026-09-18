# Client→server stream (RECV, ch0, `msgType=3`)

Source: `captures/session_20260916_000033_251.swcap` (solo, player 76561197994178477 standing next to
veh 16, pressing buttons / touching monitors / typing into a keypad). Decoded with a throwaway
Python walker — `tools/swcap` still only walks SEND `msgType=8`; `validate`/`walk` do **not** cover
this stream (see "Tooling gap" below).

## Envelope
```
+0  u32 = 1
+4  u32 = 3            msgType
+8  u16 = 1
+10 u16 recordCount    (0 = empty heartbeat, 14-B body; 437 of 1026 msgs)
+14 records...
```
Records are `u8 tag + 3 pad` (tag byte then three 0x00) followed by the payload — NOT the u32 tag of
the SEND side (the value is the same, it is just easier to read as u8+pad).
1026 messages / 1026 = fully walked, 0 unknown bytes.

## peer_id
Server-side mirrors of player input (0xA6 / 0xA7 / 0xA8 / 0x07, and the u16 right after the tag in
0x0D / 0x0E / 0x63) carry a **u16 `peer_id`** — the game's own player number (the one addon Lua sees).
It was 1 in the 2026-09-16 sessions and 2 in the 2026-09-17 sessions (both correct for those lobbies).
The client-side records don't carry it; the server stamps it.

## Tags (all lengths fixed unless noted)

| tag | len | meaning | layout |
|---|---|---|---|
| 0x66 | 20 | **camera look** (~every 16 ms while the view moves) | tag+3 · 8 B zero · f32 yaw · f32 pitch (rad) — broadcast as SEND 0xA8 (22 B, u16 peer_id prefix) |
| 0x2F | 60 | **player pose** (~every 78 ms) | tag+3 · double[3] (≈1.50,1.64,-0.98 — constant, not yet identified; possibly local offset / head) · double[3] world pos · u32 = 0x10 · u32 = 7 |
| 0x04 | 5 | follows almost every 0x2F | tag · u32 = 0 |
| 0x34 | 24 | **1 Hz heartbeat** | tag+3 · u32 counter (+60 per record = client tick) · u32 0 · u32 0x3E · u32 2 · u32 0 |
| 0x0A | 40+n | **component INTERACT (press / release)** | tag+3 · u32 vehId · u32 0 · i32 voxelX · i32 voxelY · i32 voxelZ · u8 pressed(1=down,0=up) · f32[3] hit offset within voxel (±0.125) · u8 0 · u16 n · name[n] |
| 0x0B | 32 | **keypad value set** | tag+3 · u32 vehId · u32 0 · i32 voxelX/Y/Z · f32 value · u32 valueIndex |
| 0x51 | 42 | **held-item state** (sent on every change) | tag+3 · u32 hotbarIdx · u32 itemId · f32 battery% · u32 100 · u8 aiming/active · i32 -1 · u32 0 · u8 0 · f32[3] view dir (zero when part of a swap) |
| 0x50 | 8 | hotbar select (precedes a `swap` 0x52) | tag+3 · u32 hotbarIdx |
| 0x52 | 23+n | **item ACTION request** | tag+3 · u32 playerId · u32 itemId · u16 n · action[n] (`primary` / `swap`) · u8 1 · f32 1.0 · f32 (1.0 / 0.9) — same layout as SEND 0x29 |
| 0x0C | 28 | **seat enter request** | tag+3 · u32 playerId · u32 vehId · i32 voxelX/Y/Z · u32 seatIdx — mirrored by SEND 0x14 |
| 0x0D | 8 | **seat exit request** | tag+3 · u32 playerId — mirrored by SEND 0x15 |
| 0x30 | 12 | seat state (after enter / exit) | tag+3 · u32 (1 / 4) · u16 · u16 |
| 0x64 | 20 | **seat key state** (on change) | tag+3 · u32 keyMask · 12 B zero — echoed as SEND 0xA6 |
| 0x65 | 9 | seat axis value (×8, idx 0..7, at seat enter / re-seat after respawn) | tag+3 · u32 idx · u8 value — broadcast as SEND 0xA7 (11 B) |
| 0x39 | 25 | **character VITALS** (every tick while hp changes) | tag+3 · u32 charId · u8 1 · f32 hp · u32 36000 (drops at death) · f32 100 · f32 5 (15 after respawn) — mirrored verbatim as SEND 0x67. Client-authoritative damage |
| 0x48 | 37 | **game settings bool[33]** (whole array per toggle) | tag+3 · u8[33] — mirrored as SEND 0x88; see settings.md |
| 0x31 / 0x4B / 0x2A / 0x49 | 8 / 8 / 16 / 8 | time / day-length / weather / wind-dir sliders | see settings.md (SEND 0x57 / 0x8B / 0x5A / 0x89) |
| 0x32 / 0x2C / 0x4A | 5 | settings bools with own record | tag+3 · u8 — SEND 0x59 / 0x5D / 0x8A |
| 0x1C | 8 | **vehicle despawn request** | tag+3 · u32 vehId — server answers 0x2C remove + 0x38 despawn + 0x55 + 0x62 + 0x09 in one burst |
| 0x45 | 5 | **menu open / close** (1 = opened, 0 = closed) | tag+3 · u8 — brackets every settings-menu action (sessions 10–14); not rebroadcast |
| 0x28 | 12 | **fog cleared on tile** (sent per tile while moving) | tag+3 · i32 tileX · i32 tileZ — mirrored as SEND 0x49 + 0x45 |
| 0x19 | 13 | **tile purchase** | tag+3 · i32 tileX · i32 tileZ · u8 0 — mirrored as SEND 0x4A + 0x62 |
| 0x1A | 4 | **buy all tiles** (settings menu) | tag+3 — server answers 30 × (0x4A + 0x62) |
| 0x38 | 13 | **heal request** (first-aid kit primary) | tag+3 · u32 charId · f32 amount (50.0) · u8 0 — mirrored as SEND 0x65 (13 B); sent together with 0x51 held-item + 0x52 `primary` |
| 0x37 | 4 | **death notification** (hp reached 0) | tag+3 — server answers 0x64 |
| 0x36 | 4 | **respawn request** | tag+3 — server answers the inventory burst + 0x63 respawn pos |
| 0x2E | 4 | respawn complete ack (after the `equip` actions) | tag+3 |
| 0x01 | 73 | seat-entry state snapshot (after 0x64 + 0x65×8 at seat enter) | tag+3 · 24 B zero · f32 yaw · f32 pitch · 37 B zero — broadcast as SEND 0x07 (71 B); also seen from self after respawn (session 8) |
| 0x14 | 12 | **NPC follow toggle** | tag+3 · u32 playerId · u32 npcId — broadcast as SEND 0x0D (14 B) |
| 0x13 | 12 | **NPC pick up / carry** | tag+3 · u32 playerId · u32 npcId — broadcast as SEND 0x0E (14 B) |
| 0x06 | 25 | **NPC put down** (sent as a pair) | tag+3 · u32 playerId · 17 B zero — broadcast as SEND 0x17 (25 B) |
| 0x17 | 118 | **character appearance** (sent on seat exit) | tag+3 · u32 0x6A · u8 1 · 13× RGBA outfit colours · ints — echoed verbatim as SEND 0x61 (116) |

### 0x0A observations
- Every press is a **pair** (pressed=1 then pressed=0, 80–550 ms apart) with identical voxel + hit
  offset → server gets full down/up; hold length is meaningful (Push Button, Throttle Lever drag).
- `name` is the component's display name: `Toggle Button`, `Push Button`, `Push Button (2 Sided)`,
  `Throttle Lever`, `Monitor 2x2`, `Monitor 1x1`, `Large Keypad`. Monitors carry the touch point in
  the hit offset (multiple distinct touches on the same voxel).
- The voxel triple is the block's position on the vehicle grid (buttons at y=0 z=3, monitors y=1
  z=4, keypad y=1 z=8) — the client identifies the component by **vehicle + voxel**, not by a
  component id. Same triple can hold different names over the session (0,0,3 = "Push Button" then
  "Toggle Button") ⇒ vehicle was re-edited, ids are positional.
- `Large Keypad` produced a single 0x0A with pressed=0 (opening the keypad UI), then 2.3 s later two
  0x0B records (valueIndex 0 = 4060.56, valueIndex 1 = -5972.43) = the values entered.

## Session 2 — flashlight / binoculars / night vision (`session_20260916_000951_732`)
Player 2622, on foot then seated in veh 17 (seat voxel (0,-13,1), seatIdx 7). Client stream fully
walked (0 unknown). Observed item ids (from 0x51/0x52): **15 = flashlight** (`primary` toggles,
battery 100→99.94→99.87), **6 = binoculars** (no battery; 0x51 aiming=1/0 + view dir when looking
through, no `primary`), **17 = night-vision** (`primary` toggles, battery drains); 8 and 11 were only
scrolled past. Hotbar scroll = `0x51(dir=0)` + `0x50 idx` + `0x52 swap` per step.

**Server echo to the originator (solo peer):**
| client | server (SEND) | notes |
|---|---|---|
| 0x52 `primary`/`swap` | 0x29 (identical layout, 17↔17) | ⇒ 0x29.`slot` is actually the **item id** (15/6/8/17/11 here) |
| 0x0C seat enter | 0x14 (30 B: player, u16 1, veh, voxel, seatIdx) | |
| 0x0D seat exit | 0x15 (10 B) | |
| 0x17 appearance | 0x61 (116 B) | |
| 0x51 held state / aim / battery, 0x50, 0x66, 0x2F | *nothing* | flashlight beam / binocular aim are not reflected to the originator; whether other peers get 0x51-like data needs a 2-player capture |

## Session 3 — 2 players (`session_20260916_002012_448`)  ← answers the forwarding question
Peers: 8477 (player 2622, the tester) and 6675 (player 2651, seated in veh 18 at t+3.7 s).
Everything the tester did was walked on RECV (0 unknown for peer 8477; peer 6675's *seated* input uses
a different family 0x64/0x65 + 0x18 — not decoded yet). What the **other peer** received:

| tester action (client tag) | forwarded to other peer? | server tag |
|---|---|---|
| flashlight / NVG `primary`, hotbar `swap` (0x52) | **yes** (6 → both peers 6 each) | 0x29 |
| button / lever / monitor touch / keypad open (0x0A) | **yes** (35 → both peers 35 each, ≤1 tick later) | **0x1A** (40 B, = 0x0A without the name) |
| keypad value (0x0B) | not as a record | only via 0x2E veh data push |
| seat enter (0x0C) | yes | 0x14 |
| held-item state / aim / battery (0x51), hotbar select (0x50), look (0x66), pose (0x2F) | **no** | — |

So a relay only needs 0x29 + 0x1A + 0x14/0x15 to reproduce visible interactions; flashlight beam and
binocular aiming are not on the wire at all (client-local given the 0x29 `primary` toggle).

## Session 4 — seat input, solo (`session_20260916_002752_233`)
Seated (veh 19), pressed hotkeys 1–7 and the 6 axes. Client sends **0x64 (20 B)** on every key-state
change: `tag+3 · u32 keyMask · 12 B zero`. 70 records, all mirrored 1 tick later by SEND **0xA6 (22 B)**:
`u32 tag · u16 peer_id · u32 keyMask · 12 B zero` (70:70; the old "u16=1,u16=4" reading of 0xA6 was the mask
0x0410). Seat axes are therefore sent as *key state*, not as float axis values — the receiving client
integrates them itself.

| bit | key |
|---|---|
| 0,1,2,3 | axis 1/2 keys (W/S/A/D order observed: 0→1→2→3 in single presses, combos like 0x0B = W+S+D) |
| 11,12,13,14 | axis 3/4 keys (arrow keys) |
| 15..20 | hotkeys 1..6 |
| 4 + 10 (always together) | pressed twice in the hotkey phase — hotkey 7 candidate (or a dual-bound key). Reference only: in session_20260917_002036 (3 shots from a seated vehicle weapon) the only client record per shot was 0x64 with mask 0x0410, so this mask is also what the seat weapon trigger sends. No dedicated "fire" record exists client- or server-side; only 0xA6 (mirror) and, on vehicle hits, 0x35 body damage. |

Lengths confirmed by message boundary (session 3, peer 6675, while seated at capture start):
- **0x65 (9 B)** `tag+3 · u32 idx 0..7 · u8 value` — 8 entries follow each 0x64 while seated in a seat
  that has per-axis state (3 bursts at t+78/343/406 ms, all zero). Per-axis value dump; meaning of
  the u8 (0 here) needs a capture with axes actually moved while this family is active.
- **0x18 (28 B)** `tag+3 · double[3] world pos` — once (4105.71, 10.40, -5920.26) ≈ the peer's own
  position; single occurrence, purpose unknown (respawn / interaction target?).
- **0x01 (73 B)** `tag+3 · 24 B zero · f32 yaw · f32 pitch · 37 B zero` — 3× at capture start from the
  seated peer, alongside 0x64+0x65×8. Seated-state keepalive candidate.

## Session 5 — enter → keys → exit → enter, solo (`session_20260916_003351_610`)
Fully walked (0 unknown; SEND also 100 %). Sequence per seat cycle (veh 19, seat voxel (0,-12,1) idx 8):
`0x0C enter` → `0x30 state=1` ×2 → 0x64 key states (76, mirrored by 76 SEND 0xA6) → `0x0D exit` →
`0x17 appearance` → `0x30 state=4` … → `0x0C enter` again. Server side: 0x14 ×2, 0x15 ×1, 0x61 ×1 — 1:1.
0x30 = `u8 state (1 = seated, 4 = on foot) · u8 · u16 · u16 flags` (12 B).
**0x65 / 0x18 / 0x01 did not appear at all** in this cycle ⇒ they are not part of the normal
enter/exit choreography for this seat; they came only from peer 6675 in session 3 (possibly a seat
type with axis sliders, a gamepad/analogue input, or a join-time state dump). Needs that player's setup.
0x64 also fired a few times *after* 0x0D (keys still held while standing up), so 0x64 is the
character key-state channel, not strictly seat-only.

## Session 6 — NPC follow / carry / seat (`session_20260916_003534_924`)
Player 2622, NPC 2651, veh 19. Both directions 100 % walked after adding SEND rules 0x0D/0x0E/0x17.
| t | action | client → server | server → client (broadcast) |
|---|---|---|---|
| 0.9 s | follow on | 0x14 (player, npc) | **0x0D** (14 B: tag · u16 1 · player · npc) |
| 4.5 s | follow off | 0x14 (identical) | 0x0D (identical) — toggle, no state flag |
| 6.7 / 15.4 / 23.6 s | pick up (also from the seat) | 0x13 (player, npc) | **0x0E** (14 B, same layout) |
| 9.9 / 26.2 s | put down | 0x06 ×2 (player + zeros) | **0x17** ×2 (25 B) |
| 18.3 s | seat NPC | 0x0C with playerId = **npc** (2651, veh 19, voxel (0,-12,1), seat 8) | 0x14 (30 B) with player = npc |
NPC seating reuses the seat-enter path; picking the NPC out of the seat is a plain 0x13/0x0E (no 0x15
seat-exit is emitted). 0x30 flag byte at +11 goes 1 while carrying. `stats` lists pseudo-peers 2,3,4
in this file (NPC handles used as recipient ids by the hook) — ignore them for `--peer`.

## Session 7 — seated weapon, 3 shots (`session_20260917_002036_391`)
Only 0x64 keyMask 0x0410 per shot (reference: bit 4+10 is what the seat weapon trigger sends).
No fire / projectile record in either direction; vehicle hit → SEND 0x35 body damage only.

## Session 8 — fire burn → death → respawn (`session_20260917_002237_770`)
New client tags 0x39 / 0x37 / 0x36 / 0x2E, and 0x65 / 0x01 re-observed from self (they belong to the
seat-enter burst; 0x18 still unexplained). Full sequence in damage-combat.md.

## Session 9 — fuel leak / oil (`session_20260917_011112_996`)
Nothing oil-specific in either direction (100 % walked, all tags known). The leak shows only as larger
0x2E pushes for the leaking vehicle (payload 195 → ~500 B while the tank drains, back to 64 B after):
the 0x2E payload is a list of `u8 1 · u32 byteWidth · value[byteWidth] · u8 0 · u32 componentIdx`
entries, i.e. the vehicle's changed component values (tank levels etc.). Surface oil per tile and the
"clear oil" action produced **no record at all** → oil is simulated locally by each client from the
synced fluid state, not sent by the server.

## Session 10 — fuel leak from a damaged compartment, then "clear oil" (`session_20260917_011804_825`)
100 % walked, no unknown tags. Damage: 5 × 0x35 body damage on veh 33 (+ a 0x4D object per hit).
Leak: 0x2E veh=33 pushes ~75 B for the whole run (small tank → few changed components).
t+53 s: client 0x1C despawn veh 33 → SEND 0x2C / 0x38 / 0x55 / 0x62 / 0x09.
t+77 s: client **0x45 u8=1** then **0x45 u8=0** — this is just the menu opening / closing (confirmed by
sessions 11–14); the clear-oil action itself sends **nothing** in either direction, and the server
answers nothing. Confirms session 9: surface oil is never transmitted.

## Sessions 11–14 — fog of war & tile purchase (`012608_484`, `012747_361`, `013020_623`, `013059_700`)
All 100 % walked. Fog reveal = client 0x28 per tile → SEND 0x49 + 0x45; tile purchase = client 0x19 →
SEND 0x4A + 0x62; buy-all = client 0x1A → 30 × (0x4A + 0x62); clear-all-fog = settings array only
(0x48 → 0x88, index 1). Details in reference.md (tile section) and settings.md.
Also seen during the fog flight: client 0x1B (`u32 vehId ×2`: 39, 40) and 0x29 (`u32 39`) at t+18 s
while vehicles streamed in — load acknowledgements, not yet decoded.

## Not observed in the SEND (server→client) stream (session 1)
The solo peer received no echo of its own 0x0A/0x0B: SEND had only 0xA8/0x05/0x8E/0x1A/0x39 periodics,
two 0x2E (t≈52%) and two 0x81 (t≈57%) for veh 16, two 0x1B at the end. So the server does not
reflect interactions back to the originator; whether it forwards them to *other* peers (and under
which tag) needs a 2-player capture — candidates to watch: 0x2E (per-vehicle data push) right after a
0x0A.

## Tooling gap
`Records.Decode`/`validate` filter `Dir.Send && MsgType==8`; RECV `msgType=3` bodies are parsed with
the SEND envelope (hence garbage `tick/sub/recs` in `msgs --dir R`). Adding a `msgType=3` branch with
the 6 rules above would make `validate --dir R` / `census --dir R` work for input analysis.
