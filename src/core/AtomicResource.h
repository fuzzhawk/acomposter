// Hand a large immutable object to the audio thread without locking or freeing
// on it.
//
// Loading a sample, rebuilding a graph schedule, or swapping a looper's take all
// have the same shape: the message thread builds a new object, the audio thread
// must start using it atomically, and the *old* object must not be destroyed
// until the audio thread has provably stopped touching it.
//
// The engine publishes a monotonically increasing block counter. The audio
// thread bumps it once per callback, so an object retired during block N is
// unreachable once the counter has advanced past N + 1: the callback that might
// still have held the raw pointer has finished. collect() then frees it on the
// message thread, where a free is harmless.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace acm {

using BlockCounter = std::atomic<std::uint64_t>;

template <typename T>
class AtomicResource {
public:
    explicit AtomicResource(const BlockCounter* clock = nullptr) : clock_(clock) {}

    void setClock(const BlockCounter* clock) noexcept { clock_ = clock; }

    // -- message thread ----------------------------------------------------

    void publish(std::shared_ptr<T> next) {
        auto previous = std::move(current_);
        current_ = std::move(next);
        live_.store(current_.get(), std::memory_order_release);

        if (previous)
            retired_.push_back(Retired{ std::move(previous), now() });
    }

    void clear() { publish(nullptr); }

    // Frees anything the audio thread can no longer be holding. Cheap enough to
    // call every UI frame.
    void collect() {
        const std::uint64_t cutoff = now();
        for (std::size_t i = retired_.size(); i-- > 0;) {
            if (cutoff > retired_[i].block + 1) {
                retired_[i] = std::move(retired_.back());
                retired_.pop_back();
            }
        }
    }

    // The message thread's own view, kept alive by the owning shared_ptr.
    const std::shared_ptr<T>& shared() const noexcept { return current_; }

    // -- audio thread ------------------------------------------------------

    // Valid for the duration of the current block only. Never store it.
    const T* get() const noexcept { return live_.load(std::memory_order_acquire); }

private:
    struct Retired {
        std::shared_ptr<T> object;
        std::uint64_t block;
    };

    std::uint64_t now() const noexcept {
        return clock_ ? clock_->load(std::memory_order_acquire) : 0;
    }

    const BlockCounter* clock_ = nullptr;
    std::shared_ptr<T> current_;
    std::atomic<T*> live_{ nullptr };
    std::vector<Retired> retired_;
};

} // namespace acm
