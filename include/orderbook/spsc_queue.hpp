#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace orderbook {

// x86-64 line size. std::hardware_destructive_interference_size would say this, but GCC 11
// does not provide it.
constexpr std::size_t CACHE_LINE = 64;

// Single-producer, single-consumer ring buffer. Exactly one thread may call try_push and
// exactly one other thread may call try_pop. Padded = false exists only so the benchmark
// can measure what the padding buys.
template <typename T, std::size_t Capacity, bool Padded = true>
class SpscQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two so index & MASK replaces modulo");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    [[nodiscard]] bool try_push(const T& value) {
        // relaxed: only this thread writes write_idx_
        const std::size_t write = write_idx_.load(std::memory_order_relaxed);
        // Touch the consumer's line only when the stale copy says the queue is full
        if (write - cached_read_ == Capacity) {
            // acquire: pairs with the consumer's release on read_idx_, so its read of the
            // slot we are about to reuse has finished
            cached_read_ = read_idx_.load(std::memory_order_acquire);
            if (write - cached_read_ == Capacity) {
                return false;
            }
        }
        buffer_[write & MASK] = value;
        // release: the slot write above becomes visible before the new index does
        write_idx_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) {
        // relaxed: only this thread writes read_idx_
        const std::size_t read = read_idx_.load(std::memory_order_relaxed);
        // Touch the producer's line only when the stale copy says the queue is empty
        if (read == cached_write_) {
            // acquire: pairs with the producer's release on write_idx_, so the slot is filled
            cached_write_ = write_idx_.load(std::memory_order_acquire);
            if (read == cached_write_) {
                return false;
            }
        }
        out = buffer_[read & MASK];
        // release: our read of the slot completes before the producer can overwrite it
        read_idx_.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t MASK = Capacity - 1;
    static constexpr std::size_t ALIGN = Padded ? CACHE_LINE : alignof(std::atomic<std::size_t>);

    // Indices only ever increase; the slot is index & MASK. Unsigned subtraction gives the
    // fill level even after the 64-bit counters wrap, because Capacity divides 2^64.
    // Each line holds one thread's index plus its private, possibly stale copy of the
    // other's. A stale copy is always safe: it can only make the queue look fuller to the
    // producer or emptier to the consumer, never the reverse.
    alignas(ALIGN) std::atomic<std::size_t> write_idx_{0};
    std::size_t cached_read_ = 0;  // producer only
    alignas(ALIGN) std::atomic<std::size_t> read_idx_{0};
    std::size_t cached_write_ = 0;  // consumer only
    alignas(ALIGN) std::array<T, Capacity> buffer_{};
};

}  // namespace orderbook
