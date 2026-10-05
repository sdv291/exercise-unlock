#pragma once

#include <cstdint>

namespace ExerciseUnlock::Ipc
{
    // Named pipe both sides talk over. Fixed for the whole product; if
    // it ever changes the provider DLL and the service exe must move in
    // lockstep, since the provider is shipped with the service.
    inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ExerciseUnlock";

    // Wire format is a single fixed-size Request followed by a single
    // fixed-size Response, message-mode. That's enough for v0.3 (one
    // read + one write per connection). Anything richer waits until we
    // actually need streaming updates.
    inline constexpr uint32_t kMagic   = 0x4B4C5545; // 'EULK' little-endian
    inline constexpr uint16_t kVersion = 1;

    enum class Op : uint16_t
    {
        GetStatus    = 1,
        // Zeros squat + pushup counters (call after a successful unlock
        // so one workout doesn't unlock the machine forever).
        ResetCounts  = 2,
        // Starts an unlock session for the currently-earned seconds,
        // resets rep counters, and arms the countdown / warnings /
        // auto-lock. Called by the provider from ReportResult on
        // successful sign-in.
        StartSession = 3,
        // Dumps the latest captured webcam frame to a fixed world-
        // readable path on disk so a developer tool (test-pipe.ps1)
        // can eyeball what the camera actually saw. See PipeServer for
        // the exact path.
        SaveWebcamFrame = 4,
        // Parent override — hand the child N minutes of unlocked time
        // without requiring reps. Payload is 4-byte little-endian
        // uint32 minutes appended after the 8-byte header. Restricted
        // to processes that can open the pipe (SYSTEM + Admins).
        Grant           = 5,
    };

    enum class ChallengeState : uint8_t
    {
        Waiting   = 0,
        PushUps   = 1,
        Squats    = 2,
        Completed = 3,
    };

    // WebcamProbe rolls this atomic through its lifecycle. Ok means at
    // least one frame was decoded end-to-end; that's the "yes, MF
    // works on Lock/Sign-in screen" answer v0.3 exists to produce.
    enum class WebcamState : uint8_t
    {
        Unknown  = 0,
        Probing  = 1,
        Ok       = 2,
        NoDevice = 3,
        Failed   = 4,
        // Camera deliberately released: the console shows somebody's
        // desktop rather than the lock / sign-in screen, or an unlock
        // session is running. Reopened automatically on the next lock.
        Idle     = 5,
    };

    // PoseEstimator state. v0.4a: NoModel/Loading/Ok/Failed after boot
    // proves the ONNX Runtime session is usable. v0.4b will extend to
    // "Ok" as a running live signal driven off webcam frames.
    enum class PoseState : uint8_t
    {
        Unknown = 0,
        NoModel = 1,   // .onnx file wasn't at the expected path
        Loading = 2,
        Ok      = 3,   // Load + ProveInference succeeded
        Failed  = 4,   // ORT threw at load or run
    };

    // Squat state machine phases. Standing → GoingDown → Down → GoingUp
    // → Standing is one rep. Unknown means we don't have enough joint
    // confidence to be sure — the counter freezes rather than
    // false-counts on a bad frame.
    enum class SquatPhase : uint8_t
    {
        Unknown   = 0,
        Standing  = 1,
        GoingDown = 2,
        Down      = 3,
        GoingUp   = 4,
    };

    // Push-up state machine phases. Same shape as SquatPhase but "Up"
    // means arms straight (top of rep) rather than legs straight.
    // NotInPlank surfaces the "you're not in a push-up position at
    // all" case so the tile can tell the user why nothing is counting.
    enum class PushUpPhase : uint8_t
    {
        Unknown     = 0,
        Up          = 1,
        GoingDown   = 2,
        Down        = 3,
        GoingUp     = 4,
        NotInPlank  = 5,
    };

    // Posture classifier output — tells the provider which exercise
    // the child has set themselves up for, so it can beep once at the
    // moment the detector locks on. "Ready" means several consecutive
    // frames agreed on the same classification; the provider latches
    // on the None→Squat / None→Push-up transition.
    enum class PostureKind : uint8_t
    {
        None   = 0,   // not standing in a recognizable pose yet
        Squat  = 1,   // vertical body, points stacked top-to-bottom
        PushUp = 2,   // horizontal body, head-to-feet span wide
    };

    #pragma pack(push, 1)

