// swhook.dll — captures ISteamNetworkingMessages traffic in server64.exe.
// PHASE 1: passive capture. Hooks SendMessageToUser (vtable[0]) and ReceiveMessagesOnChannel
// (vtable[1]); always calls the originals unmodified. Every send and every received message is
// written, raw, to one per-session container file (.swcap) plus a human-readable .log summary.
// Reassembly / record decoding is done OFFLINE by the C# tool — the DLL stays dumb on purpose.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include "steam_min.h"
#include "rec.h"
#include "relay.h"

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
static CRITICAL_SECTION g_cs;            // guards g_log + g_cap + counters
static uint64_t g_seq = 0;               // global, ordered across send+recv
static uint64_t g_capBytes = 0;
static uint64_t g_markCount = 0;         // marker ordinal (F8)
static volatile bool g_capturing = true; // gate: F9 start / F10 stop (default on = legacy behavior)
constexpr uint64_t kCapCap = 4ull * 1024 * 1024 * 1024;   // 4 GB safety cap

// Runtime control hotkeys (read globally via GetAsyncKeyState from the headless server process, so
// they fire even while the game client window has focus). Keys are passive-read, not consumed, so
// the game still sees them too — rebind here if they clash.
constexpr int kKeyStart  = VK_F9;    // start a NEW capture file
constexpr int kKeyStop   = VK_F10;   // stop capturing (flush + close current file)
constexpr int kKeyMark   = VK_F8;    // drop a marker into the current file
constexpr int kKeyMutate = VK_F7;    // TOGGLE the stage-B mutation demo (default OFF)
constexpr int kKeyRelay  = VK_F6;    // TOGGLE stage-C relay injection (default OFF)
constexpr int kKeyReload = VK_F5;    // reload swhook.ini (relay tuning knobs) at runtime

// ---- stage B: record-walker self-test + mutation demo ----
// The self-test is PASSIVE: it walks every outgoing type=8 message with the C++ port of the record
// rules and tallies whether it fully closes (understanding gate), plus simulates appending a 0x81
// into a scratch buffer to prove the inject arithmetic re-walks cleanly. Nothing sent is changed.
// The mutation demo (F7, default OFF) copies the send buffer, adds a fixed offset to every 0x81
// body0 position, and sends the copy — a solo, visible confirmation that our 0x81 edits reach the
// client. It only ever fires on a message the self-test fully decoded.
static volatile bool g_mutate = false;                 // F7 gate for the visible edit demo
static volatile bool g_relay  = false;                 // F6 gate for stage-C relay injection
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

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
}

// One readable summary line: dir, seq, peer, size, flags, ch, and (for a flag=0 logical-message
// head) the decoded body fields + first bytes.
static void log_line(const char* dir, uint64_t seq, uint64_t steamid,
                     int32_t ch, int32_t flags, const void* data, uint32_t cub) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    logf("%s [%llu] peer=%llu size=%u flags=%d ch=%d", dir,
         (unsigned long long)seq, (unsigned long long)steamid, cub, flags, ch);
    auto u32 = [&](uint32_t off) -> uint32_t {
        return (off + 4 <= cub) ? *reinterpret_cast<const uint32_t*>(b + off) : 0xFFFFFFFFu; };
    if (cub >= 8) {
        uint32_t frag = u32(0), total = u32(4);
        logf(" | frag=%u total=%u", frag, total);
        if (frag == 0 && cub >= 28)                       // logical-message head
            logf(" msgType=%u tick=%u sub=%u recs=%u",
                 u32(12), u32(16), u32(20), u32(24));
    }
    logf(" : ");
    uint32_t show = cub < 32 ? cub : 32;
    for (uint32_t i = 0; i < show; ++i) logf("%02X ", b[i]);
    if (cub > show) logf("... (+%u)", cub - show);
    logf("\n");
}

static uint64_t peer_id(const SteamNetworkingIdentity* id) {
    return (id && id->m_eType == kIdentityType_SteamID) ? id->m_steamID64 : 0;
}

