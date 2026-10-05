#include "Credential.h"
#include "Log.h"
#include "resource.h"
#include "PipeClient.h"
#include "guids.h"
#include "Vault.h"

#include <ntsecapi.h>
#include <strsafe.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shlwapi.lib")

extern HINSTANCE g_hInstance;

extern LONG g_dllRefCount;

namespace ExerciseUnlock
{
    // Play a WAV via PlaySound; fall back to MessageBeep if the
    // path is missing/unreadable or PlaySound returns FALSE. We
    // used to pipe this through a helper process in the signed-in
    // user's session, but it turned out PlaySound in LogonUI's
    // session (0) works fine on at least our test machine as long
    // as the WAV path is reachable and the file itself is readable
    // by SYSTEM — the real previous failure mode was ProgramData
    // ACL inheritance leaving sounds.ini sdv29-only. ServiceMain
    // now fixes that at startup, so the direct path is the whole
    // story.
    static void PlayWavOrFallback(const std::wstring& path, UINT fallbackSound)
    {
        const bool haveFile = !path.empty() && PathFileExistsW(path.c_str());
        LogF(L"PlayWavOrFallback: path='%s' exists=%d fallback=0x%X",
             path.c_str(), haveFile ? 1 : 0, fallbackSound);
        if (haveFile)
        {
            if (PlaySoundW(path.c_str(), nullptr,
                           SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
            {
                LogF(L"PlayWavOrFallback: PlaySound ok");
                return;
            }
            LogF(L"PlayWavOrFallback: PlaySound failed");
        }
        LogF(L"PlayWavOrFallback: falling back to MessageBeep(0x%X)", fallbackSound);
        MessageBeep(fallbackSound);
    }

    static HRESULT DupCoString(PCWSTR src, PWSTR* dst)
    {
        if (!dst)
        {
            return E_POINTER;
        }
        if (!src)
        {
            *dst = nullptr;
            return S_OK;
        }
        size_t chars = wcslen(src) + 1;
        auto* buf = static_cast<PWSTR>(CoTaskMemAlloc(chars * sizeof(wchar_t)));
        if (!buf)
        {
            *dst = nullptr;
            return E_OUTOFMEMORY;
        }
        wcscpy_s(buf, chars, src);
        *dst = buf;
        return S_OK;
    }

    Credential::Credential()
        : m_refCount(1),
          m_events(nullptr),
          m_pollTimer(nullptr)
    {
        InterlockedIncrement(&g_dllRefCount);
    }

    Credential::~Credential()
    {
        StopPolling();
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_events)
        {
            m_events->Release();
            m_events = nullptr;
        }
        InterlockedDecrement(&g_dllRefCount);
    }

    HRESULT Credential::Initialize(const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR* fields,
                                   DWORD fieldCount,
                                   PCWSTR userSid)
    {
        if (!fields || fieldCount == 0)
        {
            return E_INVALIDARG;
        }
        m_fields.assign(fields, fields + fieldCount);
        m_values.assign(fieldCount, std::wstring());
        // Seed the visible strings from the descriptor labels so they
        // are shown as-is by the tile.
        for (DWORD i = 0; i < fieldCount; ++i)
        {
            if (fields[i].pszLabel)
            {
                m_values[i] = fields[i].pszLabel;
            }
        }
        m_userSid = userSid ? userSid : L"";
        return S_OK;
    }

    IFACEMETHODIMP Credential::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown ||
            riid == IID_ICredentialProviderCredential ||
            riid == IID_ICredentialProviderCredential2)
        {
            *ppv = static_cast<ICredentialProviderCredential2*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) Credential::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) Credential::Release()
    {
        LONG n = InterlockedDecrement(&m_refCount);
        if (n == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP Credential::Advise(ICredentialProviderCredentialEvents* pcpce)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_events)
        {
            m_events->Release();
            m_events = nullptr;
        }
        if (pcpce)
        {
            m_events = pcpce;
            m_events->AddRef();
        }
        return S_OK;
    }

    IFACEMETHODIMP Credential::UnAdvise()
    {
        // Stop timer before dropping events — the callback holds the
        // same mutex so DeleteTimerQueueTimer(INVALID_HANDLE_VALUE)
        // will safely drain any in-flight callback first.
        StopPolling();
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_events)
        {
            m_events->Release();
            m_events = nullptr;
        }
        return S_OK;
    }

    IFACEMETHODIMP Credential::SetSelected(BOOL* pbAutoLogon)
    {
        LogF(L"Credential::SetSelected");
        if (pbAutoLogon)
        {
            *pbAutoLogon = FALSE;
        }

        // Immediate refresh so the tile doesn't flash the initial
        // "Click to check status" text; then keep it live with a
        // background poll until the tile is deselected again.
        RefreshStatus();
        StartPolling();
        return S_OK;
    }

    IFACEMETHODIMP Credential::SetDeselected()
    {
        LogF(L"Credential::SetDeselected");
        StopPolling();
        return S_OK;
    }

    void Credential::StartPolling()
    {
        if (m_pollTimer) return;
        // Queue-timer fires from a thread-pool thread. 500 ms strikes a
        // balance: fast enough that a counted rep shows up before the
        // child finishes their next one, slow enough not to hammer the
        // pipe or churn LogonUI.
        CreateTimerQueueTimer(&m_pollTimer, nullptr, PollTimerCb, this,
                              500, 500, WT_EXECUTEDEFAULT);
    }

    void Credential::StopPolling()
    {
        if (m_pollTimer)
        {
            // INVALID_HANDLE_VALUE = block until any in-flight callback
            // finishes. Safe against Advise/UnAdvise races.
            DeleteTimerQueueTimer(nullptr, m_pollTimer, INVALID_HANDLE_VALUE);
            m_pollTimer = nullptr;
        }
    }

    VOID CALLBACK Credential::PollTimerCb(PVOID param, BOOLEAN /*timedOut*/)
    {
        static_cast<Credential*>(param)->RefreshStatus();
    }

    void Credential::RefreshStatus()
    {
        std::wstring text;
        Ipc::Response resp{};
        const bool ok = SUCCEEDED(Client::QueryStatus(resp));
        if (ok) text = Client::FormatStatus(resp);
        else    text = Client::FormatUnreachable();

        // Audible feedback — two distinct cues:
        //
        //   1) Posture-ready: SystemAsterisk (Squat) /
        //      SystemExclamation (PushUp). Fires on the
        //      None → non-None transition from the service's
        //      PostureClassifier. The child treats this as
        //      "detector sees you, start the rep now".
        //
        //   2) Rep counted: SystemHand (short "ding") on every
        //      increment of squatCount / pushUpCount so the child
        //      hears each rep land without staring at the tile.
        //
        // Why MessageBeep + system aliases, not Beep(freq, ms):
        // LogonUI runs in session 0 where the waveOut path that
        // Beep() uses in Vista+ is often unavailable (no audio
        // session bound to that desktop). MessageBeep goes through
        // the Windows notification sound subsystem, which is set
        // up for the sign-in desktop and reliably audible there
        // (same path that plays "Windows Default" for a wrong
        // password).
        // Hot-reload sounds.ini so edits appear live (Reload() is
        // a cheap stat-check unless the file's mtime changed).
        m_sounds.Reload();

        if (ok)
        {
            const uint8_t kind  = resp.postureKind;
            const uint8_t kNone = static_cast<uint8_t>(Ipc::PostureKind::None);
            // Treat UINT8_MAX (first-ever poll) as "was None" so
            // the first observation of a real posture still beeps —
            // otherwise the start cue is silently swallowed when
            // the user walked into frame BEFORE clicking the tile.
            const bool wasNone = (m_lastPostureKind == UINT8_MAX ||
                                  m_lastPostureKind == kNone);
            // Five config slots map to only THREE distinct
            // MessageBeep flags, because MB_ICONQUESTION
            // (SystemQuestion) is silent-by-default on Windows
            // Vista+ and MB_OK (SystemDefault) is unreliable
            // across machines; only Asterisk/Exclamation/Hand
            // ring consistently from session 0. So:
            //
            //   SlotSquatReady    -> MB_ICONASTERISK    (SystemAsterisk)
            //   SlotPushupReady   -> MB_ICONEXCLAMATION (SystemExclamation)
            //   SlotSquatCounted  -> MB_ICONHAND        (SystemHand)
            //   SlotPushupCounted -> MB_ICONHAND        (SystemHand) -- same
            //   SlotError         -> MB_ICONHAND        (SystemHand) -- same
            //
            // The two "ready" cues stay distinct (so the child
            // hears which exercise the detector locked onto). The
            // three "action" cues share one alias on the lock
            // screen — install-sounds.ps1 writes a single WAV to
            // SystemHand and that WAV plays for all three. On a
            // signed-in user desktop PlaySound runs first with
            // the per-slot file (random-pick preserved), so the
            // consolidation only matters for the lock screen.
            if (wasNone && kind != kNone)
            {
                const bool isPushup =
                    (kind == static_cast<uint8_t>(Ipc::PostureKind::PushUp));
                const SoundConfig::Slot slot = isPushup
                    ? SoundConfig::SlotPushupReady
                    : SoundConfig::SlotSquatReady;
                PlayWavOrFallback(
                    m_sounds.PickOne(slot),
                    isPushup ? MB_ICONEXCLAMATION : MB_ICONASTERISK);
                LogF(L"posture beep: kind=%u", static_cast<unsigned>(kind));
            }
            m_lastPostureKind = kind;

            // Rep-counted ding. First sample primes the counters
            // without beeping so persisted counters from a prior
            // service run don't dump a burst of beeps on tile
            // select.
            const uint32_t sq = resp.squatCount;
            const uint32_t pu = resp.pushUpCount;
            if (m_lastSquat != UINT32_MAX && sq > m_lastSquat)
            {
                PlayWavOrFallback(
                    m_sounds.PickOne(SoundConfig::SlotSquatCounted),
                    MB_ICONHAND);
                LogF(L"rep beep: squat %u -> %u", m_lastSquat, sq);
            }
            if (m_lastPushup != UINT32_MAX && pu > m_lastPushup)
            {
                PlayWavOrFallback(
                    m_sounds.PickOne(SoundConfig::SlotPushupCounted),
                    MB_ICONHAND);
                LogF(L"rep beep: pushup %u -> %u", m_lastPushup, pu);
            }
            m_lastSquat  = sq;
            m_lastPushup = pu;
        }

        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_values[2] = text; // FIELD_STATUS
            if (m_events)
            {
                m_events->SetFieldString(this, 2, text.c_str());
            }
        }

        // LogonUI collapses our tile after ~30 s of no user activity.
        // Focus tricks don't stop it — the timeout is on real input
        // events, not on field focus. Two mitigations:
        //   1. Ask the OS to keep the display / session awake so its
        //      own idle counter never fires.
        //   2. Post a zero-delta mouse move once per poll so LogonUI's
        //      GetLastInputInfo() clock resets. Runs inside LogonUI's
        //      own process so the input lands in the sign-in session.
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
                                ES_SYSTEM_REQUIRED);
        INPUT nudge{};
        nudge.type = INPUT_MOUSE;
        nudge.mi.dwFlags = MOUSEEVENTF_MOVE;   // dx=0, dy=0 -> no visible motion
        SendInput(1, &nudge, sizeof(nudge));
    }

    IFACEMETHODIMP Credential::GetFieldState(DWORD dwFieldID,
                                             CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
                                             CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis)
    {
        if (!pcpfs || !pcpfis)
        {
            return E_POINTER;
        }
        if (dwFieldID >= m_fields.size())
        {
            return E_INVALIDARG;
        }

        // Layout:
        //   image / label / status  — always visible
        //   password field           — hidden but focused (tile keep-alive)
        //   submit button            — visible only when tile selected
        switch (m_fields[dwFieldID].cpft)
        {
        case CPFT_TILE_IMAGE:
        case CPFT_LARGE_TEXT:
            *pcpfs = CPFS_DISPLAY_IN_BOTH;
            *pcpfis = CPFIS_NONE;
            return S_OK;

        case CPFT_SMALL_TEXT:
            *pcpfs = CPFS_DISPLAY_IN_BOTH;
            *pcpfis = CPFIS_NONE;
            return S_OK;

        case CPFT_SUBMIT_BUTTON:
            *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
            *pcpfis = CPFIS_NONE;
            return S_OK;

        default:
            *pcpfs = CPFS_HIDDEN;
            *pcpfis = CPFIS_NONE;
            return S_OK;
        }
    }

    IFACEMETHODIMP Credential::GetStringValue(DWORD dwFieldID, PWSTR* ppsz)
    {
        if (dwFieldID >= m_values.size())
        {
            return E_INVALIDARG;
        }
        return DupCoString(m_values[dwFieldID].c_str(), ppsz);
    }

    // Target size for the tile bitmap. 128 is the size Microsoft's
    // Credential Provider guidance calls out, and it means LogonUI
    // doesn't have to StretchBlt the icon (which loses alpha on the
    // small "Sign-in options" chips).
    static constexpr UINT kTileSize = 128;

    // Decode a PNG that was embedded via RC into a 128x128 32bpp
    // premultiplied BGRA HBITMAP that LogonUI can render with alpha
    // over its own background. Returns nullptr and logs on failure.
    static HBITMAP LoadPngResourceAsHBitmap(HINSTANCE hInst, LPCWSTR resName)
    {
        HRSRC hRes = FindResourceW(hInst, resName, L"PNG");
        if (!hRes)
        {
            LogF(L"FindResource(PNG) failed: %lu", GetLastError());
            return nullptr;
        }
        DWORD size = SizeofResource(hInst, hRes);
        HGLOBAL hMem = LoadResource(hInst, hRes);
        BYTE* data = static_cast<BYTE*>(hMem ? LockResource(hMem) : nullptr);
        if (!data || size == 0)
        {
            LogF(L"LockResource failed");
            return nullptr;
        }

        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // RPC_E_CHANGED_MODE means COM was already inited on this
        // thread with a different apartment — we can still use it, just
        // must NOT call CoUninitialize afterwards.
        bool shouldUninit = SUCCEEDED(hrCo);

        HBITMAP hbmp = nullptr;

        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* converter = nullptr;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(data, size);
        if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);

        // Scale down to kTileSize in the WIC pipeline so LogonUI doesn't
        // have to (and lose alpha along the way).
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(
            frame, kTileSize, kTileSize, WICBitmapInterpolationModeFant);

        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(
            scaler,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);

        if (SUCCEEDED(hr))
        {
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth       = static_cast<LONG>(kTileSize);
            bmi.bmiHeader.biHeight      = -static_cast<LONG>(kTileSize); // top-down
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* pixels = nullptr;
            hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (hbmp && pixels)
            {
                UINT stride = kTileSize * 4;
                UINT total  = stride * kTileSize;
                hr = converter->CopyPixels(nullptr, stride, total,
                                           static_cast<BYTE*>(pixels));
                if (SUCCEEDED(hr))
                {
                    // LogonUI does not honour per-pixel alpha for the
                    // "Sign-in options" tile slot — a transparent PNG
                    // shows up as a pure black square there. So we
                    // pre-composite ourselves: treat the PNG's alpha
                    // as a mask for a white glyph and paint it onto a
                    // dark opaque backdrop matching the sign-in chrome.
                    // Output is fully opaque, so LogonUI has nothing
                    // left to blend or scale wrong.
                    constexpr BYTE kBgR = 0x30, kBgG = 0x30, kBgB = 0x30;
                    BYTE* p = static_cast<BYTE*>(pixels);
                    for (UINT i = 0; i < total; i += 4)
                    {
                        // WIC gave us premultiplied BGRA. For a pure
                        // white glyph, premultiplied byte value == alpha,
                        // so we can drive compositing straight off the
                        // alpha channel: comp = a*255 + (255-a)*bg.
                        BYTE a = p[i + 3];
                        p[i + 0] = static_cast<BYTE>((a * 255 + (255 - a) * kBgB) / 255);
                        p[i + 1] = static_cast<BYTE>((a * 255 + (255 - a) * kBgG) / 255);
                        p[i + 2] = static_cast<BYTE>((a * 255 + (255 - a) * kBgR) / 255);
                        p[i + 3] = 255;
                    }
                }
                else
                {
                    DeleteObject(hbmp);
                    hbmp = nullptr;
                    LogF(L"WIC CopyPixels failed: 0x%08X", hr);
                }
            }
            else
            {
                LogF(L"CreateDIBSection failed: %lu", GetLastError());
            }
        }
        else
        {
            LogF(L"WIC decode failed: 0x%08X", hr);
        }

        if (converter) converter->Release();
        if (scaler)    scaler->Release();
        if (frame)     frame->Release();
        if (decoder)   decoder->Release();
        if (stream)    stream->Release();
        if (factory)   factory->Release();

        if (shouldUninit) CoUninitialize();
        return hbmp;
    }

    IFACEMETHODIMP Credential::GetBitmapValue(DWORD /*dwFieldID*/, HBITMAP* phbmp)
    {
        if (!phbmp)
        {
            return E_POINTER;
        }
        *phbmp = nullptr;

        HBITMAP hbmp = LoadPngResourceAsHBitmap(g_hInstance, MAKEINTRESOURCEW(IDB_TILE_IMAGE));
        if (!hbmp)
        {
            return E_FAIL;
        }
        *phbmp = hbmp;
        return S_OK;
    }

    IFACEMETHODIMP Credential::GetCheckboxValue(DWORD, BOOL*, PWSTR*)
    {
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Credential::GetSubmitButtonValue(DWORD /*dwFieldID*/, DWORD* pdwAdjacentTo)
    {
        // Anchor the (disabled) submit button next to the status text so
        // it lives inside the tile layout.
        if (!pdwAdjacentTo)
        {
            return E_POINTER;
        }
        *pdwAdjacentTo = 2; // FIELD_STATUS
        return S_OK;
    }

    IFACEMETHODIMP Credential::GetComboBoxValueCount(DWORD, DWORD*, DWORD*)
    {
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Credential::GetComboBoxValueAt(DWORD, DWORD, PWSTR*)
    {
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Credential::SetStringValue(DWORD /*dwFieldID*/, PCWSTR /*psz*/)
    {
        // Accept silently — the hidden keep-alive password field may
        // receive writes from LogonUI even though the user can't see
        // it. We never read what's typed anyway.
        return S_OK;
    }

    IFACEMETHODIMP Credential::SetCheckboxValue(DWORD, BOOL)
    {
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Credential::SetComboBoxSelectedValue(DWORD, DWORD)
    {
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Credential::CommandLinkClicked(DWORD)
    {
        return E_NOTIMPL;
    }

    // Standard credential-provider recipe: turn a filled-in
    // KERB_INTERACTIVE_UNLOCK_LOGON into a flat blob whose internal
    // Buffer pointers are byte-offsets from the blob's own base.
    // Windows sample code does exactly this shape.
    static HRESULT PackUnlockLogon(const KERB_INTERACTIVE_UNLOCK_LOGON& in,
                                   BYTE** outBlob, DWORD* outSize)
    {
        const KERB_INTERACTIVE_LOGON& il = in.Logon;
        const DWORD cb = sizeof(in) +
                         il.LogonDomainName.Length +
                         il.UserName.Length +
                         il.Password.Length;

        auto* dst = static_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(CoTaskMemAlloc(cb));
        if (!dst) return E_OUTOFMEMORY;
        ZeroMemory(dst, cb);
        dst->Logon.MessageType = il.MessageType;

        BYTE* pb = reinterpret_cast<BYTE*>(dst) + sizeof(*dst);

        auto copyStr = [&](const UNICODE_STRING& src, UNICODE_STRING& dstStr)
        {
            dstStr.Length        = src.Length;
            dstStr.MaximumLength = src.Length;
            if (src.Length)
            {
                CopyMemory(pb, src.Buffer, src.Length);
                // Buffer becomes offset from base of blob, LogonUI
                // fixes it up before handing off to LSA.
                dstStr.Buffer = reinterpret_cast<PWSTR>(pb - reinterpret_cast<BYTE*>(dst));
                pb += src.Length;
            }
            else
            {
                dstStr.Buffer = nullptr;
            }
        };
        copyStr(il.LogonDomainName, dst->Logon.LogonDomainName);
        copyStr(il.UserName,        dst->Logon.UserName);
        copyStr(il.Password,        dst->Logon.Password);

        *outBlob = reinterpret_cast<BYTE*>(dst);
        *outSize = cb;
        return S_OK;
    }

    static HRESULT LookupAuthPackageMSV1_0(ULONG* outId)
    {
        HANDLE hLsa = nullptr;
        NTSTATUS status = LsaConnectUntrusted(&hLsa);
        if (status != 0) return HRESULT_FROM_NT(status);

        LSA_STRING name;
        name.Buffer        = const_cast<char*>(MSV1_0_PACKAGE_NAME);
        name.Length        = static_cast<USHORT>(strlen(MSV1_0_PACKAGE_NAME));
        name.MaximumLength = name.Length + 1;

        ULONG authPkg = 0;
        status = LsaLookupAuthenticationPackage(hLsa, &name, &authPkg);
        LsaClose(hLsa);
        if (status != 0) return HRESULT_FROM_NT(status);
        *outId = authPkg;
        return S_OK;
    }

    IFACEMETHODIMP Credential::GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
                                                CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
                                                PWSTR* ppszOptionalStatusText,
                                                CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
    {
        if (pcpgsr)  *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
        if (pcpcs)   ZeroMemory(pcpcs, sizeof(*pcpcs));
        if (ppszOptionalStatusText) *ppszOptionalStatusText = nullptr;
        if (pcpsiOptionalStatusIcon) *pcpsiOptionalStatusIcon = CPSI_NONE;

        // Ask the service how we're doing right now. If we can't reach
        // it or the child hasn't earned enough, tell LogonUI to hold
        // the tile and don't build a credential.
        Ipc::Response resp{};
        m_sounds.Reload();
        if (FAILED(Client::QueryStatus(resp)))
        {
            LogF(L"GetSerialization: pipe query failed");
            PlayWavOrFallback(m_sounds.PickOne(SoundConfig::SlotError),
                              MB_ICONERROR);
            DupCoString(L"Exercise Unlock service is not running.",
                        ppszOptionalStatusText);
            if (pcpsiOptionalStatusIcon) *pcpsiOptionalStatusIcon = CPSI_ERROR;
            return S_OK;
        }
        if (!resp.rewardEnough)
        {
            LogF(L"GetSerialization: reward not enough (earned=%u/%u)",
                 resp.rewardEarnedSeconds, resp.rewardMinUnlockSeconds);
            PlayWavOrFallback(m_sounds.PickOne(SoundConfig::SlotError),
                              MB_ICONERROR);
            DupCoString(L"Not enough exercises yet — keep going.",
                        ppszOptionalStatusText);
            return S_OK;
        }

        // Enough. Pull the parent's cached credentials from the vault.
        Vault::Credentials creds;
        HRESULT hr = Vault::Load(creds);
        if (FAILED(hr))
        {
            LogF(L"GetSerialization: Vault::Load failed 0x%08X",
                 static_cast<unsigned>(hr));
            PlayWavOrFallback(m_sounds.PickOne(SoundConfig::SlotError),
                              MB_ICONERROR);
            DupCoString(L"Vault missing. Run ExerciseUnlockAdmin (elevated) to set it up.",
                        ppszOptionalStatusText);
            if (pcpsiOptionalStatusIcon) *pcpsiOptionalStatusIcon = CPSI_ERROR;
            return S_OK;
        }

        // Build a KERB_INTERACTIVE_UNLOCK_LOGON. Empty domain -> ".".
        std::wstring dom  = creds.domain.empty() ? std::wstring(L".") : creds.domain;
        std::wstring user = creds.username;
        std::wstring pass = creds.password;

        KERB_INTERACTIVE_UNLOCK_LOGON kiul{};
        kiul.Logon.MessageType = KerbInteractiveLogon;

        auto setStr = [](UNICODE_STRING& u, std::wstring& s)
        {
            u.Length        = static_cast<USHORT>(s.size() * sizeof(wchar_t));
            u.MaximumLength = u.Length;
            u.Buffer        = s.empty() ? nullptr : s.data();
        };
        setStr(kiul.Logon.LogonDomainName, dom);
        setStr(kiul.Logon.UserName,        user);
        setStr(kiul.Logon.Password,        pass);

        BYTE* blob = nullptr;
        DWORD blobSize = 0;
        hr = PackUnlockLogon(kiul, &blob, &blobSize);
        // Plaintext password lived in `pass` AND in creds.password —
        // wipe both now that it's inside the packed blob (which
        // LogonUI hands off to LSA immediately and disposes of).
        SecureZeroMemory(pass.data(),           pass.size()           * sizeof(wchar_t));
        SecureZeroMemory(creds.password.data(), creds.password.size() * sizeof(wchar_t));
        if (FAILED(hr))
        {
            LogF(L"GetSerialization: PackUnlockLogon failed 0x%08X",
                 static_cast<unsigned>(hr));
            return hr;
        }

        ULONG authPkg = 0;
        hr = LookupAuthPackageMSV1_0(&authPkg);
        if (FAILED(hr))
        {
            LogF(L"GetSerialization: LookupAuthPackage failed 0x%08X",
                 static_cast<unsigned>(hr));
            CoTaskMemFree(blob);
            return hr;
        }

        pcpcs->clsidCredentialProvider = CLSID_ExerciseUnlockProvider;
        pcpcs->cbSerialization         = blobSize;
        pcpcs->rgbSerialization        = blob;
        pcpcs->ulAuthenticationPackage = authPkg;

        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
        LogF(L"GetSerialization: RETURN_CREDENTIAL_FINISHED user=%ls domain=%ls (%u bytes)",
             user.c_str(), dom.c_str(), blobSize);
        return S_OK;
    }

    IFACEMETHODIMP Credential::ReportResult(NTSTATUS ntsStatus,
                                            NTSTATUS /*ntsSubstatus*/,
                                            PWSTR* ppszOptionalStatusText,
                                            CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon)
    {
        if (ppszOptionalStatusText)  *ppszOptionalStatusText = nullptr;
        if (pcpsiOptionalStatusIcon) *pcpsiOptionalStatusIcon = CPSI_NONE;

        LogF(L"ReportResult: 0x%08X", static_cast<unsigned>(ntsStatus));

        // Success -> the child logged in. Arm the post-unlock
        // countdown for the current earned seconds AND zero the rep
        // counters (server-side atomic sequence in one op).
        // Failure -> leave state alone; child hasn't gone anywhere.
        if (ntsStatus == 0)
        {
            HRESULT hr = Client::StartSession();
            if (FAILED(hr))
            {
                LogF(L"ReportResult: StartSession failed 0x%08X",
                     static_cast<unsigned>(hr));
            }
        }
        return S_OK;
    }

    IFACEMETHODIMP Credential::GetUserSid(PWSTR* ppsz)
    {
        // Bind this tile to a specific local user. Modern LogonUI drops
        // credential providers that don't associate with a real user
        // account, so a valid SID is what makes the tile appear on the
        // sign-in screen next to Password / PIN for that user.
        if (!ppsz)
        {
            return E_POINTER;
        }
        if (m_userSid.empty())
        {
            *ppsz = nullptr;
            return S_FALSE;
        }
        return DupCoString(m_userSid.c_str(), ppsz);
    }
}
