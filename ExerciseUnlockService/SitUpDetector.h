#pragma once

#include "Protocol.h"
#include "PoseEstimator.h"
#include "AngleFilter.h"
#include "RepResult.h"
#include "DetectorParams.h"

namespace ExerciseUnlock::Svc
{
    // Sit-up (crunch) counter. Same shape as PushUpDetector: one BodyPose
    // per call, a state machine on one angle, gated on the posture
    // classifier, the running count and phase written into State.
    //
    // Camera in profile. The angle is the hip angle shoulder-hip-knee:
    // large (~130 deg) lying on the back with the knees bent, small
    // (~50-70) sitting up. Lying is the rest position, so the angle runs
    // "top" (lying) -> "down" (sitting up) -> "top" like the other two
    // exercises, and the calibration check treats it the same way.
    class SitUpDetector
    {
    public:
        // publish (the webcam's): Config's thresholds, the tick clock, and
        // phase / angle / the shared count written into State. A phone's
        // (publish = false, PhoneCounter) gets its clock, posture and
        // thresholds with every frame and keeps its results to itself -
        // see LastRep().
        explicit SitUpDetector(bool publish = true) : m_publish(publish) {}

        void Update(const BodyPose& pose);
        // A phone's frame: postureOk = its own classifier latched SitUp.
        void Update(const BodyPose& pose, uint64_t nowMs, bool postureOk, const SitupParams& p);
        void Reset();

        // The last finished attempt (counted or dropped, and why).
        const RepResult& LastRep() const { return m_lastRep; }

    private:
        void Step(const BodyPose& pose, uint64_t now, bool postureOk, const SitupParams& p, bool offHours);

        bool             m_publish = true;
        RepResult        m_lastRep{};
        Ipc::SitUpPhase  m_phase{ Ipc::SitUpPhase::Unknown };
        uint64_t         m_repStartTickMs{ 0 };
        uint64_t         m_lastDebugLogMs{ 0 };
        float            m_smoothedHip{ 0.0f };
        bool             m_smoothedInit{ false };
        int              m_missStreak{ 0 };
        // Smallest smoothed hip angle of the current rep (how far up).
        float            m_repMinHip{ 999.0f };
        AngleMedianFilter m_hipFilter{};
    };
}
