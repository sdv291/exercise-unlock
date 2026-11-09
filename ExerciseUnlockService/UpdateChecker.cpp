#include "UpdateChecker.h"
#include "Config.h"
#include "State.h"
#include "Log.h"
#include "Version.h"

#include <windows.h>
#include <winhttp.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <chrono>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace ExerciseUnlock::Svc
{
    // ---- helpers ----
    static int64_t NowUnix()
    {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    // Parse "1.2.3" into packed uint32 (M<<16|m<<8|p). Returns 0 on
    // garbage so an unparseable server version never looks "newer".
    static uint32_t ParseVersion(const std::wstring& s)
    {
        int M = 0, m = 0, p = 0;
        if (swscanf_s(s.c_str(), L"%d.%d.%d", &M, &m, &p) < 1) return 0;
        if (M < 0 || m < 0 || p < 0) return 0;
        if (M > 0xFFFF || m > 0xFF || p > 0xFF) return 0;
        return PackVersion(M, m, p);
    }

    // Split a wide URL into scheme / host / path for WinHttpOpenRequest.
    // Returns true on success. Only https:// is supported.
    static bool CrackHttpsUrl(const std::wstring& url,
                              std::wstring& host, INTERNET_PORT& port, std::wstring& path)
    {
        URL_COMPONENTSW uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t hostBuf[256]{}, pathBuf[1024]{};
        uc.lpszHostName = hostBuf;  uc.dwHostNameLength    = (DWORD)std::size(hostBuf);
        uc.lpszUrlPath  = pathBuf;  uc.dwUrlPathLength     = (DWORD)std::size(pathBuf);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
        if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
        host = hostBuf;
        path = pathBuf;
        port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
        return true;
    }

    // One-shot HTTPS GET. Returns body as UTF-8 bytes, or empty on
    // failure. Short timeouts so a dead server never stalls the
    // service thread.
    static std::string HttpsGet(const std::wstring& url)
    {
        std::wstring host, path;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        if (!CrackHttpsUrl(url, host, port, path))
        {
            LogF(L"update: bad URL '%s'", url.c_str());
            return {};
        }

        const std::wstring ua = std::wstring(L"ExerciseUnlock/") + kVersionString;
        HINTERNET hSession = WinHttpOpen(ua.c_str(),
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { LogF(L"update: WinHttpOpen failed %lu", GetLastError()); return {}; }
        WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 15000);

        HINTERNET hConn = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConn) { LogF(L"update: WinHttpConnect failed %lu", GetLastError());
                      WinHttpCloseHandle(hSession); return {}; }

        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hReq) { LogF(L"update: WinHttpOpenRequest failed %lu", GetLastError());
                     WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); return {}; }

        std::string body;
        BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(hReq, nullptr);
        if (ok)
        {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status / 100 != 2)
            {
                LogF(L"update: HTTP %lu for %s", status, url.c_str());
                ok = FALSE;
            }
        }
        if (ok)
        {
            char buf[4096];
            DWORD got = 0;
            while (WinHttpReadData(hReq, buf, sizeof(buf), &got) && got > 0)
            {
                body.append(buf, buf + got);
                if (body.size() > 8192) break;   // manifest is tiny; defensive cap
            }
        }
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return body;
    }

    // Trim whitespace off both ends.
    static std::wstring Trim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return {};
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    struct Manifest
    {
        std::wstring version, download, notes, sha256;
        std::vector<std::wstring> changesEn, changesUk;   // "What's new", a point each
    };

    // Parse a tiny line-based manifest:
    //   version: 1.2.0
    //   download: https://.../ExerciseUnlock-1.2.0-Setup.exe
    //   notes: https://.../releases/tag/v1.2.0
    //   sha256: <hex of the installer>
    //   change-en: <a point of "What's new">   (any number of these, from
    //   change-uk: <the same in Ukrainian>      installer\changes.txt)
    // Blank lines and `#` comments are ignored.
    static Manifest ParseManifest(const std::string& utf8)
    {
        Manifest m;
        const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                                (int)utf8.size(), nullptr, 0);
        std::wstring wide(wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                            wide.data(), wideLen);

        size_t p = 0;
        while (p < wide.size())
        {
            size_t eol = wide.find(L'\n', p);
            std::wstring line = wide.substr(p, eol == std::wstring::npos ? std::wstring::npos : (eol - p));
            p = (eol == std::wstring::npos) ? wide.size() : eol + 1;

            line = Trim(line);
            if (line.empty() || line[0] == L'#') continue;
            size_t colon = line.find(L':');
            if (colon == std::wstring::npos) continue;
            std::wstring key = Trim(line.substr(0, colon));
            std::wstring val = Trim(line.substr(colon + 1));
            for (auto& c : key) c = (wchar_t)towlower(c);

            if      (key == L"version")   m.version  = val;
            else if (key == L"download")  m.download = val;
            else if (key == L"notes")     m.notes    = val;
            else if (key == L"sha256")    m.sha256   = val;
            else if (val.empty())         continue;
            else if (key == L"change-en") m.changesEn.push_back(val);
            else if (key == L"change-uk") m.changesUk.push_back(val);
        }
        return m;
    }

    // A manifest value that goes on a command line in quotes.
    static bool Plain(const std::wstring& s)
    {
        return s.find_first_of(L"\" \t\r\n") == std::wstring::npos;
    }

    // A "What's new" point as one quoted argument: no quote inside, no
    // backslash before the closing one, not too long.
    static std::wstring QuotedArg(std::wstring s)
    {
        for (auto& c : s)
            if (c == L'"') c = L'\x201D';
        if (s.size() > 200) s.resize(200);
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return L"\"" + s + L"\"";
    }

    // An administrator's session or not. UAC gives administrators a split
    // token, and its elevation type says so; without one (UAC off, or a
    // standard account) the groups decide.
    static bool IsAdminUser(HANDLE token)
    {
        TOKEN_ELEVATION_TYPE type{};
        DWORD cb = 0;
        if (GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &cb) &&
            type != TokenElevationTypeDefault)
            return true;
        BYTE admins[SECURITY_MAX_SID_SIZE];
        DWORD size = sizeof(admins);
        BOOL member = FALSE;
        HANDLE check = nullptr;   // CheckTokenMembership takes an impersonation token
        if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size) &&
            DuplicateToken(token, SecurityIdentification, &check))
        {
            CheckTokenMembership(check, admins, &member);
            CloseHandle(check);
        }
        return member != FALSE;
    }

    // Starts ExerciseUnlockUpdate.exe (next to the service) as the user
    // the token belongs to, on their desktop.
    static bool StartWindow(HANDLE token, const std::wstring& args)
    {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir = self;
        dir.resize(dir.find_last_of(L'\\'));
        const std::wstring exe = dir + L"\\ExerciseUnlockUpdate.exe";
        std::wstring cmd = L"\"" + exe + L"\"" + args;

        void* env = nullptr;
        CreateEnvironmentBlock(&env, token, FALSE);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                             CREATE_UNICODE_ENVIRONMENT, env, dir.c_str(), &si, &pi);
        const DWORD err = GetLastError();
        if (env) DestroyEnvironmentBlock(env);
        if (!ok)
        {
            LogF(L"update: can't start %s (error %lu)", exe.c_str(), err);
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    // What has been shown, per kind of account. In the registry rather
    // than in State: the service restarts with every boot and every
    // calibrator save, and that shouldn't bring the window back each time.
    //   UpdateToasted, UpdateToastedAt   an administrator's "Download and
    //                                    install": version and when. "Later"
    //                                    means about a day later (kRemindSec).
    //   UpdateToldChild                  "ask a parent": once per version.
    static constexpr wchar_t kToastedKey[]   = L"SOFTWARE\\ExerciseUnlock";
    static constexpr wchar_t kToastedValue[] = L"UpdateToasted";
    static constexpr wchar_t kToastedAt[]    = L"UpdateToastedAt";
    static constexpr wchar_t kToldChild[]    = L"UpdateToldChild";
    // Under 24 h, so the next daily check is never just too early.
    static constexpr int64_t kRemindSec      = 20 * 3600;

    static std::wstring ReadValue(const wchar_t* name)
    {
        wchar_t buf[64]{};
        DWORD cb = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kToastedKey, name, RRF_RT_REG_SZ,
                         nullptr, buf, &cb) != ERROR_SUCCESS)
            return {};
        return buf;
    }

    static void WriteValue(const wchar_t* name, const std::wstring& value)
    {
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kToastedKey, name, REG_SZ, value.c_str(),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    }

    static bool WindowDue(const std::wstring& version)
    {
        if (ReadValue(kToastedValue) != version) return true;
        int64_t at = 0;
        DWORD cb = sizeof(at);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kToastedKey, kToastedAt, RRF_RT_REG_QWORD,
                         nullptr, &at, &cb) != ERROR_SUCCESS)
            return true;
        return NowUnix() - at >= kRemindSec;
    }

    static void SaveToasted(const std::wstring& version)
    {
        const int64_t now = NowUnix();
        WriteValue(kToastedValue, version);
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kToastedKey, kToastedAt, REG_QWORD, &now, sizeof(now));
    }

    // The newer version on offer and the window's arguments for it; empty
    // when there's none, or nothing in the manifest the window could
    // install. Only the update thread touches them.
    static std::wstring g_offerVersion, g_offerArgs;

    static void ApplyManifest(const std::string& body)
    {
        auto& st = GetState();
        const Manifest m = ParseManifest(body);
        g_offerVersion.clear();
        g_offerArgs.clear();
        if (m.version.empty())
        {
            LogF(L"update: manifest has no 'version:' line — ignoring");
            return;
        }

        const uint32_t serverVer = ParseVersion(m.version);
        if (serverVer == 0)
        {
            LogF(L"update: can't parse server version '%s'", m.version.c_str());
            return;
        }
        if (serverVer <= kVersionPacked)
        {
            LogF(L"update: up to date (installed %s, server %s)",
                 kVersionString, m.version.c_str());
            std::lock_guard<std::mutex> lk(st.updateMx);
            st.updateAvailable = false;
            st.updateLatestVersion.clear();
            st.updateDownloadUrl.clear();
            st.updateNotesUrl.clear();
            return;
        }

        {
            std::lock_guard<std::mutex> lk(st.updateMx);
            st.updateAvailable      = true;
            st.updateLatestVersion  = m.version;
            st.updateDownloadUrl    = m.download;
            st.updateNotesUrl       = m.notes;
        }
        LogF(L"update: NEW version %s available (installed %s) %s",
             m.version.c_str(), kVersionString, m.download.c_str());
        if (m.download.empty() || !Plain(m.version) || !Plain(m.download) ||
            !Plain(m.notes) || !Plain(m.sha256))
        {
            LogF(L"update: no usable 'download:' line, no window");
            return;
        }
        g_offerVersion = m.version;
        g_offerArgs = L" /version " + m.version + L" /download \"" + m.download + L"\"";
        if (!m.sha256.empty()) g_offerArgs += L" /sha256 " + m.sha256;
        if (!m.notes.empty())  g_offerArgs += L" /notes \"" + m.notes + L"\"";
        for (size_t i = 0; i < m.changesEn.size() && i < 8; ++i)
            g_offerArgs += L" /change-en " + QuotedArg(m.changesEn[i]);
        for (size_t i = 0; i < m.changesUk.size() && i < 8; ++i)
            g_offerArgs += L" /change-uk " + QuotedArg(m.changesUk[i]);
    }

    // The update window for whoever is at the console, when it's due for
    // them: an administrator gets "Download and install" (again about a
    // day later), anyone else - the child - "ask a parent", once per
    // version. Runs after every check and when someone signs in or unlocks.
    static void OfferUpdate()
    {
        if (g_offerVersion.empty()) return;
        const DWORD session = WTSGetActiveConsoleSessionId();
        HANDLE token = nullptr;
        if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token))
            return;   // nobody signed in: the next sign-in brings it
        const bool admin = IsAdminUser(token);
        const bool due = admin ? WindowDue(g_offerVersion) : ReadValue(kToldChild) != g_offerVersion;
        if (due && StartWindow(token, g_offerArgs + (admin ? L"" : L" /ask")))
        {
            LogF(L"update: %s window opened in session %lu",
                 admin ? L"install" : L"ask-a-parent", session);
            if (admin) SaveToasted(g_offerVersion);
            else       WriteValue(kToldChild, g_offerVersion);
        }
        CloseHandle(token);
    }

    bool UpdateChecker::CheckOnce()
    {
        if (!Config::UpdatesEnabled()) return true;
        const std::wstring url = Config::UpdateManifestUrl();
        if (url.empty()) return true;

        LogF(L"update: GET %s", url.c_str());
        const std::string body = HttpsGet(url);
        GetState().updateLastCheckUnix.store(NowUnix());
        if (body.empty()) return false;
        ApplyManifest(body);
        OfferUpdate();
        return true;
    }

    void UpdateChecker::Run()
    {
        LogF(L"update thread starting (installed %s)", kVersionString);

        constexpr ULONGLONG kNever = ~0ull;
        HANDLE waits[2] = { m_stopEvent, m_wake };
        // One check on startup (small jitter so service isn't blocked
        // on the pipe/webcam init). Then every N hours.
        ULONGLONG nextCheck = GetTickCount64() + 15 * 1000;
        for (;;)
        {
            const ULONGLONG now = GetTickCount64();
            const DWORD left = nextCheck == kNever ? INFINITE
                             : nextCheck > now     ? (DWORD)(nextCheck - now)
                             : 0;
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, left);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1)
            {
                // Someone signed in or unlocked: offer it once their
                // desktop has come up.
                if (WaitForSingleObject(m_stopEvent, 10 * 1000) == WAIT_OBJECT_0) break;
                OfferUpdate();
                continue;
            }

            const bool answered = CheckOnce();
            // 15 s after boot the network is often not up yet, and a PC
            // that's switched off at night never reaches the next 24 h
            // check: no answer means try again soon.
            constexpr ULONGLONG kRetryMs = 15 * 60 * 1000;
            const int hours = Config::UpdateIntervalHours();
            // 0 h = startup-only: no more checks (sign-ins still get the window).
            nextCheck = !answered                     ? GetTickCount64() + kRetryMs
                      : (hours > 0 && hours < 24 * 7) ? GetTickCount64() + (ULONGLONG)hours * 3600 * 1000
                      : kNever;
        }
        LogF(L"update thread stopping");
    }

    DWORD WINAPI UpdateChecker::Thunk(LPVOID param)
    {
        static_cast<UpdateChecker*>(param)->Run();
        return 0;
    }

    void UpdateChecker::Start(HANDLE stopEvent)
    {
        m_stopEvent = stopEvent;
        // Never closed: the control handler may still call Wake() while
        // the service stops.
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_thread = CreateThread(nullptr, 0, Thunk, this, 0, nullptr);
        if (!m_thread) LogF(L"update: CreateThread failed %lu", GetLastError());
    }

    void UpdateChecker::Wake()
    {
        if (m_wake) SetEvent(m_wake);
    }

    void UpdateChecker::Join()
    {
        if (m_thread)
        {
            WaitForSingleObject(m_thread, 5000);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
    }
}
