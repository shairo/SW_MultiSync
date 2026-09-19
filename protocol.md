# Stormworks server→client protocol (v1.15.23+) — index

Wire-protocol notes for the live gameplay stream: server→client SEND, ch0, `msgType=8`, records after
a `u32 tag`. This file is the **index + master tag table**; details live in category files under
[`protocol/`](protocol/). Length rules are implemented in **`tools/swcap/Records.cs`** (source of truth);
the analysis playbook is **`tools/swcap/README.md`**.

- [reference.md](protocol/reference.md) — envelope, walker/recordCount, `--peer`, tools, scenario map, periodic tags, non-records
- [lifecycle.md](protocol/lifecycle.md) — vehicle & object spawn / load / unload / despawn / destroy
- [sync.md](protocol/sync.md) — position/physics sync (0x81, 0x39, 0x1B, 0x1D, 0x50)
- [damage-combat.md](protocol/damage-combat.md) — 0x35 damage + combat tags
- [ui-chat.md](protocol/ui-chat.md) — chat, popups, tooltip, interaction, avatars
- [settings.md](protocol/settings.md) — world / game settings menu: bool[33] array, time, weather, wind (both directions)
- [client.md](protocol/client.md) — **client→server** RECV `msgType=3`: look, player pose, heartbeat, component interact (0x0A), keypad (0x0B)

**Status legend** — Len: ✅ fixed/variable rule confirmed · △ variable, rule not pinned · ❌ undefined (blocks walker).
Mean: ✅ decoded · △ partial · ❌ unknown.

## Master tag table

