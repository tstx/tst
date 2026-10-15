#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <optional>
#include <thread>

namespace tst {

// RingBufferSpsc: A fixed-size, lock-free circular buffer (ring buffer), FIFO.
// Designed for single-producer, single-consumer (SPSC) scenarios: exactly ONE thread
// calls push() and exactly ONE (other) thread calls pop(). With more producers or
// consumers it breaks - see ringbuffer_mpmc.hpp for that case.
//
// Why no lock is needed:
//   - head_ is written only by the producer, tail_ only by the consumer.
//     Every variable has exactly one writer, so there is nothing to fight over.
//   - The other thread only *reads* that index, to check "is there space?" /
//     "is there data?".
//
// Why no full_ flag (like in RingBuffer)?
//   Both threads would have to write it -> data race. Instead the buffer has N + 1
//   slots and one slot always stays empty:
//     head == tail      -> empty
//     head + 1 == tail  -> full
//
// ---------------------------------------------------------------------------------
// Memory order crash course (used below)
// ---------------------------------------------------------------------------------
// The compiler and the CPU may reorder memory accesses as long as the *current*
// thread can't tell the difference. Another thread CAN tell the difference. Example:
//
//     buffer_[head] = value;   // (1) write data
//     head_ = next;            // (2) publish "there is new data"
//
// If (2) becomes visible to the consumer before (1), the consumer reads a slot
// that hasn't been written yet. memory_order tells the compiler/CPU what to keep:
//
//   relaxed: Only the atomic operation itself is atomic. No ordering guarantees
//            for other memory. Good enough when you only need the value, e.g.
//            reading your own index that no other thread writes.
//
//   release (on a store): Every read/write BEFORE this store in program order
//            becomes visible to other threads no later than the store itself.
//            "I'm done preparing the data, now I publish it."
//
//   acquire (on a load): Every read/write AFTER this load in program order stays
//            after it. If the load sees the value written by a release store, then
//            everything the other thread did before that store is visible here.
//            "I saw the flag, so I may now look at the data."
//
//   release + acquire on the SAME atomic variable form a pair ("synchronizes-with"):
//
//     Producer                              Consumer
//     buffer_[h] = value;                   if (head_.load(acquire) != t)   // sees h+1
//     head_.store(h + 1, release);  ---->       value = buffer_[t];         // sees value
//
//   seq_cst (the default if you write head_ = x or head_.load()) is stronger and
//   slower: it also gives one global order of all seq_cst operations. We don't need
//   that here, the release/acquire pairs are enough.
// ---------------------------------------------------------------------------------

template <typename T, std::size_t N>
class RingBufferSpsc {
    static_assert(N > 0, "capacity must be > 0");
    static constexpr std::size_t kSlots = N + 1; // one slot always stays empty
    static constexpr std::size_t kCacheLine = 64;

public:
    // Producer thread only. Returns false if the buffer is full.
    bool push(const T& value) {
        // relaxed: head_ is our own index, nobody else writes it, so we always read
        // our own latest value. No ordering needed.
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);

        // acquire: pairs with the release store of tail_ in pop(). If we see that the
        // consumer moved tail_ past a slot, the consumer is guaranteed to be finished
        // reading that slot - so we can safely overwrite it.
        if (next == tail_.load(std::memory_order_acquire)) return false; // full

        buffer_[head] = value;

        // release: the write to buffer_[head] above must be visible before the new
        // head_ is. Pairs with the acquire load of head_ in pop().
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns nullopt if the buffer is empty.
    std::optional<T> pop() {
        // relaxed: tail_ is our own index (see push()).
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // acquire: pairs with the release store of head_ in push(). If we see the new
        // head_, we are guaranteed to also see the data the producer wrote before it.
        if (tail == head_.load(std::memory_order_acquire)) return std::nullopt; // empty

        T value = std::move(buffer_[tail]);

        // release: our read of buffer_[tail] must be finished before the producer
        // sees the slot as free (otherwise it could overwrite it while we read).
        tail_.store(increment(tail), std::memory_order_release);
        return value;
    }

    // Snapshots only: the answer may already be outdated when the other thread is
    // active. Fine for "probably empty?" checks, not for decisions that must be exact.
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    bool full() const {
        return increment(head_.load(std::memory_order_acquire)) ==
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return N; }

    // Consumer thread only. Drops all elements currently in the buffer.
    // Instead of resetting both indices (would write head_ -> two writers -> race),
    // the consumer simply jumps tail_ forward to head_, like popping everything at once.
    // acquire/release for the same reasons as in pop().
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    static constexpr std::size_t increment(std::size_t i) { return (i + 1) % kSlots; } // wrap-around

    std::array<T, kSlots> buffer_; // fixed storage, no allocation

    // Each index gets its own cache line (typically 64 bytes). If both lived in the same
    // line, every push would invalidate the consumer's cached copy of the line and every
    // pop the producer's, even though they touch different variables ("false sharing").
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
