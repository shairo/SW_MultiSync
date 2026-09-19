// swctl.exe — command-line controller for swhook.dll. Thin client over ipc_client.h; everything it
// does is one IPC command (ipc-protocol.md), so it doubles as a reference client and a smoke test
// for the protocol. `swctl inject` also carries the injector so a single exe can bring the tool up
// from a script: `swctl inject --wait && swctl relay on`.
#include "../common/ipc_client.h"   // winsock2 must precede windows.h (injector.h)
#include "../inject/injector.h"
#include "../common/version.h"
#include "../hook/relay.h"          // header-only: reads swhook.ini for the default ipcPort
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>

static int  g_port = 28215;        // swhook.ini's ipcPort next to swctl.exe, unless --port says otherwise
static bool g_json = false;

static void usage() {
    printf("swctl %s - swhook control\n"
           "usage: swctl [--port N] [--json] <command> [args]\n"
           "  --port defaults to ipcPort in swhook.ini next to swctl.exe; pass it to reach a\n"
           "  second server injected with a DLL whose ini uses another port\n"
           "  inject [--wait] [--pid N] [dll]\n"
           "                             LoadLibrary swhook.dll into server64.exe (needs admin);\n"
           "                             --pid picks one of several servers (see --port for its DLL)\n"
           "  status                     hook / relay / capture state, walk rate, totals\n"
           "  peers                      per-player table\n"
           "  vehicles                   vehicle table (id, pos, I_src, feeds)\n"
           "  watch [sec]                live status + peers with rates (Ctrl+C to quit)\n"
           "  relay on|off               enable / disable the sync relay\n"
           "  capture start|stop|mark    .swcap capture control\n"
           "  config get [key]           show config (or one key)\n"
           "  config set <key> <value>   change a knob live (see 'config get' for keys)\n"
           "  config reload|save         re-read / write back swhook.ini\n"
           "  version | ping             client and DLL versions / reachability\n"
           "  unload                     restore hooks and unmap the DLL (dev aid; then inject again)\n"
           "  raw <json>                 send a raw request line\n"
           "--json prints the DLL's raw response instead of the table.\n", SWHOOK_VERSION);
}

static ipcc::Client g_c;
static bool connect_or_die() {
    if (g_c.connected()) return true;
    if (!g_c.connect(g_port)) { fprintf(stderr, "error: %s (port %d)\n", g_c.lastError.c_str(), g_port); return false; }
    return true;
}
// Run one command; prints raw JSON in --json mode. Returns the response ("" on transport failure).
static std::string run(const char* cmd, const std::string& extra = "") {
    if (!connect_or_die()) return "";
    std::string r = g_c.cmd(cmd, extra);
    if (r.empty()) { fprintf(stderr, "error: %s\n", g_c.lastError.c_str()); return ""; }
    if (g_json) printf("%s\n", r.c_str());
    return r;
}
static bool ok_of(const std::string& r) {
    bool ok = false; json::get_bool(r.c_str(), "ok", ok);
    if (!ok) { std::string e; json::get_str(r.c_str(), "error", e); if (!g_json) fprintf(stderr, "error: %s\n", e.c_str()); }
    return ok;
}
static double N(const char* o, const char* k) { double v = 0; json::get_num(o, k, v); return v; }
static std::string S(const char* o, const char* k) { std::string v; json::get_str(o, k, v); return v; }
static std::string ID(const char* o, const char* k) { uint64_t v = 0; json::get_u64(o, k, v); return std::to_string(v); }
static std::string fmt_bytes(double b) {
    char s[32];
    if (b >= 1e9) snprintf(s, sizeof s, "%.2f GB", b / 1e9);
    else if (b >= 1e6) snprintf(s, sizeof s, "%.2f MB", b / 1e6);
    else if (b >= 1e3) snprintf(s, sizeof s, "%.1f KB", b / 1e3);
    else snprintf(s, sizeof s, "%.0f B", b);
    return s;
}

