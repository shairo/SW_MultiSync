// relay.h — stage C: cross-peer 0x81 relay (vehicle position-sync improvement).
//
// The server calls SendMessageToUser once per recipient, so this process sees every peer's stream.
// We keep, per vehicle, the freshest body0 pose + velocity (across ALL peers), and per (recipient,
// vehicle) the last tick that recipient got a fresh update. When a distant recipient B is "due" for
// a vehicle V that some near peer is feeding densely, we append a dead-reckoned 0x81(V) to B's
// outgoing message — pose extrapolated to ETA = tick+interval, time field rewritten to that ETA.
// See 目的と設計.md §3-§4.9. All functions assume the caller holds the send lock (g_cs).
//
// Observation is passive (never changes a send). Injection is gated by g_relayInject (F6, default
// OFF) and only ever runs on a message rec::walk fully decoded, on a private COPY of the buffer.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <cmath>
#include <map>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include "rec.h"

namespace relay {

// LOD: per-(recipient,vehicle) append cadence I and lag Λ chosen from the vehicle's NATIVE cadence
// (the server's own distance-LOD decision). Relay I = native_gap / kLodDenser, clamped [Imin,Imax].
// Λ = I/2 centers the rendered path on the true curve (circle test: overshoot at small Λ vs
// undershoot at large Λ cancel near Λ≈I/2). So far vehicles get a coarse I (cheap) + large Λ (tames
// the coarse-cadence overshoot, at the cost of more lag — invisible at distance).
// Hard compile-time maxima used for buffer/array sizing. NOT tunable at runtime (they bound the
// allocations below); the runtime knobs in Cfg may never exceed these.
constexpr int    kMaxPerSend = 24;   // buffer-sizing max on records appended to one message
constexpr int    kMaxRecLen  = 2048; // per-vehicle template cap

// Runtime-tunable relay knobs (loaded from swhook.ini; see load defaults / set_cfg below). Defaults
// reproduce the previously hardcoded values, so behaviour is unchanged when no config file exists.
struct Cfg {
    int    intervalMin = 5;    // finest relay cadence (ticks)
    int    intervalMax = 60;   // coarsest relay cadence (ticks)
    int    lodDenser   = 4;    // relay this many× denser than the server's native cadence
    double lagRatio    = 0.5;  // render lag as a fraction of the cadence interval (lag = interval × lagRatio)
    int    lagMin      = 0;    // floor on lag (ticks): near vehicles hold this minimum lag even when interval×ratio is smaller
    int    relayMinGap = 10;   // only relay when native gap exceeds this (else native is fine)
    int    stale       = 30;   // don't relay if freshest source older than this (ticks)
    double gapOutlier  = 30;   // reject velocity from a sample gap larger than this (ticks)
    int    maxPerSend  = 24;   // records appended to one message (clamped to kMaxPerSend)
    int    budgetWindow    = 60;   // per-recipient budget window (ticks ≈ 1s)
    int    capKbpsPerPeer  = 2000; // per-recipient relay bandwidth cap (kbit/s) — safety valve
    // startup behaviour (read once at init by the DLL; a later config.reload does not re-apply them)
    int    relay   = 0;   // 1 = auto-enable relay injection at startup
    int    capture = 0;   // 1 = auto-start .swcap capture at startup (default off: capture is for diagnosis)
    int    ipcPort = 28215; // localhost TCP port of the control-plane (GUI/CLI/other tools)
    int    autoInject = 1; // GUI only: inject as soon as server64.exe appears (the DLL ignores this key)
    // logging (hot-reloadable)
    int    log          = 1;   // 1 = write the session .log (event lines: startup, config, relay stats, hook status)
    int    dumpWalkFail = 0;   // 1 = record every type=8 frame the walker could not fully decode (walkfail_*.swcap)
    int    dumpWalkFailMax = 200; // cap on walk-fail frames per session (disk safety)
};
inline Cfg g_cfg;

// Field table: the single list every config feature is driven from (ini load/save, IPC config.get /
// config.set, the startup log). Add a knob = add a struct member + one row here.
struct CfgField {
    const char* name;
    bool   isInt;
    int    Cfg::*ip;
    double Cfg::*dp;
    bool   startupOnly;   // applied only at DLL init (reload/set changes the value but not behaviour until restart)
    const char* help;
};
inline const CfgField kCfgFields[] = {
    {"intervalMin",     true,  &Cfg::intervalMin,     nullptr, false, "finest relay cadence (ticks)"},
    {"intervalMax",     true,  &Cfg::intervalMax,     nullptr, false, "coarsest relay cadence (ticks)"},
    {"lodDenser",       true,  &Cfg::lodDenser,       nullptr, false, "relay this many x denser than native cadence"},
    {"lagRatio",        false, nullptr, &Cfg::lagRatio,         false, "render lag as fraction of interval"},
    {"lagMin",          true,  &Cfg::lagMin,          nullptr, false, "minimum render lag (ticks)"},
    {"relayMinGap",     true,  &Cfg::relayMinGap,     nullptr, false, "relay only when native gap exceeds this (ticks)"},
    {"stale",           true,  &Cfg::stale,           nullptr, false, "skip if source older than this (ticks)"},
    {"gapOutlier",      false, nullptr, &Cfg::gapOutlier,       false, "reject velocity from gaps larger than this (ticks)"},
    {"maxPerSend",      true,  &Cfg::maxPerSend,      nullptr, false, "max records appended per message (<=24)"},
    {"budgetWindow",    true,  &Cfg::budgetWindow,    nullptr, false, "bandwidth window (ticks)"},
    {"capKbpsPerPeer",  true,  &Cfg::capKbpsPerPeer,  nullptr, false, "per-peer relay bandwidth cap (kbit/s)"},
    {"relay",           true,  &Cfg::relay,           nullptr, true,  "1 = relay ON at startup"},
    {"capture",         true,  &Cfg::capture,         nullptr, true,  "1 = start .swcap capture at startup"},
    {"ipcPort",         true,  &Cfg::ipcPort,         nullptr, true,  "control-plane TCP port (127.0.0.1)"},
    {"autoInject",      true,  &Cfg::autoInject,      nullptr, true,  "GUI: 1 = inject automatically when server64.exe starts"},
    {"log",             true,  &Cfg::log,             nullptr, false, "1 = write the session .log file"},
    {"dumpWalkFail",    true,  &Cfg::dumpWalkFail,    nullptr, false, "1 = record undecodable type=8 frames to walkfail_*.swcap"},
    {"dumpWalkFailMax", true,  &Cfg::dumpWalkFailMax, nullptr, false, "max walk-fail frames per session"},
};
constexpr int kCfgFieldCount = (int)(sizeof(kCfgFields) / sizeof(kCfgFields[0]));

inline const CfgField* find_field(const char* k) {
    for (const CfgField& f : kCfgFields) if (!strcmp(f.name, k)) return &f;
    return nullptr;
}
inline double get_cfg(const CfgField& f) { return f.isInt ? (double)(g_cfg.*f.ip) : g_cfg.*f.dp; }
inline bool   get_cfg(const char* k, double& v) { const CfgField* f = find_field(k); if (!f) return false; v = get_cfg(*f); return true; }

// Apply one "key value" pair to g_cfg. Returns true if the key was recognized. Values that size
// buffers or must be sane are clamped here (the only place clamps live).
inline bool set_cfg(const char* k, double v) {
    auto ci = [](double x){ return (int)(x + 0.5); };
    const CfgField* f = find_field(k);
    if (!f) {
        if (!strcmp(k, "hotkeys")) return true;   // removed in 0.2: accepted and ignored for old ini files
        return false;
    }
    if (f->isInt) g_cfg.*f->ip = ci(v); else g_cfg.*f->dp = v;
    // clamps
    if (g_cfg.lodDenser < 1) g_cfg.lodDenser = 1;
    if (g_cfg.lagRatio < 0) g_cfg.lagRatio = 0;
    if (g_cfg.lagMin < 0) g_cfg.lagMin = 0;
    if (g_cfg.maxPerSend > kMaxPerSend) g_cfg.maxPerSend = kMaxPerSend;
    if (g_cfg.maxPerSend < 1) g_cfg.maxPerSend = 1;
    if (g_cfg.budgetWindow < 1) g_cfg.budgetWindow = 1;
    if (g_cfg.intervalMin < 1) g_cfg.intervalMin = 1;
    if (g_cfg.intervalMax < g_cfg.intervalMin) g_cfg.intervalMax = g_cfg.intervalMin;
    if (g_cfg.relay)   g_cfg.relay = 1;
    if (g_cfg.capture) g_cfg.capture = 1;
    if (g_cfg.ipcPort < 1 || g_cfg.ipcPort > 65535) g_cfg.ipcPort = 28215;
    if (g_cfg.autoInject) g_cfg.autoInject = 1;
    if (g_cfg.log) g_cfg.log = 1;
    if (g_cfg.dumpWalkFail) g_cfg.dumpWalkFail = 1;
    if (g_cfg.dumpWalkFailMax < 0) g_cfg.dumpWalkFailMax = 0;
    return true;
}

// Load "key = value" lines from an INI-style config file (blank lines and '#'/';' comments ignored;
// '=' or whitespace separates key and value). Returns the number of keys applied, or -1 if the file
// could not be opened (caller keeps defaults). Safe to call again at runtime to hot-reload.
inline int load_config_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char line[256]; int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || *p == 0) continue;
        char key[64]; double val;
        // accept "key = value", "key=value", or "key value"
        if (sscanf(p, "%63[^=# \t] = %lf", key, &val) == 2 ||
            sscanf(p, "%63[^=# \t]=%lf",   key, &val) == 2 ||
            sscanf(p, "%63s %lf",          key, &val) == 2) {
            if (set_cfg(key, val)) applied++;
        }
    }
    fclose(f);
    return applied;
}

