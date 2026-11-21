// addon.h — vehicle poses from the Stormworks addon as an extra relay source.
//
// The WebMap addon (script.lua) reads server.getVehiclePos for EVERY vehicle every 3 ticks and sends
// [id, x, y, z, qx, qy, qz, qw] (same axes and quaternion order as the native 0x81 body0) through
// WebMapBridge to the IPC command `addon.poses`. Native 0x81 feeds are at best 5 ticks apart and only
// exist near some player; the addon covers every vehicle at 3 ticks.
//
// The addon only knows its own tick counter `tc` (reset by ?reload_scripts), not the world tick. The
// offset K (world tick = tc + K) is found here by matching: when a native body0 sample (tick T, pos P)
// arrives for a moving vehicle, the addon sample at tc = T − K must sit on P (measured: median 4 cm,
// max 14 cm), while its neighbours one tick off are ≥ the vehicle's per-tick step away. Each clear
// match is a vote; K is adopted once the recent votes agree. Until then nothing is fed to the relay.
// A tc that goes backwards (script reload) forgets K and starts over.
//
// All functions assume the caller holds the send lock (g_cs), like relay.h.
#pragma once
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include "../common/json.h"
#include "relay.h"

namespace addon {

constexpr int    kRing        = 24;    // addon samples kept per vehicle for matching (≈ 72 ticks at 3)
constexpr double kMatchDist   = 0.3;   // a native sample matches an addon sample within this (m)
constexpr double kMinStep     = 0.5;   // only vehicles moving at least this per tick can disambiguate (m)
constexpr int    kVoteRing    = 16;    // recent votes considered
constexpr int    kVotesNeeded = 4;     // agreeing votes needed to adopt / change K

struct Sample { uint32_t tc; double x, y, z; };
struct Hist {
    Sample s[kRing];
    int n = 0, head = 0;               // ring: head = next write slot
    uint32_t lastVotedTick = 0;        // one vote per native tick (every peer gets the same record)
};

inline std::unordered_map<uint32_t, Hist> g_hist;
struct Pending { uint32_t V, T; double x, y, z; int tries; };
inline std::vector<Pending> g_pending;                           // native samples waiting for addon coverage
constexpr int kPendingMax = 64, kPendingTries = 4;

// The offset actually used (g_k, g_kKnown) comes from two estimators:
//  - arrival (primary, decides within seconds of injection with nothing moving): when a batch sent with
//    no other request waiting in Stormworks (fl == 0) reaches us, the newest world tick sent minus tc.
//    The addon→Bridge delay is a few ms (median 0.24 tick), so this sits on one value. Measured equal
//    to the matched offset (TPS 38.5, 64/64 samples).
//  - matched (check): native body0 positions against the interpolated addon track (try_vote below).
//    Needs a moving vehicle that a native feed carries. When it disagrees with arrival, it wins: it
//    compares the poses themselves.
// g_src: 0 = none, 1 = arrival, 2 = matched (agrees with arrival or arrival unknown), 3 = matched over a
// disagreeing arrival.
inline bool     g_kKnown = false;
inline int64_t  g_k = 0;
inline int      g_src = 0;
inline bool     g_kMatchKnown = false;
inline int64_t  g_kMatch = 0;
inline bool     g_kArrKnown = false;
inline int64_t  g_kArr = 0;
inline int64_t  g_votes[kVoteRing];
inline int      g_nVotes = 0, g_voteHead = 0;
inline bool     g_anyTc = false;
inline uint32_t g_lastTc = 0;
// addon generation (WebMapBridge 2026-11: "ep" on each batch; absent from older Bridges). A new ep = the
// addon was re-created (?reload_scripts / another world): offset estimation starts over. Within one ep tc
// only grows, so a batch whose tc went back arrived late and is dropped instead of resetting the offset.
inline bool     g_anyEp = false;
inline double   g_lastEp = 0;
inline uint64_t g_staleBatches = 0;   // same-ep batches dropped because tc went back
inline uint64_t g_epChanges = 0;
// world tick of the last batch; the fake_missile alias / standby / park lists expire when no batch came for
// `stale` ticks (the Bridge or the addon stopped), so fakes are not kept mirrored on stale orders.
inline uint32_t g_lastBatchTick = 0;
inline uint64_t g_fakeExpired = 0;
constexpr int kArrRing = 64;
constexpr int kArrMin  = 8;            // fl == 0 arrivals needed before the arrival offset is used
inline int64_t  g_arr[kArrRing];
inline int      g_nArr = 0, g_arrHead = 0;
inline uint64_t g_arrSamples = 0, g_arrSkipped = 0;
inline bool arrival_mode(int64_t& mode, int& support) {
    mode = 0; support = 0;
    for (int i = 0; i < g_nArr; i++) {
        int c = 0; for (int j = 0; j < g_nArr; j++) if (g_arr[j] == g_arr[i]) c++;
        if (c > support) { support = c; mode = g_arr[i]; }
    }
    return g_nArr > 0;
}
// counters (cumulative, IPC status)
inline uint64_t g_batches = 0, g_samples = 0, g_fed = 0, g_bad = 0, g_voteCount = 0, g_resets = 0;
inline uint64_t g_disagree = 0;        // times the matched offset overrode a different arrival offset

inline void choose_offset() {
    if (g_kMatchKnown) {
        g_k = g_kMatch; g_kKnown = true;
        int src = (g_kArrKnown && g_kArr != g_kMatch) ? 3 : 2;
        if (src == 3 && g_src != 3) g_disagree++;
        g_src = src;
    } else if (g_kArrKnown) {
        g_k = g_kArr; g_kKnown = true; g_src = 1;
    } else {
        g_k = 0; g_kKnown = false; g_src = 0;
    }
}
inline const char* source_name() {
    switch (g_src) { case 1: return "arrival"; case 2: return "matched"; case 3: return "matched-override"; }
    return "none";
}

inline void reset_offset() {
    g_kMatchKnown = false; g_kMatch = 0; g_nVotes = 0; g_voteHead = 0;
    g_kArrKnown = false; g_kArr = 0; g_nArr = 0; g_arrHead = 0;
    choose_offset();
    relay::bodies_reset_samples();
    g_hist.clear();
    g_pending.clear();
    g_resets++;
}

inline void add_arrival(int64_t k) {
    g_arr[g_arrHead] = k; g_arrHead = (g_arrHead + 1) % kArrRing;
    if (g_nArr < kArrRing) g_nArr++;
    g_arrSamples++;
    int64_t mode; int support;
    arrival_mode(mode, support);
    // a clear majority (80 %) of at least kArrMin fl == 0 arrivals
    if (support >= kArrMin && support * 5 >= g_nArr * 4) { g_kArr = mode; g_kArrKnown = true; }
    choose_offset();
}

inline void add_vote(int64_t k) {
    g_votes[g_voteHead] = k; g_voteHead = (g_voteHead + 1) % kVoteRing;
    if (g_nVotes < kVoteRing) g_nVotes++;
    g_voteCount++;
    // mode of the recent votes; adopt it when it has enough support
    int64_t best = 0; int bestN = 0;
    for (int i = 0; i < g_nVotes; i++) {
        int c = 0; for (int j = 0; j < g_nVotes; j++) if (g_votes[j] == g_votes[i]) c++;
        if (c > bestN) { bestN = c; best = g_votes[i]; }
    }
    if (bestN >= kVotesNeeded && bestN * 2 > g_nVotes) { g_kMatch = best; g_kMatchKnown = true; }
    choose_offset();
}

// Try to turn one native body0 sample (world tick T, position P) into a vote. The addon samples are 3
// ticks apart, so they are linearly interpolated to every integer tc between two consecutive samples
// (straight within cm over 3 ticks even for missiles): the tc where the track passes through P gives
// K = T − tc. Accepted only when that is unambiguous: the best tc is within kMatchDist, every other tc
// is at least 2·kMatchDist away, and the vehicle moves at least kMinStep per tick there.
// Returns true if decided (voted, or can never match), false to retry after more addon samples arrive
// (the addon sample bracketing T may reach us after the native record).
inline bool try_vote(const Hist& h, uint32_t T, double x, double y, double z) {
    Sample s[kRing]; int n = h.n;
    for (int i = 0; i < n; i++) s[i] = h.s[i];
    std::sort(s, s + n, [](const Sample& a, const Sample& b) { return a.tc < b.tc; });
    double d1 = 1e300, d2 = 1e300, step1 = 0; int64_t t1 = 0;
    for (int i = 0; i + 1 < n; i++) {
        const Sample& a = s[i]; const Sample& b = s[i + 1];
        uint32_t gap = b.tc - a.tc;
        if (gap == 0 || gap > 6) continue;
        double vx = (b.x - a.x) / gap, vy = (b.y - a.y) / gap, vz = (b.z - a.z) / gap;
        double step = std::sqrt(vx * vx + vy * vy + vz * vz);
        for (uint32_t k = (i == 0 ? 0u : 1u); k <= gap; k++) {      // shared endpoints counted once
            double dx = a.x + vx * k - x, dy = a.y + vy * k - y, dz = a.z + vz * k - z;
            double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < d1) { d2 = d1; d1 = d; t1 = (int64_t)a.tc + k; step1 = step; }
            else if (d < d2) d2 = d;
        }
    }
    if (d1 > kMatchDist) return false;                           // not covered (yet)
    if (step1 < kMinStep || d2 < 2 * kMatchDist) return true;     // too slow to tell ticks apart
    add_vote((int64_t)T - t1);
    return true;
}

