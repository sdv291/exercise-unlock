#include "PhoneLink.h"
#include "Config.h"
#include "Log.h"
#include "MiniJson.h"
#include "Paths.h"
#include "PersistentState.h"
#include "State.h"
#include "Version.h"
#include "WebcamProbe.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

// Relay protocol (relay/server.js), JSON text messages:
//   PC -> relay   {t:hello, pc, key, v}   {t:offer, token}   {t:to, sid, msg}
//                 {t:kick, sid}           {t:ping}
//   relay -> PC   {t:ready}  {t:join, sid}  {t:leave, sid}  {t:from, sid, msg}
//                 {t:pong}
//   PC -> phone   {t:config, aspect, exercises:[{id, seconds, params}]}
//                 {t:status, counts, earned, min, enough, active, why}
//                 {t:reject, why}  {t:bye, reason}
//   phone -> PC   {t:hello, lang, ua}  {t:rep, ex, n, q, dur}
//                 {t:diag, kind, ...}   (logged only)

namespace ExerciseUnlock::Svc
{
    namespace
    {
        constexpr DWORD    kTickMs        = 1000;
        // A PC that loses the internet must drop its QR code (and give the
        // webcam back) quickly: the relay answers every ping, so ~15 s of
        // silence means the connection is gone.
        constexpr uint64_t kPingEveryMs   = 5 * 1000;
        constexpr uint64_t kDeadAfterMs   = 15 * 1000;        // nothing from the relay -> reconnect
        constexpr uint64_t kRotateTokenMs = 15 * 60 * 1000;   // an unused QR code expires
        constexpr uint64_t kStatusEveryMs = 10 * 1000;
        constexpr size_t   kMaxMessage    = 64 * 1024;

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

        // Base32 (RFC 4648 alphabet, no padding) of `bytes` random bytes.
        // 10 bytes -> 16 characters, all valid in a QR alphanumeric segment.
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

        bool IsBase32(const std::string& s, size_t minLen, size_t maxLen)
        {
            if (s.size() < minLen || s.size() > maxLen) return false;
            return std::all_of(s.begin(), s.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
            });
        }

        // This PC's id + key towards the relay. Kept in the data folder
        // (SYSTEM + Administrators only) so the id survives restarts.
        bool LoadOrCreateIdentity(std::string& id, std::string& key)
        {
            const std::wstring path = DataPath(L"phone.id");
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                char buf[256]{};
                DWORD got = 0;
                ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
                CloseHandle(f);
                std::string text(buf, got);
                const size_t nl = text.find('\n');
                if (nl != std::string::npos)
                {
                    id = text.substr(0, nl);
                    key = text.substr(nl + 1);
                    while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
                    while (!id.empty() && id.back() == '\r') id.pop_back();
                    if (IsBase32(id, 16, 32) && IsBase32(key, 16, 64)) return true;
                }
            }

