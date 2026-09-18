# World / game settings (settings menu) — `session_20260917_005633_989`, 100 % walked both directions

Every change in the host's settings menu is sent client→server as a small record and rebroadcast
server→all with a **different tag number but the same payload**. No server-side validation is visible.

## Bool settings — one 33-byte array, resent whole on every toggle
| dir | tag | len | layout |
|---|---|---|---|
| client | 0x48 | 37 | tag+3 · u8[33] |
| SEND | 0x88 | 37 | tag · u8[33] (byte-identical mirror; was misfiled as "S9 character") |

Index → setting is not yet named, except **index 1 = fog of war cleared / disabled** (the "clear all fog"
menu action sends only this array with index 1 → 1, session_20260917_013020). The capture toggled, in order, indexes
2,3,4,5,6,8,9,10,11,12,13,24,28,27,26,22,21,20,17,18,16,14,15,19,23 (then back). Initial state
`1,0,0,1,0,0,0,1,1,1,0,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,1,1`. Indexes 0, 1, 7, 25, 29–32 were
never touched by the tester. Map them by toggling one named setting per capture.

Three bools have their **own** record instead (each toggled right before the related slider):
| client | SEND | len | guess |
|---|---|---|---|
| 0x32 | 0x59 | 5 (tag · u8) | day/night cycle (toggled before the time slider) |
| 0x2C | 0x5D | 5 | weather override (before fog/rain/wind sliders) |
| 0x4A | 0x8A | 5 | wind-direction override (before the wind-direction slider) |

## Sliders
| client | SEND | payload | notes |
|---|---|---|---|
| 0x31 (8) | 0x57 (8) | f32 fraction → u32 seconds | `seconds = fraction × 108000` (0.55 → 59400, 0.458 → 49500). Sent every tick while dragging |
| 0x4B (8) | 0x8B (8) | u32 | 30 / 40 / 20 seen; sent together with a 0x31 → day length (minutes?) candidate |
| 0x2A (16) | 0x5A (20) | f32[3] (+ f32 wind dir on SEND) | fog / rain / wind sliders 0..1, order TBD (tester dragged [1], [2], [0]). SEND appends the current wind direction as the 4th float |
| 0x49 (8) | 0x89 (8) | f32 rad −π..π | wind direction; also mirrored into 0x5A[3] one tick later |

Client records are `tag + 3 pad` then payload (client.md envelope); SEND records are `u32 tag` then payload.
