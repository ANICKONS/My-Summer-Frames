// My Summer Frames core: log file + lines for the MSCLoader console.
// Copyright (C) 2026 ANICKON
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <share.h>

namespace msf
{
    namespace
    {
        std::mutex logLock;
        FILE* file = nullptr;
        std::string pending; // console lines not yet taken by the mod
        std::string early;   // lines written before the file is open (UnitySetGraphicsDevice comes first)
        constexpr size_t kPendingMax = 64 * 1024;

        // console: 0 file only, '~' a detail (the mod shows it with the detailed log only), '!' a problem (always shown).
        void Write(char console, const char* fmt, va_list args)
        {
            char text[2048];
            vsnprintf(text, sizeof(text), fmt, args);
            SYSTEMTIME t;
            GetLocalTime(&t);
            char line[2200];
            snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, GetCurrentThreadId(), text);
            std::lock_guard<std::mutex> guard(logLock);
            if (file)
            {
                fputs(line, file);
                fflush(file);
            }
            else if (early.size() < kPendingMax) early += line;
            if (console && pending.size() < kPendingMax)
            {
                pending += console;
                pending += text;
                pending += '\n';
            }
        }
    }

    void LogOpen(const std::wstring& path)
    {
        std::lock_guard<std::mutex> guard(logLock);
        if (file) return;
        std::wstring previous = path + L".prev";
        MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
        if (file && !early.empty())
        {
            fputs(early.c_str(), file);
            fflush(file);
            early.clear();
        }
    }

    void Log(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write(0, fmt, args);
        va_end(args);
    }

    void LogC(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write('~', fmt, args);
        va_end(args);
    }

    void LogE(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write('!', fmt, args);
        va_end(args);
    }

    int LogDrain(char* buffer, int length)
    {
        std::lock_guard<std::mutex> guard(logLock);
        if (!buffer || length <= 1 || pending.empty()) return 0;
        // Whole lines only, so a line is never split between two calls.
        size_t take = pending.size();
        if (take > static_cast<size_t>(length - 1))
        {
            size_t cut = pending.rfind('\n', static_cast<size_t>(length - 2));
            take = cut == std::string::npos ? static_cast<size_t>(length - 1) : cut + 1;
        }
        memcpy(buffer, pending.data(), take);
        buffer[take] = 0;
        pending.erase(0, take);
        return static_cast<int>(take);
    }

    std::string Narrow(const std::wstring& text)
    {
        if (text.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }
}
