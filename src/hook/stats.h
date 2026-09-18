// stats.h — per-peer and global counters exposed over IPC (stats.get / peers.get). Cumulative only:
// the DLL never computes rates. Clients poll and diff against the previous sample (nowMs is included
// in every response), which keeps this side trivial and lets any client pick its own window.
// All mutation happens under the send lock (g_cs in dllmain.cpp); reads for IPC also take it.
#pragma once
#include <cstdint>
#include <map>

namespace stats {

struct Peer {
    uint64_t firstSeenMs = 0, lastSeenMs = 0;
    bool     connected   = true;      // cleared on ch15 close (0x02), set again on any later traffic
    // native = what the game itself handed to Steam for this recipient (before our edits)
    uint64_t sendCount = 0, sendBytes = 0;
    uint64_t recvCount = 0, recvBytes = 0;
    // our contribution: extra bytes on top of native, records appended/rewritten
    uint64_t injSends = 0, injBytes = 0, injRecords = 0;
    // understanding gate, per recipient
    uint64_t type8 = 0, walkFull = 0, walkPartial = 0;
    uint32_t lastTick = 0;             // last world tick seen in a type=8 to this peer
};

inline std::map<uint64_t, Peer> g_peers;
// Liveness is inferred from traffic: the server sends every connected peer something every tick, so
// silence means gone (a client-side disconnect message is not reliably observed). Consulted by
// peers.get: silent > kSilentMs reports connected=false; silent > kExpireMs is evicted so a long
// session with many joins does not grow the table (and the peers[] response) without bound.
constexpr uint64_t kSilentMs = 5000, kExpireMs = 10 * 60 * 1000;
inline void expire(uint64_t nowMs) {
    for (auto it = g_peers.begin(); it != g_peers.end();)
        if (nowMs - it->second.lastSeenMs > kExpireMs) it = g_peers.erase(it); else ++it;
}
inline uint64_t g_walkFailDumped = 0;     // walk-fail frames written to disk this session

inline Peer& touch(uint64_t sid, uint64_t nowMs) {
    Peer& p = g_peers[sid];
    if (!p.firstSeenMs) p.firstSeenMs = nowMs;
    p.lastSeenMs = nowMs;
    p.connected = true;
    return p;
}

} // namespace stats