static void print_status(const std::string& r) {
    const char* o = r.c_str();
    const char* walk = json::find_value(o, "walk"); const char* inj = json::find_value(o, "inject");
    bool hooked = false, relay = false, cap = false; json::get_bool(o, "hooked", hooked);
    json::get_bool(o, "relay", relay); json::get_bool(o, "capturing", cap);
    std::string herr = S(o, "hookError");
    double t8 = walk ? N(walk, "type8") : 0, full = walk ? N(walk, "full") : 0;
    printf("swhook %s  pid %.0f  uptime %.0fs  hook: %s%s%s\n", S(o, "version").c_str(), N(o, "pid"),
           N(o, "uptimeMs") / 1000, hooked ? "installed" : (herr.empty() ? "pending" : "FAILED"),
           herr.empty() ? "" : " - ", herr.c_str());
    printf("relay: %s   capture: %s %s\n", relay ? "ON" : "OFF", cap ? "ON" : "OFF", S(o, "captureFile").c_str());
    printf("peers: %.0f   vehicles: %.0f   ipc clients: %.0f\n", N(o, "peerCount"), N(o, "vehicleCount"), N(o, "ipcClients"));
    printf("walk: %.0f/%.0f type8 fully decoded (%.2f%%), %.0f dumped\n", full, t8, t8 ? 100.0 * full / t8 : 0, walk ? N(walk, "dumped") : 0);
    if (inj) printf("inject: %.0f sends, %.0f appended, %.0f rewritten, %s extra\n",
                    N(inj, "sends"), N(inj, "appended"), N(inj, "rewritten"), fmt_bytes(N(inj, "bytes")).c_str());
}

// Rates need two samples: (steamId -> previous counters). Used by `watch`.
struct PeerSample { double sendBytes = 0, injBytes = 0, recvBytes = 0; };
static std::map<std::string, PeerSample> g_prev; static double g_prevMs = 0;

static void print_peers(const std::string& r, bool rates) {
    const char* o = r.c_str();
    double now = N(o, "nowMs"); double dt = (g_prevMs && now > g_prevMs) ? (now - g_prevMs) / 1000.0 : 0;
    printf("%-18s %-5s %9s %9s %9s %7s %7s %6s %s\n", "steamId", "conn", rates ? "send/s" : "sent",
           rates ? "extra/s" : "extra", rates ? "recv/s" : "recv", "walk%", "vehs", "type8", "last");
    json::for_each_elem(json::find_value(o, "peers"), [&](const char* p) {
        std::string id = ID(p, "steamId");
        bool conn = false; json::get_bool(p, "connected", conn);
        double sb = N(p, "sendBytes"), ib = N(p, "injBytes"), rb = N(p, "recvBytes");
        double t8 = N(p, "type8"), wf = N(p, "walkFull");
        double vs = sb, vi = ib, vr = rb;
        if (rates) { PeerSample& ps = g_prev[id]; vs = dt ? (sb - ps.sendBytes) / dt : 0; vi = dt ? (ib - ps.injBytes) / dt : 0; vr = dt ? (rb - ps.recvBytes) / dt : 0; ps = {sb, ib, rb}; }
        printf("%-18s %-5s %9s %9s %9s %6.1f%% %7.0f %6.0f %.0fs ago\n", id.c_str(), conn ? "yes" : "no",
               fmt_bytes(vs).c_str(), fmt_bytes(vi).c_str(), fmt_bytes(vr).c_str(),
               t8 ? 100.0 * wf / t8 : 0, N(p, "vehicles"), t8, (now - N(p, "lastSeenMs")) / 1000);
    });
    g_prevMs = now;
}

static void print_vehicles(const std::string& r) {
    const char* o = r.c_str();
    printf("%-6s %-8s %-30s %-6s %s\n", "id", "tick", "pos (x y z)", "I_src", "feeds (steamId:gap)");
    json::for_each_elem(json::find_value(o, "vehicles"), [&](const char* v) {
        double pos[3] = {0,0,0};
        json::arr_nums(json::find_value(v, "pos"), pos, 3);
        char posS[64], srcS[16];
        snprintf(posS, sizeof posS, "%.1f %.1f %.1f", pos[0], pos[1], pos[2]);
        double sg = N(v, "srcGap");
        if (sg > 0) snprintf(srcS, sizeof srcS, "%.0f", sg); else strcpy(srcS, "-");
        printf("%-6.0f %-8.0f %-30s %-6s", N(v, "id"), N(v, "tick"), posS, srcS);
        json::for_each_elem(json::find_value(v, "feeds"), [&](const char* f) {
            printf(" %s:%.0f", ID(f, "steamId").c_str(), N(f, "gap"));
        });
        printf("\n");
    });
}

