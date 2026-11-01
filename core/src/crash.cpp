// My Summer Frames core: a crash in the game process is logged (thread, module + offset, stack) and dumped
// (msf.dmp next to the log), so faults of the bridge can be traced without a debugger on the player's machine.
// The faulting thread only hands the exception to a reporter thread (its own stack may be gone: stack overflow);
// the reporter walks the faulting thread's stack from the exception context and writes the dump.
// Copyright (C) 2026 ANICKON
#include "common.h"

#include <dbghelp.h>

namespace msf
{
    namespace
    {
        PVOID handler = nullptr;
        std::atomic<int> reported{0};
        std::wstring dumpPath;
        HANDLE crashEvent = nullptr, doneEvent = nullptr;
        EXCEPTION_POINTERS* crashInfo = nullptr;
        DWORD crashThread = 0;

        // Module unloads are logged: a crash in code that is gone names no module otherwise.
        struct UnicodeString { USHORT Length, MaximumLength; PWSTR Buffer; };
        struct DllNotification { ULONG Flags; const UnicodeString* FullDllName; const UnicodeString* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
        using DllCallback = VOID(CALLBACK*)(ULONG reason, const DllNotification* data, PVOID context);
        using RegisterFn = LONG(NTAPI*)(ULONG flags, DllCallback callback, PVOID context, PVOID* cookie);
        PVOID dllCookie = nullptr;

        VOID CALLBACK OnDll(ULONG reason, const DllNotification* data, PVOID)
        {
            if (reason != 2 || !data || !data->BaseDllName) return; // 2 = unloaded
            std::wstring name(data->BaseDllName->Buffer, data->BaseDllName->Length / sizeof(wchar_t));
            Log("module unloaded: %s at %p..%p", Narrow(name).c_str(), data->DllBase,
                static_cast<void*>(static_cast<char*>(data->DllBase) + data->SizeOfImage));
        }

        std::string Where(DWORD64 address)
        {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module) && module)
            {
                GetModuleFileNameA(module, name, MAX_PATH);
                const char* base = strrchr(name, '\\');
                char out[MAX_PATH + 32];
                snprintf(out, sizeof(out), "%s+0x%llX", base ? base + 1 : name,
                         static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
                return out;
            }
            char out[32];
            snprintf(out, sizeof(out), "0x%llX", static_cast<unsigned long long>(address));
            return out;
        }

        DWORD WINAPI Reporter(LPVOID)
        {
            WaitForSingleObject(crashEvent, INFINITE);
            EXCEPTION_POINTERS* info = crashInfo;
            DWORD code = info->ExceptionRecord->ExceptionCode;
            unsigned long long target = info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
            Log("CRASH: exception 0x%08lX at %s (address 0x%llX) on thread %lu", code,
                Where(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress)).c_str(), target, crashThread);
            HANDLE process = GetCurrentProcess();
            HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, crashThread);
            SymInitialize(process, nullptr, TRUE);
            CONTEXT ctx = *info->ContextRecord;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = ctx.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            DWORD64 last = 0;
            int repeats = 0;
            for (int i = 0; i < 64; i++)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                    break;
                if (frame.AddrPC.Offset == last) { if (++repeats > 2) continue; }
                else repeats = 0;
                last = frame.AddrPC.Offset;
                Log("CRASH:   #%02d %s", i, Where(frame.AddrPC.Offset).c_str());
            }
            if (!dumpPath.empty())
            {
                HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    MINIDUMP_EXCEPTION_INFORMATION mei = {crashThread, info, FALSE};
                    MiniDumpWriteDump(process, GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo), &mei, nullptr,
                                      nullptr);
                    CloseHandle(file);
                    Log("CRASH: dump written");
                }
            }
            if (thread) CloseHandle(thread);
            SetEvent(doneEvent);
            return 0;
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            DWORD code = info->ExceptionRecord->ExceptionCode;
            // Only the faults that end the process; C++ exceptions, debug output etc. are handled elsewhere.
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
                code != STATUS_HEAP_CORRUPTION && code != EXCEPTION_PRIV_INSTRUCTION)
                return EXCEPTION_CONTINUE_SEARCH;
            if (reported.fetch_add(1) != 0) return EXCEPTION_CONTINUE_SEARCH;
            crashInfo = info;
            crashThread = GetCurrentThreadId();
            SetEvent(crashEvent);
            WaitForSingleObject(doneEvent, 8000);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // Game exit: the game's own teardown (Unity unloads d3d11.dll while a thread of the D3D11 runtime still runs,
    // a first-chance fault the process ignores) is not ours to report.
    void UninstallCrashLog()
    {
        if (handler) RemoveVectoredExceptionHandler(handler);
        handler = nullptr;
    }

    void InstallCrashLog(const std::wstring& folder)
    {
        dumpPath = folder + L"\\msf.dmp";
        if (handler) return;
        crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE t = CreateThread(nullptr, 1 << 20, Reporter, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        handler = AddVectoredExceptionHandler(1, OnException);
        auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        if (reg) reg(0, OnDll, nullptr, &dllCookie);
    }
}
