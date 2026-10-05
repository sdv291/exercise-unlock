#pragma once

#include <cstdint>
#include <string>

namespace ExerciseUnlock::Svc
{
    // Plain-text log of user-visible events for parents to review:
    // when unlocks happened, how many reps went into them, when
    // sessions ended. Appends to
    //   %ProgramData%\ExerciseUnlock\activity.log
    // Idempotent: file is created on first Append with a restrictive
    // ACL inherited from the vault directory.
    class ActivityLog
    {
    public:
        static void ServiceStarted();
        static void ServiceStopping();

        // Fires when the child successfully unlocks and the countdown
        // is armed.
        static void UnlockGranted(uint32_t pushups,
                                  uint32_t squats,
                                  uint32_t earnedSeconds);

        // Fires when the countdown expires and the workstation is
        // auto-locked.
        static void SessionExpired(uint32_t plannedSeconds);

        // Optional freeform event.
        static void Note(const std::wstring& text);

    private:
        static void Append(const std::wstring& line);
        static std::wstring TimestampNow();
    };
}
