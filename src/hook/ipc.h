// ipc.h — control-plane transport for swhook.dll: a localhost TCP listener speaking newline-delimited
// JSON. One request object per line in, one response object per line out. Any client that can open
// a TCP socket (the bundled GUI/CLI, Python, C#, ...) talks to the DLL through this; the command
// vocabulary lives in dllmain.cpp (ipc::Handler), this file is transport only.
//
// Design notes:
//  - Bound to 127.0.0.1 only. No auth: the same machine already has admin (it injected us).
//  - One thread per client (clients are few and long-lived: the GUI, an occasional CLI call).
//  - Line cap 64 KB; a longer line closes the connection. Blank lines are ignored.
//  - The listener thread retries bind for a while (port in TIME_WAIT after a server restart).
//  - The first bytes of a connection are sniffed: "GET " / "POST " is reserved for a future HTTP
//    shim (Stormworks addon server.httpGet) — today it just answers 501 so the port is not confused.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <functional>
#include <cstdint>
#pragma comment(lib, "ws2_32.lib")

namespace ipc {

using Handler = std::function<std::string(const std::string& requestLine)>;

inline Handler  g_handler;
inline SOCKET   g_listen = INVALID_SOCKET;
inline int      g_port   = 0;
inline volatile long g_clients = 0;          // currently connected clients (for status)
inline volatile long g_requests = 0;         // total requests served
inline volatile long g_stopping = 0;         // set by stop(): threads must wind down
inline CRITICAL_SECTION g_cs;                // guards g_socks / g_hthreads
inline SOCKET g_socks[64];                   // open client sockets (closed by stop() to unblock recv)
inline int    g_nsocks = 0;
inline HANDLE g_hthreads[64];                // listener + client thread handles (stop() waits on the
inline int    g_nthreads = 0;                // handles, not a counter: a thread is "gone" only once
constexpr size_t kMaxLine = 65536;           // the OS says so — it still runs our epilogue after any flag)

inline void track(SOCKET s, bool add) {
    EnterCriticalSection(&g_cs);
    if (add) { if (g_nsocks < 64) g_socks[g_nsocks++] = s; }
    else for (int i = 0; i < g_nsocks; ++i) if (g_socks[i] == s) { g_socks[i] = g_socks[--g_nsocks]; break; }
    LeaveCriticalSection(&g_cs);
}
// Remember a thread handle; drops signalled (finished) ones first so the table does not fill up.
inline void track_thread(HANDLE h) {
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nthreads;)
        if (WaitForSingleObject(g_hthreads[i], 0) == WAIT_OBJECT_0) { CloseHandle(g_hthreads[i]); g_hthreads[i] = g_hthreads[--g_nthreads]; }
        else ++i;
    if (g_nthreads < 64) g_hthreads[g_nthreads++] = h; else CloseHandle(h);
    LeaveCriticalSection(&g_cs);
}

inline void send_all(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        int w = send(s, p, (int)n, 0);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

inline DWORD WINAPI ClientThread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    InterlockedIncrement(&g_clients);
    track(s, true);
    std::string buf; char tmp[4096];
    bool sniffed = false;
    for (;;) {
        int r = recv(s, tmp, sizeof tmp, 0);
        if (r <= 0) break;
        buf.append(tmp, (size_t)r);
        if (!sniffed && buf.size() >= 4) {
            sniffed = true;
            if (buf.compare(0, 4, "GET ") == 0 || buf.compare(0, 5, "POST ") == 0) {
                static const char resp[] = "HTTP/1.0 501 Not Implemented\r\nContent-Type: text/plain\r\n"
                                           "Connection: close\r\n\r\nswhook: HTTP shim not enabled; use JSON lines\n";
                send_all(s, resp, sizeof resp - 1);
                break;
            }
        }
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            InterlockedIncrement(&g_requests);
            std::string resp = g_handler ? g_handler(line) : std::string("{\"ok\":false,\"error\":\"no handler\"}");
            resp += '\n';
            send_all(s, resp.data(), resp.size());
            if (g_stopping) break;      // e.g. the "unload" reply just went out
        }
        if (buf.size() > kMaxLine || g_stopping) break;
    }
    track(s, false);
    shutdown(s, SD_BOTH);
    closesocket(s);
    InterlockedDecrement(&g_clients);
    return 0;
}

inline DWORD WINAPI ListenThread(LPVOID) {
    for (;;) {
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (g_stopping) { if (c != INVALID_SOCKET) closesocket(c); break; }
        if (c == INVALID_SOCKET) { Sleep(50); continue; }
        BOOL nd = TRUE; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
        HANDLE t = CreateThread(nullptr, 0, ClientThread, (LPVOID)(uintptr_t)c, 0, nullptr);
        if (t) track_thread(t); else closesocket(c);
    }
    return 0;
}

// Start the listener on 127.0.0.1:port. Returns 0 on success or the WSA error code. Never throws;
// safe to call from the DLL worker thread. Retries bind for ~10 s (port lingering from a previous
// server instance).
inline int start(int port, Handler h) {
    g_handler = h; g_port = port;
    InitializeCriticalSection(&g_cs);
    WSADATA wsa;
    int e = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (e) return e;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return WSAGetLastError();
    // Exclusive: we want a clear failure if another swhook instance owns the port, not a silent
    // hijack. (SO_REUSEADDR on Windows would allow a second bind.)
    BOOL ex = TRUE; setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&ex, sizeof ex);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int err = 0;
    for (int i = 0; i < 100; ++i) {
        if (bind(g_listen, (sockaddr*)&a, sizeof a) == 0) { err = 0; break; }
        err = WSAGetLastError();
        if (err != WSAEADDRINUSE) break;
        Sleep(100);
    }
    if (err) { closesocket(g_listen); g_listen = INVALID_SOCKET; return err; }
    if (listen(g_listen, 8) != 0) { err = WSAGetLastError(); closesocket(g_listen); g_listen = INVALID_SOCKET; return err; }
    HANDLE t = CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
    if (t) track_thread(t);
    return 0;
}

// Tear the control plane down: closes the listener and every client socket (which unblocks their
// recv), then waits for all transport threads to exit — a hard requirement before the DLL can be
// unmapped. Must NOT be called from a ClientThread (it would wait on itself); the unload path runs
// it from a dedicated thread. Returns false if a thread did not exit within timeoutMs.
inline bool stop(DWORD timeoutMs = 3000) {
    InterlockedExchange(&g_stopping, 1);
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nsocks; ++i) { shutdown(g_socks[i], SD_BOTH); closesocket(g_socks[i]); }
    g_nsocks = 0;
    HANDLE hs[64]; int n = g_nthreads;
    for (int i = 0; i < n; ++i) hs[i] = g_hthreads[i];
    g_nthreads = 0;
    LeaveCriticalSection(&g_cs);
    bool clean = n == 0 || WaitForMultipleObjects((DWORD)n, hs, TRUE, timeoutMs) < WAIT_OBJECT_0 + (DWORD)n;
    for (int i = 0; i < n; ++i) CloseHandle(hs[i]);
    if (clean) { DeleteCriticalSection(&g_cs); WSACleanup(); }
    return clean;
}

} // namespace ipc
