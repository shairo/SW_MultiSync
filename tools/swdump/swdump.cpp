// swdump: crash watcher for server64.exe (no system settings changed, no game files touched).
// Attaches as a debugger, passes every first-chance exception back to the game untouched, and only when an
// exception is unhandled (second chance / fail-fast) writes a full dump plus a text report (faulting module +
// offset, stack). A clean exit logs the exit code. Detaches without killing the server if swdump itself dies.
// After the server exits it waits for the next server64.exe (start.bat restarts it) and attaches again.
//   cl /nologo /std:c++17 /EHsc /W3 /utf-8 tools\swdump\swdump.cpp /Fe:build\swdump.exe /Fo:build\ dbghelp.lib psapi.lib
//   build\swdump.exe <dump folder>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include <cstdint>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
    SYSTEMTIME st; GetLocalTime(&st);
    char head[64]; snprintf(head, sizeof head, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    for (FILE* f : { stdout, g_log }) {
        if (!f) continue;
        fputs(head, f); va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
    }
}

static const wchar_t* g_exe = L"server64.exe";
static DWORD find_server() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, g_exe) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

// "module+0xoffset" for an address in the debuggee.
static std::string where(HANDLE proc, DWORD64 addr) {
    HMODULE mods[1024]; DWORD need = 0;
    if (EnumProcessModulesEx(proc, mods, sizeof mods, &need, LIST_MODULES_ALL)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
            MODULEINFO mi{};
            if (!GetModuleInformation(proc, mods[i], &mi, sizeof mi)) continue;
            DWORD64 b = (DWORD64)mi.lpBaseOfDll;
            if (addr >= b && addr < b + mi.SizeOfImage) {
                char name[MAX_PATH] = "?";
                GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
                char buf[400];
                char symbuf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                if (SymFromAddr(proc, addr, &disp, sym))
                    snprintf(buf, sizeof buf, "%s+0x%llx (%s+0x%llx)", name, (unsigned long long)(addr - b), sym->Name, (unsigned long long)disp);
                else
                    snprintf(buf, sizeof buf, "%s+0x%llx", name, (unsigned long long)(addr - b));
                return buf;
            }
        }
    }
    char buf[40]; snprintf(buf, sizeof buf, "0x%llx (no module)", (unsigned long long)addr);
    return buf;
}

static void report(HANDLE proc, DWORD pid, DWORD tid, const EXCEPTION_RECORD& er, const std::string& dir) {
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
    if (th) GetThreadContext(th, &ctx);
    SymInitialize(proc, nullptr, TRUE);
    logf("FATAL code=0x%08lX addr=%s thread=%lu\n", er.ExceptionCode, where(proc, (DWORD64)er.ExceptionAddress).c_str(), tid);
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2)
        logf("  access violation: %s 0x%llx\n", er.ExceptionInformation[0] == 0 ? "read" : er.ExceptionInformation[0] == 1 ? "write" : "execute",
             (unsigned long long)er.ExceptionInformation[1]);
    if (th) {
        STACKFRAME64 sf{};
        sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        CONTEXT c2 = ctx;
        for (int i = 0; i < 40; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c2, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            if (sf.AddrPC.Offset == 0) break;
            logf("  #%02d %s\n", i, where(proc, sf.AddrPC.Offset).c_str());
        }
    }
    SYSTEMTIME st; GetLocalTime(&st);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\server64_%04d%02d%02d_%02d%02d%02d_%08lX.dmp", dir.c_str(), st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, er.ExceptionCode);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        EXCEPTION_RECORD rec = er;
        EXCEPTION_POINTERS ep{ &rec, &ctx };
        MINIDUMP_EXCEPTION_INFORMATION mei{ tid, &ep, FALSE };
        BOOL ok = MiniDumpWriteDump(proc, pid, f, (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                    MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), th ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
        logf("dump %s: %s\n", ok ? "written" : "FAILED", path);
    }
    SymCleanup(proc);
    if (th) CloseHandle(th);
}

static bool watch(DWORD pid, const std::string& dir) {
    if (!DebugActiveProcess(pid)) { logf("attach to %lu failed (error %lu)\n", pid, GetLastError()); Sleep(5000); return false; }
    DebugSetProcessKillOnExit(FALSE);                  // if swdump dies, the server keeps running
    logf("attached to server64.exe pid %lu\n", pid);
    HANDLE proc = nullptr;
    uint64_t firstChance = 0;
    for (;;) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, INFINITE)) { logf("WaitForDebugEvent failed %lu\n", GetLastError()); break; }
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            proc = ev.u.CreateProcessInfo.hProcess;
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD& er = ev.u.Exception.ExceptionRecord;
            if (er.ExceptionCode == EXCEPTION_BREAKPOINT && firstChance == 0) { firstChance++; break; }   // the attach break
            if (ev.u.Exception.dwFirstChance) {
                firstChance++;
                cont = DBG_EXCEPTION_NOT_HANDLED;          // let the game's own handlers run
                break;
            }
            report(proc, pid, ev.dwThreadId, er, dir);
            cont = DBG_EXCEPTION_NOT_HANDLED;              // the process goes down as it would without us
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            logf("server64.exe pid %lu exited, code 0x%08lX (first-chance exceptions seen %llu)\n", pid,
                 ev.u.ExitProcess.dwExitCode, (unsigned long long)firstChance);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return true;
        default: break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    DebugActiveProcessStop(pid);
    return true;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    static std::wstring exe;
    if (argc > 2) { exe.assign(argv[2], argv[2] + strlen(argv[2])); g_exe = exe.c_str(); }   // test: another exe name
    std::string lp = dir + "\\swdump.log";
    g_log = _fsopen(lp.c_str(), "a", _SH_DENYNO);   // readable while swdump runs
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    logf("swdump started (dump folder %s)\n", dir.c_str());
    DWORD last = 0;
    for (;;) {
        DWORD pid = find_server();
        if (pid == 0 || pid == last) { Sleep(2000); continue; }
        last = pid;
        if (!watch(pid, dir)) last = 0;
    }
}
