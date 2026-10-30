#pragma once

#include <windows.h>
#include <atomic>

namespace ExerciseUnlock::Svc
{
    // Background thread that polls Config::UpdateManifestUrl() every
    // Config::UpdateIntervalHours() hours (plus one check at start, and
    // sooner again after a check the server didn't answer).
    // On a new version: writes update info into State and fires a
    // toast in the active user session (once per version — repeated
    // polls and service restarts with the same latest-version are
    // silent).
    class UpdateChecker
    {
    public:
        void Start(HANDLE stopEvent);
        void Join();

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();
        bool CheckOnce();   // false: no answer from the server

        HANDLE m_stopEvent{ nullptr };
        HANDLE m_thread{ nullptr };
    };
}
