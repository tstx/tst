#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace tst {

// RingBufferMpmc: Lock-free, fixed-size FIFO ring buffer.
// Safe for multiple producers and multiple consumers.
// Based on Dmitry Vyukov's bounded MPMC queue.
// Details: doc/ringbuffer_mpmc.md

template <typename T, std::size_t N>
class RingBufferMpmc {
    // Power of 2 so pos % N becomes pos & kMask.
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of 2");
    static constexpr std::size_t kMask = N - 1;
    static constexpr std::size_t kCacheLine = 64;

    struct Cell {
        std::atomic<std::size_t> seq; // == pos: free to write, == pos + 1: ready to read
        T data;
    };

public:
    RingBufferMpmc() {
        // Slot i starts as "free for position i".
        for (std::size_t i = 0; i < N; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    RingBufferMpmc(const RingBufferMpmc&) = delete;
    RingBufferMpmc& operator=(const RingBufferMpmc&) = delete;

    // Any thread. Returns false if the buffer is full.
    bool push(const T& value) {
        Cell* cell;
        std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & kMask];

            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                // Slot is free - try to claim pos.
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = value;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Any thread. Returns nullopt if the buffer is empty.
    std::optional<T> pop() {
        Cell* cell;
        std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & kMask];

            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                // Data is ready - try to claim pos.
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt; // empty
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }

        T value = std::move(cell->data);

        // Free the slot for the next lap.
        cell->seq.store(pos + N, std::memory_order_release);
        return value;
    }

    static constexpr std::size_t capacity() { return N; }

private:
    std::array<Cell, N> cells_; // fixed storage, no allocation

    // Separate cache lines to avoid false sharing.
    alignas(kCacheLine) std::atomic<std::size_t> enqueuePos_{0}; // next position to write
    alignas(kCacheLine) std::atomic<std::size_t> dequeuePos_{0}; // next position to read
};

inline void testRingBufferMpmc() {
    std::cout << "[RingBufferMpmc] Testing RingBufferMpmc<int, 4>" << std::endl;

    // Single-threaded: behaves like a normal FIFO ring buffer.
    RingBufferMpmc<int, 4> rb;
    for (int lap = 0; lap < 3; ++lap) { // several laps to exercise seq = pos + N
        for (int i = 0; i < 4; ++i) {
            assert(rb.push(lap * 10 + i));
        }
        assert(!rb.push(99)); // full
        for (int i = 0; i < 4; ++i) {
            auto val = rb.pop();
            assert(val && *val == lap * 10 + i);
        }
        assert(!rb.pop()); // empty
    }

    // Multi-threaded: every value must be consumed exactly once.
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 250'000;
    constexpr int kTotal = kProducers * kPerProducer;
    std::cout << "[RingBufferMpmc] Testing " << kProducers << " producers / " << kConsumers
              << " consumers" << std::endl;

    RingBufferMpmc<int, 1024> shared;
    std::vector<std::atomic<int>> seen(kTotal); // how often each value was popped
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                while (!shared.push(p * kPerProducer + i)) std::this_thread::yield(); // full
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) < kTotal) {
                if (auto val = shared.pop()) {
                    seen[*val].fetch_add(1, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield(); // empty
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    for (int v = 0; v < kTotal; ++v) {
        assert(seen[v].load() == 1);
    }
    assert(!shared.pop());
    std::cout << "Transferred " << kTotal << " values, each exactly once" << std::endl;
}

} // namespace tst
