#pragma once

#include <windows.h>
#include <cstdio>
#include <cwchar>

// Debug logging: writes to stdout in --console mode, to
// OutputDebugString either way (so DebugView picks it up when we run
// as a Windows service under the SCM), and appended to a rolling
// service.log next to activity.log in %ProgramData%\ExerciseUnlock\.
namespace ExerciseUnlock::Svc
{
    inline bool g_consoleMode = false;

    // Implemented in Log.cpp. Append one line to service.log with a
    // timestamp; safe to call from any thread.
    void LogWriteToFile(const wchar_t* text);
    // Same but always writes to the "trace" file at
    // C:\Users\Public\eu-service-trace.log — used as an unmistakable
    // canary when service.log misbehaves for any reason.
    void LogWriteToTraceFile(const wchar_t* text);

    // Truncate service.log and the trace file to zero bytes. Called
    // once at the start of RunUntilStopped so each service start begins
    // with a clean log and the parent only sees reps/events from the
    // current run. Also recreates the files with the correct SD so a
    // stale sdv29-only-ACL leftover is replaced in one shot.
    void LogResetFiles();

    inline void LogF(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        int n = _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        // Even on truncation (_vsnwprintf_s returns -1) the buffer is
        // still terminated, so keep logging — better a partial line
        // than a silent drop.

        OutputDebugStringW(L"[ExerciseUnlockService] ");
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\n");

        if (g_consoleMode)
        {
            fwprintf(stdout, L"[svc] %ls\n", buf);
            fflush(stdout);
        }

        LogWriteToFile(buf);
        LogWriteToTraceFile(buf);
        (void)n;
    }
}