// Format a config value the way the ini expects it (ints plain, doubles via %g).
inline void fmt_cfg(const CfgField& f, char* out, size_t n) {
    if (f.isInt) snprintf(out, n, "%d", g_cfg.*f.ip);
    else         snprintf(out, n, "%g", g_cfg.*f.dp);
}

// Write g_cfg back to `path`, preserving the file's comments and layout: every existing "key = value"
// line whose key we know gets its value rewritten in place (trailing comment kept); keys absent from
// the file are appended at the end. Written via a temp file + rename so a crash never truncates the
// ini. Returns true on success.
inline bool save_config_file(const char* path) {
    std::vector<std::string> out;
    bool seen[kCfgFieldCount] = {false};
    FILE* f = fopen(path, "rb");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string L(line);
            while (!L.empty() && (L.back() == '\n' || L.back() == '\r')) L.pop_back();
            const char* p = line; while (*p == ' ' || *p == '\t') p++;
            char key[64] = {0};
            bool comment = (*p == '#' || *p == ';' || *p == 0 || *p == '\r' || *p == '\n');
            if (!comment && sscanf(p, "%63[^=# \t]", key) == 1) {
                const CfgField* fd = find_field(key);
                if (fd) {
                    int idx = (int)(fd - kCfgFields); seen[idx] = true;
                    // keep everything from the first '#' (trailing comment) on this line
                    size_t hash = L.find('#');
                    std::string tail = hash == std::string::npos ? "" : L.substr(hash);
                    char val[64]; fmt_cfg(*fd, val, sizeof val);
                    // pad to the original comment column when possible for a tidy file
                    std::string head = std::string(fd->name) + " = " + val;
                    if (!tail.empty()) { while (head.size() < hash) head += ' '; if (head.back() != ' ') head += ' '; }
                    L = head + tail;
                }
            }
            out.push_back(L);
        }
        fclose(f);
    }
    bool anyMissing = false;
    for (int i = 0; i < kCfgFieldCount; i++) if (!seen[i]) anyMissing = true;
    if (anyMissing) {
        if (!out.empty() && !out.back().empty()) out.push_back("");
        out.push_back("# --- added by swhook (config.save) ---");
        for (int i = 0; i < kCfgFieldCount; i++) if (!seen[i]) {
            char val[64]; fmt_cfg(kCfgFields[i], val, sizeof val);
            out.push_back(std::string(kCfgFields[i].name) + " = " + val + "   # " + kCfgFields[i].help);
        }
    }
    std::string tmp = std::string(path) + ".tmp";
    FILE* w = fopen(tmp.c_str(), "wb");
    if (!w) return false;
    for (const std::string& L : out) { fputs(L.c_str(), w); fputs("\r\n", w); }
    fclose(w);
    remove(path);
    return rename(tmp.c_str(), path) == 0;
}
// interval (I) and lag (Λ) for a vehicle whose native sync gap is `gap`.
// interval = clamp(gap/lodDenser, intervalMin, intervalMax). lag = clamp(interval×lagRatio, lagMin,
// interval): scale the render lag to the cadence via lagRatio (default 0.5 = the classic interval/2),
// but never below the lagMin floor nor above the interval (lag past one resend cadence buys nothing).
// A near vehicle (small interval) with lagMin set thus holds a steady minimum lag while distant ones
// stretch out to interval×lagRatio.
inline void lod(uint32_t gap, int& interval, int& lag) {
    long i = (long)gap / g_cfg.lodDenser;
    if (i < g_cfg.intervalMin) i = g_cfg.intervalMin; if (i > g_cfg.intervalMax) i = g_cfg.intervalMax;
    interval = (int)i;
    lag = (int)(interval * g_cfg.lagRatio);
    if (lag < g_cfg.lagMin) lag = g_cfg.lagMin;
    if (lag > interval) lag = interval;
}