    struct Request
    {
        uint32_t magic;
        uint16_t version;
        uint16_t op;      // Op
    };

    // Wire copy of one skeletal joint. Coordinates are model-normalized
    // (0..1). MoveNet's native order is (y, x, confidence) — keeping it
    // as-is so the service doesn't reshape data just to reshape it back
    // on the client.
    struct WireJoint
    {
        float y;
        float x;
        float confidence;
    };
    static_assert(sizeof(WireJoint) == 12, "wire fmt");
    inline constexpr int kJointCount = 17;   // MoveNet / COCO layout

    struct Response
    {
        uint32_t magic;
        uint16_t version;
        uint16_t op;                 // echoes Request::op

        int32_t  pushUpsDone;
        int32_t  pushUpsGoal;
        int32_t  squatsDone;
        int32_t  squatsGoal;

        uint8_t  state;              // ChallengeState
        uint8_t  reserved[3];

        // If a session is active (child has already earned time), this
        // is the remaining seconds; otherwise 0.
        int64_t  timeRemainingSeconds;

        // Wall-clock seconds since epoch when the response was produced.
        // Lets the provider spot stale data if it ever caches responses.
        int64_t  serverTimestampUnix;

        // --- v0.3 webcam probe status ---
        uint8_t  webcamState;        // WebcamState
        uint8_t  reserved2[3];
        uint32_t webcamFrames;       // cumulative frames read in last probe
        uint32_t webcamWidth;        // last known frame width
        uint32_t webcamHeight;       // last known frame height
        uint32_t webcamLastHresult;  // last HRESULT observed (0 if Ok)

        // --- v0.4a pose pipeline status ---
        uint8_t  poseState;          // PoseState
        uint8_t  reserved3[3];
        uint32_t poseLastHresult;    // last HRESULT / ORT error observed

        // --- v0.4b live pose ---
        WireJoint joints[kJointCount];  // last inferred skeleton
        float     poseAvgConfidence;    // mean confidence across joints
        uint8_t   personDetected;       // avgConfidence > 0.3
        uint8_t   reserved4[3];
        uint32_t  poseFramesInferred;   // running counter (wraps at 2^32)

        // --- v0.5 squat detector ---
        uint32_t  squatCount;           // reps completed since service start
        float     squatKneeAngle;       // last measured knee angle (deg)
        uint8_t   squatPhase;           // SquatPhase
        uint8_t   reserved5[3];

        // --- v0.6 push-up detector ---
        uint32_t  pushUpCount;          // reps completed since service start
        float     pushUpElbowAngle;     // smoothed elbow angle (deg)
        float     pushUpPlankAngle;     // shoulder-hip-ankle alignment (deg)
        uint8_t   pushUpPhase;          // PushUpPhase
        uint8_t   reserved6[3];

        // --- v0.7 reward / unlock ---
        uint32_t  rewardEarnedSeconds;  // pushups*pu_s + squats*sq_s (current session)
        uint32_t  rewardMinUnlockSeconds; // threshold from config
        uint8_t   rewardEnough;         // earned >= min
        uint8_t   reserved7[3];

        // --- v0.9 unlock session (post-login countdown) ---
        uint8_t   sessionActive;        // non-zero while a timer is running
        uint8_t   reserved8[3];
        uint32_t  sessionRemainingSeconds; // 0 when not active

        // --- v1.2 posture classifier ---
        // 0=None, 1=Squat (vertical), 2=PushUp (horizontal). Latched
        // only after N consecutive frames agree, so the provider can
        // treat a None→Squat / None→PushUp transition as a reliable
        // "ready to go" signal and play the start beep.
        uint8_t   postureKind;
        uint8_t   reserved9[3];

        // --- v0.9 pre-rendered tile text ---
        // Service formats the whole tile-status string from Config
        // labels + counters + reward + session and drops it here so
        // the provider is a dumb consumer (no duplicated labels /
        // format strings in LogonUI). NUL-terminated, "\r\n" for
        // newlines between visible lines. 128 wchar_t = 256 bytes is
        // plenty for 3-4 lines.
        wchar_t   statusText[128];
    };

    #pragma pack(pop)

    static_assert(sizeof(Request)  == 8,   "wire format changed");
    static_assert(sizeof(Response) == 340 + 128 * sizeof(wchar_t),
                  "wire format changed");   // = 596
}