static void print_config(const std::string& r, const char* onlyKey) {
    const char* cfg = json::find_value(r.c_str(), "config");
    if (!cfg) return;
    json::for_each_elem(json::find_value(r.c_str(), "fields"), [&](const char* f) {
        std::string name = S(f, "name");
        if (onlyKey && name != onlyKey) return;
        bool so = false; json::get_bool(f, "startupOnly", so);
        printf("%-16s = %-8g %s%s\n", name.c_str(), N(cfg, name.c_str()), S(f, "help").c_str(), so ? "  [startup-only]" : "");
    });
}

static int cmd_inject(int argc, char** argv) {
    bool wait = false; std::string dll; DWORD pid = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--wait") || !strcmp(argv[i], "-w")) wait = true;
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = (DWORD)atoi(argv[++i]);
        else dll = argv[i];
    }
    if (dll.empty()) dll = injector::default_dll_path();
    if (!injector::is_admin()) fprintf(stderr, "warning: not running as administrator; OpenProcess will likely fail\n");
    if (!pid) {   // no --pid: the first server64.exe found (several may be running)
        pid = injector::find_pid("server64.exe");
        if (!pid && wait) { printf("waiting for server64.exe..."); fflush(stdout);
            while (!(pid = injector::find_pid("server64.exe"))) Sleep(1000); printf(" pid %lu\n", pid); }
        if (!pid) { fprintf(stderr, "server64.exe is not running\n"); return 1; }
    }
    std::string err;
    injector::Result res = injector::inject(pid, dll.c_str(), err);
    if (res == injector::Injected) printf("injected %s into pid %lu\n", dll.c_str(), pid);
    else if (res == injector::AlreadyLoaded) printf("already injected (pid %lu)\n", pid);
    else { fprintf(stderr, "inject failed: %s\n", err.c_str()); return 1; }
    // Confirm the control plane comes up (the DLL listens before waiting for Steam).
    for (int i = 0; i < 50; i++) { if (g_c.connect(g_port, 200)) { std::string r = g_c.cmd("ping"); if (!r.empty()) { printf("dll %s answering on port %d\n", S(r.c_str(), "version").c_str(), g_port); return 0; } } Sleep(100); }
    fprintf(stderr, "warning: injected but no answer on port %d yet (check captures/*.log)\n", g_port);
    return 0;
}