struct Veh {
    bool has = false;
    uint32_t curT = 0, prevT = 0;                 // prevT==0 => no velocity yet
    double cx = 0, cy = 0, cz = 0;                // freshest body0 position
    double px = 0, py = 0, pz = 0;                // previous body0 position
    float cq[4] = {0,0,0,0};                      // freshest body0 rotation quaternion (raw order)
    float pq[4] = {0,0,0,0};                      // previous body0 rotation quaternion
    int len = 0;                                  // template record length
    uint8_t bytes[kMaxRecLen];                    // latest full 0x81 record (template)
};

// state (protected by the caller's send lock)
inline std::unordered_map<uint32_t, Veh>                 g_veh;      // vehId -> freshest sample
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_fresh;   // (peer,veh) -> last fresh tick (native OR inject)
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_natLast; // (peer,veh) -> last NATIVE receipt tick
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_natGap;  // (peer,veh) -> last native gap (distance proxy)
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_natEta;  // (peer,veh) -> last native record's ETA (server's next-send time)
inline std::map<uint64_t, uint32_t> g_peerProc;                    // per-recipient last world tick we injected into
inline std::map<uint64_t, uint32_t> g_peerWin;                     // per-recipient budget window index
inline std::map<uint64_t, long>     g_peerBytes;                   // per-recipient bytes used this window
inline uint64_t g_injRecords = 0, g_injBytes = 0, g_injSends = 0, g_rewrites = 0;

