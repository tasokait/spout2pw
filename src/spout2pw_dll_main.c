/*
 * spout2pw_dll_main.c
 *
 * DllMain for spout2pw.dll (the Wine fake DLL).
 *
 * When Wine loads a fake DLL (one with the "Wine placeholder DLL" marker),
 * it forces LO_BUILTIN loadorder and calls find_builtin_dll(), which searches
 * WINEDLLPATH for a matching .so unixlib and registers the module in
 * builtin_modules. This allows __wine_init_unix_call() to succeed when called
 * from this DllMain, because the caller's module base (spout2pw.dll) is now
 * present in builtin_modules.
 *
 * The resulting global __wine_unixlib_handle is then available for subsequent
 * calls from spout2pw.exe (which is NOT in builtin_modules and would otherwise
 * fail with STATUS_DLL_NOT_FOUND). A failed NtQueryVirtualMemory lookup from
 * spout2pw.exe preserves the handle already set by this DllMain (due to the
 * `if (!status)` guard in ntdll's MemoryWineUnixFuncs handling).
 */

#include <windows.h>
#include <stdio.h>

#include "ntstatus.h"
#include "wine/unixlib.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        /*
         * Ensure the unixlib handle is loaded while we are still a registered
         * builtin module. This must happen before spout2pw.exe's ServiceMain
         * calls any UNIX_CALL macros.
         */
        __wine_init_unix_call();
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
