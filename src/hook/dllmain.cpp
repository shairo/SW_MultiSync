// swhook.dll — hooks ISteamNetworkingMessages in server64.exe.
// Hooks SendMessageToUser (vtable[0]) and ReceiveMessagesOnChannel (vtable[1]). Every frame is
// captured raw to a per-session container (.swcap); events go to a human-readable .log; type=8 gameplay
// messages are walked with rec.h and, when relay is ON, rewritten/extended by relay.h before the
// original send. All runtime control (relay on/off, capture, config, stats) goes through the
// control-plane in ipc.h: a localhost TCP port speaking JSON lines. No hotkeys since 0.2.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <string>
#include "../common/version.h"
#include "../common/json.h"
#include "steam_min.h"
#include "rec.h"
#include "relay.h"
#include "stats.h"
#include "ipc.h"

// Base directory = the folder swhook.dll lives in (resolved at load from the DLL's own path), so the
// build is portable: drop swhook.dll + inject.exe next to server64.exe and captures/, logs and
// swhook.ini all land there. g_base always ends with a backslash. Set once in DllMain.
static HMODULE g_hmod = nullptr;
static char    g_base[MAX_PATH] = {0};
static const char* BaseDir() {
    if (g_base[0]) return g_base;
    if (g_hmod && GetModuleFileNameA(g_hmod, g_base, MAX_PATH)) {
        char* slash = strrchr(g_base, '\\');
        if (slash) slash[1] = 0; else g_base[0] = 0;
    }
    if (!g_base[0]) strcpy_s(g_base, ".\\");   // last-resort: current dir
    return g_base;
}
// Build "<basedir><suffix>" into caller buffer. suffix uses '\\' separators.
static const char* BasePath(char* buf, size_t n, const char* suffix) {
    _snprintf_s(buf, n, _TRUNCATE, "%s%s", BaseDir(), suffix);
    return buf;
}

static FILE* g_log  = nullptr;
static FILE* g_cap  = nullptr;           // binary container
static char  g_capPath[MAX_PATH] = {0};  // current .swcap path ("" when none)
static char  g_logPath[MAX_PATH] = {0};
static CRITICAL_SECTION g_cs;            // guards g_log + g_cap + counters + relay/stats state
static uint64_t g_seq = 0;               // global, ordered across send+recv
static uint64_t g_capBytes = 0;
static uint64_t g_markCount = 0;         // marker ordinal
static uint64_t g_startMs = 0;           // GetTickCount64 at Worker start
static volatile bool g_capturing = true; // gate: capture.start / capture.stop
static volatile bool g_hooked = false;   // vtable slots swapped
static std::string g_hookError;          // why hooking failed (empty = ok / pending)
constexpr uint64_t kCapCap = 4ull * 1024 * 1024 * 1024;   // 4 GB safety cap

// ---- stage B: record-walker self-test + mutation demo ----
// The self-test is PASSIVE: it walks every outgoing type=8 message with the C++ port of the record
// rules and tallies whether it fully closes (understanding gate), plus simulates appending a 0x81
// into a scratch buffer to prove the inject arithmetic re-walks cleanly. Nothing sent is changed.
// The mutation demo (mutate.set, default OFF) copies the send buffer, adds a fixed offset to every
// 0x81 body0 position, and sends the copy — a solo, visible confirmation that our 0x81 edits reach
// the client. It only ever fires on a message the self-test fully decoded.
static volatile bool g_mutate = false;                 // debug gate for the visible edit demo
static volatile bool g_relay  = false;                 // gate for stage-C relay injection
constexpr double kDemoOffset = 50.0;                    // units added to 0x81 body0 X in the demo
static uint64_t g_stFull = 0, g_stPartial = 0, g_stType8 = 0, g_stAppendOk = 0, g_stAppendBad = 0;
static uint8_t g_scratch[70000];                        // append-sim buffer; used only under g_cs

// Bulk thinning: frames larger than this store only their first kBulkKeepHead bytes (transport
// header + msg header + any filename prefix), enough to identify them, dropping the payload.
// Gameplay msgType=8 tops out ~15 KB so it is never thinned; tiles (523272 B) and big mod files
// are. The container still records each frame's TRUE size, so nothing about sizing is lost.
constexpr uint32_t kBulkThreshold = 65536;
constexpr uint32_t kBulkKeepHead  = 256;

static SendMessageToUser_t        g_origSend = nullptr;
static ReceiveMessagesOnChannel_t g_origRecv = nullptr;
static void** g_vtable = nullptr;
static volatile long g_inflight = 0;     // game threads currently inside a hook (unload waits for 0)
static HANDLE g_instMutex = nullptr;     // double-injection guard; released on unload so re-inject works
static HANDLE g_workerThread = nullptr;  // Worker's handle: unload waits for it (it may still be in the Steam wait)
static volatile bool g_stopping = false; // tells Worker's wait loops to give up
// RAII bump of g_inflight for the two hook bodies.
struct InFlight { InFlight() { InterlockedIncrement(&g_inflight); } ~InFlight() { InterlockedDecrement(&g_inflight); } };

