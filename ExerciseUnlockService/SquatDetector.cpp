#include "SquatDetector.h"
#include "State.h"
#include "Log.h"
#include "PersistentState.h"
#include "Config.h"

#include <cmath>
#include <windows.h>

using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Svc
{
    // MoveNet / COCO joint indices we actually use here.
    static constexpr int kLeftHip       = 11;
    static constexpr int kRightHip      = 12;
    static constexpr int kLeftKnee      = 13;
    static constexpr int kRightKnee     = 14;
    static constexpr int kLeftAnkle     = 15;
    static constexpr int kRightAnkle    = 16;

    // Per-joint confidence gate. Below this we treat the joint as
    // unreliable and don't feed its side into the angle average.
    // Lowered 0.30 -> 0.22 after 2026-10-01 test: fast reps make
    // MoveNet drop ankle confidence to ~0.25 mid-swing, nGood goes
    // to 0 for several frames, phase resets to Unknown and the rep
    // vanishes. 0.22 keeps the triplet valid through those dips
    // while still filtering out pure garbage (<0.20 random noise).
    static constexpr float kJointConfMin = 0.22f;

    // Angle thresholds + rep-timing window live in Config now — see
    // config.ini [squat]. Calibrated defaults (profile camera, child):
    //   standing ~170, deep squat ~120, hysteresis ~10 deg,
    //   rep_ms 1000..2000.

    // Low-pass filter on the angle. Bumped 0.50 -> 0.70 after
    // 2026-10-01 lock-screen test: with α=0.50 the smoothed value
    // couldn't climb past 160 during fast reps (raw peaked 170+
    // but the next frame was already in descent at 95, so EMA
    // averaged them to ~155 and the Standing threshold of 168 was
    // never crossed → no reps counted). Matches PushUpDetector's
    // α=0.70 which solved the same problem there.
    static constexpr float kEmaAlpha = 0.70f;

    // Angle at joint b, formed by rays b->a and b->c. Returns 0 on
    // degenerate input.
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
    static bool ConfidentTriplet(const J& hip, const J& knee, const J& ankle)
    {
        return hip.confidence   >= kJointConfMin &&
               knee.confidence  >= kJointConfMin &&
               ankle.confidence >= kJointConfMin;
    }

    void SquatDetector::Reset()
    {
        m_phase = SquatPhase::Unknown;
        m_count = 0;
        m_lastDownTickMs = 0;
        m_lastStandingTickMs = 0;
        m_repStartTickMs = 0;
        m_smoothedRatio = 0.0f;
        m_smoothedInit  = false;
        m_missStreak    = 0;
        m_repMinAngle   = 999.0f;
        m_repStartHipX  = -1.0f;
        m_repOscillations = 0;
        m_repMaxAsymmetry = 0.0f;
        m_repStartHipY  = -1.0f;
        m_repMaxHipY    = -1.0f;
        m_angleFilter.Reset();
        m_hipHistory.clear();

        auto& st = GetState();
        st.squatCount.store(0);
        st.squatPhase.store(static_cast<uint8_t>(SquatPhase::Unknown));
        st.squatKneeAngle.store(0.0f);
    }

    void SquatDetector::Update(const BodyPose& pose)
    {
        auto& st = GetState();

        // Freeze during an active unlock session — reps done while the
        // child is inside their earned time shouldn't stack up as a
        // head-start for the NEXT unlock. Also freeze outside the
        // configured schedule window so evening / early-morning
        // exercising can't be banked toward a future unlock.
        if (IsSessionActive() || !Config::IsWithinActiveWindowNow())
        {
            st.squatPhase.store(static_cast<uint8_t>(SquatPhase::Unknown));
            return;
        }

        // v1.2 posture gate: never credit a rep unless the posture
        // classifier has latched Squat. MoveNet produces wild angle
        // jitter (raw bouncing 3°..178° within a second) when the
        // person is in no recognizable pose at all; the classifier
        // only latches Squat when the body is actually stacked
        // vertically, so gating on it eliminates ghost reps.
        //
        // We just SKIP (not reset) when the gate is open — the
        // classifier has its own kUnlatchFrames hysteresis so a
        // single-frame confidence dip won't drop us out of Squat;
        // but if it did, nuking m_phase would lose a legitimate
        // rep in progress. Letting the detector's own missStreak
        // decide when to tear down state keeps a real rep alive
        // across short gate blips.
        if (st.postureKind.load() !=
            static_cast<uint8_t>(Ipc::PostureKind::Squat))
        {
            return;
        }

        // Per-leg knee angles when the hip/knee/ankle triplet on that
        // side is confident. We used to AVERAGE both legs, which lets
        // a single-leg knee lift (one leg at 52°, the other straight
        // at 170°) look like a 111° "half squat" to the state machine
        // — exactly how the 2026-10-01 lock-screen test produced two
        // phantom reps from arms-out knee lifts.
        //
        // With MAX (straightest leg wins) a real squat still passes:
        // both knees bend together so max ≈ avg ≈ 120°. A one-leg
        // knee lift leaves the other leg at 170° so max stays above
        // Down threshold and nothing counts.
        float lAngle = 0.0f, rAngle = 0.0f;
        bool  lHave  = false, rHave  = false;
        int   nGood  = 0;
        if (ConfidentTriplet(pose.joints[kLeftHip],
                             pose.joints[kLeftKnee],
                             pose.joints[kLeftAnkle]))
        {
            lAngle = AngleAt(pose.joints[kLeftHip],
                             pose.joints[kLeftKnee],
                             pose.joints[kLeftAnkle]);
            lHave = true;
            ++nGood;
        }
        if (ConfidentTriplet(pose.joints[kRightHip],
                             pose.joints[kRightKnee],
                             pose.joints[kRightAnkle]))
        {
            rAngle = AngleAt(pose.joints[kRightHip],
                             pose.joints[kRightKnee],
                             pose.joints[kRightAnkle]);
            rHave = true;
            ++nGood;
        }
        if (nGood == 0)
        {
            // Tolerate ~0.5 s of missed frames without dropping
            // our phase + smoothed history. MoveNet drops confident
            // triplets for a quarter-second easily during fast
            // movement; resetting too eagerly used to lose
            // mid-session reps entirely.
            ++m_missStreak;
            if (m_missStreak >= 20)   // was 5
            {
                m_phase = SquatPhase::Unknown;
                m_smoothedInit = false;
                m_smoothedRatio = 0.0f;
                m_repStartTickMs = 0;
                st.squatPhase.store(static_cast<uint8_t>(SquatPhase::Unknown));
            }
            return;
        }
        m_missStreak = 0;

        // MAX across confident legs: a single bent knee can't trigger
        // a Down transition unless the other knee bends too.
        float rawAngleMax;
        if (lHave && rHave) rawAngleMax = (lAngle > rAngle) ? lAngle : rAngle;
        else if (lHave)     rawAngleMax = lAngle;
        else                rawAngleMax = rAngle;

        // Median + 2σ outlier filter upstream of EMA. Kills the
        // single-frame MoveNet spikes (raw jumping 100°+ between
        // consecutive frames) that EMA couldn't fully suppress.
        // Shown by the yo-WASSUP/Good-GYM project to be more
        // robust than smoothing alone on noisy pose estimators.
        const float rawAngle = m_angleFilter.Push(rawAngleMax);

        // Track peak asymmetry while a rep is in flight. A one-leg
        // knee lift makes |L - R| spike to 100°+ even if a lucky
        // MoveNet frame happens to show both legs bent together
        // enough to pass the Down gate. m_repStartTickMs != 0 means
        // we're mid-rep (Standing → GoingDown transition set it).
        if (lHave && rHave && m_repStartTickMs != 0)
        {
            const float asym = std::fabs(lAngle - rAngle);
            if (asym > m_repMaxAsymmetry) m_repMaxAsymmetry = asym;
        }

        // Average of confident hips' X/Y. X used by the walking
        // gate (hipDrift), Y used by the knee-lift gate (hipDrop:
        // a real squat moves the hip DOWN in frame, a knee lift
        // leaves it put). -1 if neither hip is confident.
        float curHipX = -1.0f;
        float curHipY = -1.0f;
        {
            int   nHip = 0;
            float sumX = 0.0f, sumY = 0.0f;
            if (pose.joints[kLeftHip].confidence  >= kJointConfMin)
            { sumX += pose.joints[kLeftHip].x;
              sumY += pose.joints[kLeftHip].y;  ++nHip; }
            if (pose.joints[kRightHip].confidence >= kJointConfMin)
            { sumX += pose.joints[kRightHip].x;
              sumY += pose.joints[kRightHip].y; ++nHip; }
            if (nHip > 0)
            {
                curHipX = sumX / nHip;
                curHipY = sumY / nHip;
            }
        }

        // Rolling buffer of recent hip Y positions. Keeps the ~1 s
        // of history we need to pick a reliable pre-descent
        // baseline when a rep starts (see m_hipHistory doc in .h).
        if (curHipY > 0.0f)
        {
            m_hipHistory.push_back(curHipY);
            if (m_hipHistory.size() > kHipHistoryMax)
                m_hipHistory.pop_front();
        }

        // Track deepest hip-Y position seen during the current rep.
        if (m_repStartTickMs != 0 && curHipY > 0.0f)
        {
            if (m_repMaxHipY < 0.0f || curHipY > m_repMaxHipY)
                m_repMaxHipY = curHipY;
        }

        // EMA smoothing on the angle.
        if (!m_smoothedInit)
        {
            m_smoothedRatio = rawAngle;
            m_smoothedInit  = true;
        }
        else
        {
            m_smoothedRatio = kEmaAlpha * rawAngle +
                              (1.0f - kEmaAlpha) * m_smoothedRatio;
        }
        const float angle = m_smoothedRatio;

        // Wire field holds the smoothed knee angle in degrees.
        st.squatKneeAngle.store(angle);

        // Track deepest point of the current descent for quality
        // scoring on the way back up.
        if (m_phase == SquatPhase::GoingDown || m_phase == SquatPhase::Down ||
            m_phase == SquatPhase::GoingUp)
        {
            if (angle < m_repMinAngle) m_repMinAngle = angle;
        }

        const uint64_t now = GetTickCount64();

        // Pull live thresholds from Config each frame so a config
        // reload / hand-edit shows up without a service restart.
        const int   mode     = Config::SquatDetectorMode();  // 0=angle, 1=floor, 2=both
        const uint64_t kMinRepMs = static_cast<uint64_t>(Config::SquatMinRepMs());
        const uint64_t kMaxRepMs = static_cast<uint64_t>(Config::SquatMaxRepMs());

        // --- compute floor-relative hip fraction when needed ---
        //   hipFrac = (ankleY - hipY) / (ankleY - shoulderY)
        // y goes 0 (top of image) to 1 (bottom), so ankle has the
        // LARGEST y. The ratio is dimensionless and camera-zoom
        // independent, as long as the camera isn't directly overhead.
        float hipFrac = 0.5f;   // standing-equivalent neutral
        bool  haveFloor = false;
        if (mode != 0)
        {
            auto confY = [&](int idx) -> float
            {
                return pose.joints[idx].confidence >= kJointConfMin
                    ? pose.joints[idx].y : -1.0f;
            };
            const float lsh = confY(5), rsh = confY(6);     // shoulders
            const float lhp = confY(kLeftHip),  rhp = confY(kRightHip);
            const float lak = confY(kLeftAnkle), rak = confY(kRightAnkle);
            // Pick side with the most confident triplet; average if both.
            float shY = -1, hpY = -1, akY = -1;
            if (lsh >= 0 && lhp >= 0 && lak >= 0) { shY = lsh; hpY = lhp; akY = lak; }
            if (rsh >= 0 && rhp >= 0 && rak >= 0)
            {
                if (shY < 0) { shY = rsh; hpY = rhp; akY = rak; }
                else         { shY = (shY+rsh)/2; hpY = (hpY+rhp)/2; akY = (akY+rak)/2; }
            }
            if (shY > 0 && akY > shY + 0.05f)   // non-degenerate height
            {
                hipFrac = (akY - hpY) / (akY - shY);
                if (hipFrac < 0) hipFrac = 0;
                if (hipFrac > 1.2f) hipFrac = 1.2f;
                haveFloor = true;
            }
        }

        // Resolve effective "standing" / "down" booleans per mode so
        // the state machine below can stay single-shape.
        auto isStandingNow = [&]() -> bool {
            const bool angleOk = (angle >= Config::SquatStandingAngle());
            const bool floorOk = haveFloor && (hipFrac >= Config::SquatStandingHipFrac());
            if (mode == 0) return angleOk;
            if (mode == 1) return floorOk;
            return angleOk && floorOk;                        // mode == 2 (both)
        };
        auto isDownNow = [&]() -> bool {
            // Both-sides requirement for Down entry: a single-leg
            // knee lift (one knee bent at ~50°, other straight,
            // other side's ankle confidence low so only the bent
            // triplet passes) produced phantom reps on 2026-10-01.
            // Requiring nGood==2 means at least one DBG-sampled
            // frame per rep must have had both legs tracked while
            // the body was deep — true for real squats, false for
            // asymmetric knee lifts where the standing leg's ankle
            // confidence stays low.
            if (nGood < 2) return false;
            const bool angleOk = (angle <= Config::SquatDownAngle());
            const bool floorOk = haveFloor && (hipFrac <= Config::SquatDownHipFrac());
            if (mode == 0) return angleOk;
            if (mode == 1) return floorOk;
            return angleOk && floorOk;
        };
        auto crossGoingDown = [&]() -> bool {
            const bool angleOk = (angle < Config::SquatGoingDownAngle());
            const bool floorOk = haveFloor && (hipFrac < Config::SquatGoingDownHipFrac());
            if (mode == 0) return angleOk;
            if (mode == 1) return floorOk;
            return angleOk || floorOk;                        // "both" starts descent on either
        };
        auto crossGoingUp = [&]() -> bool {
            const bool angleOk = (angle > Config::SquatGoingUpAngle());
            const bool floorOk = haveFloor && (hipFrac > Config::SquatGoingUpHipFrac());
            if (mode == 0) return angleOk;
            if (mode == 1) return floorOk;
            return angleOk || floorOk;
        };

        // Keep the kStand/kDown names below as "effective booleans" via
        // the lambdas. For logging continuity we still expose the raw
        // angle values the FSM would otherwise reference.
        const float kStand   = Config::SquatStandingAngle();
        const float kDown    = Config::SquatDownAngle();
        (void)kStand; (void)kDown;   // (angle shown in DBG log still)

        // Debug log once per second — now that hipFrac is computed.
        if (now - m_lastDebugLogMs >= 1000)
        {
            m_lastDebugLogMs = now;
            auto conf = [](const auto& j) { return j.confidence; };
            const wchar_t* modeStr = (mode == 0) ? L"angle"
                                   : (mode == 1) ? L"floor" : L"both";
            const float hipDxDbg = (m_repStartHipX >= 0.0f && curHipX >= 0.0f)
                ? std::fabs(curHipX - m_repStartHipX) : -1.0f;
            LogF(L"squat DBG mode=%s rawMax=%.1f filt=%.1f smooth=%.1f hipFrac=%.2f(%s) hipX=%.2f hipDx=%.2f "
                 L"| phase=%u sides=%d "
                 L"| Lhp=%.2f Lkn=%.2f Lak=%.2f Rhp=%.2f Rkn=%.2f Rak=%.2f",
                 modeStr, rawAngleMax, rawAngle, angle, hipFrac, haveFloor ? L"ok" : L"n/a",
                 curHipX, hipDxDbg,
                 static_cast<unsigned>(m_phase), nGood,
                 conf(pose.joints[kLeftHip]),  conf(pose.joints[kLeftKnee]),
                 conf(pose.joints[kLeftAnkle]),
                 conf(pose.joints[kRightHip]), conf(pose.joints[kRightKnee]),
                 conf(pose.joints[kRightAnkle]));
        }

        // Higher angle = standing (straight leg). Lower angle = squat.
        if (m_phase == SquatPhase::Unknown)
        {
            if (isStandingNow())
            {
                m_phase = SquatPhase::Standing;
                m_lastStandingTickMs = now;
            }
            else if (isDownNow())
            {
                m_phase = SquatPhase::Down;
                m_lastDownTickMs = now;
                // Seeded mid-rep at the bottom — assume the child just
                // descended and start the timer now. Mirrors the fix
                // already applied to PushUpDetector: without this, the
                // first rep after a confidence-loss spike (missStreak
                // >= 5 resetting phase=Unknown) drops with dur=0ms.
                m_repStartTickMs = now;
                m_repStartHipX   = curHipX;
            }
            else
            {
                // Intermediate — try to guess which side we're on so
                // we don't get stuck in Unknown between glitches.
                if (angle >= Config::SquatGoingUpAngle())
                {
                    m_phase = SquatPhase::Standing;
                    m_lastStandingTickMs = now;
                }
                else
                {
                    m_phase = SquatPhase::Down;
                    m_lastDownTickMs = now;
                    m_repStartTickMs = now;
                    m_repStartHipX   = curHipX;
                }
            }
            st.squatPhase.store(static_cast<uint8_t>(m_phase));
            return;
        }

        switch (m_phase)
        {
        case SquatPhase::Standing:
            if (crossGoingDown())
            {
                m_phase = SquatPhase::GoingDown;
                // Full rep starts here: user has left standing on
                // the way down. Timing gate on completion measures
                // now (top) -> now (back at top).
                m_repStartTickMs  = now;
                m_repStartHipX    = curHipX;

                // Pre-descent hip baseline: the state machine
                // detects "left Standing" a few frames LATE
                // (median+EMA smoothing), by which point the hip
                // has already dropped. Take the smallest hipY
                // seen in the recent history buffer (= highest
                // standing position) instead of curHipY, so
                // hipDrop measures the full real descent, not
                // just the tail of it.
                float baselineHipY = curHipY;
                if (!m_hipHistory.empty())
                {
                    baselineHipY = m_hipHistory.front();
                    for (float v : m_hipHistory)
                        if (v > 0.0f && v < baselineHipY) baselineHipY = v;
                }
                m_repStartHipY    = baselineHipY;
                m_repMaxHipY      = curHipY;
                m_repOscillations = 0;
                m_repMaxAsymmetry = 0.0f;
            }
            break;

        case SquatPhase::GoingDown:
            if (isDownNow())
            {
                m_phase = SquatPhase::Down;
                m_lastDownTickMs = now;
            }
            else if (isStandingNow())
            {
                // Aborted descent — didn't reach Down. No rep.
                m_phase = SquatPhase::Standing;
                m_lastStandingTickMs = now;
                m_repStartTickMs  = 0;
                m_repStartHipX    = -1.0f;
                m_repStartHipY    = -1.0f;
                m_repMaxHipY      = -1.0f;
                m_repOscillations = 0;
                m_repMaxAsymmetry = 0.0f;
            }
            break;

        case SquatPhase::Down:
            if (crossGoingUp())
            {
                m_phase = SquatPhase::GoingUp;
            }
            break;

        case SquatPhase::GoingUp:
            // Reverse direction mid-ascent is a MoveNet-noise signature,
            // not a real squat. Count the reversal; the completion
            // check below will drop the rep if any happened.
            if (isDownNow())
            {
                m_phase = SquatPhase::Down;
                m_lastDownTickMs = now;
                ++m_repOscillations;
                break;
            }
            if (isStandingNow())
            {
                const uint64_t dur = (m_repStartTickMs != 0)
                    ? (now - m_repStartTickMs) : 0;
                // Quality = how deep the rep actually went, from
                // kStand→kDown mapped to 0..1. 1.0 = touched kDown
                // (a full squat), 0.5 = only halfway between kStand
                // and kDown, etc.
                const float span = kStand - kDown;
                const float depth = kStand - m_repMinAngle;
                float quality = (span > 1.0f) ? (depth / span) : 1.0f;
                if (quality < 0.0f) quality = 0.0f;
                if (quality > 1.0f) quality = 1.0f;
                const float qMin = Config::SquatQualityMin();
                const bool timingOk = (m_repStartTickMs != 0 &&
                                       dur >= kMinRepMs && dur <= kMaxRepMs);
                const bool qualityOk = (qMin <= 0.0f) || (quality >= qMin);

                // Walking-vs-squatting gate: if the hip drifted
                // significantly across the frame between descent-start
                // and now, this wasn't a stationary squat — it was the
                // user stepping around. Only enforced when we actually
                // have both snapshot and current hip X.
                const float maxDrift = Config::SquatMaxHipDrift();
                float hipDrift = 0.0f;
                bool  hipDriftOk = true;
                if (maxDrift > 0.0f &&
                    m_repStartHipX >= 0.0f && curHipX >= 0.0f)
                {
                    hipDrift = std::fabs(curHipX - m_repStartHipX);
                    hipDriftOk = (hipDrift <= maxDrift);
                }

                // Oscillation gate: a clean rep never reverses from
                // GoingUp back to Down. Any such reversal is MoveNet
                // confusion, not biomechanics, and the counted "rep"
                // here is a merged/phantom pair rather than one real
                // squat.
                const bool oscillationOk = (m_repOscillations == 0);
                // Hip-drop gate — the primary knee-lift killer.
                // A real squat moves the hip downward in frame by
                // 0.10..0.20; a one-leg knee lift leaves it put.
                // Observed 2026-10-01 across 4 captured knee lifts:
                // hipDy was 0.01..0.03 for all of them (well under
                // the 0.07 default); hip-position tracks the body
                // reliably because MoveNet rates the hip joint one
                // of the most-confident, independent of per-leg
                // angle noise that fooled every earlier gate.
                const float minDrop = Config::SquatMinHipDrop();
                float hipDrop = 0.0f;
                bool  hipDropOk = true;
                if (minDrop > 0.0f &&
                    m_repStartHipY >= 0.0f && m_repMaxHipY >= 0.0f)
                {
                    hipDrop = m_repMaxHipY - m_repStartHipY;
                    hipDropOk = (hipDrop >= minDrop);
                }

                // asym (peak |Lknee - Rknee|) is still measured and
                // logged as a diagnostic, but NOT gated on: on
                // 2026-10-01 it DROP'd a real rep at asym=80.2 /
                // hipDy=0.14 due to a single frame of MoveNet
                // confusion. All knee lifts failed hipDy=<0.03 on
                // their own, so the asymmetry gate added no
                // detection power but did add false drops.
                if (timingOk && qualityOk && hipDriftOk &&
                    oscillationOk && hipDropOk)
                {
                    ++m_count;
                    st.squatCount.store(m_count);
                    LogF(L"squat +1  total=%u  angle=%.1f  dur=%llums  q=%.2f  "
                         L"hipDx=%.2f osc=%d asym=%.1f hipDy=%.2f",
                         m_count, angle, dur, quality,
                         hipDrift, m_repOscillations, m_repMaxAsymmetry,
                         hipDrop);
                    PersistentState::Save();
                }
                else
                {
                    LogF(L"squat DROP dur=%llums q=%.2f hipDx=%.2f osc=%d "
                         L"asym=%.1f hipDy=%.2f (window %llu..%llu, qMin=%.2f, "
                         L"maxDrift=%.2f, minDrop=%.2f)",
                         dur, quality, hipDrift, m_repOscillations,
                         m_repMaxAsymmetry, hipDrop,
                         kMinRepMs, kMaxRepMs, qMin, maxDrift,
                         minDrop);
                }
                m_phase = SquatPhase::Standing;
                m_lastStandingTickMs = now;
                m_repStartTickMs = 0;
                m_repMinAngle    = 999.0f;
                m_repStartHipX   = -1.0f;
                m_repStartHipY   = -1.0f;
                m_repMaxHipY     = -1.0f;
                m_repOscillations = 0;
                m_repMaxAsymmetry = 0.0f;
            }
            break;

        case SquatPhase::Unknown:
            break;
        }

        st.squatPhase.store(static_cast<uint8_t>(m_phase));
    }
}
