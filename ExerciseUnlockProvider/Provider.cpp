#include "Provider.h"
#include "Credential.h"
#include "Log.h"

#include <new>
#include <cwchar>
#include <shlwapi.h>
#include <shlobj.h>
#include <lm.h>
#include <sddl.h>
#include <string>
#include <vector>

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "shell32.lib")

extern LONG g_dllRefCount;

namespace ExerciseUnlock
{
    // Field indices on our tile. Even a minimal V2 provider has to expose
    // at least a label and a submit button so the tile can be "selected".
    // We keep them here as a stable enum so field-driven callbacks can
    // switch on them once we grow.
    enum FieldId : DWORD
    {
        FIELD_TILE_IMAGE = 0,
        FIELD_LABEL      = 1,
        FIELD_STATUS     = 2,
        FIELD_SUBMIT     = 3,
        FIELD_COUNT
    };

    static const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR s_fieldDescriptors[FIELD_COUNT] =
    {
        { FIELD_TILE_IMAGE, CPFT_TILE_IMAGE,    const_cast<PWSTR>(L"Image"),   GUID_NULL },
        { FIELD_LABEL,      CPFT_LARGE_TEXT,    const_cast<PWSTR>(L"Exercise Unlock"), GUID_NULL },
        { FIELD_STATUS,     CPFT_SMALL_TEXT,    const_cast<PWSTR>(L"Click to check status"), GUID_NULL },
        { FIELD_SUBMIT,     CPFT_SUBMIT_BUTTON, const_cast<PWSTR>(L"Submit"),  GUID_NULL },
    };

