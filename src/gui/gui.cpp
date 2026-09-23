// SWMultiSync.exe — Win32 GUI front-end for swhook.dll. Zero dependencies (user32/comctl32 only).
// Responsibilities: watch for server64.exe, inject swhook.dll (auto by default), then act as an
// IPC client (ipc_client.h) polling the DLL once a second. Every action is one IPC command from
// ipc-protocol.md — the GUI has no private path into the DLL.
//
// Layout: a fixed, DPI-scaled window with three tabs — Status (sync toggle, traffic, players,
// vehicles), Settings (every config knob, editable, save/reload ini), Log/Capture.
// Requires administrator (manifest below) because injecting into server64 (OpenProcess +
// CreateRemoteThread) needs it.
#include "../common/ipc_client.h"   // winsock2 before windows.h
#include "../inject/injector.h"
#include "../common/version.h"
#include "../hook/relay.h"      // header-only: Cfg table + ini load/save, used for the startup tab
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------- state
static HINSTANCE g_hi;
static HWND g_wnd;
static HFONT g_font, g_fontBig;
static double g_s = 1.0;                 // DPI scale
static int  S(int v) { return (int)(v * g_s + 0.5); }
static ipcc::Client g_c;
static int  g_port = 28215;
static bool g_autoInject = true;
// Auto-inject fires at most once per server process: after we (or anyone) got the DLL into a pid,
// its disappearance means a deliberate `swctl unload` (dev hot-swap) and must not be undone by
// the next poll — that would re-inject the stale DLL and lock the file against the rebuild.
static DWORD g_injectedPid = 0;
static std::string g_iniPath;
static std::string g_dllPath;
static std::wstring g_lastErr;

// last polled snapshot (raw JSON kept; parsed on render)
static std::string g_status, g_peersJson, g_vehJson, g_cfgJson;
static bool g_dllUp = false, g_hooked = false, g_relay = false, g_capturing = false;
static bool g_showVeh = false;      // vehicle table is a debug aid (can be hundreds of rows): off unless toggled
static std::wstring g_lastCap;      // most recent capture file path: stays shown after stop so it can be copied
static std::string g_dllVersion;
// rate computation: previous cumulative counters
struct Prev { double sendBytes = 0, injBytes = 0, recvBytes = 0; };
static std::map<std::string, Prev> g_prevPeer; static Prev g_prevTotal; static double g_prevMs = 0;
static double g_rateSend = 0, g_rateInj = 0;   // totals across peers, bytes/s
// The DLL reports connected=false after 5 s of silence and evicts peers after 10 min (stats.h);
// the GUI additionally hides rows silent for over 2 minutes to keep the table short.
static const double kPeerDropSec = 120;
// SteamID -> in-game name (peers[].name, known only for players who joined after injection); rebuilt
// on every peers.get and used wherever a SteamID is shown.
static std::map<uint64_t, std::wstring> g_names;
static std::wstring name_of(uint64_t id) { auto it = g_names.find(id); return it == g_names.end() ? L"-" : it->second; }

// ---------------------------------------------------------------- controls
enum {
    ID_TAB = 100, ID_TIMER = 1,
    // header
    ID_SERVER_TXT, ID_DLL_TXT, ID_INJECT_BTN, ID_AUTO_CHK, ID_VERSION_TXT,
    // status tab
    ID_RELAY_BTN, ID_RELAY_NOTE, ID_TRAFFIC_TXT, ID_WALK_TXT, ID_PEERS_LV, ID_VEH_LV, ID_PEERS_LBL, ID_VEH_LBL, ID_VEH_CHK,
    // settings tab
    ID_CFG_LV, ID_CFG_NAME, ID_CFG_EDIT, ID_CFG_APPLY, ID_CFG_HELP, ID_CFG_SAVE, ID_CFG_RELOAD, ID_CFG_NOTE,
    // startup tab (edits swhook.ini directly; no DLL needed)
    ID_ST_NOTE, ID_ST_DLL, ID_ST_PORT_LBL, ID_ST_PORT, ID_ST_RELAY, ID_ST_SAVE, ID_ST_INI,
    // log tab
    ID_LOGPKT_CHK, ID_DUMPWF_CHK, ID_CAP_AUTO, ID_DUMPMAX_LBL, ID_DUMPMAX, ID_DUMPMAX_APPLY, ID_CAP_TXT, ID_CAP_START, ID_CAP_STOP, ID_CAP_MARK, ID_CAP_COPY, ID_OPEN_DIR, ID_LOG_TXT, ID_LOG_NOTE,
    // startup tab: developer switch; debug tab (shown only with debugUi=1 in swhook.ini)
    ID_ST_DEBUG, ID_DBG_NOTE, ID_DBG_LV, ID_DBG_LBL, ID_DBG_HOLD_LBL, ID_DBG_HOLD_MS, ID_DBG_HOLD, ID_DBG_RELEASE, ID_DBG_NUDGE_LBL, ID_DBG_NUDGE_TP, ID_DBG_NUDGE_TILE, ID_DBG_NUDGE_UNLOAD, ID_DBG_NUDGE_RELOAD, ID_DBG_DLL_UNLOAD, ID_DBG_RESULT, ID_DBG_HELP,
};
static const int kTabs = 5;                  // the last page is the debug tab, present in the tab control only when g_debugUi
static const int kDebugTab = 4;
static bool g_debugUi = false;
static std::vector<HWND> g_tabCtl[kTabs];    // controls per tab page
static HWND g_tab;
static HWND H(int id) { return GetDlgItem(g_wnd, id); }

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int tab = -1, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, g_hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (tab >= 0) g_tabCtl[tab].push_back(c);
    return c;
}
static void set(int id, const std::wstring& t) {
    wchar_t cur[1024]; GetWindowTextW(H(id), cur, 1024);
    if (t != cur) SetWindowTextW(H(id), t.c_str());     // avoid flicker on unchanged text
}
static std::wstring W(const std::string& s) {          // UTF-8 -> UTF-16
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::wstring fmtw(const wchar_t* f, ...) {
    wchar_t b[1024]; va_list ap; va_start(ap, f); _vsnwprintf_s(b, 1024, _TRUNCATE, f, ap); va_end(ap); return b;
}
static std::wstring bytesw(double b) {
    if (b >= 1e6) return fmtw(L"%.2f MB", b / 1e6);
    if (b >= 1e3) return fmtw(L"%.1f KB", b / 1e3);
    return fmtw(L"%.0f B", b);
}
static double N(const char* o, const char* k) { double v = 0; json::get_num(o, k, v); return v; }
static bool   B(const char* o, const char* k) { bool v = false; json::get_bool(o, k, v); return v; }
static std::string Sx(const char* o, const char* k) { std::string v; json::get_str(o, k, v); return v; }

// ListView helpers (explicit W messages: the commctrl macros follow UNICODE, which we don't define
// because injector.h uses the ANSI toolhelp API)
static void lv_get(HWND lv, int r, int c, wchar_t* buf, int n) {
    LVITEMW it{}; it.iSubItem = c; it.pszText = buf; it.cchTextMax = n; buf[0] = 0;
    SendMessageW(lv, LVM_GETITEMTEXTW, r, (LPARAM)&it);
}
static void lv_set(HWND lv, int r, int c, const wchar_t* t) {
    LVITEMW it{}; it.iSubItem = c; it.pszText = (LPWSTR)t;
    SendMessageW(lv, LVM_SETITEMTEXTW, r, (LPARAM)&it);
}
static void lv_cols(HWND lv, const std::vector<std::pair<const wchar_t*, int>>& cols) {
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    int i = 0;
    for (auto& c : cols) { LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH; col.pszText = (LPWSTR)c.first; col.cx = S(c.second); SendMessageW(lv, LVM_INSERTCOLUMNW, i++, (LPARAM)&col); }
}
// Rewrite all rows in place (keeps selection/scroll stable): grow/shrink to n rows, set each cell.
static void lv_fill(HWND lv, const std::vector<std::vector<std::wstring>>& rows) {
    int have = ListView_GetItemCount(lv);
    while (have > (int)rows.size()) ListView_DeleteItem(lv, --have);
    for (int r = 0; r < (int)rows.size(); r++) {
        if (r >= have) { LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = r; it.pszText = (LPWSTR)rows[r][0].c_str(); SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it); have++; }
        for (int c = 0; c < (int)rows[r].size(); c++) {
            wchar_t cur[512]; lv_get(lv, r, c, cur, 512);
            if (rows[r][c] != cur) lv_set(lv, r, c, rows[r][c].c_str());
        }
    }
}

