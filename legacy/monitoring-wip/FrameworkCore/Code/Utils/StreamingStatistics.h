#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace utils {

class StreamingStatistics {
public:
    StreamingStatistics() = default;

    void reset() {
        _n = 0;
        _sum = 0.0;
        _sumSq = 0.0;
        _min = (std::numeric_limits<double>::max)();
        _max = (std::numeric_limits<double>::lowest)();
        _m1 = 0.0;
        _m2 = 0.0;
        _m3 = 0.0;
        _m4 = 0.0;
        _last = std::numeric_limits<double>::quiet_NaN();
    }

    void push(double value) {
        if (!std::isfinite(value)) return;
        const double delta = value - _m1;
        const double deltaN = delta / static_cast<double>(_n + 1);
        const double deltaN2 = deltaN * deltaN;
        const double term1 = delta * deltaN * static_cast<double>(_n);

        _n += 1;
        _sum += value;
        _sumSq += value * value;
        _min = (std::min)(_min, value);
        _max = (std::max)(_max, value);
        _last = value;

        const double n1 = static_cast<double>(_n);
        _m4 += term1 * deltaN2 * (n1 * n1 - 3.0 * n1 + 3.0)
             + 6.0 * deltaN2 * _m2 - 4.0 * deltaN * _m3;
        _m3 += term1 * deltaN * (n1 - 2.0) - 3.0 * deltaN * _m2;
        _m2 += term1;
        _m1 += deltaN;
    }

    size_t count() const { return _n; }
    bool empty() const { return _n == 0; }
    double last() const { return _last; }
    double min() const { return _n > 0 ? _min : std::numeric_limits<double>::quiet_NaN(); }
    double max() const { return _n > 0 ? _max : std::numeric_limits<double>::quiet_NaN(); }

    double mean() const {
        return _n > 0 ? _sum / static_cast<double>(_n)
                      : std::numeric_limits<double>::quiet_NaN();
    }

    double variance() const {
        return _n > 1 ? _m2 / static_cast<double>(_n - 1)
                      : std::numeric_limits<double>::quiet_NaN();
    }

    double stddev() const {
        const double v = variance();
        return std::isfinite(v) ? std::sqrt(v)
                                : std::numeric_limits<double>::quiet_NaN();
    }

    double rms() const {
        return _n > 0 ? std::sqrt(_sumSq / static_cast<double>(_n))
                      : std::numeric_limits<double>::quiet_NaN();
    }

    double skewness() const {
        if (_n < 3) return std::numeric_limits<double>::quiet_NaN();
        const double s2 = _m2 / static_cast<double>(_n);
        if (s2 <= 0.0) return 0.0;
        return _m3 / (static_cast<double>(_n) * s2 * std::sqrt(s2));
    }

    double kurtosis() const {
        if (_n < 4) return std::numeric_limits<double>::quiet_NaN();
        const double s2 = _m2 / static_cast<double>(_n);
        if (s2 <= 0.0) return 0.0;
        return _m4 / (static_cast<double>(_n) * s2 * s2) - 3.0;
    }

    double peakToPeak() const {
        return (_n > 0) ? _max - _min
                        : std::numeric_limits<double>::quiet_NaN();
    }

    double crestFactor() const {
        const double r = rms();
        const double a = std::abs(_last);
        return (std::isfinite(r) && r > 0.0) ? a / r
                                              : std::numeric_limits<double>::quiet_NaN();
    }

    double peak() const {
        return (_n > 0) ? (std::max)(std::abs(_min), std::abs(_max))
                        : std::numeric_limits<double>::quiet_NaN();
    }

private:
    size_t _n = 0;
    double _sum = 0.0;
    double _sumSq = 0.0;
    double _min = (std::numeric_limits<double>::max)();
    double _max = (std::numeric_limits<double>::lowest)();
    double _m1 = 0.0;
    double _m2 = 0.0;
    double _m3 = 0.0;
    double _m4 = 0.0;
    double _last = std::numeric_limits<double>::quiet_NaN();
};

}
