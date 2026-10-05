#pragma once

#include "Protocol.h"
#include "PoseEstimator.h"
#include "AngleFilter.h"

namespace ExerciseUnlock::Svc
{
    // Push-up counter. Same shape as SquatDetector — the pose-per-frame
    // thread pushes one BodyPose in per call, the detector runs a
    // state machine on the elbow angle, gates on a plank check, and
    // writes the running rep count and phase into State.
    //
    // Camera assumed to be in profile (side view). Front camera
    // won't work — same 2D projection limitation we hit for squats.
    class PushUpDetector
    {
    public:
        void Update(const BodyPose& pose);
        void Reset();

    private:
        Ipc::PushUpPhase m_phase{ Ipc::PushUpPhase::Unknown };
        uint32_t         m_count{ 0 };
        uint64_t         m_lastDownTickMs{ 0 };
        uint64_t         m_lastUpTickMs{ 0 };
        // Set when we leave Up on the way down; used to time the
        // whole rep against Config's [min..max] window.
        uint64_t         m_repStartTickMs{ 0 };
        uint64_t         m_lastDebugLogMs{ 0 };
        float            m_smoothedElbow{ 0.0f };
        float            m_smoothedPlank{ 0.0f };
        bool             m_smoothedInit{ false };
        int              m_missStreak{ 0 };
        // Min/max raw elbow seen since last DBG log.
        float            m_windowMinRawElbow{ 999.0f };
        float            m_windowMaxRawElbow{ -1.0f };
        // Deepest smoothed elbow angle observed during current rep.
        float            m_repMinElbow{ 999.0f };
        // Median + 2σ outlier filter on raw per-frame elbow angle.
        // Same role as SquatDetector's m_angleFilter — kills
        // single-frame MoveNet spikes before they reach EMA.
        AngleMedianFilter m_elbowFilter{};
    };
}
