#include "PhoneLink.h"
#include "Config.h"
#include "Log.h"
#include "MiniJson.h"
#include "Paths.h"
#include "PersistentState.h"
#include "PhoneCounter.h"
#include "RelayClient.h"
#include "State.h"
#include "Version.h"
#include "WebcamProbe.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Relay protocol (relay/server.js), JSON text messages:
//   PC -> relay   {t:hello, pc, key, v}   {t:offer, token}   {t:to, sid, msg}
//                 {t:kick, sid}           {t:ping}
//   relay -> PC   {t:ready}  {t:join, sid}  {t:leave, sid}  {t:from, sid, msg}
//                 {t:pong}
//   PC -> phone   {t:config, aspect, exercises:[{id, seconds, params}]}
//                 {t:status, counts, earned, min, enough, active, why}
//                 {t:reject, why}  {t:bye, reason}
//   PC -> phone   {t:result, ex, ok, why}   (a rep counted here, or why not)
//   phone -> PC   {t:hello, lang, ua}
//                 {t:pose, ts, w, h, k:[x, y, c] x 17}   every frame: x, y in
//                     1/10000 of the w x h frame, c in percent, ts its clock (ms)
//                 {t:diag, kind, ...}   (logged only)
//
// The reps are counted here from the poses (PhoneCounter), with the webcam's
// detectors; the phone counts too, but only for its own screen. A page that
// sends {t:rep} instead (before poses) gets {t:reject, why:"update"}.

namespace ExerciseUnlock::Svc
{
    using namespace Relay;

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
            uint64_t              lastSquatMs = 0, lastPushupMs = 0, lastSitupMs = 0;
            double                diagBudget = 10.0;
            uint64_t              diagRefillMs = 0;
            std::string           lastStatus;
            uint64_t              lastStatusMs = 0;
            // Each phone's reps, counted here from its poses (join .. leave).
            std::map<std::string, std::unique_ptr<PhoneCounter>> counters;
            uint64_t              repWarnMs = 0;   // last "rep without poses" log line
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

        // What a phone counts with: the phone's own calibration when there
        // is one, else the webcam's thresholds on the webcam's frame.
        struct PhoneSetup
        {
            SquatParams  squat;
            PushupParams pushup;
            SitupParams  situp;
            bool         phoneSpace = false;
            double       aspect = 16.0 / 9.0;
        };

        PhoneSetup CurrentSetup()
        {
            PhoneSetup s;
            s.phoneSpace = Config::PhoneProfile(s.squat, s.pushup, s.situp);
            s.aspect = CalibrationAspect();
            return s;
        }

