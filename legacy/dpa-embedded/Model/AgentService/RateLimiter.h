#pragma once
#include <deque>
#include <mutex>
namespace Model {
    // Sliding-window rate limiter (pure core, unit-tested). Caps requests to `maxRequests`
    // per `windowMs`. Pure: takes an explicit nowMs so it is deterministic under test; the
    // caller (MCP bridge) feeds the real clock. Thread-safe for defensive robustness.
    class RateLimiter {
    public:
        RateLimiter(int maxRequests, long long windowMs)
            : _max(maxRequests), _window(windowMs) {}
        // Records a request at nowMs; returns false if it would exceed the window budget.
        bool allow(long long nowMs) {
            std::lock_guard<std::mutex> lk(_mx);
            while (!_stamps.empty() && _stamps.front() <= nowMs - _window) _stamps.pop_front();
            if ((int)_stamps.size() >= _max) return false;
            _stamps.push_back(nowMs);
            return true;
        }
    private:
        int _max;
        long long _window;
        std::deque<long long> _stamps;
        std::mutex _mx;
    };
}