inline double D(const uint8_t* p) { double v; memcpy(&v, p, 8); return v; }
inline void   W(uint8_t* p, double v) { memcpy(p, &v, 8); }
inline float  F(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }
inline void   WF(uint8_t* p, float v) { memcpy(p, &v, 4); }

// Update caches from one recipient's fully-decoded type=8 body. Passive.
inline void observe(uint64_t peer, const uint8_t* body, int blen, uint32_t tick) {
    int count = (int)rec::U32(body, blen, 16), off = 20;
    for (int i = 0; i < count; i++) {
        if (off >= blen) break;
        uint32_t tag = rec::U32(body, blen, off);
        int L = rec::decode_len(body, blen, off);
        if (L <= 0) break;
        if (tag == 0x2C || tag == 0x38) {
            // Vehicle removed from the client (0x2C = universal remove: unload/despawn/destroy;
            // 0x38 = despawn discriminator). Stop relaying it immediately — otherwise we keep
            // dead-reckoning and injecting phantom 0x81 for a vehicle that no longer exists on the
            // client until the `stale` timeout. Dropping the source pose makes managed() return false
            // at once. On reload (unload case, same id) a fresh 0x81 repopulates g_veh, so this is safe.
            g_veh.erase(rec::U32(body, blen, off + 4));
        }
        if (tag == 0x81) {
            uint32_t V = rec::U32(body, blen, off + 4);
            auto key = std::make_pair(peer, V);
            auto nit = g_natLast.find(key);                                      // native cadence (distance proxy)
            if (nit != g_natLast.end() && tick > nit->second) g_natGap[key] = tick - nit->second;
            g_natLast[key] = tick;
            g_natEta[key] = rec::U32(body, blen, off + 8);                       // server's next-send time (ETA)
            auto it = g_fresh.find(key);
            if (it == g_fresh.end() || tick > it->second) g_fresh[key] = tick;   // B got V natively now
            if (off + 14 < blen && body[off + 14] == 1 && L <= kMaxRecLen && off + 31 + 24 <= blen) {
                Veh& v = g_veh[V];
                if (!v.has || tick > v.curT) {
                    if (v.has && tick > v.curT) {
                        v.prevT = v.curT; v.px = v.cx; v.py = v.cy; v.pz = v.cz;
                        memcpy(v.pq, v.cq, sizeof(v.cq));
                    }
                    v.curT = tick;
                    v.cx = D(body + off + 31); v.cy = D(body + off + 39); v.cz = D(body + off + 47);
                    for (int q = 0; q < 4; q++) v.cq[q] = F(body + off + 15 + q * 4);   // rotation @rec+15
                    v.len = L; memcpy(v.bytes, body + off, L); v.has = true;
                }
            }
        }
        off += L;
    }
}