        // Exercises + their thresholds (config.ini [squat] / [pushup] /
        // [situp], or [phone_*], tuned by the calibration). poses: stream
        // them, the reps are counted here; space "phone": the thresholds are
        // in the phone's own frame, "pc": map the poses onto the webcam's
        // (aspect).
        std::string ConfigJson(const PhoneSetup& s)
        {
            Json::Writer w;
            w.BeginObject().Key("t").String("config").Key("v").Int(1)
             .Key("aspect").Number(s.aspect)
             .Key("poses").Int(1)
             .Key("space").String(s.phoneSpace ? "phone" : "pc")
             .Key("exercises").BeginArray();

            w.BeginObject().Key("id").String("squat").Key("seconds").Int(Config::SquatSeconds())
             .Key("params").BeginObject()
                .Key("standing").Number(s.squat.standing)
                .Key("down").Number(s.squat.down)
                .Key("goingDown").Number(s.squat.goingDown)
                .Key("goingUp").Number(s.squat.goingUp)
                .Key("minRepMs").Int(s.squat.minRepMs)
                .Key("maxRepMs").Int(s.squat.maxRepMs)
                .Key("qualityMin").Number(s.squat.qualityMin)
                .Key("maxHipDrift").Number(s.squat.maxHipDrift)
                .Key("minHipDrop").Number(s.squat.minHipDrop)
                .Key("mode").Int(s.squat.mode)
                .Key("standingHipFrac").Number(s.squat.standingHipFrac)
                .Key("downHipFrac").Number(s.squat.downHipFrac)
                .Key("goingDownHipFrac").Number(s.squat.goingDownHipFrac)
                .Key("goingUpHipFrac").Number(s.squat.goingUpHipFrac)
             .EndObject().EndObject();

            w.BeginObject().Key("id").String("pushup").Key("seconds").Int(Config::PushupSeconds())
             .Key("params").BeginObject()
                .Key("up").Number(s.pushup.up)
                .Key("down").Number(s.pushup.down)
                .Key("goingDown").Number(s.pushup.goingDown)
                .Key("goingUp").Number(s.pushup.goingUp)
                .Key("plankMin").Number(s.pushup.plankMin)
                .Key("minRepMs").Int(s.pushup.minRepMs)
                .Key("maxRepMs").Int(s.pushup.maxRepMs)
                .Key("qualityMin").Number(s.pushup.qualityMin)
             .EndObject().EndObject();

            w.BeginObject().Key("id").String("situp").Key("seconds").Int(Config::SitupSeconds())
             .Key("params").BeginObject()
                .Key("lying").Number(s.situp.lying)
                .Key("rising").Number(s.situp.rising)
                .Key("lowering").Number(s.situp.lowering)
                .Key("sitting").Number(s.situp.sitting)
                .Key("minRepMs").Int(s.situp.minRepMs)
                .Key("maxRepMs").Int(s.situp.maxRepMs)
                .Key("qualityMin").Number(s.situp.qualityMin)
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
                .Key("situp").Int(r.sitUpCount)
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
            if (IsSessionActive()) { why = "session"; return false; }

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
            std::atomic<uint32_t>* counter = nullptr;
            if (ex == "squat")
            {
                if (!paced(s.lastSquatMs, Config::SquatMinRepMs())) { why = "too_fast"; return false; }
                counter = &st.squatCount;
            }
            else if (ex == "pushup")
            {
                if (!paced(s.lastPushupMs, Config::PushupMinRepMs())) { why = "too_fast"; return false; }
                counter = &st.pushUpCount;
            }
            else if (ex == "situp")
            {
                if (!paced(s.lastSitupMs, Config::SitupMinRepMs())) { why = "too_fast"; return false; }
                counter = &st.sitUpCount;
            }
            else
            {
                why = "unknown";
                return false;
            }
            if (!Config::IsWithinActiveWindowNow())
            {
                // A real rep, just not now: the tile plays the error sound.
                st.offHoursReps.fetch_add(1);
                why = "hours";
                return false;
            }
            const uint32_t total = counter->fetch_add(1) + 1;
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

        // A rep counted here from the phone's poses - or not, and why:
        // credited under the webcam's rules, and the phone told (its sound
        // and counter follow this, not its own detectors).
        void OnAttempt(Session& s, const std::string& sid, const PhoneCounter::Attempt& a)
        {
            const std::string ex = PhoneCounter::Name(a.ex);
            const RepResult& r = a.rep;
            std::string why;
            bool ok = false;
            if (r.counted)
            {
                ok = CreditRep(s, ex, why);
                if (!ok) LogF(L"phone: %hs refused (%hs)", ex.c_str(), why.c_str());
            }
            else
            {
                why = (r.tooFast || r.tooSlow) ? "timing" : r.shallow ? "depth" : r.moved ? "drift"
                    : r.hipsStayed ? "hip" : r.wobbled ? "shaky" : "other";
                LogF(L"phone: %hs not counted (%hs, %u ms)", ex.c_str(), why.c_str(), r.durMs);
            }
            Json::Writer w;
            w.BeginObject().Key("t").String("result").Key("ex").String(ex).Key("ok").Bool(ok)
             .Key("why").String(why).EndObject();
            s.sock.Send(ToPhone(sid, w.Str()));
            if (ok)
            {
                s.lastStatus = StatusJson();
                s.lastStatusMs = GetTickCount64();
                Broadcast(s, s.lastStatus);
            }
        }

        void OnPhoneMessage(Session& s, const std::string& sid, const Json::Value& msg)
        {
            const std::string t = msg.Str("t");
            if (t == "pose")
            {
                const auto it = s.counters.find(sid);
                BodyPose pose;
                uint32_t w = 0, h = 0;
                uint64_t ts = 0;
                if (it == s.counters.end() || !PhoneCounter::ParsePose(msg, pose, w, h, ts)) return;
                std::vector<PhoneCounter::Attempt> attempts;
                it->second->Feed(pose, w, h, ts, GetTickCount64(), attempts);
                for (const auto& a : attempts) OnAttempt(s, sid, a);
                return;
            }
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
                // A page that counts by itself and doesn't stream its poses:
                // an old cached copy - or a hand-made one. Not counted.
                const uint64_t now = GetTickCount64();
                if (now - s.repWarnMs >= 60000)
                {
                    s.repWarnMs = now;
                    LogF(L"phone %hs: a rep without poses - not counted (old page?)", sid.c_str());
                }
                s.sock.Send(ToPhone(sid, Simple("reject", "why", "update")));
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
                const PhoneSetup setup = CurrentSetup();
                s.counters[sid] = std::make_unique<PhoneCounter>(setup.squat, setup.pushup, setup.situp,
                                                                 setup.phoneSpace, setup.aspect);
                s.sock.Send(ToPhone(sid, ConfigJson(setup)));
                s.sock.Send(ToPhone(sid, StatusJson()));
            }
            else if (t == "leave")
            {
                s.phones.erase(m.Str("sid"));
                s.counters.erase(m.Str("sid"));
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
