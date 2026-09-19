// injector.h — shared "LoadLibrary swhook.dll into server64.exe" core, used by inject.exe, swctl.exe
// and the GUI. Pure Win32, no dependencies. Failures come back as a human-readable `err`.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace injector {

inline DWORD find_pid(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 e{ sizeof(e) };
    DWORD pid = 0;
    if (Process32First(snap, &e)) do {
        if (_stricmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; }
    } while (Process32Next(snap, &e));
    CloseHandle(snap);
    return pid;
}

// True if a module whose file name matches `name` is already loaded in `pid`.
inline bool has_module(DWORD pid, const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 m{ sizeof(m) };
    bool found = false;
    if (Module32First(snap, &m)) do {
        if (_stricmp(m.szModule, name) == 0) { found = true; break; }
    } while (Module32Next(snap, &m));
    CloseHandle(snap);
    return found;
}

// Directory of the running executable, with trailing backslash.
inline std::string exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* slash = strrchr(buf, '\\');
    if (slash) slash[1] = 0;
    return buf;
}
// Path of swhook.dll next to the running executable.
inline std::string default_dll_path() { return exe_dir() + "swhook.dll"; }

inline bool is_admin() {
    BOOL admin = FALSE; PSID sid = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &sid)) {
        CheckTokenMembership(nullptr, sid, &admin); FreeSid(sid);
    }
    return admin == TRUE;
}

enum Result { Injected = 0, Failed = 1, AlreadyLoaded = 2 };

// LoadLibrary `dll` inside `pid` via CreateRemoteThread. Needs admin.
// Only the rights this actually uses: CreateRemoteThread (PROCESS_CREATE_THREAD),
// VirtualAllocEx/Free (PROCESS_VM_OPERATION), WriteProcessMemory (PROCESS_VM_WRITE).
// Avoiding PROCESS_ALL_ACCESS also lowers the "textbook injector" score AV heuristics assign.
#define SWHOOK_INJECT_ACCESS (PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE)
inline Result inject(DWORD pid, const char* dll, std::string& err) {
    char msg[512];
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        snprintf(msg, sizeof msg, "dll not found: %s", dll); err = msg; return Failed;
    }
    if (has_module(pid, "swhook.dll")) { err = "swhook.dll is already loaded"; return AlreadyLoaded; }

    HANDLE p = OpenProcess(SWHOOK_INJECT_ACCESS, FALSE, pid);
    if (!p) { snprintf(msg, sizeof msg, "OpenProcess failed %lu (run as admin)", GetLastError()); err = msg; return Failed; }

    size_t len = strlen(dll) + 1;
    void* rem = VirtualAllocEx(p, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) { err = "VirtualAllocEx failed"; CloseHandle(p); return Failed; }
    WriteProcessMemory(p, rem, dll, len, nullptr);

    auto load = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, nullptr, 0, (LPTHREAD_START_ROUTINE)load, rem, 0, nullptr);
    if (!t) {
        snprintf(msg, sizeof msg, "CreateRemoteThread failed %lu", GetLastError()); err = msg;
        VirtualFreeEx(p, rem, 0, MEM_RELEASE); CloseHandle(p); return Failed;
    }
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0; GetExitCodeThread(t, &code);
    VirtualFreeEx(p, rem, 0, MEM_RELEASE);
    CloseHandle(t); CloseHandle(p);
    if (!code) { err = "LoadLibrary returned 0 inside the target (wrong architecture, or the DLL failed to load)"; return Failed; }
    return Injected;
}

} // namespace injector