// ---------------------------------------------------------------- IPC
static std::string ipc(const char* cmd, const std::string& extra = "") {
    if (!g_c.connected() && !g_c.connect(g_port, 300)) return "";
    std::string r = g_c.cmd(cmd, extra);
    return r;
}
static bool ipc_ok(const std::string& r) { return !r.empty() && B(r.c_str(), "ok"); }

// Settings + log tab data source. The DLL reads swhook.ini at injection, so while it is not
// running the ini IS the pending configuration: we show it and write edits straight back to it.
// Once the DLL answers, the same rows come from config.get and edits go live via config.set
// (persisted only by the explicit "iniに保存"). relay::g_cfg holds whichever snapshot is current;
// its field table gives names/help for both paths.
static bool is_log_key(const std::string& n) { return n == "log" || n == "dumpWalkFail" || n == "dumpWalkFailMax"; }

static void refresh_config() {
    if (g_dllUp) {
        std::string r = ipc("config.get");
        if (!ipc_ok(r)) return;
        g_cfgJson = r;
        const char* cfg = json::find_value(r.c_str(), "config");
        for (const relay::CfgField& f : relay::kCfgFields) { double v; if (json::get_num(cfg, f.name, v)) relay::set_cfg(f.name, v); }
    } else {
        relay::g_cfg = relay::Cfg();
        relay::load_config_file(g_iniPath.c_str());
    }
    std::vector<std::vector<std::wstring>> rows;
    for (const relay::CfgField& f : relay::kCfgFields) {
        if (f.startupOnly || is_log_key(f.name)) continue;      // startup tab / log tab
        char v[64]; relay::fmt_cfg(f, v, sizeof v);
        rows.push_back({ W(f.name), W(v), W(f.help) });
    }
    lv_fill(H(ID_CFG_LV), rows);
    CheckDlgButton(g_wnd, ID_LOGPKT_CHK, relay::g_cfg.log ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_DUMPWF_CHK, relay::g_cfg.dumpWalkFail ? BST_CHECKED : BST_UNCHECKED);
    if (GetFocus() != H(ID_DUMPMAX)) SetWindowTextW(H(ID_DUMPMAX), fmtw(L"%d", relay::g_cfg.dumpWalkFailMax).c_str());
    set(ID_CFG_NOTE, g_dllUp
        ? L"稼働中: 項目をクリック → 値を編集 → 「適用」で即反映。「iniに保存」で次回以降にも引き継がれます。"
        : L"未注入: swhook.ini の内容を表示中。「適用」で ini に書き込み、注入時にそのまま反映されます。");
    set(ID_LOG_NOTE, g_dllUp
        ? L"稼働中: 変更は即反映。「設定」タブの「iniに保存」で次回以降にも残ります。不具合報告のときは captures フォルダ内の .log（と必要なら .swcap）を送ってください。"
        : L"未注入: swhook.ini の内容を表示中。変更は ini に書き込まれ、注入時に反映されます。");
    EnableWindow(H(ID_CFG_SAVE), g_dllUp); EnableWindow(H(ID_CFG_RELOAD), g_dllUp);
}

static void cfg_set(const std::string& key, double val) {
    if (g_dllUp) {
        char extra[256]; snprintf(extra, sizeof extra, "\"key\":\"%s\",\"value\":%g", key.c_str(), val);
        std::string r = ipc("config.set", extra);
        if (!ipc_ok(r)) { MessageBoxW(g_wnd, (L"設定に失敗しました: " + W(Sx(r.c_str(), "error"))).c_str(), L"SWMultiSync", MB_ICONWARNING); return; }
    } else {
        relay::g_cfg = relay::Cfg();
        relay::load_config_file(g_iniPath.c_str());     // re-read: never clobber concurrent edits
        if (!relay::set_cfg(key.c_str(), val)) { MessageBoxW(g_wnd, L"不明な設定項目です。", L"SWMultiSync", MB_ICONWARNING); return; }
        if (!relay::save_config_file(g_iniPath.c_str())) { MessageBoxW(g_wnd, L"swhook.ini の保存に失敗しました。", L"SWMultiSync", MB_ICONWARNING); return; }
    }
    refresh_config();
}

