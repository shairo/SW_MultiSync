// inject.exe <pid|server64.exe> — LoadLibrary swhook.dll into the target via CreateRemoteThread.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static DWORD find_pid(const char* name) {
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
static bool has_module(DWORD pid, const char* name) {
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

// Exit codes: 0 = injected, 1 = error, 2 = already injected (nothing done).
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: inject <pid|exe-name> [dll-path]\n"); return 1; }

    DWORD pid = atoi(argv[1]);
    if (pid == 0) pid = find_pid(argv[1]);
    if (pid == 0) { printf("process not found: %s\n", argv[1]); return 1; }

    char dll[MAX_PATH];
    if (argc >= 3) strcpy_s(dll, argv[2]);
    else {
        GetModuleFileNameA(nullptr, dll, MAX_PATH);
        char* slash = strrchr(dll, '\\');
        strcpy_s(slash ? slash + 1 : dll, MAX_PATH, "swhook.dll");
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        printf("dll not found: %s\n", dll); return 1;
    }
    if (has_module(pid, "swhook.dll")) {
        printf("swhook.dll is already loaded in pid %lu - nothing to do\n", pid);
        return 2;
    }
    printf("injecting %s into pid %lu\n", dll, pid);

    HANDLE p = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!p) { printf("OpenProcess failed %lu (run as admin)\n", GetLastError()); return 1; }

    size_t len = strlen(dll) + 1;
    void* rem = VirtualAllocEx(p, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(p, rem, dll, len, nullptr);

    auto load = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, nullptr, 0, (LPTHREAD_START_ROUTINE)load, rem, 0, nullptr);
    if (!t) { printf("CreateRemoteThread failed %lu\n", GetLastError()); return 1; }

    WaitForSingleObject(t, INFINITE);
    DWORD code = 0; GetExitCodeThread(t, &code);
    printf("LoadLibrary returned module handle 0x%lx (0 = failed)\n", code);

    VirtualFreeEx(p, rem, 0, MEM_RELEASE);
    CloseHandle(t); CloseHandle(p);
    return code ? 0 : 1;
}
