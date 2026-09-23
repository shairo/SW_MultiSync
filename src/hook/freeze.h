// freeze.h — passive per-peer "client froze" detector for the tile-crossing freeze.
//
// Background (captures/session_20260913_214246_776_freeze.swcap, see protocol/lifecycle.md "Vehicle
// definition handshake" and tools/swcap/README.md): when a client freezes, the server keeps sending a
// complete stream and the client keeps answering everything that its network thread handles — 1 Hz
// heartbeat 0x34, camera look 0x66, tile-load acks 0x28, even vehicle-definition REQUESTS 0x1B — but
// two things stop dead, both driven by its world simulation:
//   (1) the player pose 0x2F stops changing while the vehicle it sits in keeps moving on the server;
//   (2) vehicle definitions it requested (0x1B) are never acknowledged as loaded (0x29), although
//       the server did push them (ch1 type=12) and every other peer acked within ~60 ms.
// Nothing on the wire precedes the freeze, so the DLL cannot prevent it — but it can SEE it within
// a few seconds, stamp the capture and the log, and expose it over IPC so mitigation experiments
// (debug.nudge) can be tried at the right moment and their effect measured.
//
// All calls are made under g_cs (dllmain.cpp). Pure bookkeeping; nothing here touches a send.
#pragma once
#include <cstdint>
#include <cmath>
#include <map>
#include "relay.h"

namespace freeze {

struct Peer {
    // --- pose: client 0x2F world position, and when it last changed ---
    double   sx = 0, sy = 0, sz = 0;          // last pose seen
    uint64_t poseMs = 0;                      // stamp of the last 0x2F (0 = none yet)
    uint64_t staticSinceMs = 0;               // stamp of the last pose CHANGE (== poseMs while moving)
    uint32_t poseVeh = 0;                     // 0x2F u32 @+52: the vehicle the character is in/on (0 = none)
    double   lastStep = 0;                    // distance of the last pose CHANGE (speed proxy: 0x2F comes every ~5 ticks)
    // server-side position of poseVeh when the pose went static (relay::g_veh body0)
    bool     anchored = false; double ax = 0, az = 0;
    // --- vehicle definition handshake: 0x1B request → ch1 type12 push → 0x29 loaded ---
    std::map<uint32_t, uint64_t> pending;     // vehId → ms of the 0x1B request (erased by 0x29)
    std::map<uint32_t, uint64_t> pushed;      // vehId → ms the server pushed the definition (ch1 type 12)
    uint64_t defReqs = 0, defAcks = 0;
    // --- verdict ---
    uint64_t frozenSinceMs = 0;               // 0 = healthy
    const char* reason = "";                  // "pose" | "defs" | "pose+defs"
    uint64_t events = 0;                      // freeze onsets seen for this peer
    uint64_t lastCheckMs = 0;
};
inline std::map<uint64_t, Peer> g_peers;
inline uint64_t g_events = 0;                  // total onsets, all peers (status field)

constexpr double kPoseEps    = 1e-3;   // pose change below this = "did not move"
constexpr double kVehMoveM   = 20.0;   // seated vehicle must have moved this far while the pose stood still
constexpr double kSeatDistM  = 60.0;   // pose must have been this close to poseVeh when it went static (= was in/on it)
constexpr double kMinStepM   = 0.5;    // and must have been MOVING right before (last 0x2F step >= this, ~6 m/s): a player
                                       // standing beside a vehicle someone else drives away is not a freeze

inline Peer& at(uint64_t sid) { return g_peers[sid]; }

// Client 0x2F: +28 double[3] world pos, +52 u32 vehicle.
inline void on_pose(uint64_t sid, double x, double y, double z, uint32_t veh, uint64_t now) {
    Peer& p = at(sid);
    double dx0 = x - p.sx, dy0 = y - p.sy, dz0 = z - p.sz;
    bool moved = !p.poseMs || std::fabs(dx0) > kPoseEps || std::fabs(dy0) > kPoseEps || std::fabs(dz0) > kPoseEps;
    p.sx = x; p.sy = y; p.sz = z; p.poseVeh = veh; p.poseMs = now;
    if (moved) { p.staticSinceMs = now; p.anchored = false; p.lastStep = std::sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0); return; }
    if (!p.anchored && p.lastStep >= kMinStepM) {  // first static sample after motion: remember where the vehicle was
        auto it = relay::g_veh.find(veh);
        if (veh && it != relay::g_veh.end() && it->second.has) {
            double dx = it->second.cx - x, dz = it->second.cz - z;
            if (std::sqrt(dx * dx + dz * dz) <= kSeatDistM) { p.anchored = true; p.ax = it->second.cx; p.az = it->second.cz; }
        }
    }
}
inline void on_defreq (uint64_t sid, uint32_t veh, uint64_t now) { Peer& p = at(sid); p.defReqs++; p.pending.emplace(veh, now); }
inline void on_defack (uint64_t sid, uint32_t veh, uint64_t now) { Peer& p = at(sid); p.defAcks++; p.pending.erase(veh); p.pushed.erase(veh); (void)now; }
inline void on_defpush(uint64_t sid, uint32_t veh, uint64_t now) { at(sid).pushed[veh] = now; }
// Server 0x2C (remove vehicle id) to this peer: a definition it will never be asked to load again.
inline void on_remove (uint64_t sid, uint32_t veh) { Peer& p = at(sid); p.pending.erase(veh); p.pushed.erase(veh); }

// Oldest pending request whose definition the server DID push (a request the server never answered,
// e.g. a vehicle despawned in between, is not the client's fault). Returns its age in ms, 0 if none.
inline uint64_t oldest_unacked_ms(const Peer& p, uint64_t now) {
    uint64_t worst = 0;
    for (auto& kv : p.pending) {
        auto ph = kv.second; auto it = p.pushed.find(kv.first);
        if (it == p.pushed.end()) continue;
        uint64_t age = now - (it->second > ph ? it->second : ph);
        if (age > worst) worst = age;
    }
    return worst;
}

// Evaluate one peer. Returns +1 on a freeze onset, -1 on recovery, 0 otherwise. poseMs/defMs are the
// thresholds (cfg.freezePoseMs / cfg.freezeDefMs); a threshold <= 0 disables that signal.
inline int check(uint64_t sid, uint64_t now, int poseMs, int defMs) {
    Peer& p = at(sid);
    if (now - p.lastCheckMs < 250) return 0;
    p.lastCheckMs = now;
    bool poseSig = false, defSig = false;
    if (poseMs > 0 && p.anchored && p.poseMs && now - p.staticSinceMs >= (uint64_t)poseMs) {
        auto it = relay::g_veh.find(p.poseVeh);
        if (it != relay::g_veh.end() && it->second.has) {
            double dx = it->second.cx - p.ax, dz = it->second.cz - p.az;
            poseSig = std::sqrt(dx * dx + dz * dz) >= kVehMoveM;
        }
    }
    if (defMs > 0) defSig = oldest_unacked_ms(p, now) >= (uint64_t)defMs;
    bool frozen = poseSig || defSig;
    if (frozen && !p.frozenSinceMs) {
        p.frozenSinceMs = now; p.events++; g_events++;
        p.reason = poseSig && defSig ? "pose+defs" : poseSig ? "pose" : "defs";
        return +1;
    }
    if (!frozen && p.frozenSinceMs) { p.frozenSinceMs = 0; p.reason = ""; return -1; }
    if (frozen) p.reason = poseSig && defSig ? "pose+defs" : poseSig ? "pose" : "defs";
    return 0;
}

inline void forget(uint64_t sid) { g_peers.erase(sid); }

} // namespace freeze
