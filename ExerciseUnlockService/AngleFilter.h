#pragma once

#include <deque>
#include <vector>
#include <algorithm>
#include <cmath>

namespace ExerciseUnlock::Svc
{
    // Rolling median filter with 2σ outlier rejection.
    //
    // Chases a specific MoveNet failure mode we kept running into:
    // the raw per-frame angle occasionally spikes 100°+ in a single
    // frame (e.g. ankle lost briefly, knee flipped to the other
    // side's confident triplet). EMA alone leaks that spike through
    // and perturbs the state machine; a median kills the spike
    // outright because the median of {170, 170, 50, 165, 172} is
    // 170, not 125. The 2σ step then takes the mean of samples
    // close to the median so a short burst of two bad frames in a
    // row still doesn't dominate.
    //
    // Design mirrors the approach used by yo-WASSUP/Good-GYM:
    //   history (deque, max N) <- raw each frame
    //   median = median(history)
    //   stddev = stddev(history)
    //   filtered = mean of {v in history : |v - median| <= 2σ}
    //
    // Feed the output into whatever smoothing you already have
    // (we chain into EMA). Default window = 5 frames, which at
    // ~10 Hz pose rate = ~0.5 s of context — short enough to
    // follow a real squat descent (knee angle changes ~50° in
    // 0.5 s) but long enough to drown out single-frame garbage.
    class AngleMedianFilter
    {
    public:
        explicit AngleMedianFilter(size_t window = 5) : m_window(window) {}

        void Reset() { m_history.clear(); }

        // Push one raw sample, return the median-filtered estimate.
        // During warm-up (<3 samples) returns raw unchanged so the
        // detector doesn't stall waiting for the window to fill.
        float Push(float raw)
        {
            if (m_history.size() >= m_window) m_history.pop_front();
            m_history.push_back(raw);
            if (m_history.size() < 3) return raw;

            std::vector<float> sorted(m_history.begin(), m_history.end());
            std::sort(sorted.begin(), sorted.end());
            const float median = sorted[sorted.size() / 2];

            float sum = 0.0f;
            for (float v : m_history) sum += v;
            const float mean = sum / static_cast<float>(m_history.size());

            float sq = 0.0f;
            for (float v : m_history) sq += (v - mean) * (v - mean);
            const float stddev = std::sqrt(sq / static_cast<float>(m_history.size()));
            // When stddev is ~0 everything is already consistent;
            // keep the mean rather than filtering nothing out.
            if (stddev < 0.5f) return mean;

            const float limit = 2.0f * stddev;
            float filteredSum = 0.0f;
            int   count = 0;
            for (float v : m_history)
            {
                if (std::fabs(v - median) <= limit)
                {
                    filteredSum += v;
                    ++count;
                }
            }
            return (count > 0) ? (filteredSum / count) : median;
        }

    private:
        size_t            m_window;
        std::deque<float> m_history;
    };
}
