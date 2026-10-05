#pragma once

#include "Protocol.h"

#include <atomic>
#include <mutex>
#include <vector>

// v0.2 has no real exercise state yet — the state block is stubbed and
// returned as-is over the wire. When SquatDetector / PushUpDetector land
// (v0.5+), they mutate this same block, and the pipe surface stays the
// same.
namespace ExerciseUnlock::Svc
{
    struct State
    {
        std::atomic<int32_t> pushUpsDone{ 0 };
        std::atomic<int32_t> pushUpsGoal{ 10 };
        std::atomic<int32_t> squatsDone{ 0 };
        std::atomic<int32_t> squatsGoal{ 10 };
        std::atomic<uint8_t> challengeState{
            static_cast<uint8_t>(Ipc::ChallengeState::Waiting) };
        std::atomic<int64_t> timeRemainingSeconds{ 0 };

        // v0.3 webcam probe visibility. All updated from WebcamProbe's
        // worker thread; the pipe server just reads snapshots.
        std::atomic<uint8_t>  webcamState{
            static_cast<uint8_t>(Ipc::WebcamState::Unknown) };
        std::atomic<uint32_t> webcamFrames{ 0 };
        std::atomic<uint32_t> webcamWidth{ 0 };
        std::atomic<uint32_t> webcamHeight{ 0 };
        std::atomic<uint32_t> webcamLastHresult{ 0 };

        // v0.4a pose pipeline visibility. Set once at service start.
        std::atomic<uint8_t>  poseState{
            static_cast<uint8_t>(Ipc::PoseState::Unknown) };
        std::atomic<uint32_t> poseLastHresult{ 0 };

        // v0.4b live pose. joints[] is written from the capture thread
        // while readers (the pipe accept path) copy them under the same
        // mutex — a plain atomic won't do because sizeof(joints) > 8B.
        std::mutex            poseMx;
        Ipc::WireJoint        joints[Ipc::kJointCount]{};
        std::atomic<float>    poseAvgConfidence{ 0.0f };
        std::atomic<uint8_t>  personDetected{ 0 };
        std::atomic<uint32_t> poseFramesInferred{ 0 };

        // v0.5 squat detector — updated on the pose-per-frame thread.
        std::atomic<uint32_t> squatCount{ 0 };
        std::atomic<float>    squatKneeAngle{ 0.0f };
        std::atomic<uint8_t>  squatPhase{
            static_cast<uint8_t>(Ipc::SquatPhase::Unknown) };

        // v0.6 push-up detector — same thread.
        std::atomic<uint32_t> pushUpCount{ 0 };
        std::atomic<float>    pushUpElbowAngle{ 0.0f };
        std::atomic<float>    pushUpPlankAngle{ 0.0f };
        std::atomic<uint8_t>  pushUpPhase{
            static_cast<uint8_t>(Ipc::PushUpPhase::Unknown) };

        // v1.2 posture classifier — latched None / Squat / PushUp.
        // Written from the pose-per-frame thread, read on pipe accept.
        std::atomic<uint8_t>  postureKind{
            static_cast<uint8_t>(Ipc::PostureKind::None) };

        // v0.7 reward — derived, but published on every snapshot so
        // the wire response is self-contained (provider doesn't need
        // its own copy of the config).
        // No dedicated atomic here: SnapshotInto computes these from
        // the counts above and the current Config values.

        // v0.9 session — set by SessionManager when an unlock session
        // is running. SnapshotInto derives sessionActive /
        // sessionRemainingSeconds from these.
        std::atomic<int64_t> sessionExpiresAtUnix{ 0 };  // 0 = no session
        // Unix seconds when the most recent session actually ended
        // (by expiring or being banked). Used by the grace window:
        // within Config::GraceWindowSeconds() of this timestamp, a
        // Submit grants a brief grace unlock without the earned
        // check so the child can close running apps. 0 = never.
        std::atomic<int64_t> lastSessionEndedUnix{ 0 };

        // v1.0 bank — seconds already earned in past locked periods
        // that haven't been spent on an unlock yet.
        std::atomic<uint32_t> earnedBankSeconds{ 0 };

        // v1.1 first-install timestamp (Unix seconds). Written on
        // first save; used by Config's progression math to scale
        // per-rep reward down over time.
        std::atomic<int64_t>  installStartUnix{ 0 };

        // v1.1 last-known session expiry, persisted to state.dat so
        // a service crash / reboot mid-session doesn't leave the
        // child with an untimed unlock. Same semantics as
        // sessionExpiresAtUnix above.
        // (Same field for runtime + persistence, separated only in
        // storage.)

        // v1.1 update checker. All strings guarded by updateMx so a
        // read while the checker thread is writing is safe.
        std::mutex           updateMx;
        bool                 updateAvailable{ false };
        std::wstring         updateLatestVersion;
        std::wstring         updateDownloadUrl;
        std::wstring         updateNotesUrl;
        // Last version we've already toasted — so we only pop one
        // toast per newly-released version, not every poll.
        std::wstring         updateLastToasted;
        std::atomic<int64_t> updateLastCheckUnix{ 0 };

        // Latest RGB32 frame the webcam thread captured. Kept in memory
        // so Op::SaveWebcamFrame can dump it to disk on demand (used by
        // tools/test-pipe.ps1 to see what the camera is looking at).
        // Bytes are BGRA, top-down, width*height*4 in size. Empty
        // until at least one frame has been captured.
        std::mutex           latestFrameMx;
        std::vector<uint8_t> latestFrameBgra;
        uint32_t             latestFrameWidth{ 0 };
        uint32_t             latestFrameHeight{ 0 };
    };

    // True while an unlock session is still running (child is in
    // Windows session, timer hasn't expired). Detectors read this and
    // skip counting so reps performed AFTER the unlock can't stack up
    // as a head-start for the next earn cycle.
    bool IsSessionActive();

    State& GetState();

    // Fill a wire response from the current state.
    void SnapshotInto(Ipc::Response& out);
}
