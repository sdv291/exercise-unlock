#pragma once

#include <windows.h>
#include <atomic>

namespace ExerciseUnlock::Svc
{
    // Runs an unlock session after the child logs in via our credential
    // provider:
    //   * arms a countdown for the earned seconds,
    //   * pops WTSSendMessage warnings at 10 / 5 / 1 minute left,
    //   * calls LockWorkStation() in the user's session when time is up.
    //
    // Not persistent yet — a service crash / reboot loses the session.
    // v0.9+ writes it to C:\ProgramData\ExerciseUnlock\session.dat.
    class SessionManager
    {
    public:
        void Start(HANDLE stopEvent);
        void Join();

        // Called from the pipe handler when the child signs in and
        // GetSerialization succeeds. Arms the timer for `seconds`
        // measured from now. Overwrites any running session.
        void ArmSession(uint32_t seconds);

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();

        HANDLE m_stopEvent{ nullptr };
        HANDLE m_thread{ nullptr };
        HANDLE m_wakeEvent{ nullptr };
    };
}
