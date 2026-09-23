// relay.h — stage C: cross-peer 0x81 relay (vehicle position-sync improvement).
//
// The server calls SendMessageToUser once per recipient, so this process sees every peer's stream.
// We keep, per vehicle, the freshest body0 pose + velocity (across ALL peers) and the cadence at
// which fresh samples arrive (I_src), and per (recipient, vehicle) the promise we last made (the
// source sample it was built from + its ETA). When a distant recipient B is fed a vehicle V far
// more coarsely than some near peer, every record B gets for V (rewritten native or appended) is
// pose = predict(ETA − Λ) with ETA = tick + I_src: the client glides at the true velocity and sits
// a steady Λ ticks behind reality. A new record goes out whenever a fresh sample exists or the last
// promise is about to expire (dead-reckoning bridges a stalled source until `stale`).
// See 目的と設計.md §3-§4. All functions assume the caller holds the send lock (g_cs).
//
// Observation is passive (never changes a send). Injection is gated by g_relayInject (IPC `relay.set`,
// or `relay=1` in swhook.ini at startup) and only ever runs on a message rec::walk fully decoded, on a private COPY of the buffer.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <cmath>
#include <map>
#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include "rec.h"
#include "../common/u8path.h"

namespace relay {

// Client model (verified, 目的と設計.md §2): a 0x81 carries a pose and an ETA tick; the client
// glides from where it is to that pose, arriving at the ETA, and STOPS there if nothing newer came.
// So with a record every I ticks whose pose is the true pose, the vehicle renders I ticks behind.
// Sending pose = P(ETA − Λ) instead (extrapolated along the velocity) makes it arrive at the true
// position Λ ticks after the sample and keep gliding along the predicted path: steady lag Λ at
// the true speed. The price is the prediction horizon h = ETA − Λ − sampleTick, which is where
// turning overshoot comes from. A promise's horizon is bounded by `stale` alone (see promise_of).
// Hard compile-time maxima used for buffer/array sizing. NOT tunable at runtime (they bound the
// allocations below); the runtime knobs in Cfg may never exceed these.
constexpr int    kMaxPerSend = 24;   // buffer-sizing max on records appended to one message
constexpr int    kMaxRecLen  = 2048; // per-vehicle template cap
// Bound on the prediction horizon of an UNMANAGED native rewrite (pass 1): the server's ETA is kept
// (up to ~720 ticks out), so without it a coarse-only vehicle would be dead-reckoned ~11 s ahead from
// a chord velocity (star paths, 目的と設計.md §4-7). No freeze risk: the server re-sends at its ETA.
constexpr long   kNativeHorizon = 30;

// Runtime-tunable relay knobs (loaded from swhook.ini; see load defaults / set_cfg below). Defaults
// reproduce the previously hardcoded values, so behaviour is unchanged when no config file exists.
struct Cfg {
    int    intervalMin = 5;    // finest relay cadence (ticks)
    int    intervalMax = 60;   // coarsest relay cadence (ticks)
    int    lodDenser   = 8;    // cost cap: relay a vehicle no denser than native_gap / lodDenser (0 = always I_src)
    int    distLod     = 1;    // 1 = floor the measured native gap by the server's distance rule (recipient↔vehicle), so an approaching vehicle densifies before native catches up
    int    lag         = 5;    // target render lag Λ (ticks): the relayed vehicle sits this far behind reality
    int    relayMinGap = 10;   // only relay when native gap exceeds this (else native is fine)
    double srcRatio    = 0.5;  // relay only when the source samples at most this fraction of the recipient's native gap
    int    stale       = 300;   // don't relay if freshest source older than this (ticks)
    double gapOutlier  = 0;    // reject velocity from a sample gap larger than this (ticks); <=0 = off
    int    maxPerSend  = 24;   // records appended to one message (clamped to kMaxPerSend)
    int    budgetWindow    = 60;   // per-recipient budget window (ticks ≈ 1s)
    int    capKbpsPerPeer  = 2000; // per-recipient relay bandwidth cap (kbit/s) — safety valve
    // startup behaviour (read once at init by the DLL; a later config.reload does not re-apply them)
    int    relay   = 0;   // 1 = auto-enable relay injection at startup
    int    capture = 0;   // 1 = auto-start .swcap capture at startup (default off: capture is for diagnosis)
    int    ipcPort = 28215; // localhost TCP port of the control-plane (GUI/CLI/other tools)
    int    autoInject = 1; // GUI only: inject as soon as server64.exe appears (the DLL ignores this key)
    int    debugUi    = 0; // GUI only: show the developer debug tab (hold / nudge experiments, freeze details)
    // logging (hot-reloadable)
    int    log          = 1;   // 1 = write the session .log (event lines: startup, config, relay stats, hook status)
    int    dumpWalkFail = 0;   // 1 = record every type=8 frame the walker could not fully decode (walkfail_*.swcap)
    int    dumpWalkFailMax = 200; // cap on walk-fail frames per session (disk safety)
    // client-freeze detector (freeze.h): thresholds in ms, 0 disables that signal
    int    freezePoseMs = 2000;   // pose static this long while the seated vehicle moves on the server
    int    freezeDefMs  = 3000;   // a pushed vehicle definition unacknowledged (no 0x29) this long
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
    {"intervalMin",     true,  &Cfg::intervalMin,     nullptr, false, "floor on I_src, the relay cadence (ticks)"},
    {"intervalMax",     true,  &Cfg::intervalMax,     nullptr, false, "ceiling on I_src (ticks)"},
    {"lodDenser",       true,  &Cfg::lodDenser,       nullptr, false, "cadence cost cap: no denser than native gap / this (0 = off)"},
    {"distLod",         true,  &Cfg::distLod,         nullptr, false, "1 = native gap = min(measured, distance rule 0.1*d-6) so approaching vehicles densify early"},
    {"lag",             true,  &Cfg::lag,             nullptr, false, "target render lag (ticks)"},
    {"relayMinGap",     true,  &Cfg::relayMinGap,     nullptr, false, "append only when native gap exceeds this (ticks); below it native records are still rewritten"},
    {"srcRatio",        false, nullptr, &Cfg::srcRatio,         false, "relay only when source gap <= native gap x this"},
    {"stale",           true,  &Cfg::stale,           nullptr, false, "skip if source older than this (ticks)"},
    {"gapOutlier",      false, nullptr, &Cfg::gapOutlier,       false, "reject velocity from gaps larger than this (ticks); 0 = off"},
    {"maxPerSend",      true,  &Cfg::maxPerSend,      nullptr, false, "max records appended per message (<=24)"},
    {"budgetWindow",    true,  &Cfg::budgetWindow,    nullptr, false, "bandwidth window (ticks)"},
    {"capKbpsPerPeer",  true,  &Cfg::capKbpsPerPeer,  nullptr, false, "per-peer relay bandwidth cap (kbit/s)"},
    {"relay",           true,  &Cfg::relay,           nullptr, true,  "1 = relay ON at startup"},
    {"capture",         true,  &Cfg::capture,         nullptr, true,  "1 = start .swcap capture at startup"},
    {"ipcPort",         true,  &Cfg::ipcPort,         nullptr, true,  "control-plane TCP port (127.0.0.1)"},
    {"autoInject",      true,  &Cfg::autoInject,      nullptr, true,  "GUI: 1 = inject automatically when server64.exe starts"},
    {"debugUi",         true,  &Cfg::debugUi,         nullptr, true,  "GUI: 1 = show the developer debug tab (experiments; not for normal use)"},
    {"log",             true,  &Cfg::log,             nullptr, false, "1 = write the session .log file"},
    {"dumpWalkFail",    true,  &Cfg::dumpWalkFail,    nullptr, false, "1 = record undecodable type=8 frames to walkfail_*.swcap"},
    {"dumpWalkFailMax", true,  &Cfg::dumpWalkFailMax, nullptr, false, "max walk-fail frames per session"},
    {"freezePoseMs",    true,  &Cfg::freezePoseMs,    nullptr, false, "freeze detector: pose static this long while its vehicle moves (ms, 0 = off)"},
    {"freezeDefMs",     true,  &Cfg::freezeDefMs,     nullptr, false, "freeze detector: pushed vehicle definition unacked this long (ms, 0 = off)"},
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
        if (!strcmp(k, "lagRatio") || !strcmp(k, "lagMin")) return true;   // pre-I_src model: accepted and ignored
        if (!strcmp(k, "horizonMax")) return true;   // removed: capping a promise pins it and freezes the vehicle
        return false;
    }
    if (f->isInt) g_cfg.*f->ip = ci(v); else g_cfg.*f->dp = v;
    // clamps
    if (g_cfg.lag < 0) g_cfg.lag = 0;
    if (g_cfg.lodDenser < 0) g_cfg.lodDenser = 0;
    if (g_cfg.srcRatio < 0) g_cfg.srcRatio = 0; if (g_cfg.srcRatio > 1) g_cfg.srcRatio = 1;
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
inline int load_config_file(const std::string& path) {   // UTF-8 path
    FILE* f = u8path::fopen(path, "rb");
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
inline bool save_config_file(const std::string& path) {  // UTF-8 path
    std::vector<std::string> out;
    bool seen[kCfgFieldCount] = {false};
    FILE* f = u8path::fopen(path, "rb");
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
    std::string tmp = path + ".tmp";
    FILE* w = u8path::fopen(tmp, "wb");
    if (!w) return false;
    for (const std::string& L : out) { fputs(L.c_str(), w); fputs("\r\n", w); }
    fclose(w);
    return u8path::replace(tmp, path);
}
struct Veh {
    bool has = false;
    uint32_t curT = 0, prevT = 0;                 // prevT==0 => no velocity yet
    double cx = 0, cy = 0, cz = 0;                // freshest body0 position
    double px = 0, py = 0, pz = 0;                // previous body0 position
    float cq[4] = {0,0,0,0};                      // freshest body0 rotation quaternion (raw order)
    float pq[4] = {0,0,0,0};                      // previous body0 rotation quaternion
    double srcGap = 0;                            // EMA of ticks between fresh samples (I_src); 0 = unknown
    int len = 0;                                  // template record length
    uint8_t bytes[kMaxRecLen];                    // latest full 0x81 record (template)
};
// What recipient B was last promised for vehicle V: the source sample the pose came from and the ETA.
// A native record that passes untouched is a promise too (its own ETA), so pass-through is recorded.
struct Sent { uint32_t srcT = 0, eta = 0, at = 0; };   // sample tick, promised ETA, send tick
constexpr double   kSrcEmaAlpha  = 0.2;   // smoothing of I_src (≈ last 5 samples)
constexpr uint32_t kRefreshMargin = 3;    // promises last I + this, re-sent every I: jitter never expires one

// state (protected by the caller's send lock)
inline std::unordered_map<uint32_t, Veh>                 g_veh;      // vehId -> freshest sample
inline std::unordered_map<uint32_t, uint32_t>            g_group;    // vehId -> spawn group id (from 0x2B; only vehicles spawned while hooked)
inline std::map<std::pair<uint64_t, uint32_t>, Sent>     g_sent;     // (peer,veh) -> last promise
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_natLast; // (peer,veh) -> last NATIVE receipt tick
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_natGap;  // (peer,veh) -> last native gap (distance proxy)
inline std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_natEta;  // (peer,veh) -> last native record's ETA (server's next-send time)
inline std::map<uint64_t, std::array<double, 3>> g_peerPos;        // recipient's last client 0x2F world position (distance LOD)
inline std::map<uint64_t, uint32_t> g_peerProc;                    // per-recipient last world tick we injected into
inline std::map<uint64_t, uint32_t> g_peerWin;                     // per-recipient budget window index
inline std::map<uint64_t, long>     g_peerBytes;                   // per-recipient bytes used this window
inline uint64_t g_injRecords = 0, g_injBytes = 0, g_injSends = 0, g_rewrites = 0;

inline double D(const uint8_t* p) { double v; memcpy(&v, p, 8); return v; }
inline void   W(uint8_t* p, double v) { memcpy(p, &v, 8); }
inline float  F(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }
inline void   WF(uint8_t* p, float v) { memcpy(p, &v, 4); }

// Relay cadence for (recipient, vehicle) = the measured source cadence, coarsened for far recipients
// by the cost cap (no denser than natGap / lodDenser: a 10 km vehicle need not get 12 updates/s),
// clamped.
inline int interval_of(const Veh& v, uint32_t natGap) {
    long i = v.srcGap > 0 ? (long)(v.srcGap + 0.5) : g_cfg.intervalMax;
    if (g_cfg.lodDenser > 0 && (long)natGap / g_cfg.lodDenser > i) i = (long)natGap / g_cfg.lodDenser;
    if (i < g_cfg.intervalMin) i = g_cfg.intervalMin; if (i > g_cfg.intervalMax) i = g_cfg.intervalMax;
    return (int)i;
}

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
            // This is also the teleport/respawn guard: no velocity ever spans the jump.
            uint32_t V = rec::U32(body, blen, off + 4);
            g_veh.erase(V);
            if (tag == 0x38) g_group.erase(V);                                   // despawned for good
            // This recipient no longer has V: drop its per-(peer,veh) feed state so the peer's vehicle
            // count reflects what it currently receives (a reload repopulates within two 0x81).
            auto key = std::make_pair(peer, V);
            g_natLast.erase(key); g_natGap.erase(key); g_natEta.erase(key); g_sent.erase(key);
        }
        if (tag == 0x2B) {
            // Spawn placement: ... + double[3] + u32 groupId + u32 vehId + ... (lifecycle.md). A multi-body
            // spawn emits one 0x2B per vehicle, all carrying the group's id (= the first vehicle's id).
            int nA = (int)rec::U16(body, blen, off + 44), nB = (int)rec::U16(body, blen, off + 46 + nA);
            int tl = off + 48 + nA + nB + 24;
            g_group[rec::U32(body, blen, tl + 4)] = rec::U32(body, blen, tl);
        }
        if (tag == 0x81) {
            uint32_t V = rec::U32(body, blen, off + 4);
            auto key = std::make_pair(peer, V);
            auto nit = g_natLast.find(key);                                      // native cadence (distance proxy)
            if (nit != g_natLast.end() && tick > nit->second) g_natGap[key] = tick - nit->second;
            g_natLast[key] = tick;
            uint32_t eta = rec::U32(body, blen, off + 8);
            g_natEta[key] = eta;                                                 // server's next-send time (ETA)
            Sent& sn = g_sent[key];                                              // pass-through promise (pass 1 may override)
            if (tick >= sn.srcT) { sn.srcT = tick; sn.eta = eta; sn.at = tick; }
            if (off + 14 < blen && body[off + 14] == 1 && L <= kMaxRecLen && off + 31 + 24 <= blen) {
                Veh& v = g_veh[V];
                if (!v.has || tick > v.curT) {
                    if (v.has && tick > v.curT) {
                        double dt = (double)(tick - v.curT);
                        v.srcGap = v.srcGap > 0 ? v.srcGap + (dt - v.srcGap) * kSrcEmaAlpha : dt;
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

// Velocity confidence. Discontinuities (teleport / respawn) normally arrive as unload+reload, which
// observe() handles by erasing the Veh (prevT resets, so no velocity spans the jump). gapOutlier is a
// leftover absolute guard for a jump WITHOUT an unload; off by default.
inline bool has_velocity(const Veh& v) {
    double dt = (double)v.curT - v.prevT;
    return v.prevT != 0 && dt > 0 && (g_cfg.gapOutlier <= 0 || dt <= g_cfg.gapOutlier);
}

inline void predict(const Veh& v, uint32_t eta, double& ex, double& ey, double& ez) {
    double dt = (double)v.curT - v.prevT;
    if (has_velocity(v)) {
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
    if (!has_velocity(v)) { memcpy(out, v.cq, sizeof(float) * 4); return; }
    double dot = 0; for (int i = 0; i < 4; i++) dot += (double)v.cq[i] * v.pq[i];
    double s = dot < 0 ? -1.0 : 1.0;                        // flip prev into cur's hemisphere
    double ahead = (double)eta - v.curT, f = ahead / dt;
    double e[4], n = 0;
    for (int i = 0; i < 4; i++) { e[i] = v.cq[i] + (v.cq[i] - s * v.pq[i]) * f; n += e[i] * e[i]; }
    if (n <= 1e-12) { memcpy(out, v.cq, sizeof(float) * 4); return; }
    double inv = 1.0 / std::sqrt(n);
    for (int i = 0; i < 4; i++) out[i] = (float)(e[i] * inv);
}

// Recipient position from the client 0x2F record (passive, recv path).
inline void observe_pose(uint64_t peer, double x, double y, double z) { g_peerPos[peer] = { x, y, z }; }

// The server's own distance LOD, measured on session_20260919_143912_627 (protocol/sync.md): the
// native 0x81 spacing to a recipient d metres from body0 is ≈ 0.1·d − 6 ticks, floor 5, cap 720,
// independent of speed. Decided per send, so the server keeps a long promise while a vehicle closes in.
inline uint32_t native_gap_at(double d) {
    double g = 0.1 * d - 6.0;
    if (g < 5) g = 5; if (g > 720) g = 720;
    return (uint32_t)(g + 0.5);
}

// Effective native gap for (recipient, V): the measured gap (what the server last did) floored by the
// distance rule evaluated at the predicted vehicle position now and at the next promise — the smaller
// distance wins. min() can only densify: an approaching vehicle gets the cadence the server WILL use
// once it re-decides, instead of the stale long gap; a receding one keeps the measured gap until the
// next native send. Unknown measured gap (never received natively) is returned as is.
inline uint32_t nat_gap_of(const Veh& v, uint64_t peer, uint32_t V, uint32_t tick) {
    auto git = g_natGap.find(std::make_pair(peer, V));
    uint32_t g = git == g_natGap.end() ? 0 : git->second;
    if (!g_cfg.distLod || g == 0 || !v.has) return g;
    auto pit = g_peerPos.find(peer);
    if (pit == g_peerPos.end()) return g;
    const std::array<double, 3>& p = pit->second;
    auto dist_at = [&](uint32_t t) {
        double ex, ey, ez; predict(v, t, ex, ey, ez);
        double dx = ex - p[0], dy = ey - p[1], dz = ez - p[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    double d = dist_at(tick);
    double d1 = dist_at(tick + (uint32_t)interval_of(v, g) + kRefreshMargin);
    if (d1 < d) d = d1;
    uint32_t gd = native_gap_at(d);
    return gd < g ? gd : g;
}

// A vehicle is "managed" for recipient B when we should take over its sync to B: we have a fresh
// source pose with a velocity, B is DISTANT from V (native gap above relayMinGap), AND the source is
// denser than B's own feed. The last gate is what makes the relay add information: the server's
// native record already promises "P(T) by ETA" and the client glides there smoothly; if the only
// samples we hold ARE that same coarse stream (solo player far from V), rewriting it just replaces
// the server's promise with a long extrapolation from a chord (tangent spikes on turns).
inline bool managed(uint64_t peer, uint32_t V, uint32_t tick) {
    auto vit = g_veh.find(V);
    if (vit == g_veh.end() || !vit->second.has) return false;
    const Veh& v = vit->second;
    if (!has_velocity(v)) return false;   // a held pose re-sent is worse than native (stop-and-go)
    if (tick - v.curT > (uint32_t)g_cfg.stale) return false;   // source dried up: back to native
    auto git = g_natGap.find(std::make_pair(peer, V));
    if (git == g_natGap.end() || git->second <= (uint32_t)g_cfg.relayMinGap) return false;  // unknown or close
    if (v.srcGap <= 0 || v.srcGap > git->second * g_cfg.srcRatio) return false;            // source not denser than B's feed
    return true;
}

// Fill one 0x81 record's body0 pose (at `r`, `avail` bytes) with the pose PREDICTED at `targetTick`.
inline void fill_pose(uint8_t* r, int avail, const Veh& v, uint32_t targetTick) {
    if (14 < avail && r[14] == 1 && 31 + 24 <= avail) {
        float q[4]; predict_quat(v, targetTick, q);                     // rotation @r+15 (float[4])
        for (int i = 0; i < 4; i++) WF(r + 15 + i * 4, q[i]);
        double ex, ey, ez; predict(v, targetTick, ex, ey, ez);         // position @r+31 (double[3])
        W(r + 31, ex); W(r + 39, ey); W(r + 47, ez);
    }
}

// The promise we make recipient B for V at send tick `tick`: ETA and the pose's target tick.
//   ETA    = tick + I + margin   (we re-send every I ticks, fresh sample or dead-reckoned)
//   target = ETA − Λ        so the client renders Λ behind reality at the true speed
// The horizon target − sampleTick is NOT capped: any cap pins successive targets built from the same
// sample to one point, so the client glides there once and sits still until the next sample (far
// missiles stop-and-go, 2026-09-19; a receding missile froze at the target's closest approach when
// its source gap 356 exceeded the cap, 2026-09-22). Dead reckoning is bounded by `stale` instead:
// managed() drops the vehicle once the sample is older than that.
// When this is the last record we can send before `stale` cuts the source off, ETA is stretched to
// the server's own next-send time instead, so the client keeps gliding until native takes over.
inline void promise_of(const Veh& v, uint64_t peer, uint32_t V, uint32_t tick, uint32_t& eta, uint32_t& target) {
    auto key = std::make_pair(peer, V);
    int I = interval_of(v, nat_gap_of(v, peer, V, tick));
    eta = tick + (uint32_t)I + kRefreshMargin;
    uint32_t age = tick - v.curT;
    if (age + (uint32_t)I > (uint32_t)g_cfg.stale) {                            // final bridge to native
        auto eit = g_natEta.find(key);
        if (eit != g_natEta.end() && eit->second > eta) eta = eit->second;
    }
    long tl = (long)eta - g_cfg.lag;
    target = tl > (long)v.curT ? (uint32_t)tl : v.curT;
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
// the original: (1) every native 0x81 with a known velocity gets a predicted pose (managed vehicles
// also get ETA = tick + I — leaving it alone would let the server's long-ETA glide override ours);
// (2) APPEND a promised 0x81 for each managed vehicle not present that is due: a fresher source
// sample exists than the one B's last promise was built from, or that promise is about to expire
// (dead-reckon on the old sample). Patches total (+4) and recordCount (body+16). Returns the new
// cub if ANYTHING changed, else 0.
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

    // pass 1: rewrite native records into our promise. Managed vehicles get ETA = tick + I as well;
    // every other vehicle with a velocity and a native gap above `lag` keeps the server's ETA and only
    // has its pose moved to predict(ETA − Λ): the horizon is natGap − Λ (tiny for near vehicles, capped
    // by kNativeHorizon for a coarse-only source), so even vehicles below relayMinGap — or a solo player's
    // own far vehicle — render closer to Λ behind instead of a full native interval behind.
    int rewrote = 0, count = (int)rec::U32(body, blen, 16), off = 20;
    for (int i = 0; i < count; i++) {
        if (off >= blen) break;
        uint32_t tag = rec::U32(body, blen, off);
        int L = rec::decode_len(body, blen, off);
        if (L <= 0) break;
        if (tag == 0x81) {
            uint32_t V = rec::U32(body, blen, off + 4);
            auto key = std::make_pair(peer, V);
            auto vit = g_veh.find(V);
            if (vit != g_veh.end() && vit->second.has && has_velocity(vit->second)) {
                const Veh& v = vit->second;
                bool mg = managed(peer, V, tick);
                uint32_t eta = 0, target = 0;
                if (mg) {
                    promise_of(v, peer, V, tick, eta, target);
                    memcpy(body + off + 8, &eta, 4);
                } else {
                    auto git = g_natGap.find(key);
                    if (git == g_natGap.end() || git->second <= (uint32_t)g_cfg.lag) { off += L; continue; }
                    eta = rec::U32(body, blen, off + 8);                             // server's ETA, kept
                    long tl = (long)eta - g_cfg.lag;
                    if (tl - (long)v.curT > kNativeHorizon) tl = (long)v.curT + kNativeHorizon;
                    target = tl > (long)v.curT ? (uint32_t)tl : v.curT;
                }
                fill_pose(body + off, blen - off, v, target);
                handled.insert(V); rewrote++;
                g_sent[key] = Sent{ v.curT, eta, tick };
            }
        }
        off += L;
    }

    // pass 2: append due managed vehicles not present. Collect candidates, serve NEAREST first
    // (smallest native gap = highest relevance) so the per-recipient budget favours close vehicles.
    std::vector<std::pair<uint32_t, uint32_t>> cand;                     // (gap, veh)
    for (auto& kv : g_veh) {
        uint32_t V = kv.first; const Veh& v = kv.second;
        if (handled.count(V) || !v.has || v.len <= 0) continue;
        if (!managed(peer, V, tick)) continue;
        auto key = std::make_pair(peer, V);
        auto it = g_sent.find(key);
        bool due = it == g_sent.end();
        if (!due) {
            const Sent& sn = it->second;
            int I = interval_of(v, nat_gap_of(v, peer, V, tick));
            due = tick - sn.at >= (uint32_t)I                                      // cadence elapsed: fresh sample or dead-reckon
                  || (int32_t)(sn.eta - tick) <= (int32_t)kRefreshMargin;         // (pass-through native promise expiring)
        }
        if (!due) continue;
        cand.push_back(std::make_pair(nat_gap_of(v, peer, V, tick), V));
    }
    std::sort(cand.begin(), cand.end());

    int wpos = cub, added = 0, addedBytes = 0;
    for (auto& c : cand) {
        if (added >= g_cfg.maxPerSend) break;
        uint32_t V = c.second; const Veh& v = g_veh[V];
        if (wpos + v.len > outcap) continue;
        if (!budget_ok(peer, tick, v.len)) continue;                    // per-recipient cap: skip (coarsen)
        uint32_t eta, target; promise_of(v, peer, V, tick, eta, target);
        memcpy(out + wpos, v.bytes, v.len);
        memcpy(out + wpos + 8, &eta, 4);
        fill_pose(out + wpos, v.len, v, target);
        wpos += v.len; addedBytes += v.len; added++;
        g_sent[std::make_pair(peer, V)] = Sent{ v.curT, eta, tick };
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
