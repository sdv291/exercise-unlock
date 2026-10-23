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
    // v0.9+ writes it to <DataDir>\session.dat.
    class SessionManager
    {
    public:
        void Start(HANDLE stopEvent);
        void Join();

        // Called from the pipe handler when the child signs in and
        // GetSerialization succeeds. Arms the timer for `seconds`
        // measured from now. Overwrites any running session.
        //   Earned — child's own time: also cut off at the end of
        //            today's [schedule] window (e.g. 20:00).
        //   Grace  — short close-your-apps unlock: allowed past the
        //            window end, never re-opens the grace window.
        //   Parent — Op::Grant override: only the hard caps apply.
        enum class Kind { Earned, Grace, Parent };
        void ArmSession(uint32_t seconds, Kind kind = Kind::Earned);

        // Called on WTS_SESSION_LOCK. If a session is running, stops
        // the countdown and parks the remaining seconds in
        // State::pausedSessionSeconds so they don't burn away on the
        // lock screen; the next unlock resumes them.
        void PauseOnLock();

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();

        HANDLE m_stopEvent{ nullptr };
        HANDLE m_thread{ nullptr };
        HANDLE m_wakeEvent{ nullptr };
    };
}
