// version.h — single source of truth for the tool version. Shared by swhook.dll, inject.exe and the
// GUI/CLI clients so a mismatch between the DLL and its controller can be detected over IPC.
// Bump on every user-facing release; the zip name and the log header both derive from it.
#pragma once
#define SWHOOK_VERSION_MAJOR 0
#define SWHOOK_VERSION_MINOR 2
#define SWHOOK_VERSION_PATCH 2
#define SWHOOK_VERSION      "0.2.2"
#define SWHOOK_IPC_PROTOCOL 1        // bump when a command's request/response shape changes
