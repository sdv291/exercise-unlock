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

    // Parse a tiny line-based manifest:
    //   version: 1.2.0
    //   download: https://.../ExerciseUnlock-1.2.0-Setup.exe
    //   notes: https://.../releases/tag/v1.2.0
    // Blank lines and `#` comments are ignored.
    static void ParseManifest(const std::string& utf8,
                              std::wstring& version,
                              std::wstring& downloadUrl,
                              std::wstring& notesUrl)
    {
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

            if      (key == L"version")  version     = val;
            else if (key == L"download") downloadUrl = val;
            else if (key == L"notes")    notesUrl    = val;
        }
    }

    // Reach into the active user session to show a WTSSendMessage
    // info box. Non-modal from the service's perspective — WTS does
    // the UI blocking in the user's session, not ours.
    static void ShowUpdateToast(const std::wstring& version, const std::wstring& url)
    {
        DWORD sessionId = WTSGetActiveConsoleSessionId();
        if (sessionId == 0xFFFFFFFF) return;

        std::wstring title = L"Exercise Unlock — update available";
        std::wstring body  = L"Version " + version + L" is available.\n\n";
        if (!url.empty()) body += url;
        else              body += L"See the project page for the download.";

        DWORD resp = 0;
        WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                        title.data(), (DWORD)(title.size() * sizeof(wchar_t)),
                        body.data(),  (DWORD)(body.size()  * sizeof(wchar_t)),
                        MB_OK | MB_ICONINFORMATION, 0, &resp, FALSE);
    }

    void UpdateChecker::CheckOnce()
    {
        if (!Config::UpdatesEnabled()) return;
        const std::wstring url = Config::UpdateManifestUrl();
        if (url.empty()) return;

        LogF(L"update: GET %s", url.c_str());
        const std::string body = HttpsGet(url);
        auto& st = GetState();
        st.updateLastCheckUnix.store(NowUnix());
        if (body.empty()) return;

        std::wstring ver, dl, notes;
        ParseManifest(body, ver, dl, notes);
        if (ver.empty())
        {
            LogF(L"update: manifest has no 'version:' line — ignoring");
            return;
        }

        const uint32_t serverVer = ParseVersion(ver);
        if (serverVer == 0)
        {
            LogF(L"update: can't parse server version '%s'", ver.c_str());
            return;
        }
        if (serverVer <= kVersionPacked)
        {
            LogF(L"update: up to date (installed %s, server %s)",
                 kVersionString, ver.c_str());
            std::lock_guard<std::mutex> lk(st.updateMx);
            st.updateAvailable = false;
            st.updateLatestVersion.clear();
            st.updateDownloadUrl.clear();
            st.updateNotesUrl.clear();
            return;
        }

        bool toastThis = false;
        {
            std::lock_guard<std::mutex> lk(st.updateMx);
            st.updateAvailable      = true;
            st.updateLatestVersion  = ver;
            st.updateDownloadUrl    = dl;
            st.updateNotesUrl       = notes;
            if (st.updateLastToasted != ver)
            {
                st.updateLastToasted = ver;
                toastThis = true;
            }
        }
        LogF(L"update: NEW version %s available (installed %s) %s",
             ver.c_str(), kVersionString, dl.c_str());
        if (toastThis) ShowUpdateToast(ver, dl.empty() ? notes : dl);
    }

    void UpdateChecker::Run()
    {
        LogF(L"update thread starting (installed %s)", kVersionString);

        // One check on startup (small jitter so service isn't blocked
        // on the pipe/webcam init). Then every N hours.
        if (WaitForSingleObject(m_stopEvent, 15 * 1000) == WAIT_OBJECT_0)
            return;
        CheckOnce();

        for (;;)
        {
            const int hours = Config::UpdateIntervalHours();
            // 0 h = startup-only; sleep forever (or until stop).
            const DWORD ms = (hours > 0 && hours < 24 * 7)
                ? (DWORD)hours * 3600u * 1000u
                : INFINITE;
            if (WaitForSingleObject(m_stopEvent, ms) == WAIT_OBJECT_0) break;
            CheckOnce();
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
        m_thread = CreateThread(nullptr, 0, Thunk, this, 0, nullptr);
        if (!m_thread) LogF(L"update: CreateThread failed %lu", GetLastError());
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
