#include "PoseEstimator.h"
#include "Log.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <vector>
#include <string>

#pragma comment(lib, "onnxruntime.lib")

namespace ExerciseUnlock::Svc
{
    // MoveNet SinglePose Lightning: 192x192 RGB input; TF Hub v4 uses
    // int32 (values 0..255). Some ONNX exports convert to float32 or
    // uint8 — Load() logs the actual element type from the loaded model
    // so mismatches show up immediately in DebugView.
    static constexpr int64_t kInputW = 192;
    static constexpr int64_t kInputH = 192;
    static constexpr int64_t kInputC = 3;

    struct PoseEstimator::Impl
    {
        Ort::Env env{ ORT_LOGGING_LEVEL_WARNING, "ExerciseUnlock" };
        std::unique_ptr<Ort::Session>    session;
        Ort::MemoryInfo memInfo{
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault) };
        Ort::AllocatorWithDefaultOptions allocator;
        std::string inputName;
        std::string outputName;
        ONNXTensorElementDataType inputElemType{ ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED };
        std::vector<int64_t> inputShape;   // real shape read from the model
    };

    PoseEstimator::PoseEstimator() = default;
    PoseEstimator::~PoseEstimator() = default;

    bool PoseEstimator::IsLoaded() const
    {
        return m_impl && m_impl->session;
    }

    HRESULT PoseEstimator::Load(const std::wstring& modelPath)
    {
        try
        {
            m_impl = std::make_unique<Impl>();

            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(1);       // one webcam frame at a time
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            m_impl->session = std::make_unique<Ort::Session>(
                m_impl->env, modelPath.c_str(), opts);

            auto inName  = m_impl->session->GetInputNameAllocated(0, m_impl->allocator);
            auto outName = m_impl->session->GetOutputNameAllocated(0, m_impl->allocator);
            m_impl->inputName  = inName.get();
            m_impl->outputName = outName.get();

            auto inputInfo    = m_impl->session->GetInputTypeInfo(0);
            auto inputTensor  = inputInfo.GetTensorTypeAndShapeInfo();
            m_impl->inputShape    = inputTensor.GetShape();
            m_impl->inputElemType = inputTensor.GetElementType();
            auto& inputShape = m_impl->inputShape;

            auto outputInfo   = m_impl->session->GetOutputTypeInfo(0);
            auto outputTensor = outputInfo.GetTensorTypeAndShapeInfo();
            auto outputShape  = outputTensor.GetShape();

            LogF(L"pose: loaded '%hs' -> '%hs'",
                 m_impl->inputName.c_str(), m_impl->outputName.c_str());
            LogF(L"pose: input dims=[%lld,%lld,%lld,%lld] elemType=%d",
                 inputShape.size() > 0 ? inputShape[0] : -1,
                 inputShape.size() > 1 ? inputShape[1] : -1,
                 inputShape.size() > 2 ? inputShape[2] : -1,
                 inputShape.size() > 3 ? inputShape[3] : -1,
                 static_cast<int>(m_impl->inputElemType));
            LogF(L"pose: output dims=[%lld,%lld,%lld,%lld]",
                 outputShape.size() > 0 ? outputShape[0] : -1,
                 outputShape.size() > 1 ? outputShape[1] : -1,
                 outputShape.size() > 2 ? outputShape[2] : -1,
                 outputShape.size() > 3 ? outputShape[3] : -1);
        }
        catch (const Ort::Exception& e)
        {
            LogF(L"pose Load failed: %hs (code=%d)", e.what(), static_cast<int>(e.GetOrtErrorCode()));
            m_impl.reset();
            return E_FAIL;
        }
        catch (const std::exception& e)
        {
            LogF(L"pose Load failed (std): %hs", e.what());
            m_impl.reset();
            return E_FAIL;
        }
        return S_OK;
    }

    HRESULT PoseEstimator::ProveInference()
    {
        if (!IsLoaded()) return E_UNEXPECTED;

        try
        {
            // Build the input tensor from the model's real signature so
            // NHWC (TF Hub) vs NCHW (Qualcomm) both work without an
            // extra hardcode. Any dim reported as -1 (dynamic) becomes
            // 1 for this dummy inference — matches batch=1.
            std::vector<int64_t> shape = m_impl->inputShape;
            size_t elemCount = 1;
            for (auto& d : shape)
            {
                if (d < 0) d = 1;
                elemCount *= static_cast<size_t>(d);
            }

            Ort::Value inputTensor{ nullptr };

            // Match the tensor type to what the model actually wants,
            // so a stock TF Hub MoveNet (int32) and a re-exported float
            // variant both work without user config.
            std::vector<int32_t>  bufI32;
            std::vector<uint8_t>  bufU8;
            std::vector<float>    bufF32;

            switch (m_impl->inputElemType)
            {
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
                bufI32.assign(elemCount, 0);
                inputTensor = Ort::Value::CreateTensor<int32_t>(
                    m_impl->memInfo, bufI32.data(), bufI32.size(),
                    shape.data(), shape.size());
                break;
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
                bufU8.assign(elemCount, 0);
                inputTensor = Ort::Value::CreateTensor<uint8_t>(
                    m_impl->memInfo, bufU8.data(), bufU8.size(),
                    shape.data(), shape.size());
                break;
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
                bufF32.assign(elemCount, 0.0f);
                inputTensor = Ort::Value::CreateTensor<float>(
                    m_impl->memInfo, bufF32.data(), bufF32.size(),
                    shape.data(), shape.size());
                break;
            default:
                LogF(L"pose: unsupported input elem type %d — please tell me and I'll add it",
                     static_cast<int>(m_impl->inputElemType));
                return E_NOTIMPL;
            }

            const char* inputNames[]  = { m_impl->inputName.c_str() };
            const char* outputNames[] = { m_impl->outputName.c_str() };

            auto outputs = m_impl->session->Run(
                Ort::RunOptions{ nullptr },
                inputNames, &inputTensor, 1,
                outputNames, 1);

            if (outputs.empty())
            {
                LogF(L"pose: Run returned no outputs");
                return E_FAIL;
            }

            auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            LogF(L"pose: inference OK. output dims=[%lld,%lld,%lld,%lld]",
                 outShape.size() > 0 ? outShape[0] : -1,
                 outShape.size() > 1 ? outShape[1] : -1,
                 outShape.size() > 2 ? outShape[2] : -1,
                 outShape.size() > 3 ? outShape[3] : -1);
        }
        catch (const Ort::Exception& e)
        {
            LogF(L"pose ProveInference failed: %hs", e.what());
            return E_FAIL;
        }
        return S_OK;
    }

    HRESULT PoseEstimator::Predict(const uint8_t* bgra,
                                   uint32_t w, uint32_t h,
                                   BodyPose& out)
    {
        if (!IsLoaded() || !bgra || w == 0 || h == 0)
        {
            return E_UNEXPECTED;
        }

        // Read geometry from the loaded model. MoveNet variants we've
        // seen:
        //   Qualcomm Lightning: NCHW float32 [1,3,192,192]  (0..1)
        //   Qualcomm Thunder:   NCHW float32 [1,3,256,256]  (0..1)
        //   TF Hub / Xenova Thunder: NHWC int32 [1,256,256,3] (0..255)
        // Figure out which layout + dtype from the shape and drive the
        // preprocessing accordingly.
        const auto& shape = m_impl->inputShape;
        if (shape.size() != 4 || shape[0] != 1)
        {
            return E_NOTIMPL;
        }

        bool nchw;
        int  H, W;
        if (shape[1] == 3 && shape[2] > 0 && shape[3] > 0)
        {
            nchw = true;
            H = static_cast<int>(shape[2]);
            W = static_cast<int>(shape[3]);
        }
        else if (shape[3] == 3 && shape[1] > 0 && shape[2] > 0)
        {
            nchw = false;
            H = static_cast<int>(shape[1]);
            W = static_cast<int>(shape[2]);
        }
        else
        {
            return E_NOTIMPL;
        }

        try
        {
            std::array<int64_t, 4> tensorShape{ shape[0], shape[1], shape[2], shape[3] };
            const size_t elemCount = static_cast<size_t>(H) * W * 3;
            Ort::Value inputTensor{ nullptr };

            std::vector<float>   bufF32;
            std::vector<int32_t> bufI32;
            std::vector<uint8_t> bufU8;

            // Nearest-neighbour stretch. Not aspect-preserving on
            // purpose — pose is scale-tolerant, keeps preprocessing
            // branch-free.
            auto srcAt = [&](int outX, int outY) -> const uint8_t*
            {
                const uint32_t sy = (static_cast<uint64_t>(outY) * h) / H;
                const uint32_t sx = (static_cast<uint64_t>(outX) * w) / W;
                return &bgra[(static_cast<size_t>(sy) * w + sx) * 4];
            };

            switch (m_impl->inputElemType)
            {
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            {
                bufF32.assign(elemCount, 0.0f);
                const size_t plane = static_cast<size_t>(H) * W;
                for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const uint8_t* p = srcAt(x, y);
                    const float rF = p[2] * (1.0f / 255.0f);
                    const float gF = p[1] * (1.0f / 255.0f);
                    const float bF = p[0] * (1.0f / 255.0f);
                    if (nchw)
                    {
                        const size_t o = static_cast<size_t>(y) * W + x;
                        bufF32[0 * plane + o] = rF;
                        bufF32[1 * plane + o] = gF;
                        bufF32[2 * plane + o] = bF;
                    }
                    else
                    {
                        const size_t o = (static_cast<size_t>(y) * W + x) * 3;
                        bufF32[o + 0] = rF;
                        bufF32[o + 1] = gF;
                        bufF32[o + 2] = bF;
                    }
                }
                inputTensor = Ort::Value::CreateTensor<float>(
                    m_impl->memInfo, bufF32.data(), bufF32.size(),
                    tensorShape.data(), tensorShape.size());
                break;
            }
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
            {
                bufI32.assign(elemCount, 0);
                const size_t plane = static_cast<size_t>(H) * W;
                for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const uint8_t* p = srcAt(x, y);
                    const int32_t rI = p[2];
                    const int32_t gI = p[1];
                    const int32_t bI = p[0];
                    if (nchw)
                    {
                        const size_t o = static_cast<size_t>(y) * W + x;
                        bufI32[0 * plane + o] = rI;
                        bufI32[1 * plane + o] = gI;
                        bufI32[2 * plane + o] = bI;
                    }
                    else
                    {
                        const size_t o = (static_cast<size_t>(y) * W + x) * 3;
                        bufI32[o + 0] = rI;
                        bufI32[o + 1] = gI;
                        bufI32[o + 2] = bI;
                    }
                }
                inputTensor = Ort::Value::CreateTensor<int32_t>(
                    m_impl->memInfo, bufI32.data(), bufI32.size(),
                    tensorShape.data(), tensorShape.size());
                break;
            }
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
            {
                bufU8.assign(elemCount, 0);
                const size_t plane = static_cast<size_t>(H) * W;
                for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const uint8_t* p = srcAt(x, y);
                    const uint8_t rU = p[2];
                    const uint8_t gU = p[1];
                    const uint8_t bU = p[0];
                    if (nchw)
                    {
                        const size_t o = static_cast<size_t>(y) * W + x;
                        bufU8[0 * plane + o] = rU;
                        bufU8[1 * plane + o] = gU;
                        bufU8[2 * plane + o] = bU;
                    }
                    else
                    {
                        const size_t o = (static_cast<size_t>(y) * W + x) * 3;
                        bufU8[o + 0] = rU;
                        bufU8[o + 1] = gU;
                        bufU8[o + 2] = bU;
                    }
                }
                inputTensor = Ort::Value::CreateTensor<uint8_t>(
                    m_impl->memInfo, bufU8.data(), bufU8.size(),
                    tensorShape.data(), tensorShape.size());
                break;
            }
            default:
                return E_NOTIMPL;
            }

            const char* inputNames[]  = { m_impl->inputName.c_str() };
            const char* outputNames[] = { m_impl->outputName.c_str() };

            auto outputs = m_impl->session->Run(
                Ort::RunOptions{ nullptr },
                inputNames, &inputTensor, 1,
                outputNames, 1);
            if (outputs.empty())
            {
                return E_FAIL;
            }

            // MoveNet output layout: [1, 1, 17, 3] -> (y, x, confidence).
            const float* raw = outputs[0].GetTensorData<float>();
            float sum = 0.0f;
            for (int i = 0; i < BodyPose::kJointCount; ++i)
            {
                out.joints[i].y          = raw[i * 3 + 0];
                out.joints[i].x          = raw[i * 3 + 1];
                out.joints[i].confidence = raw[i * 3 + 2];
                sum += out.joints[i].confidence;
            }
            out.avgConfidence  = sum / BodyPose::kJointCount;
            out.personDetected = (out.avgConfidence > 0.3f);
        }
        catch (const Ort::Exception& e)
        {
            LogF(L"pose Predict failed: %hs", e.what());
            return E_FAIL;
        }
        return S_OK;
    }
}
