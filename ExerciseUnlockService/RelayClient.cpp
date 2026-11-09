#include "RelayClient.h"
#include "Version.h"

#include <bcrypt.h>

#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace ExerciseUnlock::Svc::Relay
{
    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string RandomBase32(size_t bytes)
    {
        std::vector<uint8_t> buf(bytes);
        BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        static constexpr char kAbc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        std::string out;
        uint32_t acc = 0;
        int bits = 0;
        for (const uint8_t b : buf)
        {
            acc = (acc << 8) | b;
            bits += 8;
            while (bits >= 5)
            {
                out += kAbc[(acc >> (bits - 5)) & 31];
                bits -= 5;
            }
        }
        if (bits > 0) out += kAbc[(acc << (5 - bits)) & 31];
        return out;
    }

    bool ParseRelay(const std::wstring& url, RelayUrl& out)
    {
        wchar_t host[256]{};
        URL_COMPONENTS uc{};
        uc.dwStructSize     = sizeof(uc);
        uc.lpszHostName     = host;
        uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
        if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return false;
        out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
        out.host.assign(host, uc.dwHostNameLength);
        out.port = uc.nPort;
        if (out.host.empty()) return false;

        // Upper case: the whole URL then fits a QR alphanumeric
        // segment, which makes the code smaller and easier to scan.
        std::string base = out.secure ? "HTTPS://" : "HTTP://";
        for (const char c : ToUtf8(out.host))
            base += static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
        const bool defaultPort = (out.secure && out.port == INTERNET_DEFAULT_HTTPS_PORT) ||
                                 (!out.secure && out.port == INTERNET_DEFAULT_HTTP_PORT);
        if (!defaultPort) base += ":" + std::to_string(out.port);
        base += "/S/";
        out.qrBase = base;
        return true;
    }

    bool Socket::Open(const RelayUrl& u, DWORD& err)
    {
        const std::wstring agent = std::wstring(L"ExerciseUnlock/") + kVersionString;
        m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session) { err = GetLastError(); return false; }
        // Receive timeout > ping interval: a silent connection is dead.
        WinHttpSetTimeouts(m_session, 10000, 10000, 10000, 60000);
        m_connect = WinHttpConnect(m_session, u.host.c_str(), u.port, 0);
        if (!m_connect) { err = GetLastError(); return false; }

        HINTERNET req = WinHttpOpenRequest(m_connect, L"GET", L"/ws/pc", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { err = GetLastError(); return false; }
        bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                  WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(req, nullptr);
        if (!ok)
        {
            err = GetLastError();
            WinHttpCloseHandle(req);
            return false;
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 101)
        {
            err = status;   // e.g. 404 / 502: wrong URL or relay down behind the proxy
            WinHttpCloseHandle(req);
            return false;
        }
        HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
        err = ws ? NO_ERROR : GetLastError();
        WinHttpCloseHandle(req);
        m_ws.store(ws);
        return ws != nullptr;
    }

    bool Socket::Send(const std::string& text)
    {
        std::lock_guard<std::mutex> lk(m_sendMx);
        HINTERNET ws = m_ws.load();
        return ws && WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                          const_cast<char*>(text.data()),
                                          static_cast<DWORD>(text.size())) == NO_ERROR;
    }

    bool Socket::Receive(std::string& out)
    {
        out.clear();
        char buf[4096];
        for (;;)
        {
            HINTERNET ws = m_ws.load();
            if (!ws) return false;
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            if (WinHttpWebSocketReceive(ws, buf, sizeof(buf), &got, &type) != NO_ERROR) return false;
            if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                return false;   // close frame, or binary the relay never sends
            out.append(buf, got);
            if (out.size() > kMaxMessage) return false;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return true;
        }
    }

    void Socket::Shutdown()
    {
        std::lock_guard<std::mutex> lk(m_sendMx);
        if (HINTERNET ws = m_ws.load())
            WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }

    void Socket::Abort()
    {
        if (HINTERNET ws = m_ws.exchange(nullptr)) WinHttpCloseHandle(ws);
    }

    void Socket::Close()
    {
        Abort();
        if (m_connect) { WinHttpCloseHandle(m_connect); m_connect = nullptr; }
        if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
    }
}