// ---- .swcap container (v2) ----
// file header: "SWCAP" 0x02  (6 bytes)
// per event:  u8 dir(0=send,1=recv) | u64 seq | u64 tickMs | u64 steamID
//             i32 channel | i32 flags | u32 origCub | u32 storedCub | bytes[storedCub]
// origCub = the frame's true size; storedCub = bytes actually kept (< origCub when thinned).
static void cap_event(uint8_t dir, uint64_t seq, uint64_t steamid,
                      int32_t ch, int32_t flags, const void* data, uint32_t cub) {
    uint32_t stored = (cub > kBulkThreshold) ? kBulkKeepHead : cub;
    if (stored > cub) stored = cub;
    if (!g_capturing || !g_cap || g_capBytes + stored + 45 > kCapCap) return;
    uint64_t t = GetTickCount64();
    fwrite(&dir, 1, 1, g_cap);
    fwrite(&seq, 8, 1, g_cap);
    fwrite(&t, 8, 1, g_cap);
    fwrite(&steamid, 8, 1, g_cap);
    fwrite(&ch, 4, 1, g_cap);
    fwrite(&flags, 4, 1, g_cap);
    fwrite(&cub, 4, 1, g_cap);       // true size
    fwrite(&stored, 4, 1, g_cap);    // bytes stored
    if (stored && data) fwrite(data, 1, stored, g_cap);
    g_capBytes += stored + 45;
    if ((seq & 0xFF) == 0) { fflush(g_cap); if (g_log) fflush(g_log); }  // survive a kill
}

// Session .log: event lines only (startup, config, relay stats, hook status, walk-fail notes).
// Gated by cfg.log; the file is opened lazily on the first line written while enabled, so log=0 in
// the ini means no file at all, and flipping it on later just starts the file then.
static char g_stamp[32] = {0};
static void logf(const char* fmt, ...) {
    if (!relay::g_cfg.log) return;
    if (!g_log) {
        char suffix[MAX_PATH];
        _snprintf_s(suffix, _TRUNCATE, "captures\\session_%s.log", g_stamp);
        g_log = fopen(BasePath(g_logPath, MAX_PATH, suffix), "a");
        if (!g_log) { g_logPath[0] = 0; return; }
        fprintf(g_log, "swhook %s session %s (pid %lu)\n", SWHOOK_VERSION, g_stamp, GetCurrentProcessId());
    }
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
}
static void logflush() { if (g_log) fflush(g_log); }

static uint64_t peer_id(const SteamNetworkingIdentity* id) {
    return (id && id->m_eType == kIdentityType_SteamID) ? id->m_steamID64 : 0;
}

// Passive self-test on one type=8 message body (const1 at body[0]). Caller holds g_cs. Walks with
// the C++ rules; on a fully-decoded message that contains a 0x81, also simulates appending a copy of
// that 0x81 (recordCount++) into g_scratch and re-walks to prove the inject arithmetic closes. This
// is the safety gate for stage C: mutation is only ever allowed on messages that pass here.
static rec::Walk selftest(const uint8_t* body, int blen) {
    g_stType8++;
    rec::Walk w = rec::walk(body, blen);
    if (w.full) g_stFull++; else g_stPartial++;

    if (w.full && w.first81 >= 0) {
        int rl = rec::decode_len(body, blen, w.first81);
        if (rl > 0 && blen + rl <= (int)sizeof(g_scratch)) {
            memcpy(g_scratch, body, blen);                       // copy body
            memcpy(g_scratch + blen, body + w.first81, rl);      // append a duplicate 0x81
            uint32_t nc = rec::U32(g_scratch, blen + rl, 16) + 1; // recordCount++
            memcpy(g_scratch + 16, &nc, 4);
            rec::Walk w2 = rec::walk(g_scratch, blen + rl);
            if (w2.full && w2.walked == (int)nc) g_stAppendOk++; else g_stAppendBad++;
        }
    }
    if ((g_stType8 & 0xFF) == 0) {
        logf("== selftest: type8=%llu full=%llu partial=%llu (%.1f%%)  append ok=%llu bad=%llu ==\n",
             (unsigned long long)g_stType8, (unsigned long long)g_stFull,
             (unsigned long long)g_stPartial, 100.0 * g_stFull / (double)g_stType8,
             (unsigned long long)g_stAppendOk, (unsigned long long)g_stAppendBad);
        logflush();
    }
    return w;
}

