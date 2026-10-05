#pragma once

#include "Protocol.h"
#include "PoseEstimator.h"

namespace ExerciseUnlock::Svc
{
    // Classifies the user's standing posture each frame as Squat
    // (vertical, body stacked top-to-bottom), PushUp (horizontal,
    // head-to-feet span wide across the frame), or None. Hands/wrists/
    // elbows are deliberately ignored — the child may stretch their
    // arms forward during a squat, so only the torso + legs count for
    // the vertical check; only the head and ankles for the horizontal
    // one (via the span across the frame).
    //
    // The classifier is deliberately stateful: a raw per-frame answer
    // is noisy, so it latches a Ready kind only after N consecutive
    // frames agree. The provider's lock-screen tile watches for the
    // None → Squat / None → PushUp transition and plays a short beep
    // then — the child treats the beep as "detector sees you, start
    // the rep now".
    class PostureClassifier
    {
    public:
        // One pose frame through the classifier. Updates State::
        // postureKind only when the latched decision changes.
        void Update(const BodyPose& pose);

        // Explicitly drop the latch — used when the camera loses the
        // person entirely or at session boundaries.
        void Reset();

    private:
        // Frames required to LATCH a non-None kind. At ~10 Hz pose
        // this is ~0.6 s — short enough that the beep feels responsive
        // when the child steps into frame.
        static constexpr int kStableFrames = 6;
        // Frames required to UNLATCH back to None once a kind is held.
        // Deliberately larger than kStableFrames: a real squat momentarily
        // breaks the "stacked" geometry at the bottom of the rep
        // (knees can rise near hip height, confidence dips during
        // fast motion). Without this asymmetry the latch flapped
        // ~1 Hz through a single rep and the gated detector reset
        // its state every time, losing every rep (observed
        // 2026-10-01 lock-screen test: 5 squats, 0 counted).
        static constexpr int kUnlatchFrames = 20;

        Ipc::PostureKind m_latched{ Ipc::PostureKind::None };
        Ipc::PostureKind m_pending{ Ipc::PostureKind::None };
        int              m_pendingStreak{ 0 };
        uint64_t         m_lastDebugLogMs{ 0 };
    };
}