// Passive self-test on one type=8 message body (const1 at body[0]). Caller holds g_cs. Walks with
// the C++ rules; on a fully-decoded message that contains a 0x81, also simulates appending a copy of
// that 0x81 (recordCount++) into g_scratch and re-walks to prove the inject arithmetic closes. This
// is the safety gate for stage C: mutation is only ever allowed on messages that pass here.
static void selftest(const uint8_t* body, int blen) {
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
        if (g_log) fflush(g_log);
    }
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

// ---- hooks (pass-through) ----
static int32_t Hooked_Send(void* self, const SteamNetworkingIdentity* id,
                           const void* data, uint32_t cub, int32_t flags, int32_t ch) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    bool head = cub >= 28 && *reinterpret_cast<const uint32_t*>(b) == 0;   // frag==0 (single-frame head)
    const uint8_t* body = b + 8; int blen = (int)cub - 8;
    bool type8 = head && blen >= 20 && *reinterpret_cast<const uint32_t*>(body + 4) == 8;

    uint32_t tick = type8 ? *reinterpret_cast<const uint32_t*>(body + 8) : 0;
    EnterCriticalSection(&g_cs);
    uint64_t n = ++g_seq, sid = peer_id(id);
    cap_event(0, n, sid, ch, flags, data, cub);
    log_line("S", n, sid, ch, flags, data, cub);
    bool full = false;
    int newcub = 0; uint8_t* inj = nullptr;
    if (type8) {
        selftest(body, blen);
        full = rec::walk(body, blen).full;
        if (full) {
            relay::observe(sid, body, blen, tick);                   // passive: keep caches warm
            if (g_relay) {                                           // F6: build the injected copy
                int cap = (int)cub + relay::kMaxPerSend * relay::kMaxRecLen;
                inj = static_cast<uint8_t*>(malloc(cap));
                if (inj) {
                    newcub = relay::build_inject(sid, static_cast<const uint8_t*>(data), (int)cub, inj, cap, tick);
                    if ((relay::g_injSends & 0x7F) == 0 && newcub > 0)
                        logf("== relay: sends=%llu appended=%llu rewritten=%llu bytes=%llu ==\n",
                             (unsigned long long)relay::g_injSends, (unsigned long long)relay::g_injRecords,
                             (unsigned long long)relay::g_rewrites, (unsigned long long)relay::g_injBytes);
                }
            }
        }
    }
    LeaveCriticalSection(&g_cs);

    // Stage-C relay (F6, default OFF): send the enlarged COPY with dead-reckoned 0x81s appended.
    if (inj) {
        if (newcub > 0) { int32_t rc = g_origSend(self, id, inj, (uint32_t)newcub, flags, ch); free(inj); return rc; }
        free(inj);
    }
    // Stage-B visible demo (F7, default OFF): send an edited COPY, never the caller's buffer, and
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
    int32_t got = g_origRecv(self, ch, ppOut, nMax);   // fills ppOut with already-decrypted msgs
    if (got > 0 && ppOut) {
        EnterCriticalSection(&g_cs);
        for (int32_t i = 0; i < got; ++i) {
            SteamNetworkingMessage_t* m = ppOut[i];
            if (!m) continue;
            uint64_t n = ++g_seq, sid = peer_id(&m->m_identityPeer);
            cap_event(1, n, sid, m->m_nChannel, m->m_nFlags, m->m_pData, (uint32_t)m->m_cbSize);
            log_line("R", n, sid, m->m_nChannel, m->m_nFlags, m->m_pData, (uint32_t)m->m_cbSize);
        }
        LeaveCriticalSection(&g_cs);
    }
    return got;
}

