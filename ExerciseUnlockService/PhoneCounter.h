#pragma once

#include "DetectorParams.h"
#include "MiniJson.h"
#include "PoseEstimator.h"
#include "PostureClassifier.h"
#include "PushUpDetector.h"
#include "RepResult.h"
#include "SitUpDetector.h"
#include "SquatDetector.h"

#include <cstdint>
#include <vector>

namespace ExerciseUnlock::Svc
{
    // One phone's reps, counted on the PC from the poses the phone streams,
    // by the webcam's own detectors (a private set, nothing in State): what
    // the phone says it counted doesn't count. Frame times are the phone's,
    // but they may never run ahead of the PC's clock, so frames sent faster
    // than real time can't stretch into slow, valid reps.
    class PhoneCounter
    {
    public:
        enum Exercise { kSquat = 0, kPushUp = 1, kSitUp = 2, kExerciseCount };
        struct Attempt
        {
            Exercise  ex;
            RepResult rep;   // counted, or why not
        };

        // phoneSpace: the thresholds were measured on the phone itself
        // (Config::PhoneProfile). Otherwise they are the webcam's, and each
        // pose is mapped onto the webcam's frame of aspect `pcAspect` (w/h).
        PhoneCounter(const SquatParams& squat, const PushupParams& pushup, const SitupParams& situp,
                     bool phoneSpace, double pcAspect);

        // One frame: joints normalized to the phone's camera frame of w x h
        // pixels, phoneMs its frame time, pcNowMs GetTickCount64(). Every
        // attempt that got to the bottom of its rep is appended to `out`.
        void Feed(BodyPose pose, uint32_t w, uint32_t h, uint64_t phoneMs, uint64_t pcNowMs,
                  std::vector<Attempt>& out);

        Ipc::PostureKind Posture() const { return m_posture.Latched(); }

        // {t:pose, ts, w, h, k:[x, y, c] x 17} from the phone: x, y in
        // 1/10000 of its w x h frame, c in percent, ts its clock in ms.
        // Phone data is untrusted: anything out of shape or range is refused.
        static bool ParsePose(const Json::Value& msg, BodyPose& pose, uint32_t& w, uint32_t& h, uint64_t& ts);

        static const char* Name(Exercise ex);   // "squat" / "pushup" / "situp", as the phone names them

        // How far a frame's time may run ahead of the PC's (network jitter).
        static constexpr uint64_t kAheadMs = 1000;

    private:
        SquatParams  m_squatParams;
        PushupParams m_pushupParams;
        SitupParams  m_situpParams;
        bool         m_phoneSpace;
        double       m_pcAspect;

        PostureClassifier m_posture{ false };
        SquatDetector     m_squat{ false };
        PushUpDetector    m_pushup{ false };
        SitUpDetector     m_situp{ false };
        uint32_t          m_seen[kExerciseCount]{};   // LastRep().seq already reported

        bool     m_haveClock = false;
        uint64_t m_basePhone = 0, m_basePc = 0, m_lastPhone = 0, m_lastMapped = 0;
    };
}
