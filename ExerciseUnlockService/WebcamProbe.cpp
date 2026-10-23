#include "WebcamProbe.h"
#include "PoseEstimator.h"
#include "SquatDetector.h"
#include "PushUpDetector.h"
#include "PostureClassifier.h"
#include "Config.h"
#include "PersistentState.h"
#include "State.h"
#include "Log.h"
#include "Protocol.h"
#include "Paths.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wtsapi32.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cwchar>
#include <cwctype>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wtsapi32.lib")

using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Svc
{
    static constexpr int   kFramesPerProbe = 5;
    // Kept short so a camera plugged in / unmuted mid-service is picked
    // up quickly. The service reopens the device on every cycle, so
    // "hot" reconnects don't require a restart.
    static constexpr DWORD kRetryIntervalMs = 5000;

    static void PublishWebcam(WebcamState s, uint32_t frames = 0,
                              uint32_t w = 0, uint32_t h = 0,
                              HRESULT hr = S_OK)
    {
        auto& st = GetState();
        st.webcamState.store(static_cast<uint8_t>(s));
        if (frames) st.webcamFrames.store(frames);
        if (w)      st.webcamWidth.store(w);
        if (h)      st.webcamHeight.store(h);
        st.webcamLastHresult.store(static_cast<uint32_t>(hr));
    }

    static void PublishPose(const BodyPose& p, uint32_t frameNo)
    {
        auto& st = GetState();
        {
            std::lock_guard<std::mutex> lk(st.poseMx);
            for (int i = 0; i < BodyPose::kJointCount; ++i)
            {
                st.joints[i].y          = p.joints[i].y;
                st.joints[i].x          = p.joints[i].x;
                st.joints[i].confidence = p.joints[i].confidence;
            }
        }
        st.poseAvgConfidence.store(p.avgConfidence);
        st.personDetected.store(p.personDetected ? 1 : 0);
        st.poseFramesInferred.store(frameNo);
    }

    // Camera just closed: wipe everything derived from it so neither
    // the tile (posture "ready" beep) nor the tools act on a stale
    // skeleton / frame when the camera comes back on the next lock.
    static void ClearLiveState()
    {
        auto& st = GetState();
        {
            std::lock_guard<std::mutex> lk(st.poseMx);
            std::memset(st.joints, 0, sizeof(st.joints));
        }
        st.poseAvgConfidence.store(0.0f);
        st.personDetected.store(0);
        st.postureKind.store(static_cast<uint8_t>(PostureKind::None));
        st.squatPhase.store(static_cast<uint8_t>(SquatPhase::Unknown));
        st.pushUpPhase.store(static_cast<uint8_t>(PushUpPhase::Unknown));
        {
            std::lock_guard<std::mutex> lk(st.latestFrameMx);
            st.latestFrameBgra.clear();
            st.latestFrameWidth  = 0;
            st.latestFrameHeight = 0;
        }
    }

    // What the physical console is showing. The camera only runs
    // while that's LogonUI — the lock screen of a signed-in user, or
    // the sign-in screen with nobody signed in (boot, after sign-out,
    // mid fast-user-switch) — never over somebody's desktop.
    struct ConsoleView
    {
        DWORD        sessionId = 0xFFFFFFFF;
        std::wstring user;
        LONG         flags     = -1;     // WTS_SESSIONSTATE_*, -1 = unknown
        bool         logonUi   = true;
    };

    static ConsoleView QueryConsoleView()
    {
        ConsoleView v;
        v.sessionId = WTSGetActiveConsoleSessionId();
        // No session attached = console mid-switch; LogonUI is next.
        if (v.sessionId == 0xFFFFFFFF) return v;

        WTSINFOEXW* info  = nullptr;
        DWORD       bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, v.sessionId,
                                         WTSSessionInfoEx,
                                         reinterpret_cast<LPWSTR*>(&info), &bytes) ||
            !info)
        {
            // Can't tell: fail open rather than leave the child at a
            // lock screen with a dead camera.
            return v;
        }
        if (info->Level == 1)
        {
            const WTSINFOEX_LEVEL1_W& l1 = info->Data.WTSInfoExLevel1;
            v.user  = l1.UserName;
            v.flags = l1.SessionFlags;
            // Nobody signed in -> sign-in screen. Otherwise only an
            // explicit UNLOCK means "desktop"; UNKNOWN fails open too.
            v.logonUi = v.user.empty() || v.flags != WTS_SESSIONSTATE_UNLOCK;
        }
        WTSFreeMemory(info);
        return v;
    }

    // Should the camera be open right now? Pure — no logging, safe from
    // any thread (the watchdog calls it too).
    static bool ComputeCameraWanted(ConsoleView& v, const wchar_t*& why)
    {
        v = QueryConsoleView();
        bool on;
        if (IsSessionActive())
        {
            // Reps don't count during a session, and the child may
            // need the camera for a call.
            on  = false;
            why = L"unlock session running";
        }
        else if (!Config::CameraLockScreenOnly())
        {
            on  = true;
            why = L"lock_screen_only=0";
        }
        else if (v.logonUi)
        {
            on  = true;
            why = L"lock / sign-in screen";
        }
        else
        {
            on  = false;
            why = L"desktop in use";
        }
        return on;
    }

    // Same answer, logging every change so service.log shows when / why
    // the LED went on and off. Webcam thread only (s_lastOn isn't
    // synchronized).
    static bool CameraWanted()
    {
        static int s_lastOn = -1;

        ConsoleView    v;
        const wchar_t* why = L"";
        const bool     on  = ComputeCameraWanted(v, why);
        if ((on ? 1 : 0) != s_lastOn)
        {
            s_lastOn = on ? 1 : 0;
            LogF(L"camera gate: %s (%s; console session=%ld user='%s' flags=%ld)",
                 on ? L"ON" : L"OFF", why,
                 static_cast<long>(v.sessionId), v.user.c_str(),
                 static_cast<long>(v.flags));
        }
        return on;
    }

    // Ask MF's source reader for a RGB32 (BGRA byte order) top-down
    // stream. If the camera can't produce that natively, MF inserts a
    // decoder/converter transform automatically.
    static HRESULT ConfigureRgb32Output(IMFSourceReader* reader)
    {
        IMFMediaType* type = nullptr;
        HRESULT hr = MFCreateMediaType(&type);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE,  MFVideoFormat_RGB32);
        if (SUCCEEDED(hr))
        {
            hr = reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                nullptr, type);
        }
        if (type) type->Release();
        return hr;
    }

    static bool IContains(const std::wstring& haystack, const std::wstring& needle)
    {
        if (needle.empty()) return true;
        std::wstring h = haystack, n = needle;
        for (auto& c : h) c = towlower(c);
        for (auto& c : n) c = towlower(c);
        return h.find(n) != std::wstring::npos;
    }

    // True if `name` contains any comma-separated substring from
    // Config's virtual-camera blocklist (case-insensitive). Used to
    // refuse OBS Virtual Camera / DroidCam / etc. so a child can't
    // feed a recorded video into MoveNet.
    static bool IsVirtualCameraName(const std::wstring& name)
    {
        const std::wstring list = Config::VirtualCameraBlocklist();
        std::wstring token;
        for (wchar_t c : list)
        {
            if (c == L',')
            {
                if (!token.empty() && IContains(name, token)) return true;
                token.clear();
            }
            else if (c != L' ' && c != L'\t')
            {
                token.push_back(c);
            }
        }
        if (!token.empty() && IContains(name, token)) return true;
        return false;
    }

    // Enumerate video-capture devices and return true if the current
    // preferred-camera substring matches at least one of them. Used
    // by the stream loop to bail early if the preferred camera is
    // unplugged / powered off (MF ReadSample sometimes hangs instead
    // of returning an error in that case).
    static bool PreferredCameraAvailable()
    {
        const std::wstring pref = Config::CameraPreference();
        if (pref.empty()) return true;   // no preference means "any"

        IMFAttributes* attrs = nullptr;
        IMFActivate**  devs  = nullptr;
        UINT32         count = 0;

        HRESULT hr = MFCreateAttributes(&attrs, 1);
        if (SUCCEEDED(hr))
        {
            hr = attrs->SetGUID(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        }
        if (SUCCEEDED(hr))
        {
            hr = MFEnumDeviceSources(attrs, &devs, &count);
        }
        if (attrs) attrs->Release();
        if (FAILED(hr) || count == 0)
        {
            if (devs) CoTaskMemFree(devs);
            return false;
        }

        bool found = false;
        for (UINT32 i = 0; i < count; ++i)
        {
            wchar_t* n = nullptr;
            UINT32   nl = 0;
            if (SUCCEEDED(devs[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &n, &nl)))
            {
                std::wstring name = n;
                CoTaskMemFree(n);
                if (IContains(name, pref)) { found = true; break; }
            }
        }
        for (UINT32 i = 0; i < count; ++i) if (devs[i]) devs[i]->Release();
        CoTaskMemFree(devs);
        return found;
    }

    // Open a video device via MF, produce IMFSourceReader. Selection:
    //   * If config.ini [camera]/preferred is set, first friendly name
    //     that matches (case-insensitive substring) wins.
    //   * Otherwise fall back to the first enumerated device.
    // Caller owns both handles (they're only null on failure).
    static HRESULT OpenFirstCamera(IMFMediaSource** outSource,
                                   IMFSourceReader** outReader)
    {
        *outSource = nullptr;
        *outReader = nullptr;

        IMFAttributes* attrs   = nullptr;
        IMFActivate**  devices = nullptr;
        UINT32         count   = 0;

        HRESULT hr = MFCreateAttributes(&attrs, 1);
        if (SUCCEEDED(hr))
        {
            hr = attrs->SetGUID(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        }
        if (SUCCEEDED(hr))
        {
            hr = MFEnumDeviceSources(attrs, &devices, &count);
        }
        if (attrs) attrs->Release();

        if (FAILED(hr)) return hr;

        if (count == 0)
        {
            if (devices) CoTaskMemFree(devices);
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        std::wstring pref = Config::CameraPreference();
        UINT32 picked = 0;
        bool   pickedByName = false;

        // Enumerate + log every friendly name so DebugView shows what
        // strings we actually see (useful for tuning config.ini).
        std::vector<std::wstring> names(count);
        for (UINT32 i = 0; i < count; ++i)
        {
            wchar_t* name = nullptr;
            UINT32   nameLen = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
            {
                names[i] = name;
                CoTaskMemFree(name);
                LogF(L"camera[%u]: %s", i, names[i].c_str());
            }
        }

        // Refuse virtual cameras up front — a child feeding a video
        // loop through OBS Virtual Camera would trivially game the
        // rep counter otherwise.
        for (UINT32 i = 0; i < count; ++i)
        {
            if (IsVirtualCameraName(names[i]))
            {
                LogF(L"camera: refusing virtual camera device[%u]='%s'",
                     i, names[i].c_str());
                names[i].clear();  // mark unavailable for the pref match
            }
        }

        if (!pref.empty())
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                if (IContains(names[i], pref))
                {
                    picked = i;
                    pickedByName = true;
                    LogF(L"camera: matched preference '%s' -> device[%u]",
                         pref.c_str(), i);
                    break;
                }
            }
            if (!pickedByName)
            {
                // Strict mode: the parent explicitly configured a
                // preferred camera. If it isn't there, wait for it —
                // don't silently fall back to the built-in Acer or
                // whatever else is enumerated first.
                LogF(L"camera: preferred '%s' not available — waiting",
                     pref.c_str());
                for (UINT32 i = 0; i < count; ++i)
                {
                    if (devices[i]) devices[i]->Release();
                }
                CoTaskMemFree(devices);
                return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            }
        }

        hr = devices[picked]->ActivateObject(IID_PPV_ARGS(outSource));

        for (UINT32 i = 0; i < count; ++i)
        {
            if (devices[i]) devices[i]->Release();
        }
        CoTaskMemFree(devices);

        if (FAILED(hr) || !*outSource) return hr;

        // Ask the source reader to insert MF's built-in Video Processor
        // MFT if the camera can't produce our requested format natively.
        // Without this flag SetCurrentMediaType(RGB32) fails on cameras
        // that only offer NV12/YUY2/MJPEG (i.e. most of them).
        IMFAttributes* readerAttrs = nullptr;
        HRESULT hrAttrs = MFCreateAttributes(&readerAttrs, 1);
        if (SUCCEEDED(hrAttrs))
        {
            readerAttrs->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }

        hr = MFCreateSourceReaderFromMediaSource(*outSource, readerAttrs, outReader);
        if (readerAttrs) readerAttrs->Release();

        if (FAILED(hr))
        {
            (*outSource)->Shutdown();
            (*outSource)->Release();
            *outSource = nullptr;
            return hr;
        }

        HRESULT hrFmt = ConfigureRgb32Output(*outReader);
        if (FAILED(hrFmt))
        {
            LogF(L"camera: RGB32 still not accepted (0x%08X) — pose won't work",
                 hrFmt);
            // Not fatal for the probe; StreamPose will bail on the size
            // check and no bogus BGRA will be fed to the model.
        }
        return S_OK;
    }

    // ---- capture watchdog ----
    // A camera can hand over one frame and then stall (seen with an
    // OsmoPocket3 at the lock screen). The synchronous ReadSample then
    // blocks forever: no frames, no gate checks, the device stays open
    // even after unlock. The watchdog shuts the media source down from
    // outside, which fails the pending ReadSample so the webcam thread
    // closes the device and reopens it. If even that doesn't free the
    // thread, the service exits and the SCM restarts it (failure
    // actions are set by setup).
    static constexpr uint64_t kStallMs         = 8000;    // open, no frame for this long
    static constexpr uint64_t kUnwantedStuckMs = 3000;    // not wanted, still open
    static constexpr uint64_t kHardStuckMs     = 20000;   // after Shutdown, still open

    static std::mutex            g_srcMx;
    static IMFMediaSource*       g_openSource = nullptr;  // AddRef'd while the device is open
    static std::atomic<uint64_t> g_lastFrameMs{ 0 };

    static void NoteFrame() { g_lastFrameMs.store(GetTickCount64()); }

    static void WatchdogLoop(HANDLE stopEvent)
    {
        uint64_t unwantedSince = 0;
        uint64_t firedAt = 0;
        while (WaitForSingleObject(stopEvent, 1000) == WAIT_TIMEOUT)
        {
            ConsoleView    v;
            const wchar_t* why = L"";
            const bool     wanted = ComputeCameraWanted(v, why);
            const uint64_t now = GetTickCount64();

            std::lock_guard<std::mutex> lk(g_srcMx);
            if (!g_openSource)
            {
                unwantedSince = 0;
                firedAt = 0;
                continue;
            }
            if (firedAt != 0)
            {
                if (now - firedAt > kHardStuckMs)
                {
                    LogF(L"watchdog: webcam thread still stuck %llu ms after shutdown - restarting the service",
                         static_cast<unsigned long long>(now - firedAt));
                    PersistentState::Save();
                    TerminateProcess(GetCurrentProcess(), 3);
                }
                continue;
            }
            unwantedSince = wanted ? 0 : (unwantedSince ? unwantedSince : now);
            const uint64_t idle = now - g_lastFrameMs.load();
            if (idle > kStallMs)
                LogF(L"watchdog: no frame for %llu ms - resetting the camera",
                     static_cast<unsigned long long>(idle));
            else if (unwantedSince && now - unwantedSince > kUnwantedStuckMs)
                LogF(L"watchdog: camera not wanted (%s) but capture is stuck - releasing it", why);
            else
                continue;
            firedAt = now;
            g_openSource->Shutdown();
        }
    }

    // Read N frames just to prove the pipeline is alive. Publishes
    // WebcamState / frame count / dims into State along the way.
    static HRESULT ProbeStreamingReader(IMFSourceReader* reader,
                                        HANDLE stopEvent)
    {
        PublishWebcam(WebcamState::Probing);

        uint32_t framesRead = 0;
        uint32_t width = 0, height = 0;
        HRESULT hr = S_OK;

        while (framesRead < kFramesPerProbe &&
               WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT)
        {
            IMFSample* sample = nullptr;
            DWORD      streamIdx = 0;
            DWORD      flags = 0;
            LONGLONG   ts = 0;

            hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                0, &streamIdx, &flags, &ts, &sample);
            if (FAILED(hr))
            {
                LogF(L"probe: ReadSample failed 0x%08X", static_cast<unsigned>(hr));
                break;
            }

            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                if (sample) sample->Release();
                break;
            }
            if (!sample)
            {
                static int s_emptyLogged = 0;
                if (s_emptyLogged < 10)
                {
                    ++s_emptyLogged;
                    LogF(L"probe: ReadSample returned no sample (flags=0x%lX)", flags);
                }
                continue;
            }
            NoteFrame();
            {
                if (width == 0)
                {
                    IMFMediaType* mt = nullptr;
                    if (SUCCEEDED(reader->GetCurrentMediaType(
                            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &mt)))
                    {
                        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &width, &height);
                        mt->Release();
                    }
                    LogF(L"camera first frame: %ux%u", width, height);
                }
                ++framesRead;
                sample->Release();
                PublishWebcam(WebcamState::Probing, framesRead, width, height);
            }
        }

        if (framesRead > 0 && SUCCEEDED(hr))
        {
            PublishWebcam(WebcamState::Ok, framesRead, width, height);
            LogF(L"camera probe OK: %u frame(s) at %ux%u",
                 framesRead, width, height);
            return S_OK;
        }
        PublishWebcam(WebcamState::Failed, framesRead, width, height,
                      SUCCEEDED(hr) ? E_FAIL : hr);
        return SUCCEEDED(hr) ? E_FAIL : hr;
    }

    // Continuous capture + pose. Exits on stopEvent, a hard read error
    // (broken pipe, camera disconnect, etc) or — returning S_FALSE —
    // once the camera is no longer wanted (see CameraWanted).
    static HRESULT StreamPose(IMFSourceReader* reader,
                              PoseEstimator*   pose,
                              HANDLE           stopEvent,
                              HANDLE           wakeEvent)
    {
        if (!pose) return E_UNEXPECTED;

        uint32_t width = 0, height = 0;
        // Ask the reader for its current frame size once — we'll re-ask
        // only if we see a size change in-flight (rare on webcams).
        {
            IMFMediaType* mt = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &mt)))
            {
                MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &width, &height);
                mt->Release();
            }
        }
        if (width == 0 || height == 0)
        {
            LogF(L"stream: unable to read frame size");
            return E_FAIL;
        }

        BodyPose          pose_out{};
        SquatDetector     squats;
        PushUpDetector    pushups;
        PostureClassifier posture;
        uint32_t       frameNo = 0;
        HRESULT        hr = S_OK;
        uint64_t       lastEnumCheckMs = GetTickCount64();
        uint64_t       lastGateCheckMs = lastEnumCheckMs;
        // Liveness: cheap "sample a bunch of bytes" hash, compared
        // frame to frame. A prerecorded loop / frozen video ends up
        // with too many consecutive identical hashes.
        uint64_t       lastFrameHash = 0;
        int            staticStreak  = 0;

        LogF(L"stream: pose-per-frame loop started at %ux%u", width, height);

        while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT)
        {
            // Release the camera as soon as it isn't wanted any more:
            // the kid clicked Submit (session started — Zoom etc. must
            // get it back) or somebody unlocked the PC. A session
            // change pokes wakeEvent so this reacts within a frame;
            // the 1 s re-check covers anything that slipped past.
            const uint64_t nowMs = GetTickCount64();
            const bool poked = wakeEvent &&
                WaitForSingleObject(wakeEvent, 0) == WAIT_OBJECT_0;
            if (IsSessionActive() || poked || nowMs - lastGateCheckMs >= 1000)
            {
                lastGateCheckMs = nowMs;
                if (!CameraWanted())
                {
                    LogF(L"stream: camera no longer wanted — releasing");
                    hr = S_FALSE;
                    break;
                }
            }

            // Periodic sanity check — if the preferred camera has
            // disappeared from the device list (unplugged / powered
            // off), close the stream and go back to the wait loop
             // instead of hanging in ReadSample.
            if (nowMs - lastEnumCheckMs >= 5000)
            {
                lastEnumCheckMs = nowMs;
                if (!PreferredCameraAvailable())
                {
                    LogF(L"stream: preferred camera gone — closing stream");
                    break;
                }
            }

            IMFSample* sample = nullptr;
            DWORD      streamIdx = 0;
            DWORD      flags = 0;
            LONGLONG   ts = 0;

            hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                0, &streamIdx, &flags, &ts, &sample);
            if (FAILED(hr))
            {
                LogF(L"stream: ReadSample failed 0x%08X", hr);
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                LogF(L"stream: end of stream");
                if (sample) sample->Release();
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            {
                IMFMediaType* mt = nullptr;
                if (SUCCEEDED(reader->GetCurrentMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &mt)))
                {
                    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &width, &height);
                    mt->Release();
                    LogF(L"stream: media type changed -> %ux%u", width, height);
                }
            }
            if (!sample) continue;
            NoteFrame();

            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer)
            {
                BYTE*  data   = nullptr;
                DWORD  maxLen = 0;
                DWORD  curLen = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen)))
                {
                    // Once per session: dump the first frame to disk +
                    // log some pixel stats so we can eyeball what MF
                    // actually handed us (right-side-up? BGRA order?).
                    // Gated by a static flag rather than frameNo so a
                    // long streak of Predict failures doesn't re-write
                    // the file on every frame.
                    static bool debugFrameWritten = false;
                    if (!debugFrameWritten)
                    {
                        debugFrameWritten = true;
                        const size_t expected = static_cast<size_t>(width) * height * 4;
                        LogF(L"stream: first frame curLen=%lu expected=%zu", curLen, expected);
                        LogF(L"stream: first 12 bytes = %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             data[0], data[1], data[2], data[3],
                             data[4], data[5], data[6], data[7],
                             data[8], data[9], data[10], data[11]);
                        uint64_t sum = 0;
                        size_t samples = curLen < 40000 ? curLen : 40000;
                        for (size_t i = 0; i < samples; ++i) sum += data[i];
                        LogF(L"stream: avg byte = %llu (over first %zu bytes)",
                             static_cast<unsigned long long>(sum / (samples ? samples : 1)),
                             samples);

                        // Write BMP so we can visually inspect the frame
                        // (open in Paint / any image viewer). BMP layout
                        // is bottom-up 24bpp, so we downgrade from BGRA
                        // and flip vertically while writing.
                        // Data dir, not next to the exe: the frame shows
                        // whoever stood at the lock screen, and Program
                        // Files is readable by every user.
                        const std::wstring dumpPath = DataPath(L"debug_frame.bmp");
                        const wchar_t* path = dumpPath.c_str();
                        {
                            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (f != INVALID_HANDLE_VALUE)
                            {
                                #pragma pack(push, 1)
                                struct { char m[2]; uint32_t size; uint32_t rsvd; uint32_t off;
                                         uint32_t hdrSize; int32_t w; int32_t h;
                                         uint16_t planes; uint16_t bpp;
                                         uint32_t comp; uint32_t imgSize;
                                         uint32_t xppm; uint32_t yppm;
                                         uint32_t clrUsed; uint32_t clrImp; } bmp{};
                                #pragma pack(pop)
                                const uint32_t rowSize = ((width * 3 + 3) & ~3u);
                                const uint32_t imgSize = rowSize * height;
                                bmp.m[0] = 'B'; bmp.m[1] = 'M';
                                bmp.size = 54 + imgSize;
                                bmp.off  = 54;
                                bmp.hdrSize = 40;
                                bmp.w = static_cast<int32_t>(width);
                                bmp.h = static_cast<int32_t>(height); // positive = bottom-up
                                bmp.planes = 1;
                                bmp.bpp = 24;
                                bmp.imgSize = imgSize;
                                DWORD wrote = 0;
                                WriteFile(f, &bmp, sizeof(bmp), &wrote, nullptr);
                                // Emit rows bottom-up, converting BGRA -> BGR (BMP order = BGR).
                                std::vector<uint8_t> row(rowSize, 0);
                                for (int y = static_cast<int>(height) - 1; y >= 0; --y)
                                {
                                    const uint8_t* src = data + static_cast<size_t>(y) * width * 4;
                                    for (uint32_t x = 0; x < width; ++x)
                                    {
                                        row[x * 3 + 0] = src[x * 4 + 0]; // B
                                        row[x * 3 + 1] = src[x * 4 + 1]; // G
                                        row[x * 3 + 2] = src[x * 4 + 2]; // R
                                    }
                                    WriteFile(f, row.data(), rowSize, &wrote, nullptr);
                                }
                                CloseHandle(f);
                                LogF(L"stream: wrote %s", path);
                            }
                        }
                    }

                    if (curLen >= width * height * 4)
                    {
                        // Liveness hash — sample ~1024 bytes evenly
                        // across the frame. Cheap; catches identical
                        // frames regardless of resolution.
                        uint64_t h64 = 0xcbf29ce484222325ULL;
                        const size_t step = curLen / 1024;
                        for (size_t i = 0; i < curLen; i += step ? step : 1)
                        {
                            h64 ^= data[i];
                            h64 *= 0x100000001b3ULL;
                        }
                        if (h64 == lastFrameHash)
                        {
                            ++staticStreak;
                            const int limit = Config::LivenessMaxStaticFrames();
                            if (limit > 0 && staticStreak == limit)
                            {
                                LogF(L"liveness: %d identical frames — "
                                     L"marking webcam Failed (fake feed?)",
                                     staticStreak);
                                PublishWebcam(WebcamState::Failed,
                                              frameNo, width, height,
                                              HRESULT_FROM_WIN32(ERROR_DEVICE_FEATURE_NOT_SUPPORTED));
                            }
                        }
                        else
                        {
                            staticStreak = 0;
                            lastFrameHash = h64;
                        }

                        // Cache the latest BGRA frame so Op::SaveWebcamFrame
                        // over the pipe can dump it to disk without
                        // touching the camera itself (which the service
                        // owns exclusively).
                        {
                            auto& st = GetState();
                            std::lock_guard<std::mutex> lk(st.latestFrameMx);
                            const size_t bytes = static_cast<size_t>(width) * height * 4;
                            if (st.latestFrameBgra.size() != bytes)
                                st.latestFrameBgra.resize(bytes);
                            std::memcpy(st.latestFrameBgra.data(), data, bytes);
                            st.latestFrameWidth  = width;
                            st.latestFrameHeight = height;
                        }

                        HRESULT pr = pose->Predict(data, width, height, pose_out);
                        if (SUCCEEDED(pr))
                        {
                            ++frameNo;
                            PublishPose(pose_out, frameNo);
                            // Skip detector updates while liveness is
                            // flagged — never credit reps against a
                            // frozen / looped video feed.
                            const int limit = Config::LivenessMaxStaticFrames();
                            const bool livenessOk =
                                (limit <= 0) || (staticStreak < limit);
                            if (livenessOk)
                            {
                                squats.Update(pose_out);
                                pushups.Update(pose_out);
                                posture.Update(pose_out);
                            }
                        }
                        else if (frameNo == 0)
                        {
                            LogF(L"stream: pose Predict failed 0x%08X", pr);
                        }
                    }
                    buffer->Unlock();
                }
                buffer->Release();
            }
            sample->Release();
        }

        LogF(L"stream: loop stopped after %u inferred frame(s)", frameNo);
        return hr;
    }

    static void CloseCamera(IMFMediaSource* source, IMFSourceReader* reader)
    {
        if (reader) reader->Release();
        if (source)
        {
            source->Shutdown();
            source->Release();
        }
    }

    void WebcamProbe::Run()
    {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hrMf))
        {
            LogF(L"MFStartup failed: 0x%08X", hrMf);
            PublishWebcam(WebcamState::Failed, 0, 0, 0, hrMf);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            return;
        }

        LogF(L"webcam thread starting");

        while (WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT)
        {
            // Camera stays closed while it isn't wanted (unlock session
            // running, or someone's desktop on the console): LED off,
            // free for Zoom / Camera app. Re-checked every second, and
            // immediately on a session change via Wake().
            if (!CameraWanted())
            {
                PublishWebcam(WebcamState::Idle);
                HANDLE waits[2] = { m_stopEvent, m_wakeEvent.load() };
                const DWORD nWaits = waits[1] ? 2 : 1;
                if (WaitForMultipleObjects(nWaits, waits, FALSE, 1000) == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }

            IMFMediaSource*  source = nullptr;
            IMFSourceReader* reader = nullptr;

            HRESULT hr = OpenFirstCamera(&source, &reader);
            if (FAILED(hr))
            {
                if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                {
                    PublishWebcam(WebcamState::NoDevice);
                }
                else
                {
                    LogF(L"camera: open failed 0x%08X", hr);
                    PublishWebcam(WebcamState::Failed, 0, 0, 0, hr);
                }
                if (WaitForSingleObject(m_stopEvent, kRetryIntervalMs) == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }

            // Hand the open device to the watchdog (see WatchdogLoop).
            {
                std::lock_guard<std::mutex> lk(g_srcMx);
                source->AddRef();
                g_openSource = source;
                NoteFrame();   // the stall clock starts at open
            }

            hr = ProbeStreamingReader(reader, m_stopEvent);
            HRESULT streamHr = E_FAIL;
            if (SUCCEEDED(hr) && m_pose && m_pose->IsLoaded())
            {
                // Probe passed — drop straight into continuous pose
                // capture with the same open reader. Runs until the
                // camera isn't wanted any more (S_FALSE) or breaks.
                streamHr = StreamPose(reader, m_pose, m_stopEvent,
                                      m_wakeEvent.load());
            }
            else if (SUCCEEDED(hr))
            {
                LogF(L"pose not loaded — camera going idle after probe");
                // Hold the probed device only while it's still wanted.
                while (WaitForSingleObject(m_stopEvent, 1000) == WAIT_TIMEOUT &&
                       CameraWanted())
                {
                }
                streamHr = S_FALSE;
            }

            {
                std::lock_guard<std::mutex> lk(g_srcMx);
                if (g_openSource) g_openSource->Release();
                g_openSource = nullptr;
            }
            CloseCamera(source, reader);
            ClearLiveState();

            // Released on purpose (lock screen gone / session started):
            // straight back to the idle wait at the top of the loop.
            if (streamHr == S_FALSE)
            {
                continue;
            }

            // If we got here via a broken read (unplug, sleep, etc)
            // wait a bit before retrying. If the stop event fires, we
            // exit here without another camera reopen.
            if (WaitForSingleObject(m_stopEvent, kRetryIntervalMs) == WAIT_OBJECT_0)
            {
                break;
            }
        }

        LogF(L"webcam thread stopping");
        MFShutdown();
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }

    DWORD WINAPI WebcamProbe::Thunk(LPVOID param)
    {
        static_cast<WebcamProbe*>(param)->Run();
        return 0;
    }

    void WebcamProbe::Start(HANDLE stopEvent)
    {
        m_stopEvent = stopEvent;
        if (!m_wakeEvent.load())
        {
            m_wakeEvent.store(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        }
        m_thread = CreateThread(nullptr, 0, Thunk, this, 0, nullptr);
        if (!m_thread)
        {
            LogF(L"webcam CreateThread failed: %lu", GetLastError());
        }
        m_watchdog = CreateThread(nullptr, 0, WatchdogThunk, this, 0, nullptr);
    }

    DWORD WINAPI WebcamProbe::WatchdogThunk(LPVOID param)
    {
        WatchdogLoop(static_cast<WebcamProbe*>(param)->m_stopEvent);
        return 0;
    }

    void WebcamProbe::Join()
    {
        // Service stop: a ReadSample stuck on a stalled camera would hold
        // the stop forever (setup then can't replace the exe). Shut the
        // device down so the read returns, and don't wait more than a
        // few seconds for the webcam thread either way.
        {
            std::lock_guard<std::mutex> lk(g_srcMx);
            if (g_openSource) g_openSource->Shutdown();
        }
        if (m_watchdog)
        {
            WaitForSingleObject(m_watchdog, INFINITE);   // exits on the stop event
            CloseHandle(m_watchdog);
            m_watchdog = nullptr;
        }
        if (m_thread)
        {
            if (WaitForSingleObject(m_thread, 8000) == WAIT_TIMEOUT)
            {
                LogF(L"webcam thread did not stop within 8 s - leaving it behind");
                return;   // process exit takes it down
            }
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
    }

    void WebcamProbe::Wake()
    {
        if (HANDLE h = m_wakeEvent.load())
        {
            SetEvent(h);
        }
    }
}
