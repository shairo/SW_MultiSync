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
#include <tuple>
#include <set>
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
    // addon pose source (addon.h): body0 poses of every vehicle from the Stormworks addon over IPC
    int    addonPoses   = 1;      // 1 = use addon.poses samples as a relay source (once the tick offset is known)
    // per-kind gate on the addon body0 source (Stormworks_WebMap docs/progress/sync-kind-select.md). The kind comes from
    // the MapViewer Type (IPC addon.kinds via the Bridge); until the first addon.kinds arrives every vehicle is fed as before.
    int    addonMissile = 1;      // 1 = feed Missile vehicles from the addon (single body: safe)
    int    addonShip    = 0;      // 1 = feed Ship vehicles (not verified yet)
    int    addonTank    = 0;      // 1 = feed Tank vehicles' body0 (not verified yet; turret/gun sync is bodySync)
    int    addonSub     = 0;      // 1 = feed SubMarine vehicles (not verified yet)
    int    addonAir     = 0;      // 1 = feed AirCraft vehicles (not verified yet)
    int    addonOther   = 0;      // 1 = feed vehicles of any other / no Type (Torpedo, Bullet, untyped)
    // non-root body poses (prototype, docs: Stormworks_WebMap docs/progress/turret-sync.md): addon.poses "bp"
    int    bodySync     = 1;      // 1 = rewrite type-2 bodies (turret, gun) of relayed 0x81 from the addon's body poses
                                  //     (master switch for everyone; each player still opts in with ?bs on)
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
    {"addonPoses",      true,  &Cfg::addonPoses,      nullptr, false, "1 = use vehicle poses sent by the addon (IPC addon.poses) as a relay source"},
    {"addonMissile",    true,  &Cfg::addonMissile,    nullptr, false, "1 = use the addon poses for Missile vehicles (MapViewer Type)"},
    {"addonShip",       true,  &Cfg::addonShip,       nullptr, false, "1 = use the addon poses for Ship vehicles"},
    {"addonTank",       true,  &Cfg::addonTank,       nullptr, false, "1 = use the addon poses for Tank vehicles (body0; turrets are bodySync)"},
    {"addonSub",        true,  &Cfg::addonSub,        nullptr, false, "1 = use the addon poses for SubMarine vehicles"},
    {"addonAir",        true,  &Cfg::addonAir,        nullptr, false, "1 = use the addon poses for AirCraft vehicles"},
    {"addonOther",      true,  &Cfg::addonOther,      nullptr, false, "1 = use the addon poses for vehicles of any other or no Type"},
    {"bodySync",        true,  &Cfg::bodySync,        nullptr, false, "1 = rewrite turret/gun bodies (type 2) of relayed 0x81 from the addon body poses (master switch; players opt in with ?bs on)"},
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
    if (g_cfg.addonPoses) g_cfg.addonPoses = 1;
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
    uint32_t addonT = 0;                          // world tick of the last addon sample (0 = none)
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
// While the addon feeds a vehicle (its last addon sample is at most this many ticks older than a native
// one), native 0x81 only refresh the record template: mixing two sources' samples a tick apart would
// turn their few-cm disagreement into a velocity error.
constexpr uint32_t kAddonFreshTicks = 12;
// Addon samples implying more than this speed (m/tick) are a jump (teleport, respawn without an unload
// we saw): the velocity is reset instead of spanning it.
constexpr double kAddonMaxStep = 100.0;
// Alias (fake_missile experiment): vehicle F (a static, already-burning plume) is shown to every client
// exactly where vehicle T is shown. Set by the addon (addon.poses "al" = [[F, T], ...]). In each message
// to a recipient, whenever T's 0x81 goes out (native, rewritten or appended), F's body0 pose and ETA
// are copied from it; F's own native records are overwritten with the last mirrored promise so the
// client never glides F back to its real (parked) position while the alias holds.
// Snap: the client glides a vehicle from where it is shown to each new promise, so F would visibly fly
// from its parking spot to T (and back when the alias ends). The first F record of a new alias, and F's
// first native record after the alias ends, get ETA = now so the client jumps instead of gliding.
// F is placed kAliasBack m behind T along T's direction of travel (from T's previous promised position), so
// the plume trails the missile instead of covering it.
constexpr double kAliasBack = 5.0;
struct Mirror { bool has = false, snapped = false, hasDir = false; uint32_t eta = 0, sentEta = 0; float q[4] = {0,0,0,1};
                double p[3] = {0,0,0}, dir[3] = {0,0,0}; };
inline std::vector<std::pair<uint32_t, uint32_t>> g_alias;                // (F, T)
inline std::map<std::pair<uint64_t, uint32_t>, Mirror> g_mirror;          // (peer, F) -> last mirrored promise
inline std::set<std::pair<uint64_t, uint32_t>> g_releaseSnap;             // (peer, F) whose alias ended: snap back
inline uint64_t g_aliasRecords = 0;                                       // F records written (rewritten + appended)
inline uint64_t g_aliasSnaps = 0;                                         // records given ETA = now
// Latest promise of every vehicle as last sent to each peer (kept while fakes exist). A new alias used to
// wait for T's next record in a message to that peer before F could be shown at T (2-40 ticks measured,
// longer for vehicles the server updates rarely); seeding the mirror from here jumps F in the next send.
struct LastPromise { uint32_t eta = 0; float q[4] = {0,0,0,1}; double p[3] = {0,0,0}; };
inline std::map<std::pair<uint64_t, uint32_t>, LastPromise> g_lastPromise;
constexpr size_t kLastPromiseMax = 8192;                                  // runaway guard (ids churn)
inline uint64_t g_aliasCacheStarts = 0;                                   // aliases started from g_lastPromise
// Standby (addon.poses "fk" = all fakes): a fake with no alias is shown to every client kStandbyDrop m
// below its real pose (hidden underground / underwater). The first standby record per (peer, F) — and
// again after each alias ends — gets ETA = now; when its native record is absent it is appended from the
// template. Hidden 500 m down, the jump to T on a new alias is far enough that the glide is not seen near
// the parking spot.
constexpr double kStandbyDrop = 500.0;
inline std::set<uint32_t> g_standby;                                      // all fakes
inline std::set<std::pair<uint64_t, uint32_t>> g_standbySent;             // (peer, F) already shown in standby
inline uint64_t g_standbyRecords = 0;
inline void set_standby(const std::set<uint32_t>& s) {
    for (auto it = g_standbySent.begin(); it != g_standbySent.end();)
        if (!s.count(it->second)) it = g_standbySent.erase(it); else ++it;
    if (s.empty()) g_lastPromise.clear();                                   // fakes gone: nothing to seed
    g_standby = s;
}
// Park (addon.poses "pk" = [[F, S], ...], ship standby): F is mirrored like an alias of ship S, but shown
// kParkDrop m below S (under the sea) instead of on it. The addon parks fakes under ships that carry
// missiles; when one is launched the addon moves F's alias from S to the missile, which is close by, so the
// jump happens without the long trip from the parking spot. addon.h merges pk into g_alias; g_park marks
// which entries are parks.
constexpr double kParkDrop = 300.0;
inline std::set<uint32_t> g_park;                                         // fakes whose alias is a park
inline uint64_t g_parkRecords = 0;
inline bool is_aliased(uint32_t f) { for (auto& x : g_alias) if (x.first == f) return true; return false; }
inline void set_alias(const std::vector<std::pair<uint32_t, uint32_t>>& a) {
    // a target change starts a fresh mirror (the old one belongs to the previous target)
    for (auto it = g_mirror.begin(); it != g_mirror.end();) {
        bool keep = false, stillAliased = false;
        for (auto& x : a) if (x.first == it->first.second) {
            stillAliased = true;
            for (auto& y : g_alias) if (y.first == x.first && y.second == x.second) keep = true;
        }
        if (keep) { ++it; continue; }
        // F no longer aliased and the client was shown it at T: snap it back to its real pose
        // (a standby fake instead goes back to standby: forget it was shown there so it is re-sent)
        if (!stillAliased && it->second.sentEta != 0) {
            if (g_standby.count(it->first.second)) g_standbySent.erase(it->first);
            else g_releaseSnap.insert(it->first);
        }
        it = g_mirror.erase(it);
    }
    // F aliased again before its release snap went out: the new alias snaps it instead
    for (auto it = g_releaseSnap.begin(); it != g_releaseSnap.end();) {
        bool again = false;
        for (auto& x : a) if (x.first == it->second) again = true;
        if (again) it = g_releaseSnap.erase(it); else ++it;
    }
    g_alias = a;
}

