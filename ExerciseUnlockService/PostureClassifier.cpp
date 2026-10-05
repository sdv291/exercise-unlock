#include "PostureClassifier.h"
#include "State.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

// windows.h (pulled in via Log.h) defines min/max as macros, which
// collide with std::min({...}) / std::max({...}) initializer-list
// overloads below. Local helpers sidestep the whole fight.
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

namespace ExerciseUnlock::Svc
{
    // COCO joint indices we care about. Arms (elbows + wrists) are
    // deliberately absent from the vertical check — on a squat the
    // child often stretches arms forward or lets them fall, so hands
    // move independently of the body line and would corrupt the
    // "stacked vertically" test.
    static constexpr int kNose          = 0;
    static constexpr int kLeftShoulder  = 5;
    static constexpr int kRightShoulder = 6;
    static constexpr int kLeftHip       = 11;
    static constexpr int kRightHip      = 12;
    static constexpr int kLeftKnee      = 13;
    static constexpr int kRightKnee     = 14;
    static constexpr int kLeftAnkle     = 15;
    static constexpr int kRightAnkle    = 16;

    // Minimum per-joint confidence to count a joint toward the
    // classifier's geometry. Matches SquatDetector's 0.22 gate; a
    // higher threshold here starved the classifier on frames where
    // ankle confidence hovered 0.15..0.25 and left the detectors
    // ungated for too long (observed 2026-10-01: four phantom squat
    // counts logged before posture ever latched).
    static constexpr float kConfMin = 0.20f;

    // --- vertical (squat) thresholds ---
    //   dy = ankleY - shoulderY, dx = spread across body-only joints.
    //   Standing adult: dy ~ 0.55..0.85. In a deep squat the torso
    //   folds forward so the nose moves down toward the hip and dy
    //   collapses to ~0.35..0.45. 0.30 keeps the latch through the
    //   bottom of the rep (relied on by the unlatch-hysteresis).
    static constexpr float kSquatMinVertical    = 0.30f;
    static constexpr float kSquatRatioOverSpan  = 1.5f;

    // --- horizontal (push-up) thresholds ---
    //   The "head-to-feet distance > 1 m" ask was originally
    //   expressed as "body fills most of the frame width", with a
    //   0.55 threshold tuned for a camera roughly at floor level
    //   ~1 m from the subject. In practice with the camera on a
    //   desk looking down at a user on the floor the body only
    //   projects ~0.15..0.30 of the frame width even sideways —
    //   perspective foreshortening along the body axis is strong
    //   at that geometry. 0.15 keeps standing (dxHead ~ 0.05..0.10)
    //   out while admitting plank in realistic home setups; the
    //   bodyDy + dyHead ratio gates do the heavy lifting against
    //   standing / knee-lift false positives.
    static constexpr float kPushupMinSpanX     = 0.15f;
    static constexpr float kPushupRatioOverVy  = 1.8f;
    // The body line (shoulder→hip→ankle) should stay roughly flat
    // vertically for a plank. A standing person collapses into a
    // small X range but tall Y range, so this is also what rejects
    // "standing in front of camera" from being classified as pushup.
    static constexpr float kPushupMaxBodyDy    = 0.20f;

    static inline bool Confident(const Joint& j)
    {
        return j.confidence >= kConfMin;
    }

    // Return true when at least `need` of the given joint indices are
    // confident; fills avgX/avgY with the mean of only the confident
    // ones so single-side glitches don't poison the geometry.
    template <int N>
    static bool AvgConfident(const BodyPose& pose,
                             const int (&idx)[N],
                             int need,
                             float& avgX, float& avgY)
    {
        avgX = avgY = 0.0f;
        int n = 0;
        for (int i : idx)
        {
            if (Confident(pose.joints[i]))
            {
                avgX += pose.joints[i].x;
                avgY += pose.joints[i].y;
                ++n;
            }
        }
        if (n < need) return false;
        avgX /= n;
        avgY /= n;
        return true;
    }

    void PostureClassifier::Reset()
    {
        m_latched       = Ipc::PostureKind::None;
        m_pending       = Ipc::PostureKind::None;
        m_pendingStreak = 0;
        GetState().postureKind.store(static_cast<uint8_t>(Ipc::PostureKind::None));
    }

