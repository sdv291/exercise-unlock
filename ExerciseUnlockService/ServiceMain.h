#pragma once

#include <windows.h>

namespace ExerciseUnlock::Svc
{
    class SessionManager;

    inline constexpr wchar_t kServiceName[]        = L"ExerciseUnlockService";
    inline constexpr wchar_t kServiceDisplayName[] = L"Exercise Unlock Service";

    // Entry point registered with StartServiceCtrlDispatcher.
    void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);

    // Runs the pipe server on the current thread. Used both by
    // ServiceMain (as a real service) and by --console mode.
    // NO_ERROR after a requested stop; an error code when it had to give
    // up, which the service reports so the SCM's failure actions restart it.
    DWORD RunUntilStopped(HANDLE stopEvent);

    // Global singleton owned by ServiceMain — the pipe handler calls
    // ArmSession() on it when the child successfully unlocks.
    SessionManager& GetSessionManager();
}