// Called for every native body0 sample (addon.h uses it to find the addon's tick offset). May be null.
inline void (*g_onNative)(uint32_t veh, uint32_t tick, double x, double y, double z) = nullptr;

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

inline void body_native(const uint8_t* r, int L, uint32_t V, uint32_t tick);
inline void body_view_native(uint64_t peer, const uint8_t* r, int L, uint32_t V, uint32_t tick);
inline void bodies_forget(uint32_t V);

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
            if (tag == 0x38) { g_group.erase(V); bodies_forget(V); }              // despawned for good
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
                if (g_onNative) g_onNative(V, tick, D(body + off + 31), D(body + off + 39), D(body + off + 47));
                body_native(body + off, L, V, tick);                                 // turret / gun offsets (prototype)
                body_view_native(peer, body + off, L, V, tick);                      // what this client is shown (diagnostic)
                bool addonFeeds = g_cfg.addonPoses && v.addonT != 0 && tick <= v.addonT + kAddonFreshTicks;
                if (addonFeeds || (v.has && tick <= v.curT)) {
                    // pose comes from the other source (or is not newer): keep only the template fresh
                    // (non-root bodies, flags); fill_pose rewrites body0 before anything is sent.
                    v.len = L; memcpy(v.bytes, body + off, L);
                } else if (!v.has || tick > v.curT) {
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

// One body0 sample from the addon (addon.h, already mapped to the world tick). Same bookkeeping as a
// native sample but without a record template: a vehicle no native feed has carried yet is tracked
// (velocity, I_src) and becomes appendable once a native 0x81 supplies the template.
inline void observe_addon(uint32_t V, uint32_t tick, double x, double y, double z, const float q[4]) {
    Veh& v = g_veh[V];
    if (v.has && tick <= v.curT) return;                       // native (or an earlier batch) is as fresh
    if (v.has) {
        double dt = (double)(tick - v.curT);
        double dx = x - v.cx, dy = y - v.cy, dz = z - v.cz;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > kAddonMaxStep * dt) {
            v.prevT = 0; v.srcGap = 0;                          // jump: no velocity across it
        } else {
            v.srcGap = v.srcGap > 0 ? v.srcGap + (dt - v.srcGap) * kSrcEmaAlpha : dt;
            v.prevT = v.curT; v.px = v.cx; v.py = v.cy; v.pz = v.cz;
            memcpy(v.pq, v.cq, sizeof(v.cq));
        }
    }
    v.curT = tick; v.addonT = tick;
    v.cx = x; v.cy = y; v.cz = z;
    for (int i = 0; i < 4; i++) v.cq[i] = q[i];
    v.has = true;
}

// Drop addon-only vehicles (never got a native template) whose last sample is older than `stale`.
inline void prune_addon_only(uint32_t nowTick) {
    for (auto it = g_veh.begin(); it != g_veh.end();) {
        const Veh& v = it->second;
        if (v.len <= 0 && v.addonT != 0 && nowTick > v.curT + (uint32_t)g_cfg.stale) it = g_veh.erase(it);
        else ++it;
    }
}