inline void predict(const Veh& v, uint32_t eta, double& ex, double& ey, double& ez) {
    double dt = (double)v.curT - v.prevT;
    if (v.prevT != 0 && dt > 0 && dt <= g_cfg.gapOutlier) {
        double ahead = (double)eta - v.curT;
        ex = v.cx + (v.cx - v.px) / dt * ahead;
        ey = v.cy + (v.cy - v.py) / dt * ahead;
        ez = v.cz + (v.cz - v.pz) / dt * ahead;
    } else { ex = v.cx; ey = v.cy; ez = v.cz; }   // no confident velocity -> hold latest pose
}

// Predict body0 rotation at `eta`. Basis-agnostic (works for any float[4] component order): align the
// two samples to the same hemisphere (shortest arc via the 4D dot sign), linearly extrapolate the 4
// components at constant angular rate, then renormalize — the extrapolation analog of nlerp. Same
// confidence cap as position; holds the latest quaternion when there is no confident angular velocity.
inline void predict_quat(const Veh& v, uint32_t eta, float out[4]) {
    double dt = (double)v.curT - v.prevT;
    if (v.prevT == 0 || dt <= 0 || dt > g_cfg.gapOutlier) { memcpy(out, v.cq, sizeof(float) * 4); return; }
    double dot = 0; for (int i = 0; i < 4; i++) dot += (double)v.cq[i] * v.pq[i];
    double s = dot < 0 ? -1.0 : 1.0;                        // flip prev into cur's hemisphere
    double ahead = (double)eta - v.curT, f = ahead / dt;
    double e[4], n = 0;
    for (int i = 0; i < 4; i++) { e[i] = v.cq[i] + (v.cq[i] - s * v.pq[i]) * f; n += e[i] * e[i]; }
    if (n <= 1e-12) { memcpy(out, v.cq, sizeof(float) * 4); return; }
    double inv = 1.0 / std::sqrt(n);
    for (int i = 0; i < 4; i++) out[i] = (float)(e[i] * inv);
}