// Walk-fail dump (cfg.dumpWalkFail): append the WHOLE frame (transport header + body) of a type=8
// message the walker could not close to ONE per-session container, captures/walkfail_<stamp>.swcap —
// same format as the main capture, so `swcap hex/rec/stats` read it directly and each event keeps
// its seq/peer/size/time for cross-referencing. Opened lazily on the first failure (no file when
// nothing fails). Capped by dumpWalkFailMax frames per session. Caller holds g_cs.
static FILE* g_wf = nullptr;
static char  g_wfPath[MAX_PATH] = {0};
static void dump_walkfail(uint8_t dir, uint64_t seq, uint64_t sid, int32_t ch, int32_t flags, const rec::Walk& w, const void* data, uint32_t cub) {
    if (!relay::g_cfg.dumpWalkFail) return;
    if (stats::g_walkFailDumped >= (uint64_t)relay::g_cfg.dumpWalkFailMax) return;
    if (!g_wf) {
        char suffix[MAX_PATH];
        _snprintf_s(suffix, _TRUNCATE, "captures\\walkfail_%s.swcap", g_stamp);
        g_wf = fopen(BasePath(g_wfPath, MAX_PATH, suffix), "wb");
        if (!g_wf) return;
        fwrite("SWCAP\x02", 1, 6, g_wf);
        logf("== walkfail file: %s ==\n", g_wfPath);
    }
    uint64_t t = GetTickCount64(); uint32_t stored = cub;
    fwrite(&dir, 1, 1, g_wf); fwrite(&seq, 8, 1, g_wf); fwrite(&t, 8, 1, g_wf); fwrite(&sid, 8, 1, g_wf);
    fwrite(&ch, 4, 1, g_wf); fwrite(&flags, 4, 1, g_wf); fwrite(&cub, 4, 1, g_wf); fwrite(&stored, 4, 1, g_wf);
    fwrite(data, 1, cub, g_wf); fflush(g_wf);
    stats::g_walkFailDumped++;
    const uint8_t* body = static_cast<const uint8_t*>(data) + 8;
    logf("== walkfail #%llu (%s): seq=%llu peer=%llu recs=%d walked=%d stop@%d tag=0x%X ==\n",
         (unsigned long long)stats::g_walkFailDumped, dir ? "recv" : "send", (unsigned long long)seq, (unsigned long long)sid,
         w.recordCount, w.walked, w.consumed, rec::U32(body, (int)cub - 8, w.consumed));
    logflush();
}

// Mutation demo: add a fixed offset to every 0x81 body0 (type1) X position in a message body,
// in place. Same length (no recordCount/total change) → lowest-risk visible edit. `body` points into
// a private scratch copy, never the caller's buffer. Only called on a self-test-full message.
static void edit81_offset(uint8_t* body, int blen) {
    int count = (int)rec::U32(body, blen, 16), off = 20;
    for (int i = 0; i < count; i++) {
        if (off >= blen) break;
        uint32_t tag = rec::U32(body, blen, off);
        int L = rec::decode_len(body, blen, off);
        if (L <= 0) break;
        if (tag == 0x81 && off + 14 < blen && body[off + 14] == 1) {  // body0 type1
            int px = off + 15 + 16;                                    // skip type + float[4] rot
            if (px + 8 <= blen) {
                double x; memcpy(&x, body + px, 8); x += kDemoOffset; memcpy(body + px, &x, 8);
            }
        }
        off += L;
    }
}