// ---- per-kind gate on the addon source (Stormworks_WebMap docs/progress/sync-kind-select.md) ----
// IPC addon.kinds (MapViewer Type → Bridge) says which vehicles are which kind. Only the addon → body0 path is
// gated: relay from native samples, the tick-offset history and the turret / gun "bp" are not.
enum Kind : uint8_t { kKindOther = 0, kKindMissile, kKindShip, kKindTank, kKindSub, kKindAir, kKindCount };
inline const char* const kKindNames[kKindCount] = { "Other", "Missile", "Ship", "Tank", "SubMarine", "AirCraft" };
inline std::unordered_map<uint32_t, uint8_t> g_kindOf;   // vehId -> Kind (absent = Other)
inline bool     g_kindsKnown = false;                    // false until the first addon.kinds: every kind is fed
inline uint64_t g_kindUpdates = 0, g_kindSkipped = 0, g_kindDropped = 0;
inline int      g_kindGateSig = -1;                      // gate settings last swept (see kind_sweep)
inline bool kind_on(uint8_t k) {
    switch (k) {
    case kKindMissile: return g_cfg.addonMissile != 0;
    case kKindShip:    return g_cfg.addonShip != 0;
    case kKindTank:    return g_cfg.addonTank != 0;
    case kKindSub:     return g_cfg.addonSub != 0;
    case kKindAir:     return g_cfg.addonAir != 0;
    default:           return g_cfg.addonOther != 0;
    }
}
inline uint8_t kind_of(uint32_t V) { auto it = g_kindOf.find(V); return it == g_kindOf.end() ? (uint8_t)kKindOther : it->second; }
inline bool addon_kind_on(uint32_t V) { return !g_kindsKnown || kind_on(kind_of(V)); }
inline int kind_gate_sig() {
    int s = g_kindsKnown ? 1 : 0;
    for (int k = 0; k < kKindCount; k++) s |= (kind_on((uint8_t)k) ? 1 : 0) << (k + 1);
    return s;
}
// When a kind is switched off (or a vehicle's kind changes to one that is off), forget the addon-fed pose at
// once so the vehicle goes back to native at the next 0x81 instead of dead-reckoning the old addon samples
// until `stale`. Cheap: runs only when the gate settings or the kind list changed.
inline void kind_sweep(bool kindsChanged) {
    int sig = kind_gate_sig();
    if (!kindsChanged && sig == g_kindGateSig) return;
    g_kindGateSig = sig;
    for (auto it = g_veh.begin(); it != g_veh.end();) {
        Veh& v = it->second;
        if (v.addonT == 0 || addon_kind_on(it->first)) { ++it; continue; }
        g_kindDropped++;
        if (v.len <= 0) { it = g_veh.erase(it); continue; }      // addon-only (no native template yet)
        v.has = false; v.addonT = 0; v.prevT = 0; v.srcGap = 0; // keep the template; the next 0x81 refills it
        ++it;
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

// How recipient `peer` is currently given V (stats only; mirrors build_inject's decisions at the peer's
// last send tick): "fake" = fake_missile alias / park / standby rewrite, "pass" = the server's record as is,
// "addon" = pose rewritten from the WebMap addon's samples, "relay" = pose rewritten from server samples
// (another recipient's denser feed, or this one's extrapolated). A native record is rewritten when V is
// managed, or when its gap exceeds Λ (pose moved ahead, server ETA kept).
inline const char* feed_mode(uint64_t peer, uint32_t V, bool relayOn) {
    if (is_aliased(V) || g_standby.count(V)) return "fake";
    auto vit = g_veh.find(V);
    if (!relayOn || vit == g_veh.end() || !vit->second.has || !has_velocity(vit->second)) return "pass";
    const Veh& v = vit->second;
    auto pt = g_peerProc.find(peer);
    uint32_t tick = pt != g_peerProc.end() ? pt->second : v.curT;
    if (!managed(peer, V, tick)) {
        auto git = g_natGap.find(std::make_pair(peer, V));
        if (git == g_natGap.end() || git->second <= (uint32_t)g_cfg.lag) return "pass";
    }
    return (v.addonT != 0 && v.curT == v.addonT) ? "addon" : "relay";
}

// Ticks between the records recipient `peer` actually gets for V (stats only): the relay cadence when V is
// managed (we append / rewrite on our own schedule), else the server's own native gap.
inline uint32_t feed_every(uint64_t peer, uint32_t V) {
    auto git = g_natGap.find(std::make_pair(peer, V));
    uint32_t nat = git != g_natGap.end() ? git->second : 0;
    auto vit = g_veh.find(V);
    if (vit == g_veh.end() || !vit->second.has) return nat;
    auto pt = g_peerProc.find(peer);
    uint32_t tick = pt != g_peerProc.end() ? pt->second : vit->second.curT;
    if (!managed(peer, V, tick)) return nat;
    return (uint32_t)interval_of(vit->second, nat_gap_of(vit->second, peer, V, tick));
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

// ---- non-root bodies (prototype: turret / gun sync; Stormworks_WebMap docs/progress/turret-sync.md) ----
// The addon sends, for body bi >= 1 of a vehicle, the pose of ONE voxel of that body (addon.poses "bp"):
// the voxel's world position and the body's rotation. In 0x81 a type-2 body is its centre of mass relative
// to the type-1 body: world CoM = P1 + R(q1)·rel_p, world rotation q = q1 ⊗ rel_q (quaternions x, y, z, w;
// checked on sizima_v2_9, 2026-11-21). The voxel→CoM offset in body axes is measured from native records:
// off = R(q)ᵀ (CoM − voxel), so no part masses are needed. Prototype limits: body0 must be the type-1 body
// (the relay already only handles those records); bodies the addon does not report are left as they are.
constexpr int    kBodyRing        = 8;     // addon samples kept per body (≈ 24 ticks at 3)
constexpr double kBodyOffAlpha    = 0.2;   // smoothing of the measured offset
constexpr long   kBodyHorizon     = 30;    // max extrapolation of a body pose past its newest sample (ticks)
// Guard (2026-11-21, BB-05 battleship): measurements taken while a turret slews can be far off (2 m / 6°,
// some 45°) and the smoothed offset absorbed them, leaving 0.3–1.3 m after the guns stopped. A check that
// misses by more than this is discarded; kBodyResetAfter misses in a row mean the CoM really moved (ammo
// used up, damage): start again from the newest measurement. A body is written only after a check passed
// and while the latest check passed.
constexpr double kBodyRejectPos   = 0.5;   // m
constexpr double kBodyRejectAng   = 3.0;   // degrees
constexpr int    kBodyResetAfter  = 3;
struct BSample { uint32_t t = 0; double x = 0, y = 0, z = 0; double q[4] = { 0, 0, 0, 1 }; };
struct BodyTrack {
    BSample s[kBodyRing]; int n = 0, head = 0;
    bool offKnown = false; double off[3] = { 0, 0, 0 };
    uint64_t measured = 0;
    uint32_t lastNative = 0;         // one measurement per native tick (every recipient gets the same record)
    double errPos = 0, errAng = 0;   // last check of the prediction against a native record (m, degrees)
    int okRun = 0, missRun = 0;      // consecutive checks within / outside the guard
    uint64_t rejected = 0, resets = 0;
};
inline std::map<std::pair<uint32_t, int>, BodyTrack> g_bodies;   // (vehicle, body index) -> track
inline uint64_t g_bodySamples = 0, g_bodyMeasured = 0, g_bodyWrites = 0, g_bodyBuilt = 0, g_bodyErrN = 0;
inline uint64_t g_bodyRejected = 0, g_bodyResets = 0;
inline double   g_bodyErrPosSum = 0, g_bodyErrAngSum = 0, g_bodyErrPosMax = 0, g_bodyErrAngMax = 0;

inline void qmul(const double a[4], const double b[4], double o[4]) {
    double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
inline void qrot(const double q[4], const double v[3], double o[3]) {          // R(q)·v
    double p[4] = { v[0], v[1], v[2], 0 }, c[4] = { -q[0], -q[1], -q[2], q[3] }, t[4], r[4];
    qmul(q, p, t); qmul(t, c, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
inline void qrotT(const double q[4], const double v[3], double o[3]) {         // R(q)ᵀ·v
    double c[4] = { -q[0], -q[1], -q[2], q[3] };
    qrot(c, v, o);
}
// Angle between two rotations: atan2 of the relative rotation a·b* (vector part, |scalar part|), not acos of
// the dot product, which loses ~0.05° near 0 on float-rounded quaternions (near clients compare at 0.01°).
inline double qangle_deg(const double a[4], const double b[4]) {
    double c[4] = { -b[0], -b[1], -b[2], b[3] }, r[4];
    qmul(a, c, r);
    return 2.0 * std::atan2(std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), std::fabs(r[3])) * 57.29577951308232;
}
inline void qnorm(double q[4]) {
    double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n <= 1e-12) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
inline const BSample& body_latest(const BodyTrack& b) { return b.s[(b.head + kBodyRing - 1) % kBodyRing]; }
inline bool has_bodies(uint32_t V) {
    auto it = g_bodies.lower_bound(std::make_pair(V, 0));
    return it != g_bodies.end() && it->first.first == V;
}

// One addon body sample, already on the world tick (addon.h).
// Diagnostic (2026-11-21): how far the body a client is shown is from the true one. Per (vehicle, body, peer)
// keep the last type-2 rel q / rel p the SERVER sent that client (passive, before any rewrite); every addon
// sample (the true body pose in the same body0-relative frame, at its voxel) is compared with it. The voxel
// sits off the CoM, so the position error carries the offset: only the angle is compared with the native
// rotation; a sample whose pose did not change since the previous one counts as "still" (what remains once
// the gun stops = what the server never sent).
struct BodyView {
    bool have = false; double rq[4] = { 0, 0, 0, 1 }; uint32_t t = 0;
    uint64_t nat2 = 0, nat0 = 0, n = 0, stillN = 0;
    double last = 0, sum = 0, max = 0, stillLast = 0, stillSum = 0, stillMax = 0;
};
inline std::map<std::tuple<uint32_t, int, uint64_t>, BodyView> g_bodyView;   // (vehicle, body, peer)
// What each client was last sent for a body (native type 2 or our write; bodynear_pass) and its error vs the truth.
constexpr double kNearBodyDeg = 0.01;
struct BodyShown { bool have = false; double rq[4] = { 0, 0, 0, 1 }; uint64_t n = 0, stillN = 0; double last = 0, sum = 0, max = 0, stillSum = 0, stillMax = 0; };
inline std::map<std::tuple<uint32_t, int, uint64_t>, BodyShown> g_bodyShown;   // (vehicle, body, peer)
inline uint64_t g_bodyNear = 0;
// Per-player switch (2026-11-21, `?bs on|off` in game chat): the turret / gun rewrite reaches only the players
// who turned it on (default off). bodySync stays the master switch for everyone. Live only (not saved).
inline std::unordered_set<uint64_t> g_bodyPeers;
inline bool body_sync_for(uint64_t peer) { return g_cfg.bodySync && g_bodyPeers.count(peer) > 0; }
inline void body_view_native(uint64_t peer, const uint8_t* r, int L, uint32_t V, uint32_t tick) {
    if (L < 14 + 41 || r[14] != 1) return;
    int cnt = (int)(r[12] | (r[13] << 8)), p = 14;
    for (int bi = 0; bi < cnt && p < L; bi++) {
        int t = r[p], dl = t == 0 ? 0 : t == 1 ? 40 : t == 2 ? 28 : -1;
        if (dl < 0 || p + 1 + dl > L) return;
        if (bi >= 1 && g_bodies.count(std::make_pair(V, bi))) {
            BodyView& w = g_bodyView[std::make_tuple(V, bi, peer)];
            if (t == 2) {
                double q[4] = { F(r + p + 1), F(r + p + 5), F(r + p + 9), F(r + p + 13) };
                qnorm(q); for (int i = 0; i < 4; i++) w.rq[i] = q[i];
                w.have = true; w.t = tick; w.nat2++;
            } else if (t == 0) w.nat0++;
        }
        p += 1 + dl;
    }
}
inline void body_view_sample(uint32_t V, int bi, const double q[4], bool still) {
    for (auto it = g_bodyShown.lower_bound(std::make_tuple(V, bi, (uint64_t)0));
         it != g_bodyShown.end() && std::get<0>(it->first) == V && std::get<1>(it->first) == bi; ++it) {
        BodyShown& w = it->second;
        if (!w.have) continue;
        double e = qangle_deg(w.rq, q);
        w.last = e; w.sum += e; w.n++; if (e > w.max) w.max = e;
        if (still) { w.stillSum += e; w.stillN++; if (e > w.stillMax) w.stillMax = e; }
    }
    for (auto it = g_bodyView.lower_bound(std::make_tuple(V, bi, (uint64_t)0));
         it != g_bodyView.end() && std::get<0>(it->first) == V && std::get<1>(it->first) == bi; ++it) {
        BodyView& w = it->second;
        if (!w.have) continue;
        double e = qangle_deg(w.rq, q);
        w.last = e; w.sum += e; w.n++; if (e > w.max) w.max = e;
        if (still) { w.stillLast = e; w.stillSum += e; w.stillN++; if (e > w.stillMax) w.stillMax = e; }
    }
}
inline void body_sample(uint32_t V, int bi, uint32_t t, double x, double y, double z, const double q[4]) {
    if (bi < 1 || bi > 63) return;
    BodyTrack& b = g_bodies[std::make_pair(V, bi)];
    if (b.n > 0 && t <= body_latest(b).t) return;
    {
        bool still = false;
        if (b.n > 0) {
            const BSample& pv = body_latest(b);
            double dx = x - pv.x, dy = y - pv.y, dz = z - pv.z;
            still = dx * dx + dy * dy + dz * dz < 0.01 * 0.01 && qangle_deg(pv.q, q) < 0.05;
        }
        body_view_sample(V, bi, q, still);
    }
    BSample& s = b.s[b.head];
    s.t = t; s.x = x; s.y = y; s.z = z;
    for (int i = 0; i < 4; i++) s.q[i] = q[i];
    if (b.n > 0) {                                                           // same hemisphere as the previous
        const BSample& p = body_latest(b);
        if (p.q[0] * s.q[0] + p.q[1] * s.q[1] + p.q[2] * s.q[2] + p.q[3] * s.q[3] < 0) for (int i = 0; i < 4; i++) s.q[i] = -s.q[i];
    }
    qnorm(s.q);
    b.head = (b.head + 1) % kBodyRing; if (b.n < kBodyRing) b.n++;
    g_bodySamples++;
}
// The addon generation changed (new tick mapping): old samples sit on the wrong ticks. Offsets stay.
inline void bodies_reset_samples() { for (auto& kv : g_bodies) { kv.second.n = 0; kv.second.head = 0; } }
inline void bodies_forget(uint32_t V) {
    for (auto it = g_bodyView.lower_bound(std::make_tuple(V, 0, (uint64_t)0)); it != g_bodyView.end() && std::get<0>(it->first) == V;) it = g_bodyView.erase(it);
    for (auto it = g_bodyShown.lower_bound(std::make_tuple(V, 0, (uint64_t)0)); it != g_bodyShown.end() && std::get<0>(it->first) == V;) it = g_bodyShown.erase(it);
    for (auto it = g_bodies.lower_bound(std::make_pair(V, 0)); it != g_bodies.end() && it->first.first == V;) it = g_bodies.erase(it);
}

// The body pose at world tick T from the two samples around it (at most 6 ticks apart). False if not covered.
inline bool body_at(const BodyTrack& b, uint32_t T, BSample& o) {
    for (int k = 0; k < b.n; k++) {
        const BSample& a = b.s[(b.head + kBodyRing - 1 - k) % kBodyRing];
        if (a.t == T) { o = a; return true; }
        if (k + 1 >= b.n) break;
        const BSample& p = b.s[(b.head + kBodyRing - 2 - k) % kBodyRing];
        // a long gap is fine when the body did not move in it (the addon sends an unchanged body only every 60 ticks)
        bool still = std::fabs(a.x - p.x) < 0.01 && std::fabs(a.y - p.y) < 0.01 && std::fabs(a.z - p.z) < 0.01
                     && std::fabs(a.q[0] * p.q[0] + a.q[1] * p.q[1] + a.q[2] * p.q[2] + a.q[3] * p.q[3]) > 0.9999999;
        if (p.t < T && T < a.t && (a.t - p.t <= 6 || still)) {
            double f = (double)(T - p.t) / (double)(a.t - p.t);
            o.t = T; o.x = p.x + (a.x - p.x) * f; o.y = p.y + (a.y - p.y) * f; o.z = p.z + (a.z - p.z) * f;
            for (int i = 0; i < 4; i++) o.q[i] = p.q[i] + (a.q[i] - p.q[i]) * f;
            qnorm(o.q);
            return true;
        }
    }
    return false;
}
// The body pose predicted at `target` (constant rate from the newest two samples, at most kBodyHorizon ahead).
inline void body_predict(const BodyTrack& b, uint32_t target, BSample& o) {
    const BSample& a = body_latest(b);
    o = a;
    if (b.n < 2) return;
    const BSample& p = b.s[(b.head + kBodyRing - 2) % kBodyRing];
    if (a.t <= p.t || a.t - p.t > 12) return;
    long ahead = (long)target - (long)a.t;
    if (ahead > kBodyHorizon) ahead = kBodyHorizon;
    if (ahead <= 0) return;
    double f = (double)ahead / (double)(a.t - p.t);
    o.t = a.t + (uint32_t)ahead;
    o.x = a.x + (a.x - p.x) * f; o.y = a.y + (a.y - p.y) * f; o.z = a.z + (a.z - p.z) * f;
    for (int i = 0; i < 4; i++) o.q[i] = a.q[i] + (a.q[i] - p.q[i]) * f;
    qnorm(o.q);
}
inline int body_data_len(int t) { return t == 0 ? 0 : t == 1 ? 40 : t == 2 ? 28 : -1; }

// One measurement: the body's native CoM / rotation at a tick against the addon voxel pose at that tick.
// First checks the current offset (prediction error, IPC status), then updates it.
inline void body_measure(BodyTrack& b, const BSample& a, const double com[3], const double qw[4]) {
    double vox[3] = { a.x, a.y, a.z };
    if (b.offKnown) {
        double w[3]; qrot(a.q, b.off, w);
        double ex = vox[0] + w[0] - com[0], ey = vox[1] + w[1] - com[1], ez = vox[2] + w[2] - com[2];
        b.errPos = std::sqrt(ex * ex + ey * ey + ez * ez);
        b.errAng = qangle_deg(a.q, qw);
        g_bodyErrPosSum += b.errPos; g_bodyErrAngSum += b.errAng; g_bodyErrN++;
        if (b.errPos > g_bodyErrPosMax) g_bodyErrPosMax = b.errPos;
        if (b.errAng > g_bodyErrAngMax) g_bodyErrAngMax = b.errAng;
    }
    double diff[3] = { com[0] - vox[0], com[1] - vox[1], com[2] - vox[2] }, off[3];
    qrotT(a.q, diff, off);
    bool restart = !b.offKnown;
    if (b.offKnown) {
        if (b.errPos > kBodyRejectPos || b.errAng > kBodyRejectAng) {
            b.okRun = 0; b.rejected++; g_bodyRejected++;
            if (++b.missRun < kBodyResetAfter) return;                     // discard this one
            b.resets++; g_bodyResets++; restart = true;                    // the offset really changed
        } else { b.okRun++; b.missRun = 0; }
    }
    if (restart) { b.okRun = 0; b.missRun = 0; }
    for (int i = 0; i < 3; i++) b.off[i] = restart ? off[i] : b.off[i] + (off[i] - b.off[i]) * kBodyOffAlpha;
    b.offKnown = true; b.measured++; g_bodyMeasured++;
}
// A native record usually reaches us before the addon sample of the same tick: keep it and retry after
// later addon batches (like addon.h's g_pending for body0).
struct BodyPending { uint32_t V; int bi; uint32_t T; double com[3], qw[4]; };
inline std::vector<BodyPending> g_bodyPending;
constexpr int kBodyPendingMax = 256; constexpr uint32_t kBodyPendingTicks = 90;
inline uint64_t g_bodyPendingDropped = 0;
inline void bodies_retry(uint32_t nowTick) {
    for (size_t i = 0; i < g_bodyPending.size();) {
        BodyPending& p = g_bodyPending[i];
        auto it = g_bodies.find(std::make_pair(p.V, p.bi));
        BSample a;
        bool done = true;
        if (it != g_bodies.end()) {
            if (body_at(it->second, p.T, a)) body_measure(it->second, a, p.com, p.qw);
            else if ((it->second.n == 0 || body_latest(it->second).t < p.T) && nowTick <= p.T + kBodyPendingTicks) done = false;
            else g_bodyPendingDropped++;
        }
        if (done) g_bodyPending.erase(g_bodyPending.begin() + i); else i++;
    }
}

// A native 0x81 of V (r, L bytes, body0 type 1) at world tick `tick`: measure each reported body's
// voxel→CoM offset, and first check the current offset against it (the prediction error, IPC status).
inline void body_native(const uint8_t* r, int L, uint32_t V, uint32_t tick) {
    // The addon's bp samples are relative to body0 (position in body0 axes, rotation R0ᵀR) and type 1 is
    // body0 here, so the native rel p / rel q compare directly: no hull pose involved.
    if (L < 14 + 41 || r[14] != 1 || !has_bodies(V)) return;
    int cnt = (int)(r[12] | (r[13] << 8)), p = 14;
    for (int bi = 0; bi < cnt && p < L; bi++) {
        int t = r[p], dl = body_data_len(t);
        if (dl < 0 || p + 1 + dl > L) return;
        if (bi >= 1 && t == 2) {
            auto it = g_bodies.find(std::make_pair(V, bi));
            BSample a;
            if (it != g_bodies.end() && it->second.lastNative != tick) {
                BodyTrack& b = it->second;
                b.lastNative = tick;
                double rq[4] = { F(r + p + 1), F(r + p + 5), F(r + p + 9), F(r + p + 13) };
                double rp[3] = { F(r + p + 17), F(r + p + 21), F(r + p + 25) };
                qnorm(rq);
                const double* com = rp; const double* qw = rq;
                if (body_at(b, tick, a)) body_measure(b, a, com, qw);
                else if (b.n == 0 || body_latest(b).t < tick) {                   // addon sample not here yet
                    if ((int)g_bodyPending.size() >= kBodyPendingMax) { g_bodyPending.erase(g_bodyPending.begin()); g_bodyPendingDropped++; }
                    BodyPending pd{ V, bi, tick, { com[0], com[1], com[2] }, { qw[0], qw[1], qw[2], qw[3] } };
                    g_bodyPending.push_back(pd);
                }
            }
        }
        p += 1 + dl;
    }
}

inline bool body_usable(uint32_t V, int bi, uint32_t tick, const BodyTrack*& out) {
    auto it = g_bodies.find(std::make_pair(V, bi));
    if (it == g_bodies.end() || !it->second.offKnown || it->second.n == 0) return false;
    if (it->second.okRun < 1 || it->second.missRun > 0) return false;      // guard: not confirmed / last check missed
    if (tick > body_latest(it->second).t + (uint32_t)g_cfg.stale) return false;
    out = &it->second;
    return true;
}

// Rewrite the type-2 slots of record r (L bytes, body0 already filled with the pose at `target`) with
// the addon's body poses predicted at `target`. Returns the number of bodies written.
inline int body_fill(uint8_t* r, int L, uint32_t V, uint32_t target, uint32_t tick) {
    if (!g_cfg.bodySync || L < 14 + 41 || r[14] != 1 || !has_bodies(V)) return 0;
    int cnt = (int)(r[12] | (r[13] << 8)), p = 14, n = 0;
    for (int bi = 0; bi < cnt && p < L; bi++) {
        int t = r[p], dl = body_data_len(t);
        if (dl < 0 || p + 1 + dl > L) break;
        const BodyTrack* b = nullptr;
        if (bi >= 1 && t == 2 && body_usable(V, bi, tick, b)) {
            BSample o; body_predict(*b, target, o);
            // relative sample (body0 axes): rel p = voxel + R(q)·off, rel q = q
            double w[3], rp[3], rq[4] = { o.q[0], o.q[1], o.q[2], o.q[3] };
            qrot(o.q, b->off, w);
            rp[0] = o.x + w[0]; rp[1] = o.y + w[1]; rp[2] = o.z + w[2];
            qnorm(rq);
            double old[4] = { F(r + p + 1), F(r + p + 5), F(r + p + 9), F(r + p + 13) };   // keep the record's hemisphere
            if (old[0] * rq[0] + old[1] * rq[1] + old[2] * rq[2] + old[3] * rq[3] < 0) for (int i = 0; i < 4; i++) rq[i] = -rq[i];
            for (int i = 0; i < 4; i++) WF(r + p + 1 + i * 4, (float)rq[i]);
            for (int i = 0; i < 3; i++) WF(r + p + 17 + i * 4, (float)rp[i]);
            n++;
        }
        p += 1 + dl;
    }
    g_bodyWrites += n;
    return n;
}

// Copy template `src` (len bytes) to `dst`, turning type-0 slots of usable bodies into type 2 so
// body_fill has room for them (an appended record is ours: its layout may differ from the server's;
// bodyCount never changes). Returns the new length, or 0 if it does not fit in `cap`.
inline int body_build(uint8_t* dst, int cap, const uint8_t* src, int len, uint32_t V, uint32_t tick) {
    if (!g_cfg.bodySync || len < 14 + 41 || src[14] != 1 || !has_bodies(V)) {
        if (len > cap) return 0;
        memcpy(dst, src, len); return len;
    }
    int cnt = (int)(src[12] | (src[13] << 8)), p = 14, w = 14, grown = 0;
    if (cap < 14) return 0;
    memcpy(dst, src, 14);
    for (int bi = 0; bi < cnt; bi++) {
        if (p >= len) return 0;
        int t = src[p], dl = body_data_len(t);
        if (dl < 0 || p + 1 + dl > len) return 0;
        const BodyTrack* b = nullptr;
        if (bi >= 1 && t == 0 && body_usable(V, bi, tick, b)) {
            if (w + 29 > cap || w + 29 > kMaxRecLen) return 0;
            dst[w] = 2;
            memset(dst + w + 1, 0, 28);
            WF(dst + w + 13, 1.0f);                                          // identity until body_fill writes it
            w += 29; grown++;
        } else {
            if (w + 1 + dl > cap || w + 1 + dl > kMaxRecLen) return 0;
            memcpy(dst + w, src + p, 1 + dl);
            w += 1 + dl;
        }
        p += 1 + dl;
    }
    if (p != len) return 0;                                                  // not a plain 0x81: leave it
    if (grown) g_bodyBuilt++;
    return w;
}

// Near recipients (2026-11-21): a client whose native gap is <= lag gets the server's records untouched,
// and when a gun is nudged by a few hundredths of a degree the server sends that body as type 0 ("no
// change") for minutes: the client's laser and shells stay on the old aim. For each native 0x81 that
// reaches this client (as is or rewritten by pass 1; 2026-11-21: the shooter is often a "far" recipient), a type-0 body the addon reports whose true rotation differs from what this
// client was last shown by more than kNearBodyDeg becomes type 2 with the addon pose (the hull and the
// server's type-2 bodies are not touched). Grows `wpos` by 28 per body; returns the bodies written.
inline int bodynear_pass(uint64_t peer, uint8_t* out, int& wpos, int outcap, int nativeCount, int recCount, uint32_t tick,
                         const std::unordered_set<uint32_t>& handled) {
    if (!body_sync_for(peer)) return 0;
    uint8_t* body = out + 8; int n = 0;
    for (int i = 0, off = 20; i < recCount && off < wpos - 8; i++) {           // all records: appended ones are shown too
        int blen = wpos - 8;
        int L = rec::decode_len(body, blen, off);
        if (L <= 0 || off + L > blen) break;
        if (rec::U32(body, blen, off) == 0x81 && L >= 14 + 41 && body[off + 14] == 1) {
            uint32_t V = rec::U32(body, blen, off + 4);
            if (has_bodies(V)) {
                // every native record (as is, or pass 1's rewrite: its type-0 slots are left by body_fill); appended
                // records were built by body_build, which already turns usable type-0 slots into type 2
                bool untouched = i < nativeCount; (void)handled;
                int cnt = (int)rec::U16(body, blen, off + 12), p = off + 14;
                for (int bi = 0; bi < cnt && p < off + L; bi++) {
                    int t = body[p], dl = body_data_len(t);
                    if (dl < 0 || p + 1 + dl > off + L) break;
                    if (bi >= 1 && g_bodies.count(std::make_pair(V, bi))) {
                        BodyShown& sh = g_bodyShown[std::make_tuple(V, bi, peer)];
                        const BodyTrack* b = nullptr;
                        if (t == 2) {                                                 // what this client is shown now
                            double q[4] = { F(body + p + 1), F(body + p + 5), F(body + p + 9), F(body + p + 13) };
                            qnorm(q); for (int k = 0; k < 4; k++) sh.rq[k] = q[k]; sh.have = true;
                        } else if (t == 0 && untouched && body_usable(V, bi, tick, b)) {
                            BSample o; body_predict(*b, tick, o);
                            double rq[4] = { o.q[0], o.q[1], o.q[2], o.q[3] }; qnorm(rq);
                            if ((!sh.have || qangle_deg(sh.rq, rq) > kNearBodyDeg) && wpos + 28 <= outcap) {
                                double w[3]; qrot(o.q, b->off, w);
                                double rp[3] = { o.x + w[0], o.y + w[1], o.z + w[2] };
                                if (sh.have && sh.rq[0] * rq[0] + sh.rq[1] * rq[1] + sh.rq[2] * rq[2] + sh.rq[3] * rq[3] < 0)
                                    for (int k = 0; k < 4; k++) rq[k] = -rq[k];
                                memmove(body + p + 29, body + p + 1, (size_t)(wpos - 8 - (p + 1)));
                                body[p] = 2;
                                for (int k = 0; k < 4; k++) WF(body + p + 1 + k * 4, (float)rq[k]);
                                for (int k = 0; k < 3; k++) WF(body + p + 17 + k * 4, (float)rp[k]);
                                wpos += 28; L += 28; dl = 28; n++;
                                for (int k = 0; k < 4; k++) sh.rq[k] = rq[k]; sh.have = true;
                            }
                        }
                    }
                    p += 1 + dl;
                }
            }
        }
        off += L;
    }
    g_bodyNear += n;
    return n;
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
inline int alias_pass(uint64_t peer, uint8_t* out, int& wpos, int outcap, int recCount, uint32_t tick, int& added, int& addedBytes);
inline int build_inject(uint64_t peer, const uint8_t* orig, int cub, uint8_t* out, int outcap, uint32_t tick, bool relayOn = true) {
    // Rewound-world-tick guard: never modify a message whose world tick is not newer than the last
    // one we injected into for this recipient. A server rewind / resend of an older tick must pass
    // through untouched so we don't stamp future-predicted poses onto a replayed past frame (which
    // could trigger a client resend request). Capture scan (swcap resend) shows this never fires on
    // the battle logs — envelope ticks are strictly forward — but the guard is cheap insurance.
    auto pit = g_peerProc.find(peer);
    if (pit != g_peerProc.end() && tick <= pit->second) return 0;   // 0 = leave original send as-is
    const bool bodyOn = body_sync_for(peer);                         // this player turned the gun rewrite on (?bs on)
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
    for (int i = 0; relayOn && i < count; i++) {
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
                if (bodyOn) body_fill(body + off, L, V, target, tick);               // turret / gun (prototype)
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
        if (!relayOn) break;
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
        int len = bodyOn ? body_build(out + wpos, outcap - wpos, v.bytes, v.len, V, tick) : 0;   // turret / gun slots (prototype)
        if (len <= 0) { memcpy(out + wpos, v.bytes, v.len); len = v.len; }
        memcpy(out + wpos + 8, &eta, 4);
        fill_pose(out + wpos, len, v, target);
        if (bodyOn) body_fill(out + wpos, len, V, target, tick);
        wpos += len; addedBytes += len; added++;
        g_sent[std::make_pair(peer, V)] = Sent{ v.curT, eta, tick };
    }

    int nearWpos = wpos;
    int nearN = relayOn ? bodynear_pass(peer, out, wpos, outcap, count, count + added, tick, handled) : 0;   // near clients: nudged guns
    addedBytes += wpos - nearWpos;                                                              // grown records, same count
    int aliased = (g_alias.empty() && g_releaseSnap.empty() && g_standby.empty()) ? 0 : alias_pass(peer, out, wpos, outcap, count + added, tick, added, addedBytes);
    if (rewrote == 0 && added == 0 && aliased == 0 && nearN == 0) return 0;
    if (addedBytes > 0) { uint32_t total; memcpy(&total, out + 4, 4); total += addedBytes; memcpy(out + 4, &total, 4); }
    if (added > 0) { uint32_t rc; memcpy(&rc, out + 24, 4); rc += added; memcpy(out + 24, &rc, 4); }
    g_injRecords += added; g_injBytes += addedBytes; g_rewrites += rewrote; g_injSends++;
    return cub + addedBytes;
}

// Mirror T's promise onto F in this message (see g_alias). Walks the first recCount records of the copy
// (native + those pass 2 appended), then rewrites F's records or appends one from F's template. Returns
// how many F records were written; `added` / `addedBytes` grow by the appended ones.
inline int alias_pass(uint64_t peer, uint8_t* out, int& wpos, int outcap, int recCount, uint32_t tick, int& added, int& addedBytes) {
    uint8_t* body = out + 8; int blen = wpos - 8;
    int written = 0;
    // alias ended: F's own native record in this message jumps (ETA = now) back to its real pose
    if (!g_releaseSnap.empty()) {
        for (int i = 0, off = 20; i < recCount && off < blen; i++) {
            int L = rec::decode_len(body, blen, off);
            if (L <= 0) break;
            if (rec::U32(body, blen, off) == 0x81 && off + 12 <= blen && body[off + 14] == 1) {
                auto key = std::make_pair(peer, rec::U32(body, blen, off + 4));
                if (g_releaseSnap.count(key)) { memcpy(body + off + 8, &tick, 4); g_releaseSnap.erase(key); g_aliasSnaps++; }
            }
            off += L;
        }
    }
    // standby: every un-aliased fake is shown kStandbyDrop m below its real pose
    if (!g_standby.empty()) {
        std::set<uint32_t> seen;
        auto drop = [&](uint8_t* r, uint32_t V) {
            W(r + 39, D(r + 39) - kStandbyDrop);
            auto key = std::make_pair(peer, V);
            if (!g_standbySent.count(key)) { memcpy(r + 8, &tick, 4); g_standbySent.insert(key); g_aliasSnaps++; }
            g_standbyRecords++; written++;
        };
        for (int i = 0, off = 20; i < recCount && off < blen; i++) {
            int L = rec::decode_len(body, blen, off);
            if (L <= 0) break;
            if (rec::U32(body, blen, off) == 0x81 && off + 31 + 24 <= blen && body[off + 14] == 1) {
                uint32_t V = rec::U32(body, blen, off + 4);
                if (g_standby.count(V) && !is_aliased(V)) { drop(body + off, V); seen.insert(V); }
            }
            off += L;
        }
        for (uint32_t V : g_standby) {
            if (seen.count(V) || is_aliased(V) || g_standbySent.count(std::make_pair(peer, V))) continue;
            auto vit = g_veh.find(V);
            if (vit == g_veh.end() || vit->second.len <= 0) continue;               // no template yet
            const Veh& vf = vit->second;
            if (vf.bytes[14] != 1 || vf.len < 31 + 24 || wpos + vf.len > outcap || added >= kMaxPerSend) continue;
            memcpy(out + wpos, vf.bytes, vf.len);
            drop(out + wpos, V);
            wpos += vf.len; addedBytes += vf.len; added++;
            blen = wpos - 8;
        }
    }
    // remember every vehicle's latest promise to this peer (see g_lastPromise)
    if (!g_standby.empty() || !g_alias.empty()) {
        if (g_lastPromise.size() > kLastPromiseMax) g_lastPromise.clear();
        for (int i = 0, off = 20; i < recCount && off < blen; i++) {
            int L = rec::decode_len(body, blen, off);
            if (L <= 0) break;
            if (rec::U32(body, blen, off) == 0x81 && off + 31 + 24 <= blen && body[off + 14] == 1) {
                LastPromise& c = g_lastPromise[std::make_pair(peer, rec::U32(body, blen, off + 4))];
                memcpy(&c.eta, body + off + 8, 4);
                for (int q = 0; q < 4; q++) c.q[q] = F(body + off + 15 + q * 4);
                c.p[0] = D(body + off + 31); c.p[1] = D(body + off + 39); c.p[2] = D(body + off + 47);
            }
            off += L;
        }
    }
    for (auto& al : g_alias) {
        uint32_t fid = al.first, tid = al.second;
        if (fid == tid) continue;
        const bool park = g_park.count(fid) != 0;
        Mirror& m = g_mirror[std::make_pair(peer, fid)];
        int offF = -1;
        for (int i = 0, off = 20; i < recCount && off < blen; i++) {
            int L = rec::decode_len(body, blen, off);
            if (L <= 0) break;
            if (rec::U32(body, blen, off) == 0x81 && off + 31 + 24 <= blen && body[off + 14] == 1) {
                uint32_t V = rec::U32(body, blen, off + 4);
                if (V == tid) {                                                 // T's promise in this message
                    double np[3] = { D(body + off + 31), D(body + off + 39), D(body + off + 47) };
                    if (m.has) {                                                // direction of travel
                        double d[3] = { np[0] - m.p[0], np[1] - m.p[1], np[2] - m.p[2] };
                        double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (n > 0.05) { for (int k = 0; k < 3; k++) m.dir[k] = d[k] / n; m.hasDir = true; }
                    }
                    m.has = true; memcpy(&m.eta, body + off + 8, 4);
                    for (int q = 0; q < 4; q++) m.q[q] = F(body + off + 15 + q * 4);
                    m.p[0] = np[0]; m.p[1] = np[1]; m.p[2] = np[2];
                } else if (V == fid) offF = off;
            }
            off += L;
        }
        if (!m.has) {
            // T not in this message: start from T's last promise to this peer, if it was ever sent one
            auto c = g_lastPromise.find(std::make_pair(peer, tid));
            if (c == g_lastPromise.end()) continue;                             // T not seen by this peer yet
            m.has = true; m.eta = c->second.eta;
            for (int q = 0; q < 4; q++) m.q[q] = c->second.q[q];
            for (int k = 0; k < 3; k++) m.p[k] = c->second.p[k];
            g_aliasCacheStarts++;
        }
        auto put = [&](uint8_t* r) {
            // first record of this alias for this peer: jump to T now instead of gliding from the parking spot
            if (!m.snapped) { memcpy(r + 8, &tick, 4); m.snapped = true; g_aliasSnaps++; }
            else memcpy(r + 8, &m.eta, 4);
            for (int q = 0; q < 4; q++) WF(r + 15 + q * 4, m.q[q]);
            if (park) { W(r + 31, m.p[0]); W(r + 39, m.p[1] - kParkDrop); W(r + 47, m.p[2]); g_parkRecords++; return; }
            double back = m.hasDir ? kAliasBack : 0.0;
            W(r + 31, m.p[0] - m.dir[0] * back); W(r + 39, m.p[1] - m.dir[1] * back); W(r + 47, m.p[2] - m.dir[2] * back);
        };
        if (offF >= 0) { put(body + offF); m.sentEta = m.eta; written++; continue; }
        // F not in this message: append it from its template, but only when T's promise changed since F
        // was last given one (otherwise F already holds the same promise on the client)
        if (m.sentEta == m.eta) continue;
        auto vit = g_veh.find(fid);
        if (vit == g_veh.end() || vit->second.len <= 0) continue;               // no template yet
        const Veh& vf = vit->second;
        if (vf.bytes[14] != 1 || vf.len < 31 + 24 || wpos + vf.len > outcap || added >= kMaxPerSend) continue;
        memcpy(out + wpos, vf.bytes, vf.len);
        put(out + wpos);
        wpos += vf.len; addedBytes += vf.len; added++; written++;
        m.sentEta = m.eta;
    }
    g_aliasRecords += written;
    return written;
}

} // namespace relay
