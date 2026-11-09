#pragma once

namespace ExerciseUnlock::Svc
{
    // One exercise's detector settings, as config.ini names them ([squat],
    // [pushup], [situp]). The webcam's detectors take Config's live values
    // (Config::Squat() ...); a phone's get theirs from PhoneLink, which
    // may use the phone's own calibration ([phone_squat] ...).
    struct SquatParams
    {
        float standing = 165.0f, down = 120.0f, goingDown = 155.0f, goingUp = 135.0f;   // knee angle
        int   minRepMs = 1000, maxRepMs = 2000;
        float qualityMin = 0.0f;
        float maxHipDrift = 0.0f;   // frame widths, 0 = off
        float minHipDrop = 0.0f;    // frame heights, 0 = off
        int   mode = 0;             // Config::SquatMode
        float standingHipFrac = 0.45f, downHipFrac = 0.25f, goingDownHipFrac = 0.40f, goingUpHipFrac = 0.30f;
    };

    struct PushupParams
    {
        float up = 160.0f, down = 110.0f, goingDown = 150.0f, goingUp = 125.0f;   // elbow angle
        float plankMin = 140.0f;    // shoulder-hip-ankle
        int   minRepMs = 800, maxRepMs = 6000;
        float qualityMin = 0.0f;
    };

    struct SitupParams
    {
        float lying = 105.0f, rising = 95.0f, lowering = 75.0f, sitting = 60.0f;   // hip angle
        int   minRepMs = 800, maxRepMs = 6000;
        float qualityMin = 0.0f;
    };
}