// ---- hooks ----
static int32_t Hooked_Send(void* self, const SteamNetworkingIdentity* id,
                           const void* data, uint32_t cub, int32_t flags, int32_t ch) {
    InFlight guard;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    bool head = cub >= 28 && *reinterpret_cast<const uint32_t*>(b) == 0;   // frag==0 (single-frame head)
    const uint8_t* body = b + 8; int blen = (int)cub - 8;
    bool type8 = head && blen >= 20 && *reinterpret_cast<const uint32_t*>(body + 4) == 8;

    uint32_t tick = type8 ? *reinterpret_cast<const uint32_t*>(body + 8) : 0;
    EnterCriticalSection(&g_cs);
    uint64_t n = ++g_seq, sid = peer_id(id);
    cap_event(0, n, sid, ch, flags, data, cub);
    stats::Peer& ps = stats::touch(sid, GetTickCount64());
    ps.sendCount++; ps.sendBytes += cub;
    // ch15 1-byte control: 0x01 session open, 0x02 close. Close marks the peer disconnected; any later
    // traffic re-marks it (stats::touch), so a rejoin shows up naturally.
    if (ch == 15 && cub == 1 && b[0] == 0x02) ps.connected = false;
    bool full = false;
    int newcub = 0; uint8_t* inj = nullptr;
    if (type8) {
        ps.type8++;
        if (tick < ps.lastTick) {          // world tick went backwards: the relay guard leaves such frames untouched
            if (++ps.tickRewinds <= 10) { logf("== tick rewind to %llu: %u < %u (#%llu) ==\n", (unsigned long long)sid, tick, ps.lastTick, (unsigned long long)ps.tickRewinds); logflush(); }
        } else ps.lastTick = tick;
        rec::Walk w = selftest(body, blen);
        full = w.full;
        if (full) ps.walkFull++; else { ps.walkPartial++; dump_walkfail(0, n, sid, ch, flags, w, data, cub); }
        if (full) {
            relay::observe(sid, body, blen, tick);                   // passive: keep caches warm
            if (g_relay) {                                           // build the injected copy
                int cap = (int)cub + relay::kMaxPerSend * relay::kMaxRecLen;
                inj = static_cast<uint8_t*>(malloc(cap));
                if (inj) {
                    uint64_t recBefore = relay::g_injRecords + relay::g_rewrites;
                    newcub = relay::build_inject(sid, static_cast<const uint8_t*>(data), (int)cub, inj, cap, tick);
                    if (newcub > 0) {
                        ps.injSends++; ps.injBytes += (uint64_t)(newcub - (int)cub);
                        ps.injRecords += (relay::g_injRecords + relay::g_rewrites) - recBefore;
                    }
                    if ((relay::g_injSends & 0x7F) == 0 && newcub > 0)
                        logf("== relay: sends=%llu appended=%llu rewritten=%llu bytes=%llu ==\n",
                             (unsigned long long)relay::g_injSends, (unsigned long long)relay::g_injRecords,
                             (unsigned long long)relay::g_rewrites, (unsigned long long)relay::g_injBytes);
                }
            }
        }
    }
    LeaveCriticalSection(&g_cs);

    // Stage-C relay (default OFF): send the enlarged COPY with dead-reckoned 0x81s appended.
    if (inj) {
        if (newcub > 0) { int32_t rc = g_origSend(self, id, inj, (uint32_t)newcub, flags, ch); free(inj); return rc; }
        free(inj);
    }
    // Stage-B visible demo (default OFF): send an edited COPY, never the caller's buffer, and
    // only when the message fully decoded (so we never corrupt something we don't understand).
    if (g_mutate && type8 && full) {
        uint8_t* s = static_cast<uint8_t*>(malloc(cub));
        if (s) {
            memcpy(s, data, cub);
            edit81_offset(s + 8, blen);
            int32_t rc = g_origSend(self, id, s, cub, flags, ch);
            free(s);
            return rc;
        }
    }
    return g_origSend(self, id, data, cub, flags, ch);
}

