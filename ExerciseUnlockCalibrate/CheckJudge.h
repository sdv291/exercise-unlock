#pragma once

// Calibration by reps (1.6), kept apart from the UI so it can be tested on
// its own: the child does a few reps, the turning points of the angle the
// service's own detector sees in them (standing / squatting, arms straight /
// the bottom of the push-up, lying / sitting up) give the thresholds.

#include <algorithm>
#include <cmath>
#include <vector>

namespace ExerciseUnlock::Calib
{
    // Turning points of the angle the detector sees (its smoothed value),
    // whatever the thresholds: how far the child's reps really go. A turn
    // counts once the angle has come back `hyst` degrees from it. Each turn
    // also keeps a second value seen at that moment (the hips' height for
    // squats): peakAux[i] / valleyAux[i] belong to peaks[i] / valleys[i].
    // A rep is a valley: peaks[i] is the top before valleys[i].
    struct Swing
    {
        float hyst = 15.0f;
        bool  init = false, falling = false;
        float lo = 0.0f, hi = 0.0f, loAux = 0.0f, hiAux = 0.0f;
        std::vector<float> peaks, valleys, peakAux, valleyAux;

        void Reset()
        {
            init = falling = false;
            peaks.clear();
            valleys.clear();
            peakAux.clear();
            valleyAux.clear();
        }
        void Push(float a, float aux = 0.0f)
        {
            if (!init) { init = true; lo = hi = a; loAux = hiAux = aux; return; }
            if (!falling)
            {
                if (a >= hi) { hi = a; hiAux = aux; }
                if (hi - a >= hyst) { peaks.push_back(hi); peakAux.push_back(hiAux); falling = true; lo = a; loAux = aux; }
            }
            else
            {
                if (a <= lo) { lo = a; loAux = aux; }
                if (a - lo >= hyst) { valleys.push_back(lo); valleyAux.push_back(loAux); falling = false; hi = a; hiAux = aux; }
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

    // What differs between exercises: the smallest movement that is still
    // a rep, the room kept between neighbouring thresholds, whether the
    // detector has a hip-drop gate (squats), and the bottom the reps must
    // reach at least (0: any) - so shallow calibration reps cannot become
    // the standard.
    struct Rules
    {
        int  minRange;
        int  gap;
        bool hipGate;
        int  deepest;
    };
    constexpr Rules kSquatRules  { 20, 4, true, 130 };
    constexpr Rules kPushupRules { 10, 2, false, 0 };   // MoveNet reads elbows with a small span
    constexpr Rules kSitupRules  { 20, 4, false, 0 };

    enum class RepFault
    {
        None,
        Shallow,   // the reps did not reach Rules::deepest (RepPlan::bottom says how far they went)
        Range,     // too little movement / no room for thresholds (RepPlan::range)
        Hips,      // squats: the hips hardly move in the picture
    };

    struct RepPlan
    {
        RepFault   fault = RepFault::None;
        Thresholds t{};
        int        top = 0, bottom = 0;   // what the reps reach
        int        range = 0;             // Range: the degrees there were
    };

    // A typical rep (the medians of the turns) with a quarter of its range
    // kept free on each side, so a rep a little short of the calibration ones
    // still counts - MoveNet's elbows and hips wander by several degrees from
    // rep to rep, more so on a phone. Four of the five calibration reps clear
    // both thresholds by 3 degrees at least. (1.6.0 put them 10% inside the
    // band of four reps of five: push-ups and sit-ups then went uncounted.)
    // "Going down" sits near the top: the rep timer starts there, and a late
    // start would make normal reps "too fast". The hip-drop gate asks for
    // half of what the smaller reps dropped (4 of 5 drop more), within
    // 0.02..0.08 of the frame height.
    constexpr float kKeepFree = 0.25f;   // of the range, at the top and at the bottom

    inline RepPlan PlanFromReps(const std::vector<float>& peaks, const std::vector<float>& valleys,
                                const std::vector<float>& peakHips, const std::vector<float>& valleyHips,
                                const Rules& rules)
    {
        RepPlan r;
        if (peaks.empty() || valleys.empty())
        {
            r.fault = RepFault::Range;
            return r;
        }
        const float top = Quantile(peaks, 0.5), bottom = Quantile(valleys, 0.5);
        r.top = static_cast<int>(std::lround(top));
        r.bottom = static_cast<int>(std::lround(bottom));
        if (rules.deepest > 0 && r.bottom > rules.deepest)
        {
            r.fault = RepFault::Shallow;
            return r;
        }
        const float band = top - bottom;
        const int st = static_cast<int>(std::lround((std::min)(top - kKeepFree * band, Quantile(peaks, 0.2) - 3.0f)));
        const int dn = static_cast<int>(std::lround((std::max)(bottom + kKeepFree * band, Quantile(valleys, 0.8) + 3.0f)));
        if (band < rules.minRange || st - dn < 3 * rules.gap)
        {
            r.fault = RepFault::Range;
            r.range = (std::max)(0, static_cast<int>(std::lround(band)));
            return r;
        }
        int gd = st - (std::max)(rules.gap, static_cast<int>(std::lround(0.15 * (st - dn))));
        int gu = dn + (std::max)(rules.gap, static_cast<int>(std::lround(0.30 * (st - dn))));
        KeepOrder(gd, gu, rules.gap);
        r.t = { st, gd, gu, dn, 0.0f };

        if (rules.hipGate)
        {
            // Image y grows down: the hips are lower (larger y) at the bottom of a squat.
            std::vector<float> drops;
            const size_t n = (std::min)({ peaks.size(), valleys.size(), peakHips.size(), valleyHips.size() });
            for (size_t i = 0; i < n; ++i)
                if (peakHips[i] > 0.0f && valleyHips[i] > 0.0f) drops.push_back(valleyHips[i] - peakHips[i]);
            if (drops.empty() || Quantile(drops, 0.5) < 0.02f)
            {
                r.fault = RepFault::Hips;
                return r;
            }
            r.t.hipDrop = std::clamp(std::round(0.5f * Quantile(drops, 0.2) * 100.0f) / 100.0f, 0.02f, 0.08f);
        }
        return r;
    }
}
