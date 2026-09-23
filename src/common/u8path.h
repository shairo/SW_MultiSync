// u8path.h — file paths are UTF-8 std::string everywhere in this project; convert to UTF-16 only at
// the Win32/CRT boundary. The ANSI APIs (GetModuleFileNameA, fopen, LoadLibraryA...) mangle any
// character outside the system code page, and the IPC/GUI side decodes strings as UTF-8, so an
// ANSI path both breaks file access and shows up garbled. Shared by the DLL and the C++ clients.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <string>

namespace u8path {

inline std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// A path from char** argv (system code page) -> UTF-8.
inline std::string from_acp(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return "";
    std::wstring w((size_t)n, wchar_t());
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(n - 1);
    return narrow(w);
}

// Full path of `mod` (nullptr = the exe), UTF-8. Handles paths longer than MAX_PATH.
inline std::string module_path(HMODULE mod) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(mod, &buf[0], (DWORD)buf.size());
        if (n == 0) return "";
        if (n < buf.size()) { buf.resize(n); return narrow(buf); }
        buf.resize(buf.size() * 2);
    }
}
// Directory of `mod` with trailing backslash ("" on failure).
inline std::string module_dir(HMODULE mod) {
    std::string p = module_path(mod);
    size_t slash = p.find_last_of('\\');
    return slash == std::string::npos ? "" : p.substr(0, slash + 1);
}

inline FILE* fopen(const std::string& path, const char* mode) {
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
}
inline bool exists(const std::string& path) { return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES; }
inline void mkdir(const std::string& path)  { CreateDirectoryW(widen(path).c_str(), nullptr); }
inline bool replace(const std::string& from, const std::string& to) {
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace u8path
