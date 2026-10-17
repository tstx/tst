#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <optional>
#include <thread>

namespace tst {

// RingBufferSpsc: Lock-free, fixed-size FIFO ring buffer.
// Exactly one producer thread (push) and one consumer thread (pop).
// Details: doc/ringbuffer_spsc.md

template <typename T, std::size_t N>
class RingBufferSpsc {
    static_assert(N > 0, "capacity must be > 0");
    static constexpr std::size_t kSlots = N + 1; // one slot always stays empty
    static constexpr std::size_t kCacheLine = 64;

public:
    // Producer thread only. Returns false if the buffer is full.
    bool push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);

        if (next == tail_.load(std::memory_order_acquire)) return false; // full

        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns nullopt if the buffer is empty.
    std::optional<T> pop() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) return std::nullopt; // empty

        T value = std::move(buffer_[tail]);
        tail_.store(increment(tail), std::memory_order_release);
        return value;
    }

    // Snapshots only - may be outdated while the other thread is active.
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    bool full() const {
        return increment(head_.load(std::memory_order_acquire)) ==
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return N; }

    // Consumer thread only. Drops all elements currently in the buffer.
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    static constexpr std::size_t increment(std::size_t i) { return (i + 1) % kSlots; } // wrap-around

    std::array<T, kSlots> buffer_; // fixed storage, no allocation

    // Separate cache lines to avoid false sharing.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // where the next push writes
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // where the next pop reads
};

inline void testRingBufferSpsc() {
    std::cout << "[RingBufferSpsc] Testing RingBufferSpsc<int, 5>" << std::endl;

    RingBufferSpsc<int, 5> rb;
    for (int i = 0; i < 5; ++i) {
        assert(rb.push(i));
    }
    std::cout << "Buffer full: " << rb.full() << std::endl;
    assert(!rb.push(5)); // full

    for (int i = 0; i < 5; ++i) {
        auto val = rb.pop();
        assert(val && *val == i);
    }
    std::cout << "Buffer empty: " << rb.empty() << std::endl;
    assert(!rb.pop()); // empty

    rb.push(1);
    rb.push(2);
    rb.clear();
    assert(rb.empty());

    // One producer thread and one consumer thread: all values must arrive in order.
    std::cout << "[RingBufferSpsc] Testing producer/consumer threads" << std::endl;
    constexpr int kCount = 1'000'000;
    RingBufferSpsc<int, 1024> shared;

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!shared.push(i)) std::this_thread::yield(); // full -> wait
        }
    });

    std::thread consumer([&] {
        for (int expected = 0; expected < kCount;) {
            if (auto val = shared.pop()) {
                assert(*val == expected);
                ++expected;
            } else {
                std::this_thread::yield(); // empty -> wait
            }
        }
    });

    producer.join();
    consumer.join();
    assert(shared.empty());
    std::cout << "Transferred " << kCount << " values in order" << std::endl;
}

} // namespace tst
