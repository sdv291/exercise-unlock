#include "SitUpDetector.h"
#include "State.h"
#include "Log.h"
#include "PersistentState.h"
#include "Config.h"

#include <cmath>
#include <windows.h>

using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Svc
{
    // MoveNet / COCO joint indices: the hip angle shoulder-hip-knee.
    static constexpr int kLeftShoulder  = 5;
    static constexpr int kRightShoulder = 6;
    static constexpr int kLeftHip       = 11;
    static constexpr int kRightHip      = 12;
    static constexpr int kLeftKnee      = 13;
    static constexpr int kRightKnee     = 14;

    // Lying on the floor in profile the far side is often half hidden;
    // the same gate as the classifier keeps the near side usable.
    static constexpr float kJointConfMin = 0.20f;

    // Same smoothing as the other detectors.
    static constexpr float kEmaAlpha = 0.70f;

    template <typename J>
    static float AngleAt(const J& a, const J& b, const J& c)
    {
        const float ax = a.x - b.x, ay = a.y - b.y;
        const float cx = c.x - b.x, cy = c.y - b.y;
        const float dot = ax * cx + ay * cy;
        const float mag = std::sqrt(ax * ax + ay * ay) *
                          std::sqrt(cx * cx + cy * cy);
        if (mag < 1e-6f) return 0.0f;
        float cosine = dot / mag;
        if (cosine >  1.0f) cosine =  1.0f;
        if (cosine < -1.0f) cosine = -1.0f;
        return std::acos(cosine) * (180.0f / 3.14159265358979323846f);
    }

    template <typename J>
    static bool ConfidentTriplet(const J& a, const J& b, const J& c)
    {
        return a.confidence >= kJointConfMin &&
               b.confidence >= kJointConfMin &&
               c.confidence >= kJointConfMin;
    }

    void SitUpDetector::Reset()
    {
        m_phase          = SitUpPhase::Unknown;
        m_repStartTickMs = 0;
        m_smoothedHip    = 0.0f;
        m_smoothedInit   = false;
        m_missStreak     = 0;
        m_repMinHip      = 999.0f;
        m_hipFilter.Reset();
        m_lastRep        = RepResult{};

        auto& st = GetState();
        st.sitUpCount.store(0);
        st.sitUpPhase.store(static_cast<uint8_t>(SitUpPhase::Unknown));
        st.sitUpHipAngle.store(0.0f);
    }

    void SitUpDetector::Update(const BodyPose& pose)
    {
        auto& st = GetState();

        // Freeze during an active unlock session; outside the schedule
        // window recognize reps but credit nothing — same rationale as
        // SquatDetector.
        if (IsSessionActive())
        {
            st.sitUpPhase.store(static_cast<uint8_t>(SitUpPhase::Unknown));
            return;
        }
        const bool offHours = !Config::IsWithinActiveWindowNow();

        // Only while the classifier has latched SitUp (on the back, knees
        // up). Skip rather than reset — its unlatch hysteresis carries a
        // real rep through confidence dips.
        if (st.postureKind.load() != static_cast<uint8_t>(PostureKind::SitUp))
        {
            return;
        }

        // Hip angle: average of the confident sides.
        float sum = 0.0f;
        int   n   = 0;
        if (ConfidentTriplet(pose.joints[kLeftShoulder], pose.joints[kLeftHip], pose.joints[kLeftKnee]))
        {
            sum += AngleAt(pose.joints[kLeftShoulder], pose.joints[kLeftHip], pose.joints[kLeftKnee]);
            ++n;
        }
        if (ConfidentTriplet(pose.joints[kRightShoulder], pose.joints[kRightHip], pose.joints[kRightKnee]))
        {
            sum += AngleAt(pose.joints[kRightShoulder], pose.joints[kRightHip], pose.joints[kRightKnee]);
            ++n;
        }
        if (n == 0)
        {
            // Same ~0.5 s tolerance for missed frames as the others.
            if (++m_missStreak >= 20)
            {
                m_phase = SitUpPhase::Unknown;
                m_smoothedInit = false;
                m_repStartTickMs = 0;
                st.sitUpPhase.store(static_cast<uint8_t>(SitUpPhase::Unknown));
            }
            return;
        }
        m_missStreak = 0;

        // Median + 2σ outlier filter, then EMA.
        const float raw = m_hipFilter.Push(sum / static_cast<float>(n));
        if (!m_smoothedInit)
        {
            m_smoothedHip  = raw;
            m_smoothedInit = true;
        }
        else
        {
            m_smoothedHip = kEmaAlpha * raw + (1.0f - kEmaAlpha) * m_smoothedHip;
        }
        const float hip = m_smoothedHip;
        st.sitUpHipAngle.store(hip);

        if (m_phase == SitUpPhase::Rising || m_phase == SitUpPhase::Up ||
            m_phase == SitUpPhase::Lowering)
        {
            if (hip < m_repMinHip) m_repMinHip = hip;
        }

        const uint64_t now = GetTickCount64();
        const float kLying    = Config::SitupLyingAngle();
        const float kRising   = Config::SitupRisingAngle();
        const float kLowering = Config::SitupLoweringAngle();
        const float kSitting  = Config::SitupSittingAngle();
        const uint64_t kMinRepMs = static_cast<uint64_t>(Config::SitupMinRepMs());
        const uint64_t kMaxRepMs = static_cast<uint64_t>(Config::SitupMaxRepMs());

        if (now - m_lastDebugLogMs >= 1000)
        {
            m_lastDebugLogMs = now;
            LogF(L"situp DBG smooth=%.1f raw=%.1f sides=%d phase=%u",
                 hip, raw, n, static_cast<unsigned>(m_phase));
        }

        // Seed on the first usable frame.
        if (m_phase == SitUpPhase::Unknown)
        {
            if (hip >= kLowering)
            {
                m_phase = SitUpPhase::Lying;
            }
            else
            {
                // Already up: count this as a rep started now.
                m_phase = SitUpPhase::Up;
                m_repStartTickMs = now;
            }
            st.sitUpPhase.store(static_cast<uint8_t>(m_phase));
            return;
        }

        switch (m_phase)
        {
        case SitUpPhase::Lying:
            if (hip < kRising)
            {
                m_phase = SitUpPhase::Rising;
                m_repStartTickMs = now;   // the rep is timed from here
            }
            break;

        case SitUpPhase::Rising:
            if (hip <= kSitting)
            {
                m_phase = SitUpPhase::Up;
            }
            else if (hip >= kLying)
            {
                // Lay back down before sitting up far enough. No rep.
                RepResult r;
                r.seq         = m_lastRep.seq + 1;
                r.reachedDown = false;
                r.deepest     = m_repMinHip;
                r.durMs       = m_repStartTickMs ? static_cast<uint32_t>(now - m_repStartTickMs) : 0;
                m_lastRep     = r;
                m_repMinHip   = 999.0f;
                m_phase = SitUpPhase::Lying;
                m_repStartTickMs = 0;
            }
            break;

        case SitUpPhase::Up:
            if (hip > kLowering)
            {
                m_phase = SitUpPhase::Lowering;
            }
            break;

        case SitUpPhase::Lowering:
            if (hip >= kLying)
            {
                const uint64_t dur = m_repStartTickMs ? (now - m_repStartTickMs) : 0;
                // Quality: how far up the rep went, lying -> sitting = 0..1.
                const float span  = kLying - kSitting;
                const float depth = kLying - m_repMinHip;
                float quality = (span > 1.0f) ? (depth / span) : 1.0f;
                if (quality < 0.0f) quality = 0.0f;
                if (quality > 1.0f) quality = 1.0f;
                const float qMin = Config::SitupQualityMin();
                const bool timingOk  = (dur >= kMinRepMs && dur <= kMaxRepMs);
                const bool qualityOk = (qMin <= 0.0f) || (quality >= qMin);
                const bool counted   = timingOk && qualityOk;
                RepResult r;
                r.seq     = m_lastRep.seq + 1;
                r.counted = counted && !offHours;
                r.deepest = m_repMinHip;
                r.durMs   = static_cast<uint32_t>(dur);
                r.tooFast = !timingOk && dur < kMinRepMs;
                r.tooSlow = !timingOk && dur > kMaxRepMs;
                r.shallow = !qualityOk;
                m_lastRep = r;
                if (counted && offHours)
                {
                    st.offHoursReps.fetch_add(1);
                    LogF(L"situp outside the schedule - not credited  dur=%llums", dur);
                }
                else if (counted)
                {
                    // Shared total (phone reps land here too) — see SquatDetector.
                    const uint32_t total = st.sitUpCount.fetch_add(1) + 1;
                    LogF(L"situp +1  total=%u  hip=%.1f  top=%.1f  dur=%llums  q=%.2f",
                         total, hip, m_repMinHip, dur, quality);
                    PersistentState::Save();
                }
                else
                {
                    LogF(L"situp DROP dur=%llums q=%.2f (window %llu..%llu, qMin=%.2f)",
                         dur, quality, kMinRepMs, kMaxRepMs, qMin);
                }
                m_phase = SitUpPhase::Lying;
                m_repStartTickMs = 0;
                m_repMinHip = 999.0f;
            }
            else if (hip <= kSitting)
            {
                m_phase = SitUpPhase::Up;
            }
            break;

        case SitUpPhase::Unknown:
            break;
        }

        st.sitUpPhase.store(static_cast<uint8_t>(m_phase));
    }
}
