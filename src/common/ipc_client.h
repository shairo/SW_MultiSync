// ipc_client.h — client side of the swhook control-plane (see ipc-protocol.md). Shared by swctl.exe
// and the GUI. One persistent TCP connection to 127.0.0.1:port; call() sends one JSON line and
// returns the response line. Blocking, with a receive timeout so a dead DLL can't hang the UI.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include "json.h"
#pragma comment(lib, "ws2_32.lib")

namespace ipcc {

struct Client {
    SOCKET s = INVALID_SOCKET;
    std::string buf;
    std::string lastError;

    bool connected() const { return s != INVALID_SOCKET; }

    bool connect(int port, int timeoutMs = 1000) {
        close();
        static bool wsa = false;
        if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { lastError = "socket() failed"; return false; }
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // connect with timeout via non-blocking + select
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        ::connect(s, (sockaddr*)&a, sizeof a);
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        if (select(0, nullptr, &w, nullptr, &tv) <= 0) { lastError = "connect timeout (is swhook.dll injected?)"; close(); return false; }
        int err = 0, el = sizeof err; getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (err) { lastError = "connect refused (is swhook.dll injected?)"; close(); return false; }
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD to = 5000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
        return true;
    }
    void close() { if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; } buf.clear(); }

    // Send one request line, receive one response line. Empty return = transport error (lastError).
    std::string call(const std::string& requestJson) {
        if (!connected()) { lastError = "not connected"; return ""; }
        std::string line = requestJson + "\n";
        const char* p = line.data(); size_t n = line.size();
        while (n) { int w = send(s, p, (int)n, 0); if (w <= 0) { lastError = "send failed"; close(); return ""; } p += w; n -= (size_t)w; }
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) { std::string r = buf.substr(0, nl); buf.erase(0, nl + 1); return r; }
            char tmp[8192];
            int r = recv(s, tmp, sizeof tmp, 0);
            if (r <= 0) { lastError = r == 0 ? "connection closed" : "recv timeout"; close(); return ""; }
            buf.append(tmp, (size_t)r);
        }
    }
    // Convenience: {"cmd":"x"} with optional extra raw fields ("\"key\":\"v\"").
    std::string cmd(const char* name, const std::string& extra = "") {
        std::string req = std::string("{\"cmd\":\"") + name + "\"" + (extra.empty() ? "" : "," + extra) + "}";
        return call(req);
    }
};

} // namespace ipcc
