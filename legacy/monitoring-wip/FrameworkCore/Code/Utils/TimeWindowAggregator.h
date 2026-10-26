#pragma once

#include "StreamingStatistics.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace utils {

struct WindowSnapshot {
    size_t count = 0;
    double mean = std::numeric_limits<double>::quiet_NaN();
    double stddev = std::numeric_limits<double>::quiet_NaN();
    double rms = std::numeric_limits<double>::quiet_NaN();
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    double peak = std::numeric_limits<double>::quiet_NaN();
    double kurtosis = std::numeric_limits<double>::quiet_NaN();
    double last = std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    double sumSq = 0.0;
    int64_t startMs = 0;
    int64_t endMs = 0;
};

template <size_t NumWindows>
class TimeWindowAggregator {
public:
    struct WindowDef {
        int64_t intervalMs;
    };

    TimeWindowAggregator() = default;

    explicit TimeWindowAggregator(const std::array<WindowDef, NumWindows>& defs)
        : _defs(defs) {
        for (size_t i = 0; i < NumWindows; ++i) {
            _windows[i].intervalMs = defs[i].intervalMs;
        }
    }

    void push(double value, int64_t timestampMs) {
        if (!std::isfinite(value)) return;
        for (size_t i = 0; i < NumWindows; ++i) {
            auto& w = _windows[i];
            if (w.stats.empty()) {
                w.windowStartMs = timestampMs;
            }
            w.stats.push(value);
            w.windowEndMs = timestampMs;
            const int64_t elapsed = timestampMs - w.windowStartMs;
            if (elapsed >= _defs[i].intervalMs) {
                w.lastSnapshot = snapshot(w, timestampMs);
                w.stats.reset();
                w.windowStartMs = timestampMs;
            }
        }
    }

    WindowSnapshot lastSnapshot(size_t windowIndex) const {
        return _windows[windowIndex].lastSnapshot;
    }

    size_t numWindows() const { return NumWindows; }

    const char* windowName(size_t i) const {
        static const char* names[] = {"1s", "10s", "1min", "10min", "1hr"};
        return (i < NumWindows) ? names[i] : "?";
    }

private:
    struct WindowState {
        int64_t intervalMs = 0;
        int64_t windowStartMs = 0;
        int64_t windowEndMs = 0;
        StreamingStatistics stats;
        WindowSnapshot lastSnapshot;
    };

    static WindowSnapshot snapshot(const WindowState& w, int64_t nowMs) {
        WindowSnapshot s;
        s.count = w.stats.count();
        s.mean = w.stats.mean();
        s.stddev = w.stats.stddev();
        s.rms = w.stats.rms();
        s.min = w.stats.min();
        s.max = w.stats.max();
        s.peak = w.stats.peak();
        s.kurtosis = w.stats.kurtosis();
        s.last = w.stats.last();
        s.sum = w.stats.mean() * static_cast<double>(s.count);
        s.sumSq = w.stats.rms() * w.stats.rms() * static_cast<double>(s.count);
        s.startMs = w.windowStartMs;
        s.endMs = nowMs;
        return s;
    }

    std::array<WindowDef, NumWindows> _defs;
    std::array<WindowState, NumWindows> _windows;
};

using DefaultTimeWindowAggregator = TimeWindowAggregator<4>;

inline DefaultTimeWindowAggregator makeDefaultAggregator() {
    return DefaultTimeWindowAggregator({{
        {1000},      // 1 second
        {10000},     // 10 seconds
        {60000},     // 1 minute
        {600000}     // 10 minutes
    }});
}

}
