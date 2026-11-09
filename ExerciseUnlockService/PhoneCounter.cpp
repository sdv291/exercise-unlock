#include "PhoneCounter.h"

#include <algorithm>

namespace ExerciseUnlock::Svc
{
    PhoneCounter::PhoneCounter(const SquatParams& squat, const PushupParams& pushup, const SitupParams& situp,
                               bool phoneSpace, double pcAspect)
        : m_squatParams(squat), m_pushupParams(pushup), m_situpParams(situp),
          m_phoneSpace(phoneSpace), m_pcAspect(pcAspect > 0.1 ? pcAspect : 16.0 / 9.0)
    {
        m_squat.Reset();
        m_pushup.Reset();
        m_situp.Reset();
        m_posture.Reset();
    }

    bool PhoneCounter::ParsePose(const Json::Value& msg, BodyPose& pose, uint32_t& w, uint32_t& h, uint64_t& ts)
    {
        const Json::Value* k = msg.Get("k");
        const double t = msg.Num("ts", -1.0), fw = msg.Num("w", 0.0), fh = msg.Num("h", 0.0);
        if (!k || k->type != Json::Value::Type::Array || k->arr.size() != 3 * BodyPose::kJointCount) return false;
        if (!(t >= 0.0 && t < 1e12) || !(fw >= 1.0 && fw <= 10000.0) || !(fh >= 1.0 && fh <= 10000.0)) return false;
        for (int i = 0; i < BodyPose::kJointCount; ++i)
        {
            const Json::Value* v = &k->arr[static_cast<size_t>(3 * i)];
            for (int j = 0; j < 3; ++j)
                if (v[j].type != Json::Value::Type::Number) return false;
            // A joint can sit a little outside the frame.
            if (!(v[0].n >= -10000.0 && v[0].n <= 20000.0 && v[1].n >= -10000.0 && v[1].n <= 20000.0 &&
                  v[2].n >= 0.0 && v[2].n <= 100.0))
                return false;
            pose.joints[i] = { static_cast<float>(v[0].n / 10000.0), static_cast<float>(v[1].n / 10000.0),
                               static_cast<float>(v[2].n / 100.0) };
        }
        ts = static_cast<uint64_t>(t);
        w = static_cast<uint32_t>(fw);
        h = static_cast<uint32_t>(fh);
        return true;
    }

    const char* PhoneCounter::Name(Exercise ex)
    {
        switch (ex)
        {
        case kSquat:  return "squat";
        case kPushUp: return "pushup";
        case kSitUp:  return "situp";
        default:      return "?";
        }
    }

    void PhoneCounter::Feed(BodyPose pose, uint32_t w, uint32_t h, uint64_t phoneMs, uint64_t pcNowMs,
                            std::vector<Attempt>& out)
    {
        // The phone's time on the PC's clock: frames keep their own spacing
        // (network delays don't stretch a rep), but whenever they get ahead
        // of the PC's clock the clock restarts from now. A page reload
        // restarts the phone's clock too.
        if (!m_haveClock || phoneMs < m_lastPhone)
        {
            m_haveClock = true;
            m_basePhone = phoneMs;
            m_basePc = pcNowMs;
        }
        uint64_t mapped = m_basePc + (phoneMs - m_basePhone);
        if (mapped > pcNowMs + kAheadMs)
        {
            m_basePhone = phoneMs;
            m_basePc = pcNowMs;
            mapped = pcNowMs;
        }
        mapped = (std::max)(mapped, m_lastMapped);   // never backwards: rep durations are unsigned
        m_lastPhone = phoneMs;
        m_lastMapped = mapped;

        // The webcam's thresholds were measured on x / width, y / height of
        // its own frame: rescale x so a pixel distance counts the same here.
        if (!m_phoneSpace && w > 0 && h > 0)
        {
            const float k = static_cast<float>(static_cast<double>(w) / (h * m_pcAspect));
            for (Joint& j : pose.joints) j.x *= k;
        }
        float sum = 0.0f;
        for (const Joint& j : pose.joints) sum += j.confidence;
        pose.avgConfidence = sum / BodyPose::kJointCount;
        pose.personDetected = pose.avgConfidence > 0.3f;

        // Same order as the webcam's frames (WebcamProbe): the detectors
        // see the posture latched up to the previous frame.
        const Ipc::PostureKind posture = m_posture.Latched();
        m_squat.Update(pose, mapped, posture == Ipc::PostureKind::Squat, m_squatParams);
        m_pushup.Update(pose, mapped, posture == Ipc::PostureKind::PushUp, m_pushupParams);
        m_situp.Update(pose, mapped, posture == Ipc::PostureKind::SitUp, m_situpParams);
        m_posture.Update(pose);

        const RepResult* last[kExerciseCount] = { &m_squat.LastRep(), &m_pushup.LastRep(), &m_situp.LastRep() };
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            if (last[ex]->seq == m_seen[ex]) continue;
            m_seen[ex] = last[ex]->seq;
            // A descent abandoned before the bottom isn't an attempt.
            if (last[ex]->reachedDown) out.push_back({ static_cast<Exercise>(ex), *last[ex] });
        }
    }
}