// A vehicle is "managed" for recipient B when we should take over its sync to B: we have a fresh
// source pose AND B is DISTANT from V (its native cadence is slower than our interval). Close
// vehicles (native gap <= interval) are never touched — the server's dense truth is already better.
inline bool managed(uint64_t peer, uint32_t V, uint32_t tick) {
    auto vit = g_veh.find(V);
    if (vit == g_veh.end() || !vit->second.has) return false;
    if (tick - vit->second.curT > (uint32_t)g_cfg.stale) return false;   // source dried up
    auto git = g_natGap.find(std::make_pair(peer, V));
    if (git == g_natGap.end() || git->second <= (uint32_t)g_cfg.relayMinGap) return false;  // unknown or close
    return true;
}

// Fill one 0x81 record's body0 pose (at `r`, `avail` bytes) with the pose PREDICTED at `targetTick`.
// Does NOT touch the time/ETA field — the ETA stays the server's next-send time, so the client's
// promise never expires early (no freeze), while the endpoint being a future-predicted pose makes it
// glide at the true velocity. targetTick = ETA - Λ gives a steady render lag of Λ ticks.
inline void fill_pose(uint8_t* r, int avail, const Veh& v, uint32_t targetTick) {
    if (14 < avail && r[14] == 1 && 31 + 24 <= avail) {
        float q[4]; predict_quat(v, targetTick, q);                     // rotation @r+15 (float[4])
        for (int i = 0; i < 4; i++) WF(r + 15 + i * 4, q[i]);
        double ex, ey, ez; predict(v, targetTick, ex, ey, ez);         // position @r+31 (double[3])
        W(r + 31, ex); W(r + 39, ey); W(r + 47, ez);
    }
}

// targetTick = ETA - Λ, clamped to not precede the freshest sample (no negative horizon).
inline uint32_t target_of(uint32_t eta, int lag, uint32_t curT) {
    long tl = (long)eta - lag;
    return (tl > (long)curT) ? (uint32_t)tl : curT;
}

// Per-recipient bandwidth budget (safety valve). Returns true and charges `addLen` if within cap.
inline bool budget_ok(uint64_t peer, uint32_t tick, int addLen) {
    uint32_t win = tick / (uint32_t)g_cfg.budgetWindow;
    if (g_peerWin[peer] != win) { g_peerWin[peer] = win; g_peerBytes[peer] = 0; }
    long cap = (long)g_cfg.capKbpsPerPeer * 1000 / 8 * g_cfg.budgetWindow / 60;   // bytes per window
    if (g_peerBytes[peer] + addLen > cap) return false;
    g_peerBytes[peer] += addLen; return true;
}

