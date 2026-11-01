// Nearest exported function at or below an RVA of a system DLL: near_export.exe ntdll.dll 0x14fc26 0xa3810
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#pragma comment(lib, "dbghelp.lib")

int main(int argc, char** argv)
{
    if (argc < 3) return 1;
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) return 2;
    ULONG size = 0;
    auto* dir = static_cast<IMAGE_EXPORT_DIRECTORY*>(ImageDirectoryEntryToData(m, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &size));
    auto* base = reinterpret_cast<BYTE*>(m);
    auto* names = reinterpret_cast<DWORD*>(base + dir->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(base + dir->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(base + dir->AddressOfFunctions);
    for (int a = 2; a < argc; a++)
    {
        DWORD rva = strtoul(argv[a], nullptr, 16);
        DWORD best = 0;
        const char* bestName = "?";
        for (DWORD i = 0; i < dir->NumberOfNames; i++)
        {
            DWORD f = funcs[ords[i]];
            if (f <= rva && f > best) { best = f; bestName = reinterpret_cast<const char*>(base + names[i]); }
        }
        printf("%s+0x%lX = %s+0x%lX\n", argv[1], rva, bestName, rva - best);
    }
    return 0;
}