// ---------------------------------------------------------------- startup tab (ini-backed)
// The GUI reads swhook.ini itself for the startup-only keys: they matter before the DLL exists
// (ipcPort tells us where to connect) and must be editable while the server is down.
// Write one startup-only key straight to swhook.ini (these never go through the DLL: it reads them
// once at injection). Re-reads the file first so nothing saved by another tool is clobbered.
static void set_debug_ui(bool on);
static void ini_set(const char* key, double val) {
    relay::g_cfg = relay::Cfg();
    relay::load_config_file(g_iniPath.c_str());
    relay::set_cfg(key, val);
    if (!relay::save_config_file(g_iniPath.c_str()))
        MessageBoxW(g_wnd, L"swhook.ini の保存に失敗しました。", L"SWMultiSync", MB_ICONWARNING);
}
static void startup_load() {
    relay::g_cfg = relay::Cfg();
    relay::load_config_file(g_iniPath.c_str());
    if (!g_dllUp) g_port = relay::g_cfg.ipcPort;   // while connected, keep talking to the port the DLL actually uses
    g_autoInject = relay::g_cfg.autoInject != 0;
    CheckDlgButton(g_wnd, ID_AUTO_CHK, g_autoInject ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_ST_DEBUG, relay::g_cfg.debugUi ? BST_CHECKED : BST_UNCHECKED);
    set_debug_ui(relay::g_cfg.debugUi != 0);
    CheckDlgButton(g_wnd, ID_ST_RELAY, relay::g_cfg.relay ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_CAP_AUTO, relay::g_cfg.capture ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(H(ID_ST_PORT), fmtw(L"%d", g_port).c_str());
    set(ID_ST_DLL, L"DLL: " + W(g_dllPath));
    set(ID_ST_INI, L"ini: " + W(g_iniPath));
}
static void startup_save() {
    // Re-read the file first so live values other tools saved meanwhile are not clobbered.
    relay::g_cfg = relay::Cfg();
    relay::load_config_file(g_iniPath.c_str());
    wchar_t pw[16]; GetWindowTextW(H(ID_ST_PORT), pw, 16);
    relay::set_cfg("ipcPort", _wtoi(pw));
    relay::set_cfg("relay",      IsDlgButtonChecked(g_wnd, ID_ST_RELAY) == BST_CHECKED ? 1 : 0);
    bool ok = relay::save_config_file(g_iniPath.c_str());
    if (!ok) { MessageBoxW(g_wnd, L"swhook.ini の保存に失敗しました。", L"SWMultiSync", MB_ICONWARNING); return; }
    bool portChanged = relay::g_cfg.ipcPort != g_port;
    startup_load();
    refresh_config();
    MessageBoxW(g_wnd, portChanged && g_dllUp
        ? L"保存しました。ポート変更はサーバーを再起動して注入し直すと有効になります（それまでは現在のポートで接続を続けます）。"
        : L"保存しました。次に注入したときから有効になります。", L"SWMultiSync", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- rendering
static void render_header() {
    DWORD pid = injector::find_pid("server64.exe");
    set(ID_SERVER_TXT, pid ? fmtw(L"server64.exe: 稼働中 (pid %lu)", pid) : L"server64.exe: 未起動");
    std::wstring d;
    if (!pid) d = L"swhook: 待機中（サーバー起動待ち）";
    else if (!g_dllUp) d = injector::has_module(pid, "swhook.dll") ? L"swhook: 注入済み・IPC応答待ち…" : L"swhook: 未注入";
    else {
        std::string herr = Sx(g_status.c_str(), "hookError");
        d = g_hooked ? L"swhook: 動作中 (v" + W(g_dllVersion) + L")"
          : herr.empty() ? L"swhook: 注入済み・フック待ち (Steam初期化待ち)" : L"swhook: フック失敗 - " + W(herr);
    }
    set(ID_DLL_TXT, d);
    EnableWindow(H(ID_INJECT_BTN), pid && !g_dllUp);
    set(ID_VERSION_TXT, fmtw(L"GUI v%hs  /  DLL %hs", SWHOOK_VERSION, g_dllUp ? ("v" + g_dllVersion).c_str() : "-"));
}

static void render_status() {
    const char* o = g_status.c_str();
    bool up = g_dllUp;
    EnableWindow(H(ID_RELAY_BTN), up && g_hooked);
    set(ID_RELAY_BTN, !up ? L"同期改善: —" : g_relay ? L"同期改善: ON（クリックで停止）" : L"同期改善: OFF（クリックで開始）");
    if (!up) { set(ID_TRAFFIC_TXT, L"通信量: —"); set(ID_WALK_TXT, L"解析率: —"); return; }
    const char* walk = json::find_value(o, "walk"); const char* inj = json::find_value(o, "inject");
    double t8 = walk ? N(walk, "type8") : 0, full = walk ? N(walk, "full") : 0;
    set(ID_TRAFFIC_TXT, fmtw(L"通信量: ゲーム本来 %s/s  +  同期追加 %s/s   （追加合計 %s, 追記 %.0f / 書換 %.0f レコード）",
        bytesw(g_rateSend).c_str(), bytesw(g_rateInj).c_str(), bytesw(inj ? N(inj, "bytes") : 0).c_str(),
        inj ? N(inj, "appended") : 0, inj ? N(inj, "rewritten") : 0));
    set(ID_WALK_TXT, fmtw(L"解析率: %.2f%%  (%.0f / %.0f パケット解析成功、失敗 %.0f、ダンプ %.0f)   プレイヤー %.0f   車両 %.0f",
        t8 ? 100.0 * full / t8 : 0, full, t8, t8 - full, walk ? N(walk, "dumped") : 0, N(o, "peerCount"), N(o, "vehicleCount")));
}

// Freeze detector verdict for one peers[] entry (freeze{} object, see ipc-protocol.md). Short form for
// the status table, long form (with the unacked vehicle ids) for the debug tab.
static std::wstring freeze_text(const char* p, double now, bool detail = false) {
    const char* fz = json::find_value(p, "freeze");
    if (!fz) return L"-";
    double since = N(fz, "frozenSinceMs"), ev = N(fz, "events");
    std::wstring t;
    if (since > 0) t = fmtw(L"凍結? %hs %.0f秒", Sx(fz, "reason").c_str(), (now - since) / 1000);
    else if (ev > 0) t = fmtw(L"正常（過去 %.0f 回）", ev);
    else t = L"正常";
    if (!detail) return t;
    std::wstring ids;
    json::for_each_elem(json::find_value(fz, "pendingDefs"), [&](const char* e) { if (!ids.empty()) ids += L","; ids += fmtw(L"%.0f", atof(e)); });
    return t + fmtw(L"  静止 %.1f秒  未ack %.1f秒  車両 %.0f  要求/ack %.0f/%.0f", N(fz, "poseStaticMs") / 1000, N(fz, "unackedMs") / 1000,
                    N(fz, "poseVeh"), N(fz, "defReqs"), N(fz, "defAcks")) + (ids.empty() ? L"" : L"  未ack定義 [" + ids + L"]");
}

static void render_debug() {
    if (!g_debugUi) return;
    const char* o = g_peersJson.c_str();
    double now = N(o, "nowMs");
    std::vector<std::vector<std::wstring>> rows;
    json::for_each_elem(json::find_value(o, "peers"), [&](const char* p) {
        uint64_t id = 0; json::get_u64(p, "steamId", id);
        double ago = (now - N(p, "lastSeenMs")) / 1000;
        if (ago > kPeerDropSec) return;
        double pos[3] = {0,0,0}; bool hasPos = json::arr_nums(json::find_value(p, "pos"), pos, 3) == 3;
        rows.push_back({ W(std::to_string(id)), name_of(id), hasPos ? fmtw(L"%.0f, %.0f, %.0f", pos[0], pos[1], pos[2]) : L"-", freeze_text(p, now, true) });
    });
    lv_fill(H(ID_DBG_LV), rows);
    bool sel = ListView_GetNextItem(H(ID_DBG_LV), -1, LVNI_SELECTED) >= 0;
    for (int id : { ID_DBG_HOLD, ID_DBG_RELEASE, ID_DBG_NUDGE_TP, ID_DBG_NUDGE_TILE, ID_DBG_NUDGE_UNLOAD, ID_DBG_NUDGE_RELOAD }) EnableWindow(H(id), g_dllUp && g_hooked && sel);
    EnableWindow(H(ID_DBG_DLL_UNLOAD), g_dllUp);
    set(ID_DBG_LBL, g_dllUp ? fmtw(L"プレイヤー (%d) — 操作対象を選択  ／  凍結検知 合計 %.0f 件", (int)rows.size(), N(g_status.c_str(), "freezeEvents")) : L"プレイヤー — DLL 未接続");
}
// The selected debug-tab peer as a JSON fragment for debug.* requests ("" when nothing is selected).
static std::string debug_peer() {
    int i = ListView_GetNextItem(H(ID_DBG_LV), -1, LVNI_SELECTED);
    if (i < 0) return "";
    wchar_t id[32]; lv_get(H(ID_DBG_LV), i, 0, id, 32);
    char a[32]; WideCharToMultiByte(CP_UTF8, 0, id, -1, a, 32, nullptr, nullptr);
    return std::string("\"peer\":") + a;
}
static void debug_cmd(const char* cmd, const std::string& extra) {
    std::string peer = debug_peer();
    if (peer.empty()) { set(ID_DBG_RESULT, L"プレイヤーを選択してください"); return; }
    std::string r = ipc(cmd, peer + extra);
    set(ID_DBG_RESULT, W(std::string(cmd) + " -> " + (r.empty() ? "(no reply)" : r)));
}

static void render_peers() {
    const char* o = g_peersJson.c_str();
    double now = N(o, "nowMs"); double dt = (g_prevMs && now > g_prevMs) ? (now - g_prevMs) / 1000.0 : 0;
    std::vector<std::vector<std::wstring>> rows;
    double totS = 0, totI = 0;
    g_names.clear();
    json::for_each_elem(json::find_value(o, "peers"), [&](const char* p) {
        uint64_t id = 0; json::get_u64(p, "steamId", id);
        std::string ids = std::to_string(id), nm = Sx(p, "name");
        if (!nm.empty()) g_names[id] = W(nm);
        double sb = N(p, "sendBytes"), ib = N(p, "injBytes"), rb = N(p, "recvBytes"), t8 = N(p, "type8"), wf = N(p, "walkFull");
        Prev& pv = g_prevPeer[ids];
        double vs = dt ? (sb - pv.sendBytes) / dt : 0, vi = dt ? (ib - pv.injBytes) / dt : 0, vr = dt ? (rb - pv.recvBytes) / dt : 0;
        pv = { sb, ib, rb }; totS += sb; totI += ib;
        double ago = (now - N(p, "lastSeenMs")) / 1000;
        if (ago > kPeerDropSec) return;                      // long gone: hide (the DLL keeps the counters)
        bool conn = B(p, "connected");
        double t3 = N(p, "type3"), rwf = N(p, "rwalkFull"), pos[3] = {0,0,0};
        bool hasPos = json::arr_nums(json::find_value(p, "pos"), pos, 3) == 3;
        // client sync report (1 Hz heartbeat): "+N f" = frames behind as the client reports it (what the
        // in-game player list shows), then server tick - client tick; "-" once the heartbeat is stale
        const char* sy = json::find_value(p, "sync");
        std::wstring lag = L"-", tps = L"-";
        if (sy && now - N(sy, "hbMs") < 3000) {
            lag = N(sy, "clientTick") ? fmtw(L"+%.0ff (%.0f)", N(sy, "framesBehind"), N(sy, "tickLag")) : L"読込中";
            tps = fmtw(L"%.0f", N(sy, "clientTps"));
        }
        rows.push_back({ name_of(id), W(ids), conn ? L"接続中" : L"切断", lag, tps, bytesw(vs) + L"/s", bytesw(vi) + L"/s", bytesw(vr) + L"/s",
                         fmtw(L"%.1f%%", t8 ? 100.0 * wf / t8 : 0), fmtw(L"%.1f%%", t3 ? 100.0 * rwf / t3 : 0),
                         hasPos ? fmtw(L"%.0f, %.0f, %.0f", pos[0], pos[1], pos[2]) : L"-",
                         fmtw(L"%.0f", N(p, "vehicles")), fmtw(L"%.0f 秒前", ago), freeze_text(p, now) });
    });
    if (dt) { g_rateSend = (totS - g_prevTotal.sendBytes) / dt; g_rateInj = (totI - g_prevTotal.injBytes) / dt; }
    g_prevTotal = { totS, totI, 0 }; g_prevMs = now;
    lv_fill(H(ID_PEERS_LV), rows);
    set(ID_PEERS_LBL, fmtw(L"参加プレイヤー (%d)", (int)rows.size()));
}

static void render_vehicles() {
    if (!g_showVeh) { lv_fill(H(ID_VEH_LV), {}); set(ID_VEH_LBL, L"車両一覧は非表示です（デバッグ用）"); return; }
    const char* o = g_vehJson.c_str();
    std::vector<std::vector<std::wstring>> rows;
    json::for_each_elem(json::find_value(o, "vehicles"), [&](const char* v) {
        double pos[3] = {0,0,0};
        json::arr_nums(json::find_value(v, "pos"), pos, 3);
        double sg = N(v, "srcGap"), grp;
        bool hasGrp = json::get_num(v, "group", grp);
        std::wstring feeds; int nf = 0; double minGap = 1e9;
        json::for_each_elem(json::find_value(v, "feeds"), [&](const char* f) {
            uint64_t id = 0; json::get_u64(f, "steamId", id); double g = N(f, "gap");
            if (g < minGap) minGap = g; nf++;
            if (!feeds.empty()) feeds += L", ";
            auto it = g_names.find(id);
            feeds += it != g_names.end() ? it->second + fmtw(L":%.0f", g) : fmtw(L"%llu:%.0f", (unsigned long long)id, g);
        });
        rows.push_back({ fmtw(L"%.0f", N(v, "id")), hasGrp ? fmtw(L"%.0f", grp) : L"不明", fmtw(L"%.0f, %.0f, %.0f", pos[0], pos[1], pos[2]),
                         sg > 0 ? fmtw(L"%.0f", sg) : L"-", fmtw(L"%d", nf), feeds });
    });
    lv_fill(H(ID_VEH_LV), rows);
    set(ID_VEH_LBL, fmtw(L"車両 (%d)  — グループ: 注入前スポーンは不明 / I_src: 最密な更新間隔tick / 受信者: 名前(不明ならSteamID):間隔tick", (int)rows.size()));
}

static void render_log() {
    EnableWindow(H(ID_CAP_COPY), !g_lastCap.empty());
    if (!g_dllUp) {
        set(ID_CAP_TXT, g_lastCap.empty() ? L"録画: —（注入後に操作できます）" : L"録画: —（最後のファイル: " + g_lastCap + L"）");
        set(ID_LOG_TXT, L""); EnableWindow(H(ID_CAP_START), false); EnableWindow(H(ID_CAP_STOP), false); EnableWindow(H(ID_CAP_MARK), false); return;
    }
    const char* o = g_status.c_str();
    if (g_capturing) { std::string cf = Sx(o, "captureFile"); if (!cf.empty()) g_lastCap = W(cf); }
    set(ID_CAP_TXT, g_capturing ? L"録画中: " + g_lastCap + fmtw(L"  (%s)", bytesw(N(o, "captureBytes")).c_str())
                  : g_lastCap.empty() ? L"録画: 停止中" : L"録画: 停止中（最後のファイル: " + g_lastCap + L"）");
    set(ID_LOG_TXT, L"ログ: " + W(Sx(o, "logFile")));
    EnableWindow(H(ID_CAP_START), true); EnableWindow(H(ID_CAP_STOP), g_capturing); EnableWindow(H(ID_CAP_MARK), g_capturing);
}
static void copy_text(const std::wstring& t) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (h) { memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    CloseClipboard();
}

// ---------------------------------------------------------------- polling
static void poll() {
    DWORD pid = injector::find_pid("server64.exe");
    if (pid && injector::has_module(pid, "swhook.dll")) g_injectedPid = pid;   // however it got there
    else if (pid && g_autoInject && pid != g_injectedPid) {
        std::string err;
        if (injector::inject(pid, g_dllPath.c_str(), err) != injector::Failed) g_injectedPid = pid;
        if (!err.empty()) g_lastErr = W(err);                // result shows up via the status line
    }
    if (!pid) { g_c.close(); g_port = relay::g_cfg.ipcPort; }   // server gone: next DLL will use the ini's port
    std::string st = pid ? ipc("status") : "";
    bool wasUp = g_dllUp;
    g_dllUp = ipc_ok(st);
    if (g_dllUp) {
        g_status = st;
        g_hooked = B(st.c_str(), "hooked"); g_relay = B(st.c_str(), "relay"); g_capturing = B(st.c_str(), "capturing");
        g_dllVersion = Sx(st.c_str(), "version");
        std::string p = ipc("peers.get"); if (ipc_ok(p)) g_peersJson = p;
        if (g_showVeh) { std::string v = ipc("vehicles.get"); if (ipc_ok(v)) g_vehJson = v; }
        if (!wasUp) refresh_config();          // came up: switch the settings view to live values
    } else {
        g_status.clear(); g_peersJson.clear(); g_vehJson.clear(); g_prevPeer.clear(); g_prevMs = 0; g_rateSend = g_rateInj = 0;
        if (wasUp) refresh_config();           // went down: fall back to the ini view
    }
    render_header(); render_peers(); render_vehicles(); render_status(); render_log(); render_debug();
}

// ---------------------------------------------------------------- UI build
static void show_tab(int t) {
    for (int i = 0; i < kTabs; i++) for (HWND c : g_tabCtl[i]) ShowWindow(c, i == t ? SW_SHOW : SW_HIDE);
}
// Developer switch (swhook.ini debugUi): add or remove the debug page from the tab control. It is
// always the last page, so the page indices of the normal tabs never move.
static void set_debug_ui(bool on) {
    if (on == g_debugUi) return;
    g_debugUi = on;
    if (on) { TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = (LPWSTR)L"デバッグ"; SendMessageW(g_tab, TCM_INSERTITEMW, kDebugTab, (LPARAM)&it); }
    else {
        if (TabCtrl_GetCurSel(g_tab) == kDebugTab) { TabCtrl_SetCurSel(g_tab, 0); show_tab(0); }
        SendMessageW(g_tab, TCM_DELETEITEM, kDebugTab, 0);
    }
}

static void build_ui() {
    // header
    mk(L"STATIC", L"", 0, 12, 10, 300, 20, ID_SERVER_TXT);
    mk(L"STATIC", L"", 0, 12, 32, 460, 20, ID_DLL_TXT);
    mk(L"BUTTON", L"今すぐ注入", WS_TABSTOP | BS_PUSHBUTTON, 480, 10, 110, 26, ID_INJECT_BTN);
    mk(L"BUTTON", L"サーバー起動を検出したら自動で注入", WS_TABSTOP | BS_AUTOCHECKBOX, 600, 12, 300, 22, ID_AUTO_CHK);
    mk(L"STATIC", L"", SS_RIGHT, 600, 34, 300, 20, ID_VERSION_TXT);

    g_tab = mk(L"SysTabControl32", L"", WS_TABSTOP | WS_CLIPSIBLINGS, 12, 60, 888, 560, ID_TAB);
    const wchar_t* names[] = { L"状態", L"設定", L"起動・注入", L"ログ・録画" };
    for (int i = 0; i < kTabs - 1; i++) { TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = (LPWSTR)names[i]; SendMessageW(g_tab, TCM_INSERTITEMW, i, (LPARAM)&it); }
    const int X = 24, Y = 92, WID = 864;   // page area

    // --- status tab
    HWND rb = mk(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, X, Y, 300, 40, ID_RELAY_BTN, 0);
    mk(L"STATIC", L"※ 一度注入すればこの画面を閉じても効き続けます（サーバーを終了するまで）", 0, X, Y + 44, 400, 18, ID_RELAY_NOTE, 0);
    SendMessageW(rb, WM_SETFONT, (WPARAM)g_fontBig, TRUE);
    mk(L"STATIC", L"", 0, X + 316, Y + 2, WID - 316, 18, ID_TRAFFIC_TXT, 0);
    mk(L"STATIC", L"", 0, X + 316, Y + 22, WID - 316, 18, ID_WALK_TXT, 0);
    mk(L"STATIC", L"参加プレイヤー", 0, X, Y + 66, 400, 18, ID_PEERS_LBL, 0);
    HWND pl = mk(L"SysListView32", L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, X, Y + 86, WID, 176, ID_PEERS_LV, 0, WS_EX_CLIENTEDGE);
    lv_cols(pl, { {L"名前", 110}, {L"SteamID", 125}, {L"状態", 50}, {L"遅延 (tick差)", 85}, {L"TPS", 40}, {L"送信/s", 70}, {L"同期追加/s", 75}, {L"受信/s", 70}, {L"解析率", 55}, {L"受信解析率", 70}, {L"位置 (x, y, z)", 140}, {L"車両", 40},{L"最終通信", 60}, {L"凍結検知", 110} });
    mk(L"STATIC", L"車両", 0, X, Y + 272, 640, 18, ID_VEH_LBL, 0);
    mk(L"BUTTON", L"車両一覧を表示（デバッグ用）", WS_TABSTOP | BS_AUTOCHECKBOX, X + WID - 220, Y + 270, 220, 22, ID_VEH_CHK, 0);
    HWND vl = mk(L"SysListView32", L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, X, Y + 292, WID, 220, ID_VEH_LV, 0, WS_EX_CLIENTEDGE);
    lv_cols(vl, { {L"ID", 50}, {L"グループ", 60}, {L"位置 (x, y, z)", 210}, {L"I_src", 50}, {L"受信者数", 60}, {L"受信者 (名前:間隔)", 400} });

    // --- settings tab
    mk(L"STATIC", L"項目をクリックして値を編集し「適用」。適用した値は稼働中すぐ効きます。「iniに保存」で次回以降にも引き継がれます。",
       0, X, Y, WID, 18, ID_CFG_NOTE, 1);
    HWND cl = mk(L"SysListView32", L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, X, Y + 24, WID, 380, ID_CFG_LV, 1, WS_EX_CLIENTEDGE);
    lv_cols(cl, { {L"項目", 150}, {L"値", 80}, {L"説明", 620} });
    mk(L"STATIC", L"（項目を選択）", 0, X, Y + 416, 150, 22, ID_CFG_NAME, 1);
    mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, X + 156, Y + 413, 120, 24, ID_CFG_EDIT, 1, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"適用", WS_TABSTOP | BS_PUSHBUTTON, X + 284, Y + 412, 80, 26, ID_CFG_APPLY, 1);
    mk(L"STATIC", L"", 0, X, Y + 444, WID, 18, ID_CFG_HELP, 1);
    mk(L"BUTTON", L"iniに保存", WS_TABSTOP | BS_PUSHBUTTON, X + WID - 240, Y + 412, 110, 26, ID_CFG_SAVE, 1);
    mk(L"BUTTON", L"iniを再読込", WS_TABSTOP | BS_PUSHBUTTON, X + WID - 120, Y + 412, 110, 26, ID_CFG_RELOAD, 1);

    // --- startup tab (swhook.ini, read/written by the GUI itself)
    mk(L"STATIC", L"ここは swhook.ini の起動時設定です。サーバー稼働中に変えても効かず、次に注入したときから有効になります。", 0, X, Y, WID, 18, ID_ST_NOTE, 2);
    mk(L"BUTTON", L"注入した時点で同期改善を ON にする (relay)", WS_TABSTOP | BS_AUTOCHECKBOX, X, Y + 30, WID, 22, ID_ST_RELAY, 2);
    mk(L"STATIC", L"制御ポート (ipcPort) 127.0.0.1:", 0, X, Y + 64, 220, 22, ID_ST_PORT_LBL, 2);
    mk(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, X + 226, Y + 61, 90, 24, ID_ST_PORT, 2, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"iniに保存", WS_TABSTOP | BS_PUSHBUTTON, X, Y + 98, 110, 26, ID_ST_SAVE, 2);
    mk(L"STATIC", L"", 0, X, Y + 138, WID, 18, ID_ST_DLL, 2);
    mk(L"STATIC", L"", 0, X, Y + 158, WID, 18, ID_ST_INI, 2);
    mk(L"BUTTON", L"開発者向け「デバッグ」タブを表示する (debugUi) — 通信遮断・テレポート等の実験操作。通常利用では OFF", WS_TABSTOP | BS_AUTOCHECKBOX, X, Y + 200, WID, 22, ID_ST_DEBUG, 2);

    // --- debug tab (developer only; the page always exists, its tab item only while debugUi=1)
    mk(L"STATIC", L"フリーズ調査用の実験操作です。実プレイヤーの受信内容を変えるので、了解を得た相手にだけ使ってください。操作はすべて captures\\*.log に記録されます。",
       0, X, Y, WID, 18, ID_DBG_NOTE, kDebugTab);
    mk(L"STATIC", L"プレイヤー", 0, X, Y + 26, WID, 18, ID_DBG_LBL, kDebugTab);
    HWND dl = mk(L"SysListView32", L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, X, Y + 46, WID, 200, ID_DBG_LV, kDebugTab, WS_EX_CLIENTEDGE);
    lv_cols(dl, { {L"SteamID", 140}, {L"名前", 110}, {L"位置 (x, y, z)", 150}, {L"凍結検知（pose静止 / 車両定義の未ack）", 450} });
    mk(L"STATIC", L"通信遮断 (debug.hold)：このプレイヤー宛の送信を指定 ms 間すべて棄却（再送しない）。同期の穴に対する挙動の実験用。", 0, X, Y + 258, WID, 18, ID_DBG_HOLD_LBL, kDebugTab);
    mk(L"EDIT", L"5000", WS_TABSTOP | ES_NUMBER, X, Y + 280, 80, 24, ID_DBG_HOLD_MS, kDebugTab, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"遮断開始 (ms)", WS_TABSTOP | BS_PUSHBUTTON, X + 88, Y + 279, 130, 26, ID_DBG_HOLD, kDebugTab);
    mk(L"BUTTON", L"今すぐ解放", WS_TABSTOP | BS_PUSHBUTTON, X + 226, Y + 279, 110, 26, ID_DBG_RELEASE, kDebugTab);
    mk(L"STATIC", L"緩和プローブ (debug.nudge)：次の tick メッセージに 1 回だけレコードを追加。凍結中のプレイヤーに試し、復帰するか観察。", 0, X, Y + 318, WID, 18, ID_DBG_NUDGE_LBL, kDebugTab);
    mk(L"BUTTON", L"テレポート (0x5E, 現在位置へ)", WS_TABSTOP | BS_PUSHBUTTON, X, Y + 340, 220, 26, ID_DBG_NUDGE_TP, kDebugTab);
    mk(L"BUTTON", L"タイル再ロード (0x45, 現在タイル)", WS_TABSTOP | BS_PUSHBUTTON, X + 228, Y + 340, 230, 26, ID_DBG_NUDGE_TILE, kDebugTab);
    mk(L"BUTTON", L"タイルアンロード (0x46, 現在タイル)", WS_TABSTOP | BS_PUSHBUTTON, X + 466, Y + 340, 240, 26, ID_DBG_NUDGE_UNLOAD, kDebugTab);
    mk(L"BUTTON", L"タイル再ロード (0x46 → 0.5 秒後 0x45)", WS_TABSTOP | BS_PUSHBUTTON, X, Y + 372, 260, 26, ID_DBG_NUDGE_RELOAD, kDebugTab);
    mk(L"BUTTON", L"DLL をアンロード (swctl unload)", WS_TABSTOP | BS_PUSHBUTTON, X + 604, Y + 372, 260, 26, ID_DBG_DLL_UNLOAD, kDebugTab);
    mk(L"STATIC", L"", 0, X, Y + 410, WID, 36, ID_DBG_RESULT, kDebugTab);
    mk(L"STATIC", L"凍結検知の意味: 「pose」= 自機の位置報告が止まったまま、乗っている車両がサーバー側で動いている。「defs」= サーバーが送った車両定義を 0x29 で ack しない。"
       L"どちらもクライアントのワールド更新が止まった署名（詳細: tools/swcap/README.md）。検知時は .log に FREEZE? 行と録画マーカーが自動で入ります。",
       0, X, Y + 452, WID, 54, ID_DBG_HELP, kDebugTab);

    // --- log tab
    mk(L"BUTTON", L"ログファイルを書く (log) — captures\\session_<日時>.log。起動・設定変更・同期統計などのイベントを記録", WS_TABSTOP | BS_AUTOCHECKBOX, X, Y, WID, 22, ID_LOGPKT_CHK, 3);
    mk(L"BUTTON", L"解析に失敗したパケットを記録する (dumpWalkFail) — captures\\walkfail_<日時>.swcap に1セッション1ファイル", WS_TABSTOP | BS_AUTOCHECKBOX, X, Y + 26, WID, 22, ID_DUMPWF_CHK, 3);
    mk(L"STATIC", L"　　1セッションあたりの保存上限 (dumpWalkFailMax):", 0, X, Y + 52, 330, 22, ID_DUMPMAX_LBL, 3);
    mk(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, X + 336, Y + 49, 80, 24, ID_DUMPMAX, 3, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"適用", WS_TABSTOP | BS_PUSHBUTTON, X + 424, Y + 48, 70, 26, ID_DUMPMAX_APPLY, 3);
    mk(L"BUTTON", L"注入した時点で録画 (.swcap) を開始する (capture) — 通常は OFF。調査依頼時のみ", WS_TABSTOP | BS_AUTOCHECKBOX, X, Y + 92, WID, 22, ID_CAP_AUTO, 3);
    mk(L"STATIC", L"", 0, X, Y + 122, WID, 18, ID_CAP_TXT, 3);
    mk(L"BUTTON", L"録画開始（新規ファイル）", WS_TABSTOP | BS_PUSHBUTTON, X, Y + 146, 180, 26, ID_CAP_START, 3);
    mk(L"BUTTON", L"録画停止", WS_TABSTOP | BS_PUSHBUTTON, X + 190, Y + 146, 100, 26, ID_CAP_STOP, 3);
    mk(L"BUTTON", L"マーカーを打つ", WS_TABSTOP | BS_PUSHBUTTON, X + 300, Y + 146, 120, 26, ID_CAP_MARK, 3);
    mk(L"BUTTON", L"ファイル名をコピー", WS_TABSTOP | BS_PUSHBUTTON, X + 430, Y + 146, 150, 26, ID_CAP_COPY, 3);
    mk(L"STATIC", L"", 0, X, Y + 184, WID, 18, ID_LOG_TXT, 3);
    mk(L"BUTTON", L"captures フォルダを開く", WS_TABSTOP | BS_PUSHBUTTON, X, Y + 208, 180, 26, ID_OPEN_DIR, 3);
    mk(L"STATIC", L"上のログ設定は稼働中すぐ効き、「設定」タブの「iniに保存」で次回以降にも残ります。不具合報告のときは captures フォルダ内の .log（と必要なら .swcap）を送ってください。",
       0, X, Y + 248, WID, 36, ID_LOG_NOTE, 3);
    show_tab(0);
}

static void on_command(int id) {
    switch (id) {
    case ID_INJECT_BTN: {
        DWORD pid = injector::find_pid("server64.exe");
        if (!pid) { MessageBoxW(g_wnd, L"server64.exe が起動していません。", L"SWMultiSync", MB_ICONINFORMATION); return; }
        std::string err; injector::Result r = injector::inject(pid, g_dllPath.c_str(), err);
        if (r == injector::Failed) MessageBoxW(g_wnd, (L"注入に失敗しました:\n" + W(err)).c_str(), L"SWMultiSync", MB_ICONERROR);
        poll(); break; }
    case ID_RELAY_BTN: ipc("relay.set", g_relay ? "\"enabled\":false" : "\"enabled\":true"); poll(); break;
    case ID_VEH_CHK: g_showVeh = IsDlgButtonChecked(g_wnd, ID_VEH_CHK) == BST_CHECKED; g_vehJson.clear(); poll(); break;
    case ID_CFG_APPLY: {
        wchar_t name[64], val[64]; GetWindowTextW(H(ID_CFG_NAME), name, 64); GetWindowTextW(H(ID_CFG_EDIT), val, 64);
        if (name[0] == L'（' || !val[0]) return;
        char n8[64]; WideCharToMultiByte(CP_UTF8, 0, name, -1, n8, 64, nullptr, nullptr);
        cfg_set(n8, _wtof(val)); break; }
    case ID_CFG_SAVE: { std::string r = ipc("config.save");
        MessageBoxW(g_wnd, ipc_ok(r) ? (L"保存しました:\n" + W(Sx(r.c_str(), "path"))).c_str() : L"保存に失敗しました。", L"SWMultiSync", ipc_ok(r) ? MB_ICONINFORMATION : MB_ICONWARNING); break; }
    case ID_CFG_RELOAD: ipc("config.reload"); refresh_config(); break;
    case ID_AUTO_CHK: g_autoInject = IsDlgButtonChecked(g_wnd, ID_AUTO_CHK) == BST_CHECKED; ini_set("autoInject", g_autoInject ? 1 : 0); break;
    case ID_CAP_AUTO: ini_set("capture", IsDlgButtonChecked(g_wnd, ID_CAP_AUTO) == BST_CHECKED ? 1 : 0); break;
    case ID_ST_SAVE: startup_save(); break;
    case ID_ST_DEBUG: { bool on = IsDlgButtonChecked(g_wnd, ID_ST_DEBUG) == BST_CHECKED; ini_set("debugUi", on ? 1 : 0); set_debug_ui(on); break; }
    case ID_DBG_HOLD: { wchar_t v[16]; GetWindowTextW(H(ID_DBG_HOLD_MS), v, 16); debug_cmd("debug.hold", ",\"ms\":" + std::to_string(v[0] ? _wtoi(v) : 5000)); break; }
    case ID_DBG_RELEASE:    debug_cmd("debug.release", ""); break;
    case ID_DBG_NUDGE_TP:   debug_cmd("debug.nudge", ",\"kind\":\"tp\""); break;
    case ID_DBG_NUDGE_TILE: debug_cmd("debug.nudge", ",\"kind\":\"tile\""); break;
    case ID_DBG_NUDGE_UNLOAD: debug_cmd("debug.nudge", ",\"kind\":\"unload\""); break;
    case ID_DBG_NUDGE_RELOAD: debug_cmd("debug.nudge", ",\"kind\":\"reload\""); break;
    case ID_DBG_DLL_UNLOAD: {
        // Dev hot-swap: the DLL restores the vtable and unmaps itself; auto-inject stays off for this
        // server pid (g_injectedPid), so rebuild + 「注入」 is the way back in.
        if (MessageBoxW(g_wnd, L"swhook.dll をサーバーからアンロードします（フック解除・IPC 停止）。\n再注入は「起動・注入」タブの注入ボタンから。よろしいですか？", L"SWMultiSync", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
        std::string r = ipc("unload");
        set(ID_DBG_RESULT, W(std::string("unload -> ") + (r.empty() ? "(no reply)" : r)));
        break; }
    case ID_LOGPKT_CHK: cfg_set("log", IsDlgButtonChecked(g_wnd, ID_LOGPKT_CHK) == BST_CHECKED ? 1 : 0); break;
    case ID_DUMPMAX_APPLY: { wchar_t v[16]; GetWindowTextW(H(ID_DUMPMAX), v, 16); if (v[0]) cfg_set("dumpWalkFailMax", _wtof(v)); break; }
    case ID_DUMPWF_CHK: cfg_set("dumpWalkFail", IsDlgButtonChecked(g_wnd, ID_DUMPWF_CHK) == BST_CHECKED ? 1 : 0); break;
    case ID_CAP_START: ipc("capture.start"); poll(); break;
    case ID_CAP_STOP:  ipc("capture.stop");  poll(); break;
    case ID_CAP_MARK:  { std::string r = ipc("capture.mark"); if (ipc_ok(r)) set(ID_CAP_TXT, fmtw(L"マーカー #%.0f を記録しました", N(r.c_str(), "mark"))); break; }
    case ID_CAP_COPY:  { size_t k = g_lastCap.find_last_of(L"\\/"); copy_text(k == std::wstring::npos ? g_lastCap : g_lastCap.substr(k + 1)); break; }
    case ID_OPEN_DIR:  ShellExecuteW(g_wnd, L"open", W(injector::exe_dir() + "captures").c_str(), nullptr, nullptr, SW_SHOW); break;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND: on_command(LOWORD(wp)); return 0;
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->idFrom == ID_TAB && n->code == TCN_SELCHANGE) show_tab(TabCtrl_GetCurSel(g_tab));
        if (n->idFrom == ID_DBG_LV && (n->code == LVN_ITEMCHANGED || n->code == NM_CLICK)) render_debug();
        if (n->idFrom == ID_CFG_LV && (n->code == LVN_ITEMCHANGED || n->code == NM_CLICK)) {
            int i = ListView_GetNextItem(n->hwndFrom, -1, LVNI_SELECTED);
            if (i >= 0) {
                wchar_t name[64], val[64], help[512];
                lv_get(n->hwndFrom, i, 0, name, 64); lv_get(n->hwndFrom, i, 1, val, 64); lv_get(n->hwndFrom, i, 2, help, 512);
                SetWindowTextW(H(ID_CFG_NAME), name); SetWindowTextW(H(ID_CFG_EDIT), val); SetWindowTextW(H(ID_CFG_HELP), help);
            }
        }
        return 0; }
    case WM_TIMER: poll(); return 0;
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY: KillTimer(h, ID_TIMER); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    g_hi = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    // --port N on the command line overrides swhook.ini's ipcPort (the GUI does not read the ini).
    g_dllPath = injector::default_dll_path();
    g_iniPath = injector::exe_dir() + "swhook.ini";
    if (!injector::is_admin())
        MessageBoxW(nullptr, L"管理者権限がありません。注入には管理者権限が必要です。\n右クリック →「管理者として実行」で起動してください。", L"SWMultiSync", MB_ICONWARNING);

    HDC dc = GetDC(nullptr); g_s = GetDeviceCaps(dc, LOGPIXELSX) / 96.0; ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{ sizeof ncm }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(13);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(16); ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_fontBig = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"SWMultiSyncMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    HICON appIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));   // IDI_APPICON from version.rc; nullptr if absent
    wc.hIcon = appIcon ? appIcon : LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{ 0, 0, S(912), S(632) }; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowW(wc.lpszClassName, fmtw(L"SW MultiSync v%hs", SWHOOK_VERSION).c_str(),
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    build_ui();
    startup_load();
    refresh_config();
    if (wcsstr(cmd, L"--port")) g_port = _wtoi(wcsstr(cmd, L"--port") + 6);   // explicit override wins over the ini
    poll();
    SetTimer(g_wnd, ID_TIMER, 1000, nullptr);
    ShowWindow(g_wnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
