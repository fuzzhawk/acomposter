// Wait-free single-producer / single-consumer ring buffer.
//
// Used for every message that crosses the UI <-> audio boundary. The capacity is
// rounded up to a power of two so the index wrap is a mask rather than a modulo.
#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <vector>

namespace acm {

template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue carries messages across threads; keep them POD so no "
                  "destructor or allocation ever runs on the audio thread.");

public:
    explicit SpscQueue(std::size_t capacity) {
        std::size_t n = 1;
        while (n < capacity) n <<= 1;
        slots_.resize(n);
        mask_ = n - 1;
    }

    // Producer side.
    bool push(const T& item) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & mask_;
        if (next == read_.load(std::memory_order_acquire))
            return false; // full
        slots_[w] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false; // empty
        out = slots_[r];
        read_.store((r + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const noexcept { return mask_; }

    std::size_t sizeApprox() const noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return (w - r) & mask_;
    }

private:
    std::vector<T> slots_;
    std::size_t mask_ = 0;

    // Keep the two cursors on separate cache lines; sharing one line turns every
    // push into a cache-line ping-pong with the consumer.
    alignas(64) std::atomic<std::size_t> write_{ 0 };
    alignas(64) std::atomic<std::size_t> read_{ 0 };
};

} // namespace acm
