#pragma once

#include <windows.h>
#include <string>
#include <memory>

namespace ExerciseUnlock::Svc
{
    struct Joint
    {
        float x{ 0.0f };         // normalized 0..1
        float y{ 0.0f };         // normalized 0..1
        float confidence{ 0.0f };
    };

    struct BodyPose
    {
        static constexpr int kJointCount = 17;  // MoveNet / COCO layout
        Joint joints[kJointCount]{};
        float avgConfidence{ 0.0f };
        bool  personDetected{ false };
    };

    // Wraps an ONNX Runtime session over MoveNet SinglePose Lightning.
    //
    // v0.4a: prove we can load the model file and run inference at all.
    // ProveInference() feeds a zero-filled input and logs the output
    // shape — that's the "the plumbing works" checkpoint before we
    // start pumping real camera frames through in v0.4b.
    class PoseEstimator
    {
    public:
        PoseEstimator();
        ~PoseEstimator();

        // Load the .onnx from an absolute path. Publishes shape and
        // input dtype to the log so we can spot mismatches between
        // whatever MoveNet variant the user downloaded and what our
        // Predict path expects.
        HRESULT Load(const std::wstring& modelPath);

        // Runs one inference on an all-zeros input tensor. Cheap way to
        // confirm the session is usable end to end before v0.4b hooks
        // in the real webcam frames.
        HRESULT ProveInference();

        // Real prediction over a BGRA frame from the webcam. Nearest-
        // neighbour stretched to 192x192, normalized [0..1], run through
        // MoveNet, output parsed into 17 joints. Aspect-preserving resize
        // is a v0.5+ improvement — for now we accept a bit of squash.
        HRESULT Predict(const uint8_t* bgraPixels,
                        uint32_t width, uint32_t height,
                        BodyPose& out);

        bool IsLoaded() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
