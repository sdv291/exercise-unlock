#pragma once

#include <cstdint>

namespace ExerciseUnlock::Svc
{
    // How a detector's last finished attempt ended: counted, or why not.
    // The service only logs these reasons; ExerciseUnlockCalibrate's
    // test screen shows them to the parent.
    struct RepResult
    {
        uint32_t seq         = 0;       // +1 per finished attempt
        bool     counted     = false;
        bool     reachedDown = true;    // false: went back up before the "down" threshold
        float    deepest     = 0.0f;    // smallest smoothed angle of the attempt
        uint32_t durMs       = 0;
        bool     tooFast     = false;
        bool     tooSlow     = false;
        bool     shallow     = false;   // quality below the minimum
        bool     moved       = false;   // squat: hips drifted sideways (walking)
        bool     wobbled     = false;   // squat: went back down on the way up
        bool     hipsStayed  = false;   // squat: hips did not drop (knee lift)
        float    hipDrop     = 0.0f;    // squat: how far the hips went down (frame heights), 0 = not measured
    };
}
