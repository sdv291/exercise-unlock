#pragma once

// The calibration check's verdict, kept apart from the UI so it can be
// tested on its own. After reps that did not count, the reps themselves
// say what to do: loosen the thresholds to what they really reach, blame
// the pace or the movement, or - when no thresholds would fit - ask for
// new photos.

#include "RepResult.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ExerciseUnlock::Calib
{
    using Svc::RepResult;

    // Turning points of the angle the detector sees (its smoothed value),
    // whatever the thresholds: how far the child's reps really go. A turn
    // counts once the angle has come back `hyst` degrees from it.
    struct Swing
    {
        float hyst = 15.0f;
        bool  init = false, falling = false;
        float lo = 0.0f, hi = 0.0f;
        std::vector<float> peaks, valleys;

        void Reset()
        {
            init = falling = false;
            peaks.clear();
            valleys.clear();
        }
        void Push(float a)
        {
            if (!init) { init = true; lo = hi = a; return; }
            if (!falling)
            {
                hi = (std::max)(hi, a);
                if (hi - a >= hyst) { peaks.push_back(hi); falling = true; lo = a; }
            }
            else
            {
                lo = (std::min)(lo, a);
                if (a - lo >= hyst) { valleys.push_back(lo); falling = false; hi = a; }
            }
        }
    };

    // Nearest-rank quantile (q = 0.8: four of five values are at most this).
    inline float Quantile(std::vector<float> v, double q)
    {
        std::sort(v.begin(), v.end());
        const int n = static_cast<int>(v.size());
        return v[static_cast<size_t>(std::clamp(static_cast<int>(std::ceil(q * n)) - 1, 0, n - 1))];
    }

    // Keeps hi at least `gap` above lo, moving both around their middle.
    inline void KeepOrder(int& hi, int& lo, int gap)
    {
        if (hi - lo >= gap) return;
        const int mid = (hi + lo) / 2;
        hi = mid + (gap + 1) / 2;
        lo = hi - gap;
    }

    // One exercise's thresholds: top = standing (squat) / arms straight
    // (push-up) / lying (sit-up). hipDrop 0 = that gate is off.
    struct Thresholds
    {
        int   top = 0, goingDown = 0, goingUp = 0, down = 0;
        float hipDrop = 0.0f;
    };

    enum class Verdict
    {
        Adjust,              // new thresholds in Judgement::t, check again
        Pace,                // reps too fast / too slow: check again
        Form,                // moved / went down again on the way up: check again
        Retry,               // no clear reason: check again (counts as a round)
        NewPhotosRange,      // too little movement / no room for thresholds (Judgement::range)
        NewPhotosHips,       // the hips hardly move in the picture
        NewPhotosRepeated,   // too many rounds already
    };

    struct Judgement
    {
        Verdict    verdict = Verdict::Retry;
        Thresholds t{};
        int        range = 0;             // NewPhotosRange: the degrees there were
        int        bottom = 0, top = 0;   // what the reps reach (for the log)
    };

    constexpr int kMaxRounds = 2;          // adjustments / unexplained rounds before new photos

    // What differs between exercises: the smallest movement that is still
    // a rep, the room kept between neighbouring thresholds, and whether
    // the detector has a hip-drop gate (squats).
    struct Rules
    {
        int  minRange;
        int  gap;
        bool hipGate;
    };
    constexpr Rules kSquatRules  { 20, 4, true };
    constexpr Rules kPushupRules { 10, 2, false };   // MoveNet reads elbows with a small span
    constexpr Rules kSitupRules  { 20, 4, false };

    // attempts: the detector's finished attempts; peaks / valleys: the
    // Swing of the same reps; rounds: Adjust / Retry verdicts so far.
    inline Judgement Judge(const std::vector<RepResult>& attempts, const std::vector<float>& peaks,
                           const std::vector<float>& valleys, const Thresholds& cur, const Rules& rules, int rounds)
    {
        const int minRange = rules.minRange, gap = rules.gap;
        Judgement j;
        j.t = cur;

        int timing = 0, fast = 0, form = 0, hips = 0;
        std::vector<float> drops;
        for (const RepResult& r : attempts)
        {
            if (r.hipDrop > 0.0f) drops.push_back(r.hipDrop);
            if (r.counted || !r.reachedDown) continue;
            if (r.tooFast || r.tooSlow) { ++timing; if (r.tooFast) ++fast; }
            else if (r.hipsStayed) ++hips;
            else if (r.moved || r.wobbled) ++form;
        }

        // The band four of five reps cover - one odd rep may still miss.
        j.bottom = valleys.empty() ? cur.down : static_cast<int>(std::lround(Quantile(valleys, 0.8)));
        j.top = peaks.empty() ? cur.top : static_cast<int>(std::lround(Quantile(peaks, 0.2)));
        const int band = j.top - j.bottom;

        // Thresholds laid out inside that band. It is the detector's own
        // smoothed angle, so small margins do (the photos are held poses
        // and needed wide ones). "Down" never asks for deeper reps than
        // the photos did. "Going down" sits near the top: the rep timer
        // starts there, and a late start makes normal reps "too fast".
        const int margin = (std::max)(3, static_cast<int>(std::lround(0.10 * band)));
        const int st = j.top - margin;
        const int dn = (std::max)(cur.down, j.bottom + margin);
        int gd = st - (std::max)(gap, static_cast<int>(std::lround(0.15 * (st - dn))));
        int gu = dn + (std::max)(gap, static_cast<int>(std::lround(0.30 * (st - dn))));
        KeepOrder(gd, gu, gap);

        const bool notDeep = j.bottom + margin > cur.down;   // some reps stop above "down"
        const bool notUp = st < cur.top;                     // some reps stop below "standing"
        const bool lateTimer = fast > 0 && gd > cur.goingDown + 2;
        float hip = cur.hipDrop;
        if (rules.hipGate && hips >= 2 && cur.hipDrop > 0.0f)
        {
            if (drops.empty() || *(std::max_element)(drops.begin(), drops.end()) < 0.02f)
            {
                j.verdict = Verdict::NewPhotosHips;
                return j;
            }
            hip = std::clamp(std::round(0.7f * Quantile(drops, 0.2f) * 100.0f) / 100.0f, 0.02f, cur.hipDrop);
        }

        if (!notDeep && !notUp && !lateTimer && hip >= cur.hipDrop)
        {
            // The angles are reached: it is the pace or the movement.
            if (timing > 0 && timing >= form) j.verdict = Verdict::Pace;
            else if (form > 0) j.verdict = Verdict::Form;
            else j.verdict = rounds >= kMaxRounds ? Verdict::NewPhotosRepeated : Verdict::Retry;
            return j;
        }
        if (rounds >= kMaxRounds)
        {
            j.verdict = Verdict::NewPhotosRepeated;
            return j;
        }
        // Too little movement to tell a rep from standing still, or
        // thresholds with no room left between them.
        j.range = band < minRange ? band : st - dn;
        if (band < minRange || st - dn < 3 * gap)
        {
            j.verdict = Verdict::NewPhotosRange;
            return j;
        }

        j.t.top = st;
        j.t.goingDown = gd;
        j.t.goingUp = gu;
        j.t.down = dn;
        j.t.hipDrop = hip;
        j.verdict = Verdict::Adjust;
        return j;
    }
}