    void PostureClassifier::Update(const BodyPose& pose)
    {
        // Average positions for the four body-line groups. Arms are
        // skipped on purpose (see header comment).
        float shX, shY, hpX, hpY, knX, knY, akX, akY;
        const int shoulders[] = { kLeftShoulder, kRightShoulder };
        const int hips[]      = { kLeftHip,      kRightHip };
        const int knees[]     = { kLeftKnee,     kRightKnee };
        const int ankles[]    = { kLeftAnkle,    kRightAnkle };

        const bool haveShoulders = AvgConfident(pose, shoulders, 1, shX, shY);
        const bool haveHips      = AvgConfident(pose, hips,      1, hpX, hpY);
        const bool haveKnees     = AvgConfident(pose, knees,     1, knX, knY);
        const bool haveAnkles    = AvgConfident(pose, ankles,    1, akX, akY);
        const bool haveNose      = Confident(pose.joints[kNose]);

        Ipc::PostureKind raw = Ipc::PostureKind::None;

        // --- push-up (horizontal) — CHECKED FIRST ---
        //   The whole body is laid out across the frame. Head and
        //   ankles should be far apart in X and roughly at the same
        //   Y as the shoulder/hip line.
        //
        //   Checked BEFORE squat because the squat check used to
        //   win on perspective-stretched pushups: a user doing
        //   pushups on the floor with the camera elevated on a desk
        //   produces enough nose/ankle Y spread (perspective
        //   foreshortening) to trigger Squat's dy >= 0.30. The
        //   horizontal-shape signal (small body dy, large head dx)
        //   is a stricter test — if it passes we commit to PushUp
        //   and skip the Squat check entirely.
        float dxHead = 0.0f, dyHead = 0.0f, bodyDyPushup = 0.0f;
        bool  pushupShape = false;
        if (haveNose && haveShoulders && haveHips && haveAnkles)
        {
            dxHead = std::fabs(pose.joints[kNose].x - akX);
            dyHead = std::fabs(pose.joints[kNose].y - akY);
            float minY = shY;
            if (hpY < minY) minY = hpY;
            if (akY < minY) minY = akY;
            float maxY = shY;
            if (hpY > maxY) maxY = hpY;
            if (akY > maxY) maxY = akY;
            bodyDyPushup = maxY - minY;

            pushupShape =
                (dxHead >= kPushupMinSpanX) &&
                (dyHead <= 1e-3f || dxHead >= dyHead * kPushupRatioOverVy) &&
                (bodyDyPushup <= kPushupMaxBodyDy);
            if (pushupShape) raw = Ipc::PostureKind::PushUp;
        }

        // --- squat (vertical) — only if NOT a pushup ---
        //   Need torso + legs tracked. Head is optional — if the nose
        //   is confident we use it as the top anchor; otherwise we
        //   fall back to shoulders. Horizontal spread uses only
        //   shoulder/hip/knee/ankle so arms stretched forward during
        //   the squat don't widen dx.
        float dySquat = 0.0f, bodyDxSquat = 0.0f;
        bool  stacked = false, squatShape = false;
        if (raw == Ipc::PostureKind::None &&
            haveShoulders && haveHips && haveKnees && haveAnkles)
        {
            const float topY = haveNose ? pose.joints[kNose].y : shY;
            dySquat = akY - topY;
            float minX = shX;
            if (hpX < minX) minX = hpX;
            if (knX < minX) minX = knX;
            if (akX < minX) minX = akX;
            float maxX = shX;
            if (hpX > maxX) maxX = hpX;
            if (knX > maxX) maxX = knX;
            if (akX > maxX) maxX = akX;
            bodyDxSquat = maxX - minX;

            constexpr float kStackSlack = 0.02f;
            stacked = (hpY > shY - kStackSlack) &&
                      (akY > hpY - kStackSlack);

            squatShape = (dySquat >= kSquatMinVertical) &&
                         (bodyDxSquat <= 1e-3f ||
                          dySquat >= bodyDxSquat * kSquatRatioOverSpan) &&
                         stacked;
            if (squatShape) raw = Ipc::PostureKind::Squat;
        }

        // Debug trace once per second: show the exact metrics that
        // drove the classification so misclassifications (e.g.
        // perspective-stretched pushup looking "vertical") are
        // diagnosable from the service log.
        const uint64_t now = GetTickCount64();
        if (now - m_lastDebugLogMs >= 1000)
        {
            m_lastDebugLogMs = now;
            const wchar_t* rawName =
                (raw == Ipc::PostureKind::Squat)  ? L"Squat"  :
                (raw == Ipc::PostureKind::PushUp) ? L"PushUp" : L"None";
            LogF(L"posture DBG raw=%s | pushup: dxHead=%.2f dyHead=%.2f bodyDy=%.2f shape=%d "
                 L"| squat: dy=%.2f bodyDx=%.2f stacked=%d shape=%d "
                 L"| noseY=%.2f shY=%.2f hpY=%.2f knY=%.2f akY=%.2f",
                 rawName,
                 dxHead, dyHead, bodyDyPushup, pushupShape ? 1 : 0,
                 dySquat, bodyDxSquat, stacked ? 1 : 0, squatShape ? 1 : 0,
                 haveNose ? pose.joints[kNose].y : -1.0f,
                 shY, hpY, knY, akY);
        }

        // --- latch with asymmetric hysteresis ---
        // Latching a NEW kind requires kStableFrames of agreement
        // (fast enough that the start beep feels responsive).
        // UNlatching back to None requires kUnlatchFrames of
        // disagreement (long enough that a mid-rep confidence dip
        // doesn't tear down the gate on the counter).
        if (raw == m_pending)
        {
            ++m_pendingStreak;
        }
        else
        {
            m_pending       = raw;
            m_pendingStreak = 1;
        }

        if (m_pending != m_latched)
        {
            const bool goingToNone = (m_pending == Ipc::PostureKind::None);
            const int  need = goingToNone ? kUnlatchFrames : kStableFrames;
            if (m_pendingStreak >= need)
            {
                const Ipc::PostureKind prev = m_latched;
                m_latched = m_pending;
                GetState().postureKind.store(static_cast<uint8_t>(m_latched));
                const wchar_t* name = (m_latched == Ipc::PostureKind::Squat)  ? L"Squat"
                                    : (m_latched == Ipc::PostureKind::PushUp) ? L"PushUp"
                                    : L"None";
                LogF(L"posture: latched %s (was %u)", name, static_cast<unsigned>(prev));
            }
        }
    }
}
