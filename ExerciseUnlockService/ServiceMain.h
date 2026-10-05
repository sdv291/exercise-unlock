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
    void RunUntilStopped(HANDLE stopEvent);

    // Global singleton owned by ServiceMain — the pipe handler calls
    // ArmSession() on it when the child successfully unlocks.
    SessionManager& GetSessionManager();
}