int main(int argc, char** argv) {
    relay::load_config_file((injector::exe_dir() + "swhook.ini").c_str());   // absent/unreadable: keep 28215
    g_port = relay::g_cfg.ipcPort;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")) g_json = true;
        else break;
    }
    if (i >= argc) { usage(); return 1; }
    std::string cmd = argv[i++];
    int rest = argc - i; char** ra = argv + i;
    auto arg = [&](int k) -> const char* { return k < rest ? ra[k] : ""; };
    std::string r;

    if (cmd == "help" || cmd == "-h" || cmd == "--help") { usage(); return 0; }
    if (cmd == "inject") return cmd_inject(rest, ra);
    if (cmd == "version") {
        printf("swctl %s (protocol %d)\n", SWHOOK_VERSION, SWHOOK_IPC_PROTOCOL);
        if (g_c.connect(g_port, 500)) { r = g_c.cmd("ping"); if (!r.empty()) { if (g_json) printf("%s\n", r.c_str()); else printf("dll   %s (protocol %.0f) pid %.0f\n", S(r.c_str(), "version").c_str(), N(r.c_str(), "protocol"), N(r.c_str(), "pid")); } }
        else printf("dll   not reachable on port %d\n", g_port);
        return 0;
    }
    if (cmd == "unload")   { r = run("unload");       if (r.empty() || !ok_of(r)) return 1; if (!g_json) printf("unload requested; DLL unmaps in ~1 s\n"); return 0; }
    if (cmd == "ping")     { r = run("ping");         if (r.empty() || !ok_of(r)) return 1; if (!g_json) printf("ok dll %s\n", S(r.c_str(), "version").c_str()); return 0; }
    if (cmd == "status")   { r = run("status");       if (r.empty() || !ok_of(r)) return 1; if (!g_json) print_status(r); return 0; }
    if (cmd == "peers")    { r = run("peers.get");    if (r.empty() || !ok_of(r)) return 1; if (!g_json) print_peers(r, false); return 0; }
    if (cmd == "vehicles") { r = run("vehicles.get"); if (r.empty() || !ok_of(r)) return 1; if (!g_json) print_vehicles(r); return 0; }
    if (cmd == "watch") {
        int sec = rest ? atoi(arg(0)) : 1; if (sec < 1) sec = 1;
        for (;;) {
            r = run("stats.get"); if (r.empty()) return 1;
            if (!g_json) { system("cls"); print_status(r); printf("\n"); print_peers(r, true); }
            Sleep(sec * 1000);
        }
    }
    if (cmd == "relay" || cmd == "mutate") {
        std::string v = arg(0); bool on = v == "on" || v == "1" || v == "true";
        if (!on && !(v == "off" || v == "0" || v == "false")) { usage(); return 1; }
        r = run(cmd == "relay" ? "relay.set" : "mutate.set", std::string("\"enabled\":") + (on ? "true" : "false"));
        if (r.empty() || !ok_of(r)) return 1;
        bool rl = false; json::get_bool(r.c_str(), "relay", rl);
        if (!g_json) printf("relay %s\n", rl ? "ON" : "OFF");
        return 0;
    }
    if (cmd == "capture") {
        std::string v = arg(0);
        const char* c = v == "start" ? "capture.start" : v == "stop" ? "capture.stop" : v == "mark" ? "capture.mark" : nullptr;
        if (!c) { usage(); return 1; }
        r = run(c); if (r.empty() || !ok_of(r)) return 1;
        if (!g_json) { if (v == "start") printf("capturing to %s\n", S(r.c_str(), "captureFile").c_str());
                       else if (v == "mark") printf("mark #%.0f\n", N(r.c_str(), "mark")); else printf("capture stopped\n"); }
        return 0;
    }
    if (cmd == "config") {
        std::string v = arg(0);
        if (v == "get" || v.empty()) { r = run("config.get"); if (r.empty() || !ok_of(r)) return 1; if (!g_json) print_config(r, rest > 1 ? arg(1) : nullptr); return 0; }
        if (v == "set") {
            if (rest < 3) { usage(); return 1; }
            std::string key = arg(1); double val = atof(arg(2));
            char extra[256]; snprintf(extra, sizeof extra, "\"key\":\"%s\",\"value\":%g", key.c_str(), val);
            r = run("config.set", extra); if (r.empty() || !ok_of(r)) return 1;
            bool so = false; json::get_bool(r.c_str(), "startupOnly", so);
            if (!g_json) printf("%s = %g%s\n", key.c_str(), N(r.c_str(), "value"), so ? "  (startup-only: takes effect after restart; use 'config save')" : "");
            return 0;
        }
        if (v == "reload") { r = run("config.reload"); if (r.empty() || !ok_of(r)) return 1; if (!g_json) printf("reloaded %.0f keys\n", N(r.c_str(), "keys")); return 0; }
        if (v == "save")   { r = run("config.save");   if (r.empty() || !ok_of(r)) return 1; if (!g_json) printf("saved %s\n", S(r.c_str(), "path").c_str()); return 0; }
        usage(); return 1;
    }
    if (cmd == "raw") {
        if (!rest) { usage(); return 1; }
        if (!connect_or_die()) return 1;
        r = g_c.call(arg(0)); if (r.empty()) { fprintf(stderr, "error: %s\n", g_c.lastError.c_str()); return 1; }
        printf("%s\n", r.c_str()); return 0;
    }
    usage(); return 1;
}