// ---- runtime capture control (F9 start / F10 stop / F8 marker) ----
// Roll a fresh .swcap file. Caller must hold g_cs. Filename carries ms so rapid F9 presses differ.
static void open_cap() {
    if (g_cap) { fflush(g_cap); fclose(g_cap); g_cap = nullptr; }
    SYSTEMTIME st; GetLocalTime(&st);
    char suffix[MAX_PATH], p[MAX_PATH];
    _snprintf_s(suffix, _TRUNCATE, "captures\\session_%04d%02d%02d_%02d%02d%02d_%03d.swcap",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    BasePath(p, MAX_PATH, suffix);
    g_cap = fopen(p, "wb");
    g_capBytes = 0;
    if (g_cap) fwrite("SWCAP\x02", 1, 6, g_cap);
    logf("== capture file: %s ==\n", p); if (g_log) fflush(g_log);
}
static void close_cap() {                 // caller must hold g_cs
    if (g_cap) { fflush(g_cap); fclose(g_cap); g_cap = nullptr; }
}

// Marker event: same container layout, dir=2. steamId field carries the marker ordinal; no payload.
// Lets you annotate "I did action X now" while playing; `swcap marks` lists them on the t+ms clock.
static void write_marker() {
    EnterCriticalSection(&g_cs);
    if (g_capturing && g_cap) {
        uint8_t dir = 2; uint64_t seq = ++g_seq, t = GetTickCount64(), mid = ++g_markCount;
        int32_t ch = 0, flags = 0; uint32_t cub = 0, stored = 0;
        fwrite(&dir, 1, 1, g_cap); fwrite(&seq, 8, 1, g_cap); fwrite(&t, 8, 1, g_cap);
        fwrite(&mid, 8, 1, g_cap); fwrite(&ch, 4, 1, g_cap); fwrite(&flags, 4, 1, g_cap);
        fwrite(&cub, 4, 1, g_cap); fwrite(&stored, 4, 1, g_cap);
        fflush(g_cap);
        logf("== MARK #%llu (seq %llu) ==\n", (unsigned long long)mid, (unsigned long long)seq);
        if (g_log) fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
}

// Load swhook.ini (relay tuning knobs) from the project dir. Missing file = keep defaults (silent
// on first load beyond a note). Logs the effective values so a live capture records the config used.
static void load_relay_config() {
    char ini[MAX_PATH];
    int n = relay::load_config_file(BasePath(ini, MAX_PATH, "swhook.ini"));
    if (n < 0) logf("== config: no swhook.ini, using defaults ==\n");
    else       logf("== config: swhook.ini loaded (%d keys) ==\n", n);
    const relay::Cfg& c = relay::g_cfg;
    logf("   intervalMin=%d intervalMax=%d lodDenser=%d lagRatio=%.3f lagMin=%d relayMinGap=%d "
         "stale=%d gapOutlier=%.0f maxPerSend=%d budgetWindow=%d capKbpsPerPeer=%d\n",
         c.intervalMin, c.intervalMax, c.lodDenser, c.lagRatio, c.lagMin, c.relayMinGap, c.stale,
         c.gapOutlier, c.maxPerSend, c.budgetWindow, c.capKbpsPerPeer);
    if (g_log) fflush(g_log);
}

static bool key_edge(int vk, bool& prev) {   // true only on the press-down transition
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fired = down && !prev; prev = down; return fired;
}

static DWORD WINAPI ControlThread(LPVOID) {
    bool pStart = false, pStop = false, pMark = false, pMutate = false, pRelay = false, pReload = false;
    for (;;) {
        if (key_edge(kKeyReload, pReload)) {
            EnterCriticalSection(&g_cs); load_relay_config(); LeaveCriticalSection(&g_cs);
        }
        if (key_edge(kKeyStart, pStart)) {
            EnterCriticalSection(&g_cs); open_cap(); g_capturing = true; LeaveCriticalSection(&g_cs);
        }
        if (key_edge(kKeyStop, pStop)) {
            EnterCriticalSection(&g_cs); g_capturing = false; close_cap(); LeaveCriticalSection(&g_cs);
        }
        if (key_edge(kKeyMark, pMark)) write_marker();
        if (key_edge(kKeyMutate, pMutate)) {
            g_mutate = !g_mutate;
            EnterCriticalSection(&g_cs);
            logf("== MUTATE demo %s (F7) ==\n", g_mutate ? "ON" : "OFF");
            if (g_log) fflush(g_log);
            LeaveCriticalSection(&g_cs);
        }
        if (key_edge(kKeyRelay, pRelay)) {
            g_relay = !g_relay;
            EnterCriticalSection(&g_cs);
            logf("== RELAY inject %s (F6) ==\n", g_relay ? "ON" : "OFF");
            if (g_log) fflush(g_log);
            LeaveCriticalSection(&g_cs);
        }
        Sleep(40);
    }
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

static DWORD WINAPI Worker(LPVOID) {
    // Double-injection guard: a per-process named mutex. Re-injecting the SAME dll is already a
    // LoadLibrary no-op (DllMain not re-run), but injecting a differently-named/pathed COPY would
    // run DllMain again and re-hook — the 2nd swap_slot would set g_origSend = &Hooked_Send, causing
    // infinite recursion. If the mutex already exists in this process, another instance hooked first
    // — bail out before touching anything (no CS, no files, no vtable swap).
    char mname[64];
    _snprintf_s(mname, _TRUNCATE, "swhook_installed_%lu", GetCurrentProcessId());
    HANDLE mtx = CreateMutexA(nullptr, FALSE, mname);   // leaked on purpose (lives for the process)
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    InitializeCriticalSection(&g_cs);

    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32];
    _snprintf_s(stamp, _TRUNCATE, "%04d%02d%02d_%02d%02d%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char p[MAX_PATH], suffix[MAX_PATH];
    CreateDirectoryA(BasePath(p, MAX_PATH, "captures"), nullptr);
    _snprintf_s(suffix, _TRUNCATE, "captures\\session_%s.log", stamp);
    g_log = fopen(BasePath(p, MAX_PATH, suffix), "w");
    logf("swhook session %s (pid %lu)\n", stamp, GetCurrentProcessId());

    // Config first: it decides the startup mode (auto-relay / capture on-off / hotkeys).
    load_relay_config();
    g_relay     = relay::g_cfg.relay   != 0;      // auto-enable relay if configured (no F6 needed)
    g_capturing = relay::g_cfg.capture != 0;      // auto-start capture unless disabled
    if (g_capturing) {
        _snprintf_s(suffix, _TRUNCATE, "captures\\session_%s.swcap", stamp);
        g_cap = fopen(BasePath(p, MAX_PATH, suffix), "wb");
        if (g_cap) fwrite("SWCAP\x02", 1, 6, g_cap);
    }
    logf("== startup: relay=%s capture=%s hotkeys=%s ==\n",
         g_relay ? "ON" : "OFF", g_capturing ? "ON" : "OFF",
         relay::g_cfg.hotkeys ? "ON" : "OFF");

    HMODULE api = nullptr;
    for (int i = 0; i < 600 && !api; ++i) { api = GetModuleHandleA("steam_api64.dll"); if (!api) Sleep(100); }
    if (!api) { logf("steam_api64.dll never loaded\n"); fflush(g_log); return 0; }

    auto GetHUser = reinterpret_cast<HSteamUser(*)()>(
        GetProcAddress(api, "SteamGameServer_GetHSteamUser"));
    auto FindOrCreate = reinterpret_cast<void*(*)(HSteamUser, const char*)>(
        GetProcAddress(api, "SteamInternal_FindOrCreateGameServerInterface"));
    if (!GetHUser || !FindOrCreate) { logf("missing steam_api exports\n"); fflush(g_log); return 0; }

    void* iface = nullptr;
    for (int i = 0; i < 1200 && !iface; ++i) {
        HSteamUser h = GetHUser();
        if (h) iface = FindOrCreate(h, "SteamNetworkingMessages002");
        if (!iface) Sleep(100);
    }
    if (!iface) { logf("no SteamNetworkingMessages002\n"); fflush(g_log); return 0; }

    g_vtable = *reinterpret_cast<void***>(iface);
    logf("interface=%p vtable=%p\n", iface, (void*)g_vtable);
    g_origSend = reinterpret_cast<SendMessageToUser_t>(
        swap_slot(kVT_SendMessageToUser, (void*)&Hooked_Send));
    g_origRecv = reinterpret_cast<ReceiveMessagesOnChannel_t>(
        swap_slot(kVT_ReceiveMessagesOnChannel, (void*)&Hooked_Recv));
    logf("hooks installed: send orig=%p recv orig=%p\n", (void*)g_origSend, (void*)g_origRecv);
    if (relay::g_cfg.hotkeys) {
        logf("hotkeys: F9=start(new file)  F10=stop  F8=marker  F7=mutate-demo  F6=relay  F5=reload-config\n");
        CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
    } else {
        logf("hotkeys DISABLED (hands-off mode) — no key input; behaviour fixed by swhook.ini\n");
    }
    fflush(g_log);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hmod = h;                       // remember our own module for BaseDir()
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
