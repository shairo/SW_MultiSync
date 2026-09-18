// rec.h — C++ port of the msgType=8 record-length rules (mirror of tools/swcap/Records.cs).
// Used in-process to (a) GATE mutation — we only ever touch a message our walker fully decodes —
// and (b) locate 0x81 vehicle-sync records for edit/inject. Keep in lockstep with Records.cs:
// every length rule here must match, or the offline validator and the live hook disagree.
#pragma once
#include <cstdint>

namespace rec {

inline uint32_t U16(const uint8_t* b, int len, int o) {
    return (o + 2 <= len) ? (uint32_t)(b[o] | (b[o + 1] << 8)) : 0xFFFFu;
}
inline uint32_t U32(const uint8_t* b, int len, int o) {
    return (o + 4 <= len) ? (uint32_t)(b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((uint32_t)b[o+3]<<24)) : 0xFFFFFFFFu;
}

// Length of the record whose tag u32 is at `off` in body[0..len). Returns -1 if the rule is unknown
// or the computed length would overrun (walk must stop). Mirrors Records.Decode.
inline int decode_len(const uint8_t* b, int len, int off) {
    uint32_t tag = U32(b, len, off);
    int L = -1;
    switch (tag) {
        // --- fixed-length (see protocol/*.md for meaning) ---
        case 0x39: L = 36; break;
        case 0x38: L = 8;  break;   // vehicle despawn marker (with 0x2C)
        case 0x1D: L = 34; break;   // object position/physics sync (object 0x1B)
        case 0xB5: L = 136; break;  // vehicle teleport / setVehiclePos (4x4 matrix)
        case 0x5E: L = 29; break;   // player teleport: map fast-travel
        case 0x3F: L = 26; break;   // tree felled marker
        case 0xAD: L = 20; break;   // tree fall physics
        case 0x46: L = 12; break;   // tile streaming ring (x,z)
        case 0x49: L = 12; break;   // fog-of-war tile revealed (x,z), pairs w/ 0x45
        case 0x45: L = 13; break;   // fog tile state (x,z,u8)
        case 0x4A: L = 12; break;   // tile purchased (x,z), + 0x62
        case 0x96: L = 13; break;   // vehicle create marker (13-B pair)
        case 0x37: L = 54; break;   // vehicle spawn placement
        case 0xA8: L = 22; break;   // player look broadcast (peer_id, yaw, pitch)
        case 0x05: L = 34; break;
        case 0xA6: L = 22; break;   // seat key state (peer_id, keyMask)
        case 0x1A: L = 40; break;   // component interact broadcast
        case 0xA7: L = 11; break;   // seat axis (peer_id, idx, value)
        case 0x07: L = 71; break;   // seat-entry snapshot (peer_id, yaw, pitch)
        case 0x1B: L = 34; break;
        case 0x09: L = 4;  break;
        case 0x14: L = 30; break;   // seat enter
        case 0x15: L = 10; break;   // seat exit
        case 0x20: L = 52; break;
        case 0x55: L = 31; break;
        case 0x62: L = 12; break;   // load/purchase companion (u32 20000, u32 5)
        case 0x67: L = 25; break;   // character vitals (hp)
        case 0x88: L = 37; break;   // game settings bool[33]
        case 0x94: L = 8;  break;
        case 0x4F: L = 8;  break;
        case 0x0B: L = 38; break;
        case 0x0C: L = 14; break;
        case 0x0D: L = 14; break;   // NPC follow toggle
        case 0x0E: L = 14; break;   // NPC pick-up
        case 0x17: L = 25; break;   // NPC put-down
        case 0x63: L = 30; break;   // respawn position
        case 0x26: L = 35; break;   // inventory slot state
        case 0x65: L = 13; break;   // heal applied
        case 0x2A: L = 14; break;   // character hp set (was mis-read as a 24+len "tooltip" = 0x2A + 0x95)
        case 0x27: L = 12; break;
        case 0x35: L = 34; break;   // body damage
        case 0x61: L = 116; break;  // character appearance
        case 0x19: L = 54; break;
        case 0x2C: L = 8;  break;   // vehicle remove
        case 0x3B: L = 8;  break;   // map-object family (u32 id)
        case 0x4C: L = 8;  break;
        case 0x8F: L = 8;  break;
        case 0x3D: L = 8;  break;
        case 0x30: L = 36; break;
        case 0x34: L = 49; break;
        case 0x5A: L = 20; break;   // weather f32[3] + wind dir
        case 0x57: L = 8;  break;   // time of day
        case 0x89: L = 8;  break;   // wind direction
        case 0x8B: L = 8;  break;   // day length?
        case 0x59: L = 5;  break;   // settings bools
        case 0x5D: L = 5;  break;
        case 0x8A: L = 5;  break;
        case 0x92: L = 44; break;   // combat: pos + f32[4]
        case 0x76: L = 32; break;   // combat: pos + f32
        case 0x31: L = 20; break;   // voxel event (veh, x, y, z)
        case 0x9D: L = 67; break;
        case 0xA9: L = 32; break;
        case 0x74: L = 40; break;
        case 0x75: L = 36; break;
        case 0x91: L = 28; break;   // per-peer lobby records
        case 0x5F: L = 10; break;
        case 0x16: L = 11; break;
        case 0xB8: L = 52; break;   // vehicle component/body state runs
        case 0xB9: L = 48; break;
        // --- subtype / payload-length driven ---
        case 0x28: L = (U32(b, len, off + 8) == 1) ? 12 : 46; break;
        case 0x50: L = 12 + (int)U32(b, len, off + 8); break;          // NPC/object state
        case 0x2D: case 0x2F: case 0x2E: case 0x4D:
            L = 12 + (int)U32(b, len, off + 8); break;
        case 0x47: L = 16 + (int)U32(b, len, off + 12); break;
        case 0x64: L = 20 + (int)U32(b, len, off + 16); break;         // character death (payload of component-value entries)
        // --- string-length driven ---
        case 0x01: {                                                    // chat / system message
            int a2 = (int)U16(b, len, off + 4);
            int b2 = (int)U16(b, len, off + 6 + a2);
            L = 8 + a2 + b2; break;
        }
        case 0x29: L = 23 + (int)U16(b, len, off + 12); break;         // item action mirror
        case 0x2B: {                                                    // spawn placement-2: two strings
            int nA = (int)U16(b, len, off + 44);
            int nB = (int)U16(b, len, off + 46 + nA);
            L = 90 + nA + nB; break;
        }
        case 0x95: L = 10 + (int)U16(b, len, off + 8); break;          // map-object create marker (id + label)
        case 0x7B: {                                                    // addon notification (title, subtitle, type)
            int n1 = (int)U16(b, len, off + 4);
            int n2 = (int)U16(b, len, off + 6 + n1);
            L = 12 + n1 + n2; break;
        }
        case 0x03: L = 11 + (int)U16(b, len, off + 6); break;          // player joined (peer_id, name)
        case 0x8D: {                                                    // peer-addressed message
            int n1 = (int)U16(b, len, off + 12);
            int n2 = (int)U16(b, len, off + 14 + n1);
            L = 16 + n1 + n2; break;
        }
        case 0x3A: { // map object (addon addMapObject): 64-B header + u32 + u16 label + u16 hover + 12
            int p = off + 68;
            if (p + 2 <= len) { int l1 = (int)U16(b, len, p); p += 2 + l1;
                if (p + 2 <= len) { int l2 = (int)U16(b, len, p); p += 2 + l2; } }
            p += 12;
            L = p - off; break;
        }
        case 0x8E: {                                                    // popup
            int p = off + 4;
            int nameSize = (int)U16(b, len, p); p += 2 + nameSize;
            p += 2;
            int textSize = (int)U16(b, len, p); p += 2 + textSize;
            p += 16 + 12;
            L = p - off; break;
        }
        case 0x81: {                                                    // vehicle sync (bodies)
            int p = off + 12;
            int bodyCount = (int)U16(b, len, p); p += 2;
            for (int i = 0; i < bodyCount; i++) {
                if (p >= len) return -1;
                int type = b[p]; p += 1;
                if (type == 1) p += 40;
                else if (type == 2) p += 28;
                else if (type != 0) return -1;     // unknown body type
            }
            L = p - off; break;
        }
        // 0x06 (character full state on player join) has no rule yet — walk stops there by design.
        default: return -1;
    }
    if (L <= 0 || off + L > len) return -1;
    return L;
}

// Walk result. full = every record decoded and consumed the body exactly (records begin at off 20).
struct Walk {
    bool full;
    int  consumed;
    int  recordCount;      // declared count (body+16)
    int  walked;           // records actually decoded
    int  first81;          // body offset of the first 0x81, or -1
};

// Walk a msgType=8 message body (const1 at body[0]; records at body[20]).
inline Walk walk(const uint8_t* body, int len) {
    Walk w{ false, 20, 0, 0, -1 };
    if (len < 20) { w.consumed = 0; return w; }
    int count = (int)U32(body, len, 16);
    w.recordCount = count;
    int off = 20;
    for (int i = 0; i < count; i++) {
        if (off >= len) break;
        uint32_t tag = U32(body, len, off);
        int L = decode_len(body, len, off);
        if (L <= 0) { w.consumed = off; w.walked = i; return w; }
        if (tag == 0x81 && w.first81 < 0) w.first81 = off;
        off += L; w.walked = i + 1;
    }
    w.consumed = off;
    w.full = (off == len && w.walked == count);
    return w;
}

} // namespace rec
