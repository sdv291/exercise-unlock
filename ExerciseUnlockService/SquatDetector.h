#pragma once

#include "Protocol.h"
#include "PoseEstimator.h"
#include "AngleFilter.h"

#include <deque>

namespace ExerciseUnlock::Svc
{
    // Consumes BodyPose frames from the capture loop, tracks the knee
    // angle over time as a state machine, and writes the running rep
    // count / current phase / last angle into State.
    //
    // v0.5 owns just squats. PushUpDetector (v0.6) will mirror this
    // shape for elbow angles + torso alignment.
    class SquatDetector
    {
    public:
        // Push one pose frame through the state machine. Cheap enough
        // to call on every camera frame; the phase transitions are
        // gated by both angle thresholds and dwell time so a jitter
        // pose won't false-count.
        void Update(const BodyPose& pose);

        // Explicitly reset the counter and phase — used when a new
        // challenge starts (v0.7+).
        void Reset();

    private:
        Ipc::SquatPhase m_phase{ Ipc::SquatPhase::Unknown };
        uint32_t        m_count{ 0 };
        uint64_t        m_lastDownTickMs{ 0 };
        uint64_t        m_lastStandingTickMs{ 0 };
        // Set when we leave Standing on the way down. Used to time
        // the whole rep (start of descent -> back at standing) against
        // Config's [min..max] rep window; 0 = no valid start yet
        // (first frames after a Reset, or seeded mid-cycle).
        uint64_t        m_repStartTickMs{ 0 };
        uint64_t        m_lastDebugLogMs{ 0 };
        float           m_smoothedRatio{ 0.0f };  // EMA of the metric
        bool            m_smoothedInit{ false };
        int             m_missStreak{ 0 };
        // Deepest smoothed angle seen during the current descent —
        // used to compute per-rep quality (how far into the "down"
        // band the rep actually got).
        float           m_repMinAngle{ 999.0f };
        // Hip X at the moment the descent began (-1 = unknown). Used
        // to reject reps where the hip drifted across the frame
        // between descent and rep completion — i.e. walking, not
        // squatting. In normalized frame coordinates (0..1).
        float           m_repStartHipX{ -1.0f };
        // Count of GoingUp → Down phase reversals during the current
        // rep. A legit squat ascends monotonically; a MoveNet-noise
        // "rep" bounces back into Down one or more times mid-ascent.
        // Reps completing with oscillations > 0 are dropped.
        int             m_repOscillations{ 0 };
        // Peak |leftKneeAngle - rightKneeAngle| observed while the
        // rep was in flight. A real two-leg squat keeps the two
        // knees roughly synchronized (asymmetry < ~25°). Only
        // tracked on sides=2 frames; was ineffective on its own
        // for knee lifts because at the moment of maximum bend
        // only the moving leg was confident. Kept as a cheap
        // secondary signal; hip-drop below is the primary gate.
        float           m_repMaxAsymmetry{ 0.0f };
        // Hip Y position when the descent began (-1 = unknown).
        // In normalized frame coords (0=top, 1=bottom); a real
        // squat makes the hip DROP by 0.10..0.20 as the person
        // folds down. A knee lift leaves the torso upright, so
        // hip Y moves <0.03. Comparing peak hip Y during the
        // rep against this start value separates "person actually
        // squatted" from "person stayed standing and moved a leg".
        float           m_repStartHipY{ -1.0f };
        // Peak (largest) hip Y observed during the rep — i.e.,
        // how far down the hip got.
        float           m_repMaxHipY{ -1.0f };
        // Median + 2σ outlier filter on the raw per-frame knee
        // angle. Chained upstream of the EMA so a single-frame
        // MoveNet spike (raw 170→50→170 in one frame) is dropped
        // outright instead of leaking into the smoothed value.
        AngleMedianFilter m_angleFilter{};
        // Rolling buffer of recent hip Y values (one per frame).
        // Used to pick a reliable "pre-descent" baseline when a
        // rep starts: the state machine's Standing→GoingDown
        // transition lags the actual motion by a few frames
        // (median+EMA smoothing), so curHipY at the transition
        // moment is already partway down. Taking the MIN of the
        // last ~1 s of hip Y gives the true standing-level
        // reference instead. Size matched to ~1 s of frames.
        static constexpr size_t kHipHistoryMax = 12;
        std::deque<float>       m_hipHistory;
    };
}
