#include "ServiceMain.h"
#include "Log.h"

#include <windows.h>
#include <cstdio>
#include <cwchar>

using namespace ExerciseUnlock::Svc;

// Console/CLI mode. Handy for `sc create` before installing as a real
// service, and for iterating on the pipe protocol without dealing with
// SCM latency. Ctrl+C stops the server.
static HANDLE g_consoleStopEvent = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl)
{
    switch (ctrl)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        LogF(L"console stop requested");
        if (g_consoleStopEvent) SetEvent(g_consoleStopEvent);
        return TRUE;
    default:
        return FALSE;
    }
}

static int RunConsole()
{
    g_consoleMode = true;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_consoleStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_consoleStopEvent)
    {
        fwprintf(stderr, L"CreateEvent failed: %lu\n", GetLastError());
        return 1;
    }

    LogF(L"running in --console mode. Press Ctrl+C to stop.");
    RunUntilStopped(g_consoleStopEvent);

    CloseHandle(g_consoleStopEvent);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--console") == 0 ||
            _wcsicmp(argv[i], L"-c") == 0)
        {
            return RunConsole();
        }
        if (_wcsicmp(argv[i], L"--help") == 0 ||
            _wcsicmp(argv[i], L"-h") == 0 ||
            _wcsicmp(argv[i], L"/?") == 0)
        {
            fwprintf(stdout,
                L"ExerciseUnlockService\n"
                L"  --console   run in the current console (Ctrl+C to stop)\n"
                L"  (no args)   registered SCM entry point; run via `sc start`\n");
            return 0;
        }
    }

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table))
    {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        {
            fwprintf(stderr,
                L"ExerciseUnlockService was launched outside the SCM.\n"
                L"Use --console for interactive mode, or install via:\n"
                L"    sc create ExerciseUnlockService binPath= \"<full path>\" start= auto\n"
                L"    sc start ExerciseUnlockService\n");
            return 1;
        }
        fwprintf(stderr, L"StartServiceCtrlDispatcher failed: %lu\n", err);
        return 1;
    }

    return 0;
}