    // Read a newline-separated list of usernames from
    // %ProgramData%\ExerciseUnlock\users.allow. When the file
    // exists and is non-empty, EnumerateLocalUserSids restricts
    // tile creation to those names (case-insensitive). Empty /
    // missing file -> every local user gets a tile (old
    // behavior; useful for single-child homes). Lines starting
    // with ';' or '#' are treated as comments, as is whitespace.
    static std::vector<std::wstring> ReadAllowedUserNames()
    {
        std::vector<std::wstring> allow;
        wchar_t buf[MAX_PATH]{};
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                        nullptr, SHGFP_TYPE_CURRENT, buf)))
            return allow;
        const std::wstring path =
            std::wstring(buf) + L"\\ExerciseUnlock\\users.allow";

        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            LogF(L"users.allow: not present (err=%lu) — allowing all local users",
                 GetLastError());
            return allow;
        }
        char raw[4096]{};
        DWORD got = 0;
        ReadFile(f, raw, sizeof(raw) - 1, &got, nullptr);
        CloseHandle(f);

        // Skip a UTF-8 BOM if the file was saved by PowerShell's
        // `Set-Content -Encoding UTF8` or Notepad's "UTF-8" option
        // (both prefix the content with EF BB BF). Without this
        // the first username entry parses as "﻿name" and
        // never matches a real local account.
        const char* rawPtr = raw;
        DWORD       rawLen = got;
        if (rawLen >= 3 &&
            (unsigned char)rawPtr[0] == 0xEF &&
            (unsigned char)rawPtr[1] == 0xBB &&
            (unsigned char)rawPtr[2] == 0xBF)
        {
            rawPtr += 3;
            rawLen -= 3;
        }

        int need = MultiByteToWideChar(CP_UTF8, 0, rawPtr, rawLen, nullptr, 0);
        std::wstring text(need, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, rawPtr, rawLen, text.data(), need);

        size_t i = 0;
        while (i < text.size())
        {
            // consume whitespace / newlines (treat a stray UTF-16 BOM
            // code point as whitespace too, in case the file was
            // saved as UTF-16 LE)
            while (i < text.size() &&
                   (text[i] == L' ' || text[i] == L'\t' ||
                    text[i] == L'\r' || text[i] == L'\n' ||
                    text[i] == 0xFEFF))
                ++i;
            // skip comment line
            if (i < text.size() && (text[i] == L';' || text[i] == L'#'))
            {
                while (i < text.size() && text[i] != L'\n') ++i;
                continue;
            }
            size_t s = i;
            while (i < text.size() &&
                   text[i] != L'\r' && text[i] != L'\n' &&
                   text[i] != L';'  && text[i] != L'#')
                ++i;
            size_t e = i;
            while (e > s && (text[e - 1] == L' ' || text[e - 1] == L'\t'))
                --e;
            if (e > s) allow.emplace_back(text.substr(s, e - s));
        }
        LogF(L"users.allow: loaded %zu entries", allow.size());
        return allow;
    }

    static bool AllowListPermits(const std::vector<std::wstring>& allow,
                                 const wchar_t* userName)
    {
        if (allow.empty()) return true;   // no filter -> permit all
        for (const auto& name : allow)
        {
            if (_wcsicmp(name.c_str(), userName) == 0) return true;
        }
        return false;
    }

    // Enumerate local user accounts and convert each to a string SID.
    // We skip disabled accounts and the built-in Guest so the sign-in
    // screen stays clean. LogonUI runs as SYSTEM which is enough to
    // read the local SAM, so no credential handshake is needed.
    // If users.allow is present and non-empty, only accounts whose
    // name appears in it get a tile.
    static HRESULT EnumerateLocalUserSids(std::vector<std::wstring>& out)
    {
        out.clear();
        const auto allow = ReadAllowedUserNames();

        USER_INFO_1* buffer = nullptr;
        DWORD entriesRead = 0;
        DWORD totalEntries = 0;
        DWORD resumeHandle = 0;

        NET_API_STATUS status = NetUserEnum(
            nullptr,                    // local machine
            1,                          // USER_INFO_1: gives us flags + name
            FILTER_NORMAL_ACCOUNT,      // regular user accounts only
            reinterpret_cast<LPBYTE*>(&buffer),
            MAX_PREFERRED_LENGTH,
            &entriesRead,
            &totalEntries,
            &resumeHandle);

        if (status != NERR_Success && status != ERROR_MORE_DATA)
        {
            LogF(L"NetUserEnum failed: %lu", static_cast<unsigned long>(status));
            return HRESULT_FROM_WIN32(status);
        }

        for (DWORD i = 0; i < entriesRead; ++i)
        {
            const USER_INFO_1& u = buffer[i];
            if (u.usri1_flags & UF_ACCOUNTDISABLE)
            {
                continue;
            }
            if (!AllowListPermits(allow, u.usri1_name))
            {
                LogF(L"local user: %s (filtered out by users.allow)", u.usri1_name);
                continue;
            }

            // Resolve name -> SID via LookupAccountName. Sizes are grown
            // on demand — SIDs are variable-length.
            BYTE sidBuf[SECURITY_MAX_SID_SIZE] = {};
            DWORD sidSize = sizeof(sidBuf);
            wchar_t domain[256] = {};
            DWORD domainSize = static_cast<DWORD>(std::size(domain));
            SID_NAME_USE use{};
            if (!LookupAccountNameW(nullptr, u.usri1_name,
                                    sidBuf, &sidSize,
                                    domain, &domainSize, &use))
            {
                LogF(L"LookupAccountName '%s' failed: %lu",
                     u.usri1_name, GetLastError());
                continue;
            }
            if (use != SidTypeUser)
            {
                continue;
            }

            LPWSTR sidStr = nullptr;
            if (!ConvertSidToStringSidW(sidBuf, &sidStr))
            {
                LogF(L"ConvertSidToStringSid failed: %lu", GetLastError());
                continue;
            }
            out.emplace_back(sidStr);
            LogF(L"local user: %s  sid=%s", u.usri1_name, sidStr);
            LocalFree(sidStr);
        }

        if (buffer)
        {
            NetApiBufferFree(buffer);
        }
        return S_OK;
    }

    Provider::Provider()
        : m_refCount(1),
          m_cpus(CPUS_INVALID)
    {
        InterlockedIncrement(&g_dllRefCount);
    }

    Provider::~Provider()
    {
        ReleaseCredentials();
        InterlockedDecrement(&g_dllRefCount);
    }

    void Provider::ReleaseCredentials()
    {
        for (auto* c : m_credentials)
        {
            if (c)
            {
                c->Release();
            }
        }
        m_credentials.clear();
    }

    HRESULT Provider::CreateCredentialsForLocalUsers()
    {
        std::vector<std::wstring> sids;
        HRESULT hr = EnumerateLocalUserSids(sids);
        if (FAILED(hr))
        {
            return hr;
        }
        if (sids.empty())
        {
            LogF(L"no local users found — no tiles will be created");
            return S_OK;
        }

        for (const auto& sid : sids)
        {
            Credential* c = new (std::nothrow) Credential();
            if (!c)
            {
                return E_OUTOFMEMORY;
            }
            HRESULT h = c->Initialize(s_fieldDescriptors, FIELD_COUNT, sid.c_str());
            if (FAILED(h))
            {
                c->Release();
                return h;
            }
            m_credentials.push_back(c);
        }
        LogF(L"created %u credential tile(s)",
             static_cast<unsigned>(m_credentials.size()));
        return S_OK;
    }

    IFACEMETHODIMP Provider::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_ICredentialProvider)
        {
            *ppv = static_cast<ICredentialProvider*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) Provider::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) Provider::Release()
    {
        LONG n = InterlockedDecrement(&m_refCount);
        if (n == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP Provider::SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD /*dwFlags*/)
    {
        LogF(L"SetUsageScenario cpus=%u", static_cast<unsigned>(cpus));

        // In v0.1 we only want the tile to appear on the logon and unlock
        // screens. Credui / change-password scenarios are intentionally
        // rejected so we don't pop up in unexpected places.
        switch (cpus)
        {
        case CPUS_LOGON:
        case CPUS_UNLOCK_WORKSTATION:
        {
            m_cpus = cpus;
            ReleaseCredentials();
            return CreateCredentialsForLocalUsers();
        }
        case CPUS_CREDUI:
        case CPUS_CHANGE_PASSWORD:
        default:
            return E_NOTIMPL;
        }
    }

    IFACEMETHODIMP Provider::SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* /*pcpcs*/)
    {
        // Nothing to pre-load in v0.1.
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Provider::Advise(ICredentialProviderEvents* /*pcpe*/, UINT_PTR /*upAdviseContext*/)
    {
        // We don't yet push events to LogonUI (e.g. "tile state changed"
        // as the child finishes exercises). That comes in v0.2 when the
        // provider talks to the service.
        return S_OK;
    }

    IFACEMETHODIMP Provider::UnAdvise()
    {
        return S_OK;
    }

    IFACEMETHODIMP Provider::GetFieldDescriptorCount(DWORD* pdwCount)
    {
        if (!pdwCount)
        {
            return E_POINTER;
        }
        *pdwCount = FIELD_COUNT;
        return S_OK;
    }

    IFACEMETHODIMP Provider::GetFieldDescriptorAt(DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd)
    {
        if (!ppcpfd)
        {
            return E_POINTER;
        }
        if (dwIndex >= FIELD_COUNT)
        {
            return E_INVALIDARG;
        }

        auto* dst = static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(
            CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR)));
        if (!dst)
        {
            return E_OUTOFMEMORY;
        }

        const auto& src = s_fieldDescriptors[dwIndex];
        dst->dwFieldID  = src.dwFieldID;
        dst->cpft       = src.cpft;
        dst->guidFieldType = src.guidFieldType;

        size_t chars = wcslen(src.pszLabel) + 1;
        auto* label = static_cast<PWSTR>(CoTaskMemAlloc(chars * sizeof(wchar_t)));
        if (!label)
        {
            CoTaskMemFree(dst);
            return E_OUTOFMEMORY;
        }
        wcscpy_s(label, chars, src.pszLabel);
        dst->pszLabel = label;

        *ppcpfd = dst;
        return S_OK;
    }

    IFACEMETHODIMP Provider::GetCredentialCount(DWORD* pdwCount, DWORD* pdwDefault, BOOL* pbAutoLogonWithDefault)
    {
        if (!pdwCount || !pdwDefault || !pbAutoLogonWithDefault)
        {
            return E_POINTER;
        }

        *pdwCount = static_cast<DWORD>(m_credentials.size());
        // Do not preselect our tile — the child should have to click it,
        // and the PIN/password tiles must stay obviously usable as the
        // parent's emergency path.
        *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
        *pbAutoLogonWithDefault = FALSE;
        LogF(L"GetCredentialCount -> %u", static_cast<unsigned>(*pdwCount));
        return S_OK;
    }

    IFACEMETHODIMP Provider::GetCredentialAt(DWORD dwIndex, ICredentialProviderCredential** ppcpc)
    {
        if (!ppcpc)
        {
            return E_POINTER;
        }
        if (dwIndex >= m_credentials.size() || !m_credentials[dwIndex])
        {
            return E_INVALIDARG;
        }
        return m_credentials[dwIndex]->QueryInterface(IID_PPV_ARGS(ppcpc));
    }
}
