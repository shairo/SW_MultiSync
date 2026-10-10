// tiles.h — per-peer ledger of the tile load/unload records the SERVER sent each player, for the
// ghost-chunk investigation: `?unstuck` fixes the stall by re-sending 0x45 for the 5x5 ring, so the
// client was waiting on a tile it believed unloaded. The ledger answers "did the server ever send
// that load (and did the client ack it)?" at the moment the player asks — see unstuck.h report.
//
// Fed from the native type=8 stream only (records our own nudges append are never walked), plus the
// client's 0x28 tile ack. Reset on the peer's join request and on its session close, so a rejoin
// starts clean. The DLL may be injected (or re-injected) mid-session: a tile with no entry is then
// genuinely UNKNOWN, not "never loaded" — the report must say so instead of guessing.
//
// All calls are made under g_cs (dllmain.cpp). Pure bookkeeping; nothing here touches a send.
#pragma once
#include <cstdint>
#include <map>
#include <utility>

namespace tiles {

struct Tile {
    uint8_t  last = 0;           // 0x45 load / 0x46 unload — the last server record for this tile
    uint8_t  flag = 0;           // 0x45 trailing u8 (purchased) of the last load
    uint32_t tick = 0;           // world tick of that record
    uint64_t ms = 0;             // GetTickCount64 of that record
    uint32_t loads = 0, unloads = 0;
    uint64_t ackMs = 0;          // first client 0x28 after the last server load (0 = none yet)
    uint64_t injMs = 0;          // last time WE re-sent a load (?unstuck): acks after it are ours, not the server's
};
struct Peer {
    std::map<std::pair<int32_t, int32_t>, Tile> t;
    uint64_t sinceMs = 0;        // ledger start: DLL load or the peer's last join/close
    uint64_t gaps = 0;           // type=8 msgs to this peer that did not fully walk since then (may hide tile records)
};
inline std::map<uint64_t, Peer> g_peers;

inline Peer& at(uint64_t sid, uint64_t now) {
    Peer& p = g_peers[sid];
    if (!p.sinceMs) p.sinceMs = now;
    return p;
}
inline void reset(uint64_t sid, uint64_t now) { g_peers.erase(sid); at(sid, now); }

inline void on_load(uint64_t sid, int32_t x, int32_t z, uint8_t flag, uint32_t tick, uint64_t now) {
    Tile& t = at(sid, now).t[{ x, z }];
    t.last = 0x45; t.flag = flag; t.tick = tick; t.ms = now; t.loads++; t.ackMs = 0;
}
inline void on_unload(uint64_t sid, int32_t x, int32_t z, uint32_t tick, uint64_t now) {
    Tile& t = at(sid, now).t[{ x, z }];
    t.last = 0x46; t.tick = tick; t.ms = now; t.unloads++;
}
// Client 0x28 (x,z). Also sent unprompted on fog clearing; credited only to a pending server load
// that we have not re-sent ourselves since.
inline void on_ack(uint64_t sid, int32_t x, int32_t z, uint64_t now) {
    auto pit = g_peers.find(sid); if (pit == g_peers.end()) return;
    auto it = pit->second.t.find({ x, z }); if (it == pit->second.t.end()) return;
    Tile& t = it->second;
    if (t.last == 0x45 && !t.ackMs && t.injMs < t.ms) t.ackMs = now;
}
inline void on_gap(uint64_t sid, uint64_t now) { at(sid, now).gaps++; }
inline void on_inject(uint64_t sid, int32_t x, int32_t z, uint64_t now) {
    auto pit = g_peers.find(sid); if (pit == g_peers.end()) return;
    auto it = pit->second.t.find({ x, z }); if (it != pit->second.t.end()) it->second.injMs = now;
}

} // namespace tiles
