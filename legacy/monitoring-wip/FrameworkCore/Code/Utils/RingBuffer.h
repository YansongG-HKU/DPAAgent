#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <cassert>

namespace utils {

template <typename T, size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_nothrow_copy_assignable_v<T>
                  || std::is_move_assignable_v<T>,
                  "T must be assignable");

public:
    RingBuffer() = default;
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    bool push(const T& value) {
        const size_t head = _head.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;
        if (next == _tail.load(std::memory_order_acquire))
            return false;
        _buffer[head] = value;
        _head.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& value) {
        const size_t head = _head.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;
        if (next == _tail.load(std::memory_order_acquire))
            return false;
        _buffer[head] = std::move(value);
        _head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& value) {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        if (tail == _head.load(std::memory_order_acquire))
            return false;
        value = std::move(_buffer[tail]);
        _tail.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    const T& peek() const {
        assert(!empty());
        return _buffer[_tail.load(std::memory_order_relaxed)];
    }

    const T& peekAt(size_t index) const {
        assert(index < size());
        const size_t tail = _tail.load(std::memory_order_relaxed);
        return _buffer[(tail + index) & kMask];
    }

    void popDiscard() {
        assert(!empty());
        _tail.store((_tail.load(std::memory_order_relaxed) + 1) & kMask,
                     std::memory_order_release);
    }

    void clear() {
        _tail.store(_head.load(std::memory_order_relaxed),
                     std::memory_order_release);
    }

    size_t size() const {
        const size_t h = _head.load(std::memory_order_acquire);
        const size_t t = _tail.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }

    bool empty() const { return size() == 0; }
    bool full() const { return size() == kMask; }
    static constexpr size_t capacity() { return kMask; }

    class iterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;
        using iterator_category = std::forward_iterator_tag;

        iterator(const RingBuffer* buf, size_t idx) : _buf(buf), _idx(idx) {}
        const T& operator*() const { return _buf->peekAt(_idx); }
        iterator& operator++() { ++_idx; return *this; }
        bool operator!=(const iterator& o) const { return _idx != o._idx; }

    private:
        const RingBuffer* _buf;
        size_t _idx;
    };

    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, size()); }

private:
    static constexpr size_t kMask = Capacity - 1;
    std::array<T, Capacity> _buffer{};
    alignas(64) std::atomic<size_t> _head{0};
    alignas(64) std::atomic<size_t> _tail{0};
};

}
