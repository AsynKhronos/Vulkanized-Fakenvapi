#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Fixed-capacity, allocation-free bounded MPMC queue based on per-cell sequence
// numbers. The steady-state producer/consumer paths perform only atomic loads,
// one reservation CAS, one payload copy, and a release publication store.
template <typename T, std::size_t Capacity>
class FixedMpmcRing {
    static_assert(Capacity >= 2, "ring capacity must be at least two");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "ring capacity must be a power of two");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "ring requires lock-free 64-bit atomics");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "ring payload assignment must not throw");

    struct Cell {
        std::atomic<std::uint64_t> sequence{0};
        T value{};
    };

public:
    FixedMpmcRing() noexcept { reset(); }

    FixedMpmcRing(const FixedMpmcRing&) = delete;
    FixedMpmcRing& operator=(const FixedMpmcRing&) = delete;

    void reset() noexcept {
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
        for (std::uint64_t i = 0; i < Capacity; ++i)
            cells_[i].sequence.store(i, std::memory_order_relaxed);
    }

    [[nodiscard]] bool try_enqueue(const T& value) noexcept {
        std::uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[pos & kMask];
            const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::int64_t>(sequence) -
                                    static_cast<std::int64_t>(pos);

            if (difference == 0) {
                if (!enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    continue;
                }

                cell.value = value;
                cell.sequence.store(pos + 1, std::memory_order_release);
                return true;
            }

            if (difference < 0)
                return false;

            pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool try_dequeue(T* out) noexcept {
        if (!out)
            return false;

        std::uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[pos & kMask];
            const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::int64_t>(sequence) -
                                    static_cast<std::int64_t>(pos + 1);

            if (difference == 0) {
                if (!dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    continue;
                }

                *out = cell.value;
                cell.sequence.store(pos + Capacity, std::memory_order_release);
                return true;
            }

            if (difference < 0)
                return false;

            pos = dequeue_pos_.load(std::memory_order_relaxed);
        }
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    std::array<Cell, Capacity> cells_{};
    alignas(64) std::atomic<std::uint64_t> enqueue_pos_{0};
    alignas(64) std::atomic<std::uint64_t> dequeue_pos_{0};
};