// relay::g_onNative hook: a native body0 sample of V at world tick T (every recipient gets the same
// record: one evaluation per (V, T)).
inline void on_native(uint32_t V, uint32_t T, double x, double y, double z) {
    auto it = g_hist.find(V);
    if (it == g_hist.end()) return;
    Hist& h = it->second;
    if (h.lastVotedTick == T) return;
    h.lastVotedTick = T;
    if (h.n >= 2 && try_vote(h, T, x, y, z)) return;
    if ((int)g_pending.size() >= kPendingMax) g_pending.erase(g_pending.begin());
    g_pending.push_back(Pending{ V, T, x, y, z, 0 });
}

// After an addon batch: retry the pending native samples of the vehicles it carried.
inline void retry_pending() {
    for (size_t i = 0; i < g_pending.size();) {
        Pending& p = g_pending[i];
        auto it = g_hist.find(p.V);
        bool done = it == g_hist.end() || ++p.tries > kPendingTries
                    || (it->second.n >= 2 && try_vote(it->second, p.T, p.x, p.y, p.z));
        if (done) g_pending.erase(g_pending.begin() + i); else i++;
    }
}

inline void install() { relay::g_onNative = &on_native; }

// IPC `addon.kinds` (Bridge, from the MapViewer vehicle_kinds): {"cmd":"addon.kinds","Missile":[id,...],"Ship":[...],
// "Tank":[...],"SubMarine":[...],"AirCraft":[...]}. Replaces the whole list; vehicles in none of them are Other.
// Returns the number of ids read.
inline int on_kinds(const char* line) {
    std::unordered_map<uint32_t, uint8_t> kinds;
    int n = 0;
    for (int k = relay::kKindMissile; k < relay::kKindCount; k++) {
        const char* p = json::find_value(line, relay::kKindNames[k]);
        if (!p || *p != '[') continue;
        json::for_each_elem(p, [&](const char* e) {
            double v = strtod(e, nullptr);
            if (v >= 0 && v <= 4294967295.0) { kinds[(uint32_t)v] = (uint8_t)k; n++; }
        });
    }
    relay::g_kindOf.swap(kinds);
    relay::g_kindsKnown = true;
    relay::g_kindUpdates++;
    relay::kind_sweep(true);
    return n;
}

