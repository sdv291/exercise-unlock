#pragma once

#include <windows.h>
#include <atomic>

namespace ExerciseUnlock::Svc
{
    // Background thread that polls Config::UpdateManifestUrl() every
    // Config::UpdateIntervalHours() hours (plus one check at start).
    // On a new version: writes update info into State and fires a
    // toast in the active user session (once per version — repeated
    // polls of the same latest-version are silent).
    class UpdateChecker
    {
    public:
        void Start(HANDLE stopEvent);
        void Join();

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();
        void CheckOnce();

        HANDLE m_stopEvent{ nullptr };
        HANDLE m_thread{ nullptr };
    };
}
