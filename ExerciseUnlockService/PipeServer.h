#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace ExerciseUnlock::Svc
{
    // Serves \\.\pipe\ExerciseUnlock. Each ConnectNamedPipe accept
    // creates a fresh pipe instance and spawns a worker thread that
    // handles exactly one Request/Response round trip before closing.
    // On stop, Run waits for outstanding client threads to finish so
    // the service process actually exits promptly — otherwise a client
    // stuck in ReadFile can keep the exe locked past `sc stop`.
    class PipeServer
    {
    public:
        // Runs the accept loop on the caller's thread until Stop() is
        // called or the passed stop event is signaled.
        void Run(HANDLE stopEvent);

        void Stop();

    private:
        struct ClientCtx
        {
            PipeServer* self;
            HANDLE      pipe;
        };

        static DWORD WINAPI ClientThreadThunk(LPVOID param);
        void HandleClient(HANDLE pipe);

        void TrackClientThread(HANDLE thread);
        void JoinAllClientThreads(DWORD perThreadTimeoutMs);

        std::atomic<bool>     m_stop{ false };
        std::mutex            m_clientsMx;
        std::vector<HANDLE>   m_clientThreads;
    };
}