static int32_t Hooked_Recv(void* self, int32_t ch, SteamNetworkingMessage_t** ppOut, int32_t nMax) {
    InFlight guard;
    int32_t got = g_origRecv(self, ch, ppOut, nMax);   // fills ppOut with already-decrypted msgs
    if (got > 0 && ppOut) {
        EnterCriticalSection(&g_cs);
        uint64_t now = GetTickCount64();
        for (int32_t i = 0; i < got; ++i) {
            SteamNetworkingMessage_t* m = ppOut[i];
            if (!m) continue;
            uint64_t n = ++g_seq, sid = peer_id(&m->m_identityPeer);
            cap_event(1, n, sid, m->m_nChannel, m->m_nFlags, m->m_pData, (uint32_t)m->m_cbSize);
            stats::Peer& ps = stats::touch(sid, now);
            ps.recvCount++; ps.recvBytes += (uint64_t)m->m_cbSize;
            // Client stream (ch0 msgType=3, single-frame): walk it for the understanding KPI and pick
            // the player's world position out of 0x2F. Passive — nothing on the recv path is modified.
            const uint8_t* b = static_cast<const uint8_t*>(m->m_pData); uint32_t cub = (uint32_t)m->m_cbSize;
            if (m->m_nChannel == 0 && cub >= 22 && *reinterpret_cast<const uint32_t*>(b) == 0) {
                const uint8_t* body = b + 8; int blen = (int)cub - 8;
                if (rec::U32(body, blen, 0) == 1 && rec::U32(body, blen, 4) == 3) {
                    ps.type3++;
                    rec::Walk w = rec::walk_client(body, blen);
                    if (w.full) ps.rwalkFull++; else { ps.rwalkPartial++; dump_walkfail(1, n, sid, m->m_nChannel, m->m_nFlags, w, b, cub); }
                    if (w.first81 >= 0 && w.first81 + 52 <= blen) {
                        memcpy(&ps.px, body + w.first81 + 28, 8); memcpy(&ps.py, body + w.first81 + 36, 8); memcpy(&ps.pz, body + w.first81 + 44, 8);
                        ps.posMs = now;
                    }
                }
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    return got;
}

// ---- capture control (capture.start / capture.stop / capture.mark) ----
// Roll a fresh .swcap file. Caller must hold g_cs. Filename carries ms so rapid restarts differ.
static void open_cap() {
    if (g_cap) { fflush(g_cap); fclose(g_cap); g_cap = nullptr; }
    SYSTEMTIME st; GetLocalTime(&st);
    char suffix[MAX_PATH];
    _snprintf_s(suffix, _TRUNCATE, "captures\\session_%04d%02d%02d_%02d%02d%02d_%03d.swcap",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    BasePath(g_capPath, MAX_PATH, suffix);
    g_cap = fopen(g_capPath, "wb");
    g_capBytes = 0;
    if (g_cap) fwrite("SWCAP\x02", 1, 6, g_cap); else g_capPath[0] = 0;
    logf("== capture file: %s ==\n", g_capPath); logflush();
}
static void close_cap() {                 // caller must hold g_cs
    if (g_cap) { fflush(g_cap); fclose(g_cap); g_cap = nullptr; }
    g_capPath[0] = 0;
}

// Marker event: same container layout, dir=2. steamId field carries the marker ordinal; no payload.
// Lets you annotate "I did action X now" while playing; `swcap marks` lists them on the t+ms clock.
// Returns the marker ordinal or 0 when not capturing. Caller must hold g_cs.
static uint64_t write_marker() {
    if (!(g_capturing && g_cap)) return 0;
    uint8_t dir = 2; uint64_t seq = ++g_seq, t = GetTickCount64(), mid = ++g_markCount;
    int32_t ch = 0, flags = 0; uint32_t cub = 0, stored = 0;
    fwrite(&dir, 1, 1, g_cap); fwrite(&seq, 8, 1, g_cap); fwrite(&t, 8, 1, g_cap);
    fwrite(&mid, 8, 1, g_cap); fwrite(&ch, 4, 1, g_cap); fwrite(&flags, 4, 1, g_cap);
    fwrite(&cub, 4, 1, g_cap); fwrite(&stored, 4, 1, g_cap);
    fflush(g_cap);
    logf("== MARK #%llu (seq %llu) ==\n", (unsigned long long)mid, (unsigned long long)seq);
    logflush();
    return mid;
}

// Load swhook.ini from the DLL dir. Missing file = keep defaults. Logs the effective values so a
// live capture records the config used. Caller must hold g_cs.
static int load_relay_config() {
    char ini[MAX_PATH];
    int n = relay::load_config_file(BasePath(ini, MAX_PATH, "swhook.ini"));
    if (n < 0) logf("== config: no swhook.ini, using defaults ==\n");
    else       logf("== config: swhook.ini loaded (%d keys) ==\n", n);
    logf("  ");
    for (const relay::CfgField& f : relay::kCfgFields) {
        char v[64]; relay::fmt_cfg(f, v, sizeof v);
        logf(" %s=%s", f.name, v);
    }
    logf("\n");
    logflush();
    return n;
}

// ---- IPC command handlers ----
// Every handler runs on an IPC client thread; anything touching hook state takes g_cs.
static void write_config(json::JsonW& w) {
    w.key("config").obj();
    for (const relay::CfgField& f : relay::kCfgFields) w.kv(f.name, relay::get_cfg(f));
    w.end();
}
static void write_status(json::JsonW& w) {
    uint64_t now = GetTickCount64();
    w.kv("version", SWHOOK_VERSION).kv("protocol", SWHOOK_IPC_PROTOCOL);
    w.kv("pid", (unsigned)GetCurrentProcessId()).kv("uptimeMs", now - g_startMs).kv("nowMs", now);
    w.kvb("hooked", g_hooked).kv("hookError", g_hookError);
    w.kvb("relay", g_relay).kvb("capturing", g_capturing && g_cap != nullptr).kvb("mutate", g_mutate);
    w.kv("captureFile", g_capPath).kv("captureBytes", g_capBytes).kv("logFile", g_logPath).kv("walkFailFile", g_wfPath);
    w.kv("ipcPort", ipc::g_port).kv("ipcClients", (int)ipc::g_clients).kv("ipcRequests", (int)ipc::g_requests);
    w.kv("peerCount", (int)stats::g_peers.size()).kv("vehicleCount", (int)relay::g_veh.size());
    w.key("walk").obj()
        .kv("type8", g_stType8).kv("full", g_stFull).kv("partial", g_stPartial)
        .kv("appendOk", g_stAppendOk).kv("appendBad", g_stAppendBad)
        .kv("dumped", stats::g_walkFailDumped).end();
    w.key("inject").obj()
        .kv("sends", relay::g_injSends).kv("appended", relay::g_injRecords)
        .kv("rewritten", relay::g_rewrites).kv("bytes", relay::g_injBytes).end();
    w.kv("seq", g_seq).kv("marks", g_markCount);
}
static void write_peers(json::JsonW& w) {
    uint64_t now = GetTickCount64();
    stats::expire(now);
    w.key("peers").arr();
    for (auto& kv : stats::g_peers) {
        const stats::Peer& p = kv.second;
        int vehs = 0; for (auto& g : relay::g_natGap) if (g.first.first == kv.first) vehs++;
        bool connected = p.connected && now - p.lastSeenMs <= stats::kSilentMs;
        w.obj().kv("steamId", kv.first).kvb("connected", connected)
         .kv("firstSeenMs", p.firstSeenMs).kv("lastSeenMs", p.lastSeenMs).kv("lastTick", p.lastTick)
         .kv("sendCount", p.sendCount).kv("sendBytes", p.sendBytes)
         .kv("recvCount", p.recvCount).kv("recvBytes", p.recvBytes)
         .kv("injSends", p.injSends).kv("injBytes", p.injBytes).kv("injRecords", p.injRecords)
         .kv("type8", p.type8).kv("walkFull", p.walkFull).kv("walkPartial", p.walkPartial)
         .kv("tickRewinds", p.tickRewinds).kv("vehicles", vehs)
         .kv("type3", p.type3).kv("rwalkFull", p.rwalkFull).kv("rwalkPartial", p.rwalkPartial);
        if (p.posMs) w.key("pos").arr().num(p.px).num(p.py).num(p.pz).end().kv("posMs", p.posMs);
        w.end();
    }
    w.end();
}
static void write_vehicles(json::JsonW& w) {
    w.key("vehicles").arr();
    for (auto& kv : relay::g_veh) {
        const relay::Veh& v = kv.second;
        if (!v.has) continue;
        w.obj().kv("id", (unsigned)kv.first).kv("tick", (unsigned)v.curT)
         .key("pos").arr().num(v.cx).num(v.cy).num(v.cz).end()
         .key("rot").arr().num((double)v.cq[0]).num((double)v.cq[1]).num((double)v.cq[2]).num((double)v.cq[3]).end()
         .kv("srcGap", v.srcGap);   // I_src: EMA of ticks between fresh samples (0 = unknown)
        auto grp = relay::g_group.find(kv.first);
        if (grp != relay::g_group.end()) w.kv("group", (unsigned)grp->second);   // absent: spawned before we hooked
        // which recipients see this vehicle, and how coarsely (native gap in ticks = distance proxy)
        w.key("feeds").arr();
        for (auto& g : relay::g_natGap) if (g.first.second == kv.first)
            w.obj().kv("steamId", g.first.first).kv("gap", (unsigned)g.second).end();
        w.end();
        w.end();
    }
    w.end();
}

static void* swap_slot(int index, void* repl);   // defined with the install code below

// ---- unload (development aid) ----
// Restores the vtable, waits for in-flight hook calls to drain, tears down IPC and files, then
// unmaps the DLL from a thread of its own so the next `swctl inject` loads fresh code without a
// server restart. Ordering matters: nothing that could still be running (game threads inside the
// hooks, IPC threads) may exist when FreeLibraryAndExitThread runs. The residual risk — a game
// thread that read the old slot value and got descheduled for longer than the grace period — is
// accepted for a development tool; players are not disconnected by this.
static DWORD WINAPI UnloadThread(LPVOID) {
    Sleep(50);                                              // let the "ok" reply leave the socket
    if (g_hooked && g_vtable) {
        swap_slot(kVT_SendMessageToUser, (void*)g_origSend);
        swap_slot(kVT_ReceiveMessagesOnChannel, (void*)g_origRecv);
        g_hooked = false;
    }
    ULONGLONG t0 = GetTickCount64();
    while (g_inflight > 0 && GetTickCount64() - t0 < 5000) Sleep(5);
    Sleep(500);                                             // grace for a thread already past the slot read
    g_stopping = true;
    bool ipcClean = ipc::stop(3000);
    bool workerDone = !g_workerThread || WaitForSingleObject(g_workerThread, 3000) == WAIT_OBJECT_0;
    if (g_workerThread) { CloseHandle(g_workerThread); g_workerThread = nullptr; }
    EnterCriticalSection(&g_cs);
    logf("== unload: hooks restored, inflight=%ld, ipc %s, worker %s ==\n", g_inflight,
         ipcClean ? "stopped" : "TIMEOUT (leaking DLL)", workerDone ? "exited" : "STUCK (leaking DLL)");
    logflush();
    if (g_cap) { fclose(g_cap); g_cap = nullptr; }
    if (g_wf)  { fclose(g_wf);  g_wf  = nullptr; }
    if (g_log) { fclose(g_log); g_log = nullptr; }
    LeaveCriticalSection(&g_cs);
    if (g_instMutex) { CloseHandle(g_instMutex); g_instMutex = nullptr; }
    if (!ipcClean || !workerDone || g_inflight > 0) return 0;   // a thread may still run our code: stay mapped
    DeleteCriticalSection(&g_cs);
    FreeLibraryAndExitThread(g_hmod, 0);
}

static std::string ipc_handle(const std::string& line) {
    std::string cmd;
    json::JsonW w; w.obj();
    if (!json::get_str(line.c_str(), "cmd", cmd)) {
        w.kvb("ok", false).kv("error", "missing cmd").end(); return w.out;
    }
    std::string id; if (json::get_str(line.c_str(), "id", id)) w.kv("id", id);   // echoed for clients that pipeline

    EnterCriticalSection(&g_cs);
    if (cmd == "ping" || cmd == "version") {
        w.kvb("ok", true).kv("version", SWHOOK_VERSION).kv("protocol", SWHOOK_IPC_PROTOCOL)
         .kv("pid", (unsigned)GetCurrentProcessId()).kvb("hooked", g_hooked);
    } else if (cmd == "status") {
        w.kvb("ok", true); write_status(w); write_config(w);
    } else if (cmd == "stats.get") {
        w.kvb("ok", true); write_status(w); write_peers(w);
    } else if (cmd == "peers.get") {
        w.kvb("ok", true).kv("nowMs", GetTickCount64()); write_peers(w);
    } else if (cmd == "vehicles.get") {
        w.kvb("ok", true).kv("nowMs", GetTickCount64()); write_vehicles(w);
    } else if (cmd == "relay.set" || cmd == "mutate.set") {
        bool en;
        if (!json::get_bool(line.c_str(), "enabled", en)) { w.kvb("ok", false).kv("error", "missing enabled"); }
        else {
            if (cmd == "relay.set") { g_relay = en; logf("== RELAY inject %s (ipc) ==\n", en ? "ON" : "OFF"); }
            else                    { g_mutate = en; logf("== MUTATE demo %s (ipc) ==\n", en ? "ON" : "OFF"); }
            logflush();
            w.kvb("ok", true).kvb("relay", g_relay).kvb("mutate", g_mutate);
        }
    } else if (cmd == "capture.start") {
        open_cap(); g_capturing = g_cap != nullptr;
        w.kvb("ok", g_capturing).kv("captureFile", g_capPath);
    } else if (cmd == "capture.stop") {
        g_capturing = false; close_cap();
        w.kvb("ok", true);
    } else if (cmd == "capture.mark") {
        uint64_t m = write_marker();
        w.kvb("ok", m != 0).kv("mark", m);
        if (!m) w.kv("error", "not capturing");
    } else if (cmd == "config.get") {
        w.kvb("ok", true); write_config(w);
        w.key("fields").arr();
        for (const relay::CfgField& f : relay::kCfgFields)
            w.obj().kv("name", f.name).kv("type", f.isInt ? "int" : "double").kvb("startupOnly", f.startupOnly).kv("help", f.help).end();
        w.end();
    } else if (cmd == "config.set") {
        std::string key; double val;
        if (!json::get_str(line.c_str(), "key", key) || !json::get_num(line.c_str(), "value", val))
            w.kvb("ok", false).kv("error", "need key and value");
        else if (!relay::set_cfg(key.c_str(), val))
            w.kvb("ok", false).kv("error", "unknown key");
        else {
            const relay::CfgField* f = relay::find_field(key.c_str());
            double eff = f ? relay::get_cfg(*f) : val;
            logf("== config.set %s=%g (ipc) ==\n", key.c_str(), eff); logflush();
            w.kvb("ok", true).kv("key", key).kv("value", eff).kvb("startupOnly", f ? f->startupOnly : false);
        }
    } else if (cmd == "config.reload") {
        int n = load_relay_config();
        w.kvb("ok", n >= 0).kv("keys", n); write_config(w);
        if (n < 0) w.kv("error", "swhook.ini not found");
    } else if (cmd == "unload") {
        HANDLE t = CreateThread(nullptr, 0, UnloadThread, nullptr, 0, nullptr);
        if (t) { CloseHandle(t); w.kvb("ok", true).kvb("hooked", g_hooked); logf("== unload requested (ipc) ==\n"); logflush(); }
        else w.kvb("ok", false).kv("error", "CreateThread failed");
    } else if (cmd == "config.save") {
        char ini[MAX_PATH];
        bool ok = relay::save_config_file(BasePath(ini, MAX_PATH, "swhook.ini"));
        logf("== config.save -> %s: %s ==\n", ini, ok ? "ok" : "FAILED"); logflush();
        w.kvb("ok", ok).kv("path", ini);
        if (!ok) w.kv("error", "write failed");
    } else {
        w.kvb("ok", false).kv("error", "unknown cmd: " + cmd);
    }
    LeaveCriticalSection(&g_cs);
    w.end();
    return w.out;
}

static void* swap_slot(int index, void* repl) {
    void** slot = &g_vtable[index];
    void* orig = *slot;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    *slot = repl;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return orig;
}

static void fail(const char* why) {
    EnterCriticalSection(&g_cs);
    g_hookError = why;
    logf("%s\n", why); logflush();
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI Worker(LPVOID) {
    // Double-injection guard: a per-process named mutex. Re-injecting the SAME dll is already a
    // LoadLibrary no-op (DllMain not re-run), but injecting a differently-named/pathed COPY would
    // run DllMain again and re-hook — the 2nd swap_slot would set g_origSend = &Hooked_Send, causing
    // infinite recursion. If the mutex already exists in this process, another instance hooked first
    // — bail out before touching anything (no CS, no files, no vtable swap).
    char mname[64];
    _snprintf_s(mname, _TRUNCATE, "swhook_installed_%lu", GetCurrentProcessId());
    HANDLE mtx = CreateMutexA(nullptr, FALSE, mname);   // held for the DLL's lifetime; closed on unload
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) { if (mtx) CloseHandle(mtx); return 0; }
    g_instMutex = mtx;

    InitializeCriticalSection(&g_cs);
    g_startMs = GetTickCount64();

    SYSTEMTIME st; GetLocalTime(&st);
    char* stamp = g_stamp;
    _snprintf_s(g_stamp, _TRUNCATE, "%04d%02d%02d_%02d%02d%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char p[MAX_PATH], suffix[MAX_PATH];
    CreateDirectoryA(BasePath(p, MAX_PATH, "captures"), nullptr);

    // Config first: it decides the startup mode (auto-relay / capture on-off / ipc port / log).
    // The .log opens lazily inside logf() once cfg.log is known.
    EnterCriticalSection(&g_cs);
    load_relay_config();
    g_relay     = relay::g_cfg.relay   != 0;      // auto-enable relay if configured
    g_capturing = relay::g_cfg.capture != 0;      // auto-start capture unless disabled
    if (g_capturing) {
        _snprintf_s(suffix, _TRUNCATE, "captures\\session_%s.swcap", stamp);
        g_cap = fopen(BasePath(g_capPath, MAX_PATH, suffix), "wb");
        if (g_cap) fwrite("SWCAP\x02", 1, 6, g_cap); else g_capPath[0] = 0;
    }
    logf("== startup: relay=%s capture=%s ==\n", g_relay ? "ON" : "OFF", g_capturing ? "ON" : "OFF");
    LeaveCriticalSection(&g_cs);

    // Control plane up BEFORE the (possibly long) wait for Steam, so a GUI can already see status.
    int ie = ipc::start(relay::g_cfg.ipcPort, ipc_handle);
    if (ie) logf("== ipc: FAILED to listen on 127.0.0.1:%d (wsa error %d) ==\n", relay::g_cfg.ipcPort, ie);
    else    logf("== ipc: listening on 127.0.0.1:%d ==\n", relay::g_cfg.ipcPort);
    logflush();

    HMODULE api = nullptr;
    for (int i = 0; i < 600 && !api && !g_stopping; ++i) { api = GetModuleHandleA("steam_api64.dll"); if (!api) Sleep(100); }
    if (g_stopping) return 0;
    if (!api) { fail("steam_api64.dll never loaded"); return 0; }

    auto GetHUser = reinterpret_cast<HSteamUser(*)()>(
        GetProcAddress(api, "SteamGameServer_GetHSteamUser"));
    auto FindOrCreate = reinterpret_cast<void*(*)(HSteamUser, const char*)>(
        GetProcAddress(api, "SteamInternal_FindOrCreateGameServerInterface"));
    if (!GetHUser || !FindOrCreate) { fail("missing steam_api exports"); return 0; }

    void* iface = nullptr;
    for (int i = 0; i < 1200 && !iface && !g_stopping; ++i) {
        HSteamUser h = GetHUser();
        if (h) iface = FindOrCreate(h, "SteamNetworkingMessages002");
        if (!iface) Sleep(100);
    }
    if (g_stopping) return 0;
    if (!iface) { fail("no SteamNetworkingMessages002"); return 0; }

    g_vtable = *reinterpret_cast<void***>(iface);
    logf("interface=%p vtable=%p\n", iface, (void*)g_vtable);
    g_origSend = reinterpret_cast<SendMessageToUser_t>(
        swap_slot(kVT_SendMessageToUser, (void*)&Hooked_Send));
    g_origRecv = reinterpret_cast<ReceiveMessagesOnChannel_t>(
        swap_slot(kVT_ReceiveMessagesOnChannel, (void*)&Hooked_Recv));
    if (!g_origSend || !g_origRecv) { fail("vtable swap failed (VirtualProtect)"); return 0; }
    g_hooked = true;
    logf("hooks installed: send orig=%p recv orig=%p\n", (void*)g_origSend, (void*)g_origRecv);
    logflush();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hmod = h;                       // remember our own module for BaseDir()
        DisableThreadLibraryCalls(h);
        g_workerThread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
