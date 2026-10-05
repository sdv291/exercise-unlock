#pragma once

#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

// Lightweight debug logging. LogonUI hosts the DLL in a separate session
// that is hard to attach a debugger to, so we tee output to:
//   * OutputDebugStringW (viewable via DebugView with "Capture Global
//     Win32" on, if a developer is actively watching), and
//   * C:\Users\Public\exerciseunlock-provider.log — world-writable so
//     the SYSTEM-hosted LogonUI session can append to it, and visible
//     from any user account for post-mortem checking. Each line is
//     prefixed with a local timestamp so audit against service.log
//     is straightforward.
namespace ExerciseUnlock
{
    inline void LogWriteProviderFile(const wchar_t* line)
    {
        HANDLE f = CreateFileW(
            L"C:\\Users\\Public\\exerciseunlock-provider.log",
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t stamped[700]{};
        _snwprintf_s(stamped, _TRUNCATE,
                     L"%04u-%02u-%02uT%02u:%02u:%02u.%03u  %s\r\n",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     line);

        int need = WideCharToMultiByte(CP_UTF8, 0, stamped, -1,
                                       nullptr, 0, nullptr, nullptr);
        if (need > 1)
        {
            std::string utf8(need - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, stamped, -1,
                                utf8.data(), need - 1, nullptr, nullptr);
            DWORD wrote = 0;
            WriteFile(f, utf8.data(),
                      static_cast<DWORD>(utf8.size()), &wrote, nullptr);
        }
        CloseHandle(f);
    }

    inline void LogF(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        int n = _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        if (n < 0)
        {
            return;
        }
        OutputDebugStringW(L"[ExerciseUnlockProvider] ");
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\n");
        LogWriteProviderFile(buf);
    }
}
