#pragma once

#include <windows.h>
#include <atomic>

namespace ExerciseUnlock::Svc
{
    // Background thread that polls Config::UpdateManifestUrl() every
    // Config::UpdateIntervalHours() hours (plus one check at start, and
    // sooner again after a check the server didn't answer).
    // On a new version: writes update info into State and opens the
    // update window (ExerciseUnlockUpdate.exe) for whoever is at the
    // console, after a check and when someone signs in or unlocks:
    //   * an administrator: "Download and install" - once per version,
    //     again about a day later if it still isn't installed;
    //   * anyone else (the child): "ask a parent", once per version.
    // Repeated polls and service restarts in between are silent.
    class UpdateChecker
    {
    public:
        void Start(HANDLE stopEvent);
        void Join();
        // Someone signed in, unlocked or switched to the console
        // (SERVICE_CONTROL_SESSIONCHANGE): offer the update to them.
        void Wake();

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();
        bool CheckOnce();   // false: no answer from the server

        HANDLE m_stopEvent{ nullptr };
        HANDLE m_wake{ nullptr };
        HANDLE m_thread{ nullptr };
    };
}
