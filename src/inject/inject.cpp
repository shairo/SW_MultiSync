// inject.exe <pid|server64.exe> [dll-path] — LoadLibrary swhook.dll into the target.
// Thin wrapper over injector.h (shared with swctl.exe and the GUI). Exit codes: 0 = injected,
// 1 = error, 2 = already injected (nothing done).
#include "injector.h"
#include "../common/version.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);   // paths are UTF-8
    if (argc < 2) { printf("inject %s\nusage: inject <pid|exe-name> [dll-path]\n", SWHOOK_VERSION); return 1; }
    DWORD pid = atoi(argv[1]);
    if (pid == 0) pid = injector::find_pid(argv[1]);
    if (pid == 0) { printf("process not found: %s\n", argv[1]); return 1; }
    std::string dll = argc >= 3 ? u8path::from_acp(argv[2]) : injector::default_dll_path();
    printf("injecting %s into pid %lu\n", dll.c_str(), pid);
    std::string err;
    injector::Result r = injector::inject(pid, dll, err);
    if (r == injector::Injected) printf("ok\n");
    else printf("%s\n", err.c_str());
    return (int)r;
}
