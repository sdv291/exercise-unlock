#pragma once

namespace ExerciseUnlock::Svc
{
    // Small binary snapshot at
    //   %ProgramData%\ExerciseUnlock\state.dat
    // that survives service restarts (and reboots) so the child
    // doesn't lose partial-earn progress every time the service
    // stops. Contains just the rep counters — session state is
    // intentionally NOT persisted (see notes in Load()).
    class PersistentState
    {
    public:
        // Read state.dat if present and populate the in-memory State
        // atomics. Called once at service start.
        static void Load();

        // Snapshot current atomics to disk. Cheap (< 30 bytes I/O),
        // called after each rep + on any counter reset.
        static void Save();
    };
}
