#pragma once

#include <windows.h>
#include <atomic>

namespace ExerciseUnlock::Svc
{
    // Phone camera via the relay server ([phone] relay in config.ini).
    //
    // While the lock screen is up (same rule as the webcam, see
    // CameraWantedNow) this keeps a WebSocket to <relay>/ws/pc and offers a
    // pairing token; the lock-screen QR window shows <relay>/S/<TOKEN>. A
    // phone that opens it gets the exercises and calibrated thresholds,
    // counts reps itself and reports "+1 squat"; reps are credited here
    // under the same rules as webcam reps (schedule, no running session,
    // not faster than the rep timing allows). On unlock the phone is told
    // and the connection closes.
    class PhoneLink
    {
    public:
        // stopEvent is borrowed: the caller keeps it alive until Join.
        void Start(HANDLE stopEvent);
        void Join();
        // Re-check the lock screen now (session change).
        void Wake();

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();

        HANDLE m_stopEvent = nullptr;
        HANDLE m_thread    = nullptr;
        std::atomic<HANDLE> m_wakeEvent{ nullptr };
    };
}
