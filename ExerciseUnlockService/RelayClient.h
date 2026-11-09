#pragma once

// A PC's WebSocket to the relay server (relay/server.js, /ws/pc): the
// service's PhoneLink and the calibrator's phone source both talk to the
// phone through it.

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

namespace ExerciseUnlock::Svc::Relay
{
    constexpr size_t kMaxMessage = 64 * 1024;

    std::string  ToUtf8(const std::wstring& w);
    std::wstring ToWide(const std::string& s);

    // Base32 (RFC 4648 alphabet, no padding) of `bytes` random bytes.
    // 10 bytes -> 16 characters, all valid in a QR alphanumeric segment.
    std::string RandomBase32(size_t bytes);

    struct RelayUrl
    {
        bool          secure = true;
        std::wstring  host;
        INTERNET_PORT port = 0;
        std::string   qrBase;   // "HTTPS://HOST/S/"
    };

    bool ParseRelay(const std::wstring& url, RelayUrl& out);

    // One WebSocket to /ws/pc. Send is thread-safe; Receive runs on
    // its own thread; Shutdown / Abort make a blocked Receive return.
    class Socket
    {
    public:
        ~Socket() { Close(); }

        bool Open(const RelayUrl& u, DWORD& err);
        bool Send(const std::string& text);
        // Whole text message, false once the socket is closed / broken.
        bool Receive(std::string& out);
        // Polite close: the relay answers and Receive returns.
        void Shutdown();
        // Hard close for a dead connection.
        void Abort();
        void Close();

    private:
        HINTERNET              m_session = nullptr;
        HINTERNET              m_connect = nullptr;
        std::atomic<HINTERNET> m_ws{ nullptr };
        std::mutex             m_sendMx;
    };
}