            id = RandomBase32(16);    // 26 chars
            key = RandomBase32(20);   // 32 chars
            EnsureDataDir();
            f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
            {
                LogF(L"phone: cannot write %s (%lu)", path.c_str(), GetLastError());
                return false;
            }
            const std::string text = id + "\n" + key + "\n";
            DWORD wrote = 0;
            WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
            CloseHandle(f);
            LogF(L"phone: new relay identity %hs...", id.substr(0, 4).c_str());
            return true;
        }

        struct RelayUrl
        {
            bool          secure = true;
            std::wstring  host;
            INTERNET_PORT port = 0;
            std::string   qrBase;   // "HTTPS://HOST/S/"
        };

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

        // One WebSocket to /ws/pc. Send is thread-safe; Receive runs on
        // its own thread; Shutdown / Abort make a blocked Receive return.
        class Socket
        {
        public:
            ~Socket() { Close(); }

            bool Open(const RelayUrl& u, DWORD& err)
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

            bool Send(const std::string& text)
            {
                std::lock_guard<std::mutex> lk(m_sendMx);
                HINTERNET ws = m_ws.load();
                return ws && WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                                  const_cast<char*>(text.data()),
                                                  static_cast<DWORD>(text.size())) == NO_ERROR;
            }

            // Whole text message, false once the socket is closed / broken.
            bool Receive(std::string& out)
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

            // Polite close: the relay answers and Receive returns.
            void Shutdown()
            {
                std::lock_guard<std::mutex> lk(m_sendMx);
                if (HINTERNET ws = m_ws.load())
                    WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            }

            // Hard close for a dead connection.
            void Abort()
            {
                if (HINTERNET ws = m_ws.exchange(nullptr)) WinHttpCloseHandle(ws);
            }

            void Close()
            {
                Abort();
                if (m_connect) { WinHttpCloseHandle(m_connect); m_connect = nullptr; }
                if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
            }

        private:
            HINTERNET              m_session = nullptr;
            HINTERNET              m_connect = nullptr;
            std::atomic<HINTERNET> m_ws{ nullptr };
            std::mutex             m_sendMx;
        };

        // One relay connection. Shared with its receive thread.
        struct Session
        {
            RelayUrl              url;
            Socket                sock;
            std::mutex            mx;          // everything below
            bool                  ready = false;
            std::set<std::string> phones;      // relay sids
            std::string           token;
            uint64_t              tokenAtMs = 0;
            uint64_t              lastSquatMs = 0, lastPushupMs = 0;
            double                diagBudget = 10.0;
            uint64_t              diagRefillMs = 0;
            std::string           lastStatus;
            uint64_t              lastStatusMs = 0;
            std::atomic<uint64_t> lastRxMs{ 0 };
            std::atomic<bool>     closed{ false };
        };

        void SetPhoneState(Ipc::PhoneState s, const std::wstring& url)
        {
            auto& st = GetState();
            {
                std::lock_guard<std::mutex> lk(st.phoneMx);
                st.phoneUrl = url;
            }
            const uint8_t prev = st.phoneState.exchange(static_cast<uint8_t>(s));
            if (prev != static_cast<uint8_t>(s))
            {
                static const wchar_t* kNames[] = { L"off", L"connecting", L"waiting for a phone", L"phone connected", L"relay unreachable" };
                LogF(L"phone: %s", kNames[static_cast<int>(s)]);
            }
        }

        void PublishState(Session& s)
        {
            if (!s.ready) { SetPhoneState(Ipc::PhoneState::Connecting, L""); return; }
            SetPhoneState(s.phones.empty() ? Ipc::PhoneState::Waiting : Ipc::PhoneState::Connected,
                          ToWide(s.url.qrBase + s.token));
        }

        std::string ToPhone(const std::string& sid, const std::string& msg)
        {
            Json::Writer w;
            w.BeginObject().Key("t").String("to").Key("sid").String(sid).Key("msg").Raw(msg).EndObject();
            return w.Str();
        }

        void Broadcast(Session& s, const std::string& msg)
        {
            for (const auto& sid : s.phones) s.sock.Send(ToPhone(sid, msg));
        }

        // Aspect ratio (w/h) of the camera the angles were calibrated
        // with; the phone maps its poses into the same convention.
        double CalibrationAspect()
        {
            const float cfg = Config::CalibrationAspect();
            if (cfg > 0.5f && cfg < 4.0f) return cfg;
            auto& st = GetState();
            const uint32_t w = st.webcamWidth.load(), h = st.webcamHeight.load();
            if (w > 0 && h > 0) return static_cast<double>(w) / h;
            return 16.0 / 9.0;
        }

        // Exercises + the thresholds the webcam detectors use (config.ini
        // [squat] / [pushup], tuned by the calibration).
        std::string ConfigJson()
        {
            Json::Writer w;
            w.BeginObject().Key("t").String("config").Key("v").Int(1)
             .Key("aspect").Number(CalibrationAspect())
             .Key("exercises").BeginArray();

            w.BeginObject().Key("id").String("squat").Key("seconds").Int(Config::SquatSeconds())
             .Key("params").BeginObject()
                .Key("standing").Number(Config::SquatStandingAngle())
                .Key("down").Number(Config::SquatDownAngle())
                .Key("goingDown").Number(Config::SquatGoingDownAngle())
                .Key("goingUp").Number(Config::SquatGoingUpAngle())
                .Key("minRepMs").Int(Config::SquatMinRepMs())
                .Key("maxRepMs").Int(Config::SquatMaxRepMs())
                .Key("qualityMin").Number(Config::SquatQualityMin())
                .Key("maxHipDrift").Number(Config::SquatMaxHipDrift())
                .Key("minHipDrop").Number(Config::SquatMinHipDrop())
                .Key("mode").Int(Config::SquatDetectorMode())
                .Key("standingHipFrac").Number(Config::SquatStandingHipFrac())
                .Key("downHipFrac").Number(Config::SquatDownHipFrac())
                .Key("goingDownHipFrac").Number(Config::SquatGoingDownHipFrac())
                .Key("goingUpHipFrac").Number(Config::SquatGoingUpHipFrac())
             .EndObject().EndObject();

            w.BeginObject().Key("id").String("pushup").Key("seconds").Int(Config::PushupSeconds())
             .Key("params").BeginObject()
                .Key("up").Number(Config::PushupUpAngle())
                .Key("down").Number(Config::PushupDownAngle())
                .Key("goingDown").Number(Config::PushupGoingDownAngle())
                .Key("goingUp").Number(Config::PushupGoingUpAngle())
                .Key("plankMin").Number(Config::PushupPlankMinAngle())
                .Key("minRepMs").Int(Config::PushupMinRepMs())
                .Key("maxRepMs").Int(Config::PushupMaxRepMs())
                .Key("qualityMin").Number(Config::PushupQualityMin())
             .EndObject().EndObject();

            w.EndArray().EndObject();
            return w.Str();
        }

        std::string StatusJson()
        {
            Ipc::Response r{};
            SnapshotInto(r);
            const bool session = IsSessionActive();
            const bool hours   = Config::IsWithinActiveWindowNow();
            Json::Writer w;
            w.BeginObject().Key("t").String("status")
             .Key("counts").BeginObject()
                .Key("squat").Int(r.squatCount)
                .Key("pushup").Int(r.pushUpCount)
             .EndObject()
             .Key("earned").Int(r.rewardEarnedSeconds)
             .Key("min").Int(r.rewardMinUnlockSeconds)
             .Key("enough").Bool(r.rewardEnough != 0)
             .Key("active").Bool(!session && hours)
             .Key("why").String(session ? "session" : !hours ? "hours" : "")
             .EndObject();
            return w.Str();
        }

        std::string Simple(const char* type, const char* key, const std::string& value)
        {
            Json::Writer w;
            w.BeginObject().Key("t").String(type).Key(key).String(value).EndObject();
            return w.Str();
        }

        void OfferToken(Session& s)
        {
            s.token = RandomBase32(10);
            s.tokenAtMs = GetTickCount64();
            s.sock.Send(Simple("offer", "token", s.token));
            PublishState(s);
        }

        // Credit one phone rep under the webcam's rules. Caller holds s.mx.
        bool CreditRep(Session& s, const std::string& ex, std::string& why)
        {
            if (IsSessionActive())                 { why = "session"; return false; }
            if (!Config::IsWithinActiveWindowNow()) { why = "hours";   return false; }

            // Not faster than a real rep allows (70 % of the configured
            // minimum: network jitter can bunch two reports together).
            const uint64_t now = GetTickCount64();
            auto paced = [now](uint64_t& last, int minRepMs) {
                const uint64_t gap = static_cast<uint64_t>((std::max)(300, minRepMs * 7 / 10));
                if (last != 0 && now - last < gap) return false;
                last = now;
                return true;
            };
            auto& st = GetState();
            uint32_t total = 0;
            if (ex == "squat")
            {
                if (!paced(s.lastSquatMs, Config::SquatMinRepMs())) { why = "too_fast"; return false; }
                total = st.squatCount.fetch_add(1) + 1;
            }
            else if (ex == "pushup")
            {
                if (!paced(s.lastPushupMs, Config::PushupMinRepMs())) { why = "too_fast"; return false; }
                total = st.pushUpCount.fetch_add(1) + 1;
            }
            else
            {
                why = "unknown";
                return false;
            }
            PersistentState::Save();
            LogF(L"phone: %hs +1  total=%u", ex.c_str(), total);
            return true;
        }

        // "key=value ..." of a flat phone message (one nested level), for
        // the log. Phone text is untrusted: values are cut short.
        std::string Flatten(const Json::Value& v, const std::string& prefix = "")
        {
            std::string out;
            for (const auto& [key, val] : v.obj)
            {
                if (prefix.empty() && key == "t") continue;
                const std::string name = prefix + key.substr(0, 16);
                std::string text;
                switch (val.type)
                {
                case Json::Value::Type::Number:
                {
                    char buf[32]{};
                    std::snprintf(buf, sizeof(buf), "%g", val.n);
                    text = buf;
                    break;
                }
                case Json::Value::Type::String: text = val.s.substr(0, 24); break;
                case Json::Value::Type::Bool:   text = val.b ? "yes" : "no"; break;
                case Json::Value::Type::Object:
                    if (prefix.empty()) { out += Flatten(val, name + "."); }
                    continue;
                default:
                    continue;
                }
                out += name + "=" + text + " ";
                if (out.size() > 400) break;
            }
            return out;
        }

        void OnPhoneMessage(Session& s, const std::string& sid, const Json::Value& msg)
        {
            const std::string t = msg.Str("t");
            if (t == "diag")
            {
                // What the phone's detectors saw (posture changes, dropped
                // reps, a summary every 10 s) - the phone has no log of its own.
                // Bursts of up to 10 are fine (start + first frame + posture
                // arrive together), ~3 per second on average.
                const uint64_t now = GetTickCount64();
                s.diagBudget = (std::min)(10.0, s.diagBudget + static_cast<double>(now - s.diagRefillMs) / 300.0);
                s.diagRefillMs = now;
                if (s.diagBudget < 1.0) return;
                s.diagBudget -= 1.0;
                LogF(L"phone %hs diag: %hs", sid.c_str(), Flatten(msg).c_str());
                return;
            }
            if (t == "rep")
            {
                std::string why;
                if (CreditRep(s, msg.Str("ex"), why))
                {
                    s.lastStatus = StatusJson();
                    s.lastStatusMs = GetTickCount64();
                    Broadcast(s, s.lastStatus);
                }
                else
                {
                    LogF(L"phone: %hs refused (%hs)", msg.Str("ex").substr(0, 16).c_str(), why.c_str());
                    s.sock.Send(ToPhone(sid, Simple("reject", "why", why)));
                }
            }
            else if (t == "hello")
            {
                LogF(L"phone %hs: %hs", sid.c_str(), msg.Str("ua").substr(0, 100).c_str());
            }
        }

        void OnRelayMessage(Session& s, const std::string& text)
        {
            Json::Value m;
            if (!Json::Parser::Parse(text, m) || !m.IsObject()) return;
            s.lastRxMs = GetTickCount64();
            const std::string t = m.Str("t");

            std::lock_guard<std::mutex> lk(s.mx);
            if (t == "ready")
            {
                s.ready = true;
                OfferToken(s);
            }
            else if (t == "join")
            {
                const std::string sid = m.Str("sid");
                s.phones.insert(sid);
                PublishState(s);
                s.sock.Send(ToPhone(sid, ConfigJson()));
                s.sock.Send(ToPhone(sid, StatusJson()));
            }
            else if (t == "leave")
            {
                s.phones.erase(m.Str("sid"));
                PublishState(s);
            }
            else if (t == "from")
            {
                const Json::Value* msg = m.Get("msg");
                const std::string sid = m.Str("sid");
                if (msg && msg->IsObject() && s.phones.count(sid)) OnPhoneMessage(s, sid, *msg);
            }
            // "pong": lastRxMs above is all it is for.
        }
    }

    // ---------------------------------------------------------------- thread

    void PhoneLink::Start(HANDLE stopEvent)
    {
        m_stopEvent = stopEvent;
        if (!m_wakeEvent.load()) m_wakeEvent.store(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        m_thread = CreateThread(nullptr, 0, &PhoneLink::Thunk, this, 0, nullptr);
        if (!m_thread) LogF(L"phone: CreateThread failed %lu", GetLastError());
    }

    void PhoneLink::Join()
    {
        Wake();
        if (!m_thread) return;
        if (WaitForSingleObject(m_thread, 10000) == WAIT_TIMEOUT)
        {
            LogF(L"phone: thread did not stop within 10 s - leaving it behind");
            return;
        }
        CloseHandle(m_thread);
        m_thread = nullptr;
    }

    void PhoneLink::Wake()
    {
        if (HANDLE e = m_wakeEvent.load()) SetEvent(e);
    }

    DWORD WINAPI PhoneLink::Thunk(LPVOID param)
    {
        static_cast<PhoneLink*>(param)->Run();
        return 0;
    }

    void PhoneLink::Run()
    {
        std::string id, key;
        bool haveIdentity = false;
        int failures = 0;
        HANDLE waits[2] = { m_stopEvent, m_wakeEvent.load() };
        const DWORD nWaits = waits[1] ? 2 : 1;
        // Sleeps up to `ms`; a Wake() ends it early. True = service stopping.
        auto stopRequested = [&](DWORD ms) {
            return WaitForMultipleObjects(nWaits, waits, FALSE, ms) == WAIT_OBJECT_0;
        };

        while (WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT)
        {
            RelayUrl url;
            const std::wstring relay = Config::PhoneRelayUrl();
            const bool configured = Config::PhoneEnabled() && !relay.empty() && ParseRelay(relay, url);
            if (!configured || !CameraWantedNow())
            {
                SetPhoneState(Ipc::PhoneState::Off, L"");
                failures = 0;
                if (stopRequested(2000)) break;
                continue;
            }
            if (!haveIdentity) haveIdentity = LoadOrCreateIdentity(id, key);
            if (!haveIdentity)
            {
                if (stopRequested(30000)) break;
                continue;
            }

            // ---- connect
            auto s = std::make_shared<Session>();
            s->url = url;
            SetPhoneState(Ipc::PhoneState::Connecting, L"");
            DWORD err = 0;
            if (!s->sock.Open(url, err))
            {
                ++failures;
                if (failures == 1 || failures % 10 == 0)
                    LogF(L"phone: cannot reach %s (error %lu, attempt %d)", relay.c_str(), err, failures);
                SetPhoneState(Ipc::PhoneState::Error, L"");
                const DWORD backoff = static_cast<DWORD>((std::min)(60000, 2000 << (std::min)(failures, 5)));
                if (stopRequested(backoff)) break;
                continue;
            }
            failures = 0;
            LogF(L"phone: connected to %s", relay.c_str());
            s->lastRxMs = GetTickCount64();
            {
                Json::Writer w;
                w.BeginObject().Key("t").String("hello").Key("pc").String(id).Key("key").String(key)
                 .Key("v").Int(1).Key("app").String(ToUtf8(kVersionString)).EndObject();
                s->sock.Send(w.Str());
            }
            HANDLE rxDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::thread rx([s, rxDone] {
                std::string msg;
                while (s->sock.Receive(msg)) OnRelayMessage(*s, msg);
                s->closed = true;
                SetEvent(rxDone);
            });

            // ---- run until unlocked / disconnected / stopping
            std::string bye;
            uint64_t lastPing = GetTickCount64();
            for (;;)
            {
                if (stopRequested(kTickMs)) { bye = "ended"; break; }
                if (s->closed) { LogF(L"phone: relay closed the connection"); break; }
                if (!Config::PhoneEnabled()) { bye = "ended"; break; }
                if (!CameraWantedNow()) { bye = IsSessionActive() ? "unlocked" : "ended"; break; }
                const uint64_t now = GetTickCount64();
                if (now - s->lastRxMs.load() > kDeadAfterMs) { LogF(L"phone: relay silent - reconnecting"); break; }
                if (now - lastPing >= kPingEveryMs)
                {
                    lastPing = now;
                    s->sock.Send("{\"t\":\"ping\"}");
                }
                std::lock_guard<std::mutex> lk(s->mx);
                if (s->ready && s->phones.empty() && now - s->tokenAtMs >= kRotateTokenMs) OfferToken(*s);
                if (!s->phones.empty())
                {
                    // Webcam reps, the schedule window and the earned time
                    // change without a phone message: keep the phone current.
                    const std::string st = StatusJson();
                    if (st != s->lastStatus || now - s->lastStatusMs >= kStatusEveryMs)
                    {
                        s->lastStatus = st;
                        s->lastStatusMs = now;
                        Broadcast(*s, st);
                    }
                }
            }

            // ---- tell the phones why, then hang up
            {
                std::lock_guard<std::mutex> lk(s->mx);
                if (!bye.empty() && !s->closed)
                    Broadcast(*s, Simple("bye", "reason", bye));
            }
            SetPhoneState(Ipc::PhoneState::Off, L"");
            s->sock.Shutdown();
            if (WaitForSingleObject(rxDone, 3000) == WAIT_TIMEOUT) s->sock.Abort();
            rx.join();
            CloseHandle(rxDone);
            LogF(L"phone: disconnected (%hs)", bye.empty() ? "connection lost" : bye.c_str());
            if (bye.empty() && stopRequested(2000)) break;   // brief pause before reconnecting
        }
        SetPhoneState(Ipc::PhoneState::Off, L"");
    }
}
