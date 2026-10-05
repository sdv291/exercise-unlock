#pragma once

#include <windows.h>
#include <atomic>

namespace ExerciseUnlock::Svc
{
    class PoseEstimator;

    // Background thread that owns the webcam. Whenever the camera is
    // wanted it does two phases back to back:
    //   1. Probe: open, decode a few frames, publish WebcamState so we
    //      know the pipeline is usable (v0.3 behaviour).
    //   2. Stream: keep the device open, request RGB32, feed every
    //      frame through PoseEstimator, publish BodyPose.
    // "Wanted" = no unlock session running AND (with [camera]
    // lock_screen_only=1, the default) the console is showing the
    // lock / sign-in screen. Otherwise the device is closed (LED off)
    // and WebcamState reads Idle.
    class WebcamProbe
    {
    public:
        // Non-owning: pose estimator must outlive this object.
        void Bind(PoseEstimator* pose) { m_pose = pose; }

        // Launches the worker thread. stopEvent is borrowed (not
        // duplicated / closed) — the caller owns its lifetime and must
        // keep it alive until Join returns.
        void Start(HANDLE stopEvent);

        // Blocks until the worker exits. Safe to call multiple times.
        void Join();

        // Re-evaluate "is the camera wanted" right now instead of at
        // the next 1 s poll. Called from the SCM handler on every
        // session change (lock / unlock / logon / logoff).
        void Wake();

    private:
        static DWORD WINAPI Thunk(LPVOID param);
        void Run();

        PoseEstimator* m_pose      = nullptr;
        HANDLE         m_stopEvent = nullptr;
        HANDLE         m_thread    = nullptr;
        // Auto-reset. Lives for the whole process: the SCM handler may
        // call Wake() at any point, even while the service is stopping.
        std::atomic<HANDLE> m_wakeEvent{ nullptr };
    };
}