// IPC `addon.poses`: {"cmd":"addon.poses","ep":E,"tc":N,"data":[[id,x,y,z,qx,qy,qz,qw],...]} ("ep" optional).
// Returns the number of records parsed (fed or not); `fed` counts those handed to the relay.
inline int on_batch(const char* line, uint32_t nowTick, int& fedOut) {
    fedOut = 0;
    double tcd;
    if (!json::get_num(line, "tc", tcd) || tcd < 0 || tcd > 4294967295.0) { g_bad++; return -1; }
    uint32_t tc = (uint32_t)tcd;
    const char* data = json::find_value(line, "data");
    if (!data || *data != '[') { g_bad++; return -1; }
    double ep;
    if (json::get_num(line, "ep", ep)) {
        if (g_anyEp && ep != g_lastEp) { reset_offset(); g_anyTc = false; g_epChanges++; }   // new addon generation
        g_anyEp = true; g_lastEp = ep;
        if (g_anyTc && tc < g_lastTc) { g_staleBatches++; return 0; }   // late batch of this ep: ignore
    } else if (g_anyTc && tc < g_lastTc) reset_offset();        // older Bridge (no ep): ?reload_scripts restarted tc
    g_anyTc = true; g_lastTc = tc;
    g_lastBatchTick = nowTick;
    g_batches++;
    double fl;
    if (json::get_num(line, "fl", fl) && fl == 0 && nowTick > 0) add_arrival((int64_t)nowTick - (int64_t)tc);
    else g_arrSkipped++;
    int parsed = 0;
    relay::kind_sweep(false);                                   // a kind switched off since the last batch
    json::for_each_elem(data, [&](const char* e) {
        double v[8];
        if (*e != '[' || json::arr_nums(e, v, 8) != 8) { g_bad++; return; }
        for (double c : v) if (!std::isfinite(c)) { g_bad++; return; }
        if (v[0] < 0 || v[0] > 4294967295.0) { g_bad++; return; }
        uint32_t V = (uint32_t)v[0];
        parsed++; g_samples++;
        Hist& h = g_hist[V];
        h.s[h.head] = Sample{ tc, v[1], v[2], v[3] };
        h.head = (h.head + 1) % kRing; if (h.n < kRing) h.n++;
        if (!g_kKnown || !relay::g_cfg.addonPoses) return;
        if (!relay::addon_kind_on(V)) { relay::g_kindSkipped++; return; }   // kind switched off (sync-kind-select.md)
        int64_t t = (int64_t)tc + g_k;
        if (t <= 0 || t > (int64_t)nowTick + 60) return;        // nonsense offset: wait for new votes
        float q[4] = { (float)v[4], (float)v[5], (float)v[6], (float)v[7] };
        relay::observe_addon(V, (uint32_t)t, v[1], v[2], v[3], q);
        fedOut++; g_fed++;
    });
    if (!g_pending.empty()) retry_pending();
    // turret / gun prototype: "bp":[[id, body, x, y, z, qx, qy, qz, qw], ...] = one voxel of each non-root body
    // (its world position and the body's rotation), same tc as data. Used only once the tick offset is known.
    {
        const char* bp = json::find_value(line, "bp");
        if (bp && *bp == '[' && g_kKnown) {
            int64_t t = (int64_t)tc + g_k;
            if (t > 0 && t <= (int64_t)nowTick + 60) json::for_each_elem(bp, [&](const char* e) {
                double v[9];
                if (*e != '[' || json::arr_nums(e, v, 9) != 9) { g_bad++; return; }
                for (double c : v) if (!std::isfinite(c)) { g_bad++; return; }
                if (v[0] < 0 || v[0] > 4294967295.0 || v[1] < 1 || v[1] > 63) { g_bad++; return; }
                double q[4] = { v[5], v[6], v[7], v[8] };
                relay::body_sample((uint32_t)v[0], (int)v[1], (uint32_t)t, v[2], v[3], v[4], q);
            });
        }
        if (!relay::g_bodyPending.empty()) relay::bodies_retry(nowTick);
    }
    // fake_missile standby (relay.h g_standby): "fk":[F,...] = all fakes; absent = none. Before "al":
    // set_alias sends a released fake back to standby only if it is listed here.
    {
        std::set<uint32_t> fk;
        const char* fp = json::find_value(line, "fk");
        if (fp && *fp == '[') json::for_each_elem(fp, [&](const char* e) {
            double v = strtod(e, nullptr);
            if (v > 0 && v < 4294967295.0) fk.insert((uint32_t)v);
        });
        if (fk != relay::g_standby) relay::set_standby(fk);
    }
    // fake_missile alias (relay.h g_alias): "al":[[F,T],...]; absent = no alias
    {
        std::vector<std::pair<uint32_t, uint32_t>> al;
        const char* ap = json::find_value(line, "al");
        if (ap && *ap == '[') json::for_each_elem(ap, [&](const char* e) {
            double v[2];
            if (*e == '[' && json::arr_nums(e, v, 2) == 2 && v[0] > 0 && v[1] > 0 && v[0] < 4294967295.0 && v[1] < 4294967295.0)
                al.push_back(std::make_pair((uint32_t)v[0], (uint32_t)v[1]));
        });
        // ship standby (relay.h g_park): "pk":[[F,S],...] joins the alias list; g_park says which are parks
        std::set<uint32_t> pk;
        const char* pp = json::find_value(line, "pk");
        if (pp && *pp == '[') json::for_each_elem(pp, [&](const char* e) {
            double v[2];
            if (*e == '[' && json::arr_nums(e, v, 2) == 2 && v[0] > 0 && v[1] > 0 && v[0] < 4294967295.0 && v[1] < 4294967295.0) {
                uint32_t f = (uint32_t)v[0];
                bool dup = false;
                for (auto& x : al) if (x.first == f) dup = true;                // an alias wins over a park
                if (!dup) { al.push_back(std::make_pair(f, (uint32_t)v[1])); pk.insert(f); }
            }
        });
        relay::g_park = pk;
        if (al != relay::g_alias) relay::set_alias(al);
    }
    // forget vehicles the addon stopped reporting (despawned) so the map does not grow
    if ((g_batches & 0xFF) == 0) {
        for (auto it = g_hist.begin(); it != g_hist.end();) {
            const Hist& h = it->second;
            const Sample& last = h.s[(h.head + kRing - 1) % kRing];
            if (h.n > 0 && tc > last.tc + 3600) it = g_hist.erase(it); else ++it;
        }
        relay::prune_addon_only(nowTick);
    }
    return parsed;
}

// Called on each outgoing type=8 send (world tick T). Drops the fake_missile alias / standby / park lists
// when the addon feed stopped for `stale` ticks: they are orders from the addon, valid only while it sends.
inline void expire_fakes(uint32_t T) {
    if (relay::g_alias.empty() && relay::g_standby.empty() && relay::g_park.empty()) return;
    if (g_lastBatchTick == 0 || T <= g_lastBatchTick + (uint32_t)relay::g_cfg.stale) return;
    relay::set_standby({});          // first: released aliases then snap back to the real pose, not to standby
    relay::g_park.clear();
    relay::set_alias({});
    g_fakeExpired++;
}

} // namespace addon