| tag | dec | purpose | Len | Mean | file |
|-----|-----|---------|-----|------|------|
| 0x81 | 129 | vehicle body position/attitude sync (**relay target**); `time`=future-tick ETA | ✅ | ✅ | sync |
| 0x39 | 57 | vehicle low-frequency position sync | ✅ | ✅ | sync |
| 0x1B | 27 | set NUMBER signal on a vehicle part (float value) | ✅ | ✅ | sync |
| 0x1D | 29 | **object** position/physics sync (new; object's 0x1B), fixed 34, batched in runs | ✅ | △ | sync |
| 0x50 | 80 | NPC/object state: per-body position + HP | ✅ | ✅ | sync |
| 0xB5 | 181 | vehicle transform/teleport (setVehiclePos): 4x4 matrix | ✅ | ✅ | sync |
| 0x5E | 94 | PLAYER teleport (map fast-travel): double[3] pos | ✅ | ✅ | sync |
| 0x55 | 85 | PLAYER teleport (addon setPlayerPos): double[3] pos | ✅ | ✅ | sync |
| 0x96 | 150 | vehicle create marker (13-B pair) | ✅ | ✅ | lifecycle |
| 0x37 | 55 | vehicle spawn placement (pos): 54 + nA (vehicle name @+50) + nB (@+52+nA) | ✅ | ✅ | lifecycle |
| 0x2B | 43 | vehicle spawn placement-2 (90+nA+nB: location name + display name; always + 0x62) | ✅ | ✅ | lifecycle |
| 0x2D | 45 | vehicle load state (push family) | ✅ | △ | lifecycle |
| 0x2F | 47 | vehicle load state (zlib transform) | ✅ | △ | lifecycle |
| 0x2C | 44 | **universal remove** vehicle id (unload/despawn/destroy) | ✅ | ✅ | lifecycle |
| 0x38 | 56 | **despawn-only** marker (with 0x2C) | ✅ | ✅ | lifecycle |
| 0x4D | 77 | spawn full-state (vehicle wreck & **object spawn**) | ✅ | ✅ | lifecycle |
| 0x4F | 79 | **object despawn/collect** | ✅ | ✅ | lifecycle |
| 0x35 | 53 | body damage application (per body, pos+magnitude) | ✅ | ✅ | damage-combat |
| 0x2E | 46 | per-vehicle bulk data push (state) | ✅ | △ | damage-combat |
| 0x27 | 39 | combat | ✅ | ❌ | damage-combat |
| 0x19 | 25 | combat | ✅ | ❌ | damage-combat |
| 0x30 | 48 | combat list entry | ✅ | ❌ | damage-combat |
| 0x34 | 52 | combat | ✅ | ❌ | damage-combat |
| 0x28 | 40 | S9 damage (subtype-driven length) | ✅ | ❌ | damage-combat |
| 0x61 | 97 | character appearance (ARGB palette); sent on seat exit w/ 0x15 | ✅ | ✅ | ui-chat |
| 0x3A | 58 | vehicle MAP label (addon; map-screen text); pairs w/ 0x3B | ✅ | ✅ | ui-chat |
| 0x3B | 59 | precedes 0x3A (map-label run marker) | ✅ | △ | ui-chat |
| 0x4A | 74 | tile purchased (12 B: x,z) + 0x62; buy-all = 30 in one message | ✅ | ✅ | reference |
| 0x95 | 149 | map-object create marker (10+n: objId + label), precedes 0x3A | ✅ | ✅ | ui-chat |
| 0x49 0x45 | 73 69 | fog-of-war tile revealed (x,z) pair — mirror of client 0x28 | ✅ | ✅ | reference |
| 0x46 | 70 | tile streaming ring (x,z), precedes 0x47 runs | ✅ | △ | reference |
| 0x3F | 63 | tree felled (tile coord + treeIndex + type) | ✅ | ✅ | reference |
| 0xAD | 173 | tree fall physics (float[3]); pairs w/ 0x3F | ✅ | △ | reference |
| 0x92 0x76 0x31 0x9D 0x74 0x75 0xA9 | 146 118 49 157 116 117 169 | combat-capture records, lengths verified (44/32/20/67/40/36/32), meaning candidate | ✅ | △ | damage-combat |
| 0x7B | 123 | addon notification (title/subtitle/type) | ✅ | ✅ | damage-combat |
| 0x4C 0x8F 0x3D | 76 143 61 | map-object remove family (8 B, u32 id) with 0x3B | ✅ | △ | damage-combat |
| 0x03 0x8D 0x91 0x5F 0x16 | 3 141 145 95 22 | lobby / per-peer records (player joined, peer message, …) | ✅ | △ | damage-combat |
| 0xB8 0xB9 0xBA | 184 185 186 | vehicle voxel runs: 2×(veh, idx, i32[3]) + u32,u32 / u32 / u8 (52 / 48 / 45 B) | ✅ | △ | damage-combat |
| 0xBF 0xC0 | 191 192 | vehicle voxel runs ×4: + u32 RGBA (88 B) / each entry + f32 (100 B); paired on the same voxels | ✅ | △ | damage-combat |
| 0x04 | 4 | 16 B fixed, always last record of its message (walkfail_20260919_125459 ×13) | ✅ | ❌ | — |
| 0x88 | 136 | game settings bool[33] broadcast (37 B) — mirror of client 0x48 | ✅ | ✅ | settings |
| 0x57 | 87 | time of day u32 seconds (8 B) — client 0x31 | ✅ | ✅ | settings |
| 0x8B | 139 | day length? u32 (8 B) — client 0x4B | ✅ | △ | settings |
| 0x5A | 90 | weather f32[3] + wind dir (20 B) — client 0x2A | ✅ | ✅ | settings |
| 0x89 | 137 | wind direction f32 (8 B) — client 0x49 | ✅ | ✅ | settings |
| 0x59 0x5D 0x8A | 89 93 138 | settings bools (5 B) — client 0x32 / 0x2C / 0x4A | ✅ | △ | settings |
| 0x26 | 38 | inventory slot state (35 B) | ✅ | ✅ | damage-combat |
| 0x64 | 100 | character death broadcast (20 + payload) | ✅ | ✅ | damage-combat |
| 0x06 | 6 | character full state on player join (large, variable) | ❌ | △ | damage-combat |
| 0x63 | 99 | respawn position (30 B) | ✅ | ✅ | damage-combat |
| 0x65 | 101 | heal applied (13 B) — mirror of client 0x38 | ✅ | ✅ | damage-combat |
| 0x01 | 1 | chat / system / addon-log message | ✅ | ✅ | ui-chat |
| 0x8E | 142 | popup / setPopupScreen | ✅ | ✅ | ui-chat |
| 0x2A | 42 | character hp set (14 B: charId · f32 hp · u8 · u8) — old "tooltip" reading was 0x2A + 0x95 | ✅ | ✅ | damage-combat |
| 0x29 | 41 | equip/inventory action (player+slot+"equip"/"swap") | ✅ | ✅ | ui-chat |
| 0x1A | 26 | set BOOL signal on a vehicle part (0x1B's on/off twin) | ✅ | ✅ | ui-chat |
| 0x14 | 20 | seat SIT (enter): characterId + vehicleId | ✅ | ✅ | ui-chat |
| 0x0D | 13 | NPC FOLLOW toggle: u16 peer_id + playerId + npcId (14 B) | ✅ | ✅ | client |
| 0x0E | 14 | NPC PICK-UP / carry: same layout as 0x0D (14 B) | ✅ | ✅ | client |
| 0x17 | 23 | NPC PUT-DOWN: playerId + 17 B zero (25 B, emitted ×2) | ✅ | ✅ | client |
| 0x15 | 21 | seat STAND (exit); paired with 0x61 | ✅ | ✅ | ui-chat |
| 0x20 | 32 | equip-adjacent (S4) | ✅ | ❌ | ui-chat |
| 0x67 | 103 | character vitals (hp) — mirror of client 0x39 | ✅ | ✅ | damage-combat |
| 0x0B 0x0C | 11 12 | other-player avatar pair | ✅ | ✅ | ui-chat |
| 0x05 0x09 0x62 0x94 0x47 | 5 9 98 148 71 | periodic / spawn-burst / misc | ✅ | ❌ | reference |
| 0x07 | 7 | seat-entry snapshot broadcast (71 B) — mirror of client 0x01 | ✅ | ✅ | client |
| 0xA6 0xA7 0xA8 | 166 167 168 | player input broadcasts: seat keyMask / seat axis / camera look — mirrors of client 0x64 / 0x65 / 0x66 | ✅ | ✅ | client |

This table lists only real records. Values that appear only as walker-misread artifacts (0x00, 0x100,
0xA9) are intentionally omitted — see the "Not a record / unverified" note in
[reference.md](protocol/reference.md).

## Carryover from pre-update protocol
Vehicle ID is still a raw little-endian u32 carried directly (values 16..19 for a 4-vehicle session).
0x39/0x8E/0x01 survived the v1.15.23 "Multiplayer API Update" with identical tag IDs and layouts.