// Build the relay message for recipient `peer` into `out` (>= outcap bytes). Two passes on a COPY of
// the original: (1) for any native 0x81 of a managed (distant) vehicle, KEEP its ETA and replace the
// pose with predict(ETA-Λ) — the server's long glide becomes a correct-velocity glide, no freeze;
// (2) APPEND, at the re-aim cadence, a predicted 0x81 for managed vehicles not present this message,
// using ETA = the server's next-send time (E_nat) so those promises are also freeze-safe. Patches
// total (+4) and recordCount (body+16). Returns the new cub if ANYTHING changed, else 0.
inline int build_inject(uint64_t peer, const uint8_t* orig, int cub, uint8_t* out, int outcap, uint32_t tick) {
    // Rewound-world-tick guard: never modify a message whose world tick is not newer than the last
    // one we injected into for this recipient. A server rewind / resend of an older tick must pass
    // through untouched so we don't stamp future-predicted poses onto a replayed past frame (which
    // could trigger a client resend request). Capture scan (swcap resend) shows this never fires on
    // the battle logs — envelope ticks are strictly forward — but the guard is cheap insurance.
    auto pit = g_peerProc.find(peer);
    if (pit != g_peerProc.end() && tick <= pit->second) return 0;   // 0 = leave original send as-is
    g_peerProc[peer] = tick;

    memcpy(out, orig, cub);
    uint8_t* body = out + 8; int blen = cub - 8;
    std::unordered_set<uint32_t> handled;

    // pass 1: rewrite the pose of native records already in the message (keep their ETA)
    int rewrote = 0, count = (int)rec::U32(body, blen, 16), off = 20;
    for (int i = 0; i < count; i++) {
        if (off >= blen) break;
        uint32_t tag = rec::U32(body, blen, off);
        int L = rec::decode_len(body, blen, off);
        if (L <= 0) break;
        if (tag == 0x81) {
            uint32_t V = rec::U32(body, blen, off + 4);
            if (managed(peer, V, tick)) {
                int interval, lag; lod(g_natGap[std::make_pair(peer, V)], interval, lag);
                // Deadline = OUR next relay send (tick + interval), NOT the server's far ETA. The
                // server's ETA for a distant vehicle is up to ~720t (12s) ahead; using it as the
                // client's glide deadline forces a correct-velocity endpoint ~12s extrapolated out — the
                // vehicle then drifts toward a far predicted point and never tracks reality (the observed
                // freeze). Because we re-send at our cadence (dense LOD), a short tick+interval deadline
                // is always refreshed before it expires, so no freeze; extrapolation is bounded to lag.
                uint32_t D = tick + (uint32_t)interval;
                memcpy(body + off + 8, &D, 4);                           // ETA = our next-send tick
                fill_pose(body + off, blen - off, g_veh[V], target_of(D, lag, g_veh[V].curT));
                handled.insert(V); rewrote++;
                g_fresh[std::make_pair(peer, V)] = tick;
            }
        }
        off += L;
    }

    // pass 2: append due managed vehicles not present. Collect candidates, serve NEAREST first
    // (smallest native gap = highest relevance) so the per-recipient budget favours close vehicles.
    std::vector<std::pair<uint32_t, uint32_t>> cand;                     // (gap, veh)
    for (auto& kv : g_veh) {
        uint32_t V = kv.first;
        if (handled.count(V) || !kv.second.has || kv.second.len <= 0) continue;
        if (!managed(peer, V, tick)) continue;
        auto key = std::make_pair(peer, V);
        uint32_t gap = g_natGap[key];
        int interval, lag; lod(gap, interval, lag);
        auto it = g_fresh.find(key);
        if (it != g_fresh.end() && tick - it->second < (uint32_t)interval) continue;  // re-aimed within its cadence
        cand.push_back(std::make_pair(gap, V));
    }
    std::sort(cand.begin(), cand.end());

    int wpos = cub, added = 0, addedBytes = 0;
    for (auto& c : cand) {
        if (added >= g_cfg.maxPerSend) break;
        uint32_t V = c.second; Veh& v = g_veh[V];
        if (wpos + v.len > outcap) continue;
        if (!budget_ok(peer, tick, v.len)) continue;                    // per-recipient cap: skip (coarsen)
        auto key = std::make_pair(peer, V);
        int interval, lag; lod(c.first, interval, lag);
        // Deadline = our own next relay send (tick + interval), so the client glides to a pose only
        // `lag` ahead and we refresh it every `interval` ticks. (Using the server's far ETA here made
        // the client aim ~12s out and freeze — see pass 1.)
        uint32_t Eapp = tick + (uint32_t)interval;
        memcpy(out + wpos, v.bytes, v.len);
        memcpy(out + wpos + 8, &Eapp, 4);                                // ETA = our next-send tick
        fill_pose(out + wpos, v.len, v, target_of(Eapp, lag, v.curT));
        wpos += v.len; addedBytes += v.len; added++;
        g_fresh[key] = tick;
    }

    if (rewrote == 0 && added == 0) return 0;
    if (added > 0) {
        uint32_t total; memcpy(&total, out + 4, 4); total += addedBytes; memcpy(out + 4, &total, 4);
        uint32_t rc;    memcpy(&rc, out + 24, 4);   rc += added;          memcpy(out + 24, &rc, 4);
    }
    g_injRecords += added; g_injBytes += addedBytes; g_rewrites += rewrote; g_injSends++;
    return cub + addedBytes;
}

} // namespace relay
