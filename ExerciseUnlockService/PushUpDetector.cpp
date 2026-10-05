#include "PushUpDetector.h"
#include "State.h"
#include "Log.h"
#include "PersistentState.h"
#include "Config.h"

#include <cmath>
#include <cstdlib>
#include <windows.h>

using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Svc
{
    // MoveNet / COCO joint indices for arms + torso alignment.
    static constexpr int kLeftShoulder  = 5;
    static constexpr int kRightShoulder = 6;
    static constexpr int kLeftElbow     = 7;
    static constexpr int kRightElbow    = 8;
    static constexpr int kLeftWrist     = 9;
    static constexpr int kRightWrist    = 10;
    static constexpr int kLeftHip       = 11;
    static constexpr int kRightHip      = 12;
    static constexpr int kLeftAnkle     = 15;
    static constexpr int kRightAnkle    = 16;

    // Lowered along with SquatDetector's floor on 2026-10-01: fast
    // reps briefly dip ankle / wrist confidence into the 0.20-0.25
    // band, which previously triggered missStreak->Unknown resets.
    static constexpr float kJointConfMin = 0.18f;

    // Elbow angle thresholds + plank gate + rep-timing window live in
    // Config now — see config.ini [pushup]. Calibrated defaults
    // (profile camera, child): up 165, down 155, hysteresis 3 deg,
    // plank >= 140, rep_ms 1000..2000. MoveNet in profile reads
    // shallower than true 3D so the observed band is intentionally
    // narrow.

    // Bumped from 0.5 to 0.7 after seeing raw=[35..167] deltas per
    // second smoothed to just 66 — the state machine missed real
    // transitions because the smoothed value never caught up.
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

    void PushUpDetector::Reset()
    {
        m_phase = PushUpPhase::Unknown;
        m_count = 0;
        m_lastDownTickMs = 0;
        m_lastUpTickMs   = 0;
        m_repStartTickMs = 0;
        m_smoothedElbow  = 0.0f;
        m_smoothedPlank  = 0.0f;
        m_smoothedInit   = false;
        m_missStreak     = 0;
        m_repMinElbow    = 999.0f;
        m_elbowFilter.Reset();

        auto& st = GetState();
        st.pushUpCount.store(0);
        st.pushUpPhase.store(static_cast<uint8_t>(PushUpPhase::Unknown));
        st.pushUpElbowAngle.store(0.0f);
        st.pushUpPlankAngle.store(0.0f);
    }

    void PushUpDetector::Update(const BodyPose& pose)
    {
        auto& st = GetState();

        // Freeze during an active unlock session and outside the
        // configured schedule window — same rationale as SquatDetector.
        if (IsSessionActive() || !Config::IsWithinActiveWindowNow())
        {
            st.pushUpPhase.store(static_cast<uint8_t>(PushUpPhase::Unknown));
            return;
        }

        // v1.2 posture gate: no reps unless the classifier latched
        // PushUp. Standing with arms outstretched or walking around
        // generated phantom pushup counts in earlier builds. Skip
        // (don't reset) — the classifier's unlatch hysteresis keeps
        // us latched through a real rep's confidence dips, and if
        // it does blip for a moment we don't want to lose the
        // in-progress rep state. The detector's own missStreak will
        // tear down state after sustained posture loss.
        if (st.postureKind.load() !=
            static_cast<uint8_t>(Ipc::PostureKind::PushUp))
        {
            return;
        }

        // Elbow angle: shoulder-elbow-wrist. Average both arms when
        // both have a confident triplet.
        float sumElbow = 0.0f;
        int   nElbow   = 0;
        if (ConfidentTriplet(pose.joints[kLeftShoulder],
                             pose.joints[kLeftElbow],
                             pose.joints[kLeftWrist]))
        {
            sumElbow += AngleAt(pose.joints[kLeftShoulder],
                                pose.joints[kLeftElbow],
                                pose.joints[kLeftWrist]);
            ++nElbow;
        }
        if (ConfidentTriplet(pose.joints[kRightShoulder],
                             pose.joints[kRightElbow],
                             pose.joints[kRightWrist]))
        {
            sumElbow += AngleAt(pose.joints[kRightShoulder],
                                pose.joints[kRightElbow],
                                pose.joints[kRightWrist]);
            ++nElbow;
        }

        // Plank angle: shoulder-hip-ankle. Use whichever side is
        // fully confident (in profile view usually just one). Also
        // capture that side's shoulder / ankle positions so we can
        // check body orientation below.
        float plank = 0.0f;
        bool  havePlank = false;
        float bodyDx = 0.0f, bodyDy = 0.0f;
        if (ConfidentTriplet(pose.joints[kLeftShoulder],
                             pose.joints[kLeftHip],
                             pose.joints[kLeftAnkle]))
        {
            plank    = AngleAt(pose.joints[kLeftShoulder],
                               pose.joints[kLeftHip],
                               pose.joints[kLeftAnkle]);
            bodyDx = pose.joints[kLeftShoulder].x - pose.joints[kLeftAnkle].x;
            bodyDy = pose.joints[kLeftShoulder].y - pose.joints[kLeftAnkle].y;
            havePlank = true;
        }
        else if (ConfidentTriplet(pose.joints[kRightShoulder],
                                  pose.joints[kRightHip],
                                  pose.joints[kRightAnkle]))
        {
            plank    = AngleAt(pose.joints[kRightShoulder],
                               pose.joints[kRightHip],
                               pose.joints[kRightAnkle]);
            bodyDx = pose.joints[kRightShoulder].x - pose.joints[kRightAnkle].x;
            bodyDy = pose.joints[kRightShoulder].y - pose.joints[kRightAnkle].y;
            havePlank = true;
        }

        // Body orientation hint — logged only.
        const bool horizontalBody =
            havePlank && (std::abs(bodyDx) > std::abs(bodyDy));

        // Wrists-near-ankles: in a real push-up hands are on the floor
        // at the same height as the feet, so |wrist.y - ankle.y| stays
        // small. In a standing bicep curl the wrists sit at hip level
        // while the ankles are on the floor, so the y-gap balloons.
        // Camera-angle-invariant (works whether the OsmoPocket is
        // side-on, head-on, or overhead), unlike |dx|>|dy| which
        // depends on which axis the body projects onto in the image.
        auto wrist_y = [&](int shIdx, int wrIdx) -> float {
            const auto& w = pose.joints[wrIdx];
            const auto& s = pose.joints[shIdx];
            return (w.confidence >= kJointConfMin) ? w.y :
                   (s.confidence >= kJointConfMin) ? s.y : -1.0f;
        };
        const float wL = wrist_y(kLeftShoulder,  kLeftWrist);
        const float wR = wrist_y(kRightShoulder, kRightWrist);
        const float aL = (pose.joints[kLeftAnkle].confidence  >= kJointConfMin) ? pose.joints[kLeftAnkle].y  : -1.0f;
        const float aR = (pose.joints[kRightAnkle].confidence >= kJointConfMin) ? pose.joints[kRightAnkle].y : -1.0f;
        float wrist = (wL >= 0 && wR >= 0) ? (wL + wR) * 0.5f : (wL >= 0 ? wL : wR);
        float ankle = (aL >= 0 && aR >= 0) ? (aL + aR) * 0.5f : (aL >= 0 ? aL : aR);
        const bool wristsNearFloor =
            (wrist >= 0 && ankle >= 0) && (std::abs(wrist - ankle) < 0.25f);

        // Hard gate: need at least one confident elbow triplet, a
        // confident plank triplet, and wrists near ankle height
        // (blocks standing bicep curls without depending on which
        // axis the body projects onto in the camera frame).
        if (nElbow == 0 || !havePlank || !wristsNearFloor)
        {
            const uint64_t nowT = GetTickCount64();
            if (nowT - m_lastDebugLogMs >= 1000)
            {
                m_lastDebugLogMs = nowT;
                auto c = [](const auto& j) { return j.confidence; };
                LogF(L"pushup SKIP nElbow=%d havePlank=%d horiz=%d wrFloor=%d "
                     L"|wrist-ankle|=%.2f dx=%.2f dy=%.2f "
                     L"| Lw=%.2f Rw=%.2f Lhp=%.2f Rhp=%.2f Lak=%.2f Rak=%.2f",
                     nElbow, havePlank ? 1 : 0, horizontalBody ? 1 : 0,
                     wristsNearFloor ? 1 : 0,
                     (wrist >= 0 && ankle >= 0) ? std::abs(wrist - ankle) : -1.0f,
                     bodyDx, bodyDy,
                     c(pose.joints[kLeftWrist]), c(pose.joints[kRightWrist]),
                     c(pose.joints[kLeftHip]),   c(pose.joints[kRightHip]),
                     c(pose.joints[kLeftAnkle]), c(pose.joints[kRightAnkle]));
            }
            ++m_missStreak;
            if (m_missStreak >= 20)   // was 5 — same reason as SquatDetector
            {
                m_phase = PushUpPhase::Unknown;
                m_smoothedInit = false;
                m_smoothedElbow = 0.0f;
                m_smoothedPlank = 0.0f;
                m_repStartTickMs = 0;
                st.pushUpPhase.store(static_cast<uint8_t>(PushUpPhase::Unknown));
            }
            return;
        }
        m_missStreak = 0;

        const float rawElbowAvg = sumElbow / nElbow;
        // Median + 2σ outlier filter upstream of EMA. MoveNet
        // elbow confidence drops sharply when the arm is near-
        // straight (top of rep), leading to occasional raw spikes
        // that EMA leaked through; median kills them cleanly.
        const float rawElbow = m_elbowFilter.Push(rawElbowAvg);

        if (!m_smoothedInit)
        {
            m_smoothedElbow = rawElbow;
            m_smoothedPlank = havePlank ? plank : 0.0f;
            m_smoothedInit  = true;
        }
        else
        {
            m_smoothedElbow = kEmaAlpha * rawElbow +
                              (1.0f - kEmaAlpha) * m_smoothedElbow;
            if (havePlank)
            {
                m_smoothedPlank = kEmaAlpha * plank +
                                  (1.0f - kEmaAlpha) * m_smoothedPlank;
            }
        }
        const float elbow    = m_smoothedElbow;
        const float plankAvg = m_smoothedPlank;

        st.pushUpElbowAngle.store(elbow);
        st.pushUpPlankAngle.store(plankAvg);

        if (m_phase == PushUpPhase::GoingDown || m_phase == PushUpPhase::Down ||
            m_phase == PushUpPhase::GoingUp)
        {
            if (elbow < m_repMinElbow) m_repMinElbow = elbow;
        }

        // Track raw elbow min/max within the current debug window so
        // the once-per-second DBG line shows the actual dip depth of
        // any push-up that happened between snapshots.
        if (rawElbow < m_windowMinRawElbow) m_windowMinRawElbow = rawElbow;
        if (rawElbow > m_windowMaxRawElbow) m_windowMaxRawElbow = rawElbow;

        const uint64_t now = GetTickCount64();

        if (now - m_lastDebugLogMs >= 1000)
        {
            m_lastDebugLogMs = now;
            LogF(L"pushup DBG smooth=%.1f raw=[%.1f..%.1f] plank=%.1f phase=%u",
                 elbow, m_windowMinRawElbow, m_windowMaxRawElbow,
                 plankAvg, static_cast<unsigned>(m_phase));
            m_windowMinRawElbow =  999.0f;
            m_windowMaxRawElbow = -1.0f;
        }

        // Pull live thresholds from Config each frame so a config
        // reload / hand-edit shows up without a service restart.
        const float kUp       = Config::PushupUpAngle();
        const float kDown     = Config::PushupDownAngle();
        const float kGoDown   = Config::PushupGoingDownAngle();
        const float kGoUp     = Config::PushupGoingUpAngle();
        const float kPlankMin = Config::PushupPlankMinAngle();
        const uint64_t kMinRepMs = static_cast<uint64_t>(Config::PushupMinRepMs());
        const uint64_t kMaxRepMs = static_cast<uint64_t>(Config::PushupMaxRepMs());

        // Plank gate: hard requirement. We only reached here because
        // havePlank was true (see the skip block above), so plankAvg is
        // meaningful — a "straight body" from the camera reads ~150
        // and we accept anything above the config threshold.
        if (plankAvg < kPlankMin)
        {
            m_phase = PushUpPhase::NotInPlank;
            st.pushUpPhase.store(static_cast<uint8_t>(PushUpPhase::NotInPlank));
            return;
        }

        // Seed on first plank-valid frame.
        if (m_phase == PushUpPhase::Unknown || m_phase == PushUpPhase::NotInPlank)
        {
            if (elbow >= kUp)
            {
                m_phase = PushUpPhase::Up;
                m_lastUpTickMs = now;
            }
            else if (elbow <= kDown)
            {
                m_phase = PushUpPhase::Down;
                m_lastDownTickMs = now;
                // Seeded mid-rep at the bottom — assume the child
                // just descended and start the timer now. On the
                // way back up we treat this as a half-cycle: the
                // timing gate uses half the [min..max] window.
                m_repStartTickMs = now;
            }
            else
            {
                // Intermediate elbow — try to guess which side of
                // the cycle we're on so we don't get stuck in
                // Unknown between confidence losses.
                if (elbow >= kGoUp)
                {
                    m_phase = PushUpPhase::Up;
                    m_lastUpTickMs = now;
                }
                else
                {
                    m_phase = PushUpPhase::Down;
                    m_lastDownTickMs = now;
                    m_repStartTickMs = now;
                }
            }
            st.pushUpPhase.store(static_cast<uint8_t>(m_phase));
            return;
        }

        switch (m_phase)
        {
        case PushUpPhase::Up:
            if (elbow < kGoDown)
            {
                m_phase = PushUpPhase::GoingDown;
                // Full rep starts here: user has left Up on the
                // way down.
                m_repStartTickMs = now;
            }
            break;

        case PushUpPhase::GoingDown:
            if (elbow <= kDown)
            {
                m_phase = PushUpPhase::Down;
                m_lastDownTickMs = now;
            }
            else if (elbow >= kUp)
            {
                // Aborted descent — didn't reach Down. No rep.
                m_phase = PushUpPhase::Up;
                m_lastUpTickMs = now;
                m_repStartTickMs = 0;
            }
            break;

        case PushUpPhase::Down:
            if (elbow > kGoUp)
            {
                m_phase = PushUpPhase::GoingUp;
            }
            break;

        case PushUpPhase::GoingUp:
            if (elbow >= kUp)
            {
                const uint64_t dur = (m_repStartTickMs != 0)
                    ? (now - m_repStartTickMs) : 0;
                // Quality: how far into the "down" band the rep dipped.
                //   1.0 = elbow reached kDown
                //   0.5 = elbow only halfway between kUp and kDown
                const float span = kUp - kDown;
                const float depth = kUp - m_repMinElbow;
                float quality = (span > 1.0f) ? (depth / span) : 1.0f;
                if (quality < 0.0f) quality = 0.0f;
                if (quality > 1.0f) quality = 1.0f;
                const float qMin = Config::PushupQualityMin();
                const bool timingOk = (dur >= kMinRepMs && dur <= kMaxRepMs);
                const bool qualityOk = (qMin <= 0.0f) || (quality >= qMin);
                if (timingOk && qualityOk)
                {
                    ++m_count;
                    st.pushUpCount.store(m_count);
                    LogF(L"pushup +1  total=%u  elbow=%.1f  plank=%.1f  dur=%llums  q=%.2f",
                         m_count, elbow, plankAvg, dur, quality);
                    PersistentState::Save();
                }
                else
                {
                    LogF(L"pushup DROP dur=%llums q=%.2f (window %llu..%llu, qMin=%.2f)",
                         dur, quality, kMinRepMs, kMaxRepMs, qMin);
                }
                m_phase = PushUpPhase::Up;
                m_lastUpTickMs = now;
                m_repStartTickMs = 0;
                m_repMinElbow    = 999.0f;
            }
            else if (elbow <= kDown)
            {
                m_phase = PushUpPhase::Down;
                m_lastDownTickMs = now;
            }
            break;

        case PushUpPhase::Unknown:
        case PushUpPhase::NotInPlank:
            break;
        }

        st.pushUpPhase.store(static_cast<uint8_t>(m_phase));
    }
}
