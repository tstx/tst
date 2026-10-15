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

// RingBufferMpmc: A fixed-size, lock-free circular buffer (ring buffer), FIFO.
// Safe for MULTIPLE producers and MULTIPLE consumers (MPMC) at the same time.
// Based on Dmitry Vyukov's "bounded MPMC queue":
// https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
//
// Read ringbuffer_spsc.hpp first - it explains relaxed / acquire / release.
//
// ---------------------------------------------------------------------------------
// Why the SPSC trick doesn't work anymore
// ---------------------------------------------------------------------------------
// In SPSC every index had exactly one writer. Now several producers share the write
// position. If two producers both read head == 7, both would write slot 7 and one
// value is lost. So:
//
//   1. Claiming a position must be atomic -> compare_exchange (CAS) on the position.
//      Exactly one thread wins position 7, the others retry with 8, 9, ...
//
//   2. Claiming a slot and filling it are two separate steps. Between them, a consumer
//      must not read the half-written slot. And a producer that is one lap ahead must
//      not overwrite a slot whose consumer is still reading it. A shared head/tail
//      can't express this anymore -> every slot gets its own state: a sequence number.
//
// ---------------------------------------------------------------------------------
// The sequence number idea
// ---------------------------------------------------------------------------------
// Positions are counters that only grow: 0, 1, 2, 3, ... (they never wrap at N).
// Position pos lives in slot (pos % N). Each slot stores `seq`, which says which
// position the slot is ready for:
//
//   seq == pos      -> slot is free, ready to be WRITTEN for position pos
//   seq == pos + 1  -> slot contains the data for position pos, ready to be READ
//
// Lifecycle of slot i (N = 4, i = 1):
//
//   initial                seq = 1   (free for position 1)
//   producer writes pos 1  seq = 2   (= pos + 1: readable)
//   consumer reads  pos 1  seq = 5   (= pos + N: free for position 5, the next lap)
//   producer writes pos 5  seq = 6
//   consumer reads  pos 5  seq = 9   ... and so on
//
// A thread at position pos looks at its slot's seq and compares:
//
//   Producer (wants seq == pos):
//     seq == pos  -> free, try to claim position pos with CAS
//     seq <  pos  -> slot still holds data from the previous lap -> buffer is full
//     seq >  pos  -> another producer already took pos -> reload position, retry
//
//   Consumer (wants seq == pos + 1):
//     seq == pos + 1 -> data ready, try to claim position pos with CAS
//     seq <  pos + 1 -> nobody has written pos yet -> buffer is empty
//     seq >  pos + 1 -> another consumer already took pos -> reload position, retry
//
// "full" / "empty" also cover a slot that was claimed but isn't finished yet
// (e.g. a producer claimed it but hasn't written the data). That's okay: the caller
// just sees "try again later" for a moment.
//
// The comparison uses a signed difference (seq - pos) so it still works when the
// counters overflow after 2^64 operations (wrap-around arithmetic).
// ---------------------------------------------------------------------------------

template <typename T, std::size_t N>
class RingBufferMpmc {
    // pos % N is slow (division). With N a power of 2 we can use pos & (N - 1) instead.
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of 2");
    static constexpr std::size_t kMask = N - 1;
    static constexpr std::size_t kCacheLine = 64;

    struct Cell {
        std::atomic<std::size_t> seq; // which position this slot is ready for (see above)
        T data;
    };

public:
    RingBufferMpmc() {
        // Slot i starts as "free for position i".
        // relaxed: the constructor runs before any other thread can see the object.
        // (Handing the object to a thread, e.g. via std::thread, already synchronizes.)
        for (std::size_t i = 0; i < N; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    // Copying/moving while other threads use it makes no sense -> forbid it.
    RingBufferMpmc(const RingBufferMpmc&) = delete;
    RingBufferMpmc& operator=(const RingBufferMpmc&) = delete;

    // Any thread. Returns false if the buffer is full.
    bool push(const T& value) {
        Cell* cell;

        // relaxed: this is only a first guess. If it's outdated, the seq check or the
        // CAS below will notice and we retry. No data is read based on this value alone.
        std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & kMask];

            // acquire: pairs with the release store of seq in pop(). If we see that a
            // consumer freed this slot (seq == pos), its read of the old data is
            // guaranteed to be finished -> we may overwrite it.
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                // Slot is free for pos. Try to claim pos: "if enqueuePos_ is still pos,
                // set it to pos + 1". Only one producer can succeed.
                //
                // compare_exchange_weak may fail spuriously (even if the value matches)
                // on some CPUs - fine, we're in a loop anyway. It's cheaper than _strong.
                //
                // relaxed: the CAS only hands out "tickets". It doesn't protect any data;
                // the data is protected by the seq acquire/release pair. The atomicity of
                // the CAS alone guarantees that no two producers get the same pos.
                //
                // On failure, CAS writes the current value of enqueuePos_ into pos, so the
                // next loop iteration automatically tries the newer position.
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break; // we own position pos (and thus this slot) now
                }
            } else if (diff < 0) {
                return false; // slot still holds unread data from the previous lap -> full
            } else {
                // Another producer already claimed pos and moved on. Our pos is stale.
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }

        // Nobody else can touch this slot now: other producers want a different seq,
        // consumers wait until seq == pos + 1.
        cell->data = value;

        // release: the data write above must be visible before the new seq is.
        // Pairs with the acquire load of seq in pop(): a consumer that sees pos + 1
        // also sees the data.
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Any thread. Returns nullopt if the buffer is empty.
    std::optional<T> pop() {
        Cell* cell;

        // relaxed: only a first guess, same reasoning as in push().
        std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &cells_[pos & kMask];

            // acquire: pairs with the release store of seq in push(). If we see
            // seq == pos + 1, the producer's write of data is visible to us.
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                // Data for pos is ready. Try to claim it; only one consumer succeeds.
                // relaxed: same reasoning as the CAS in push().
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break; // we own position pos now
                }
            } else if (diff < 0) {
                return std::nullopt; // nothing written for pos yet -> empty
            } else {
                // Another consumer already took pos. Our pos is stale.
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }

        T value = std::move(cell->data);

        // Mark the slot as free for the next lap (position pos + N).
        // release: our read of data must be finished before a producer sees the slot
        // as free and overwrites it. Pairs with the acquire load of seq in push().
        cell->seq.store(pos + N, std::memory_order_release);
        return value;
    }

    static constexpr std::size_t capacity() { return N; }

private:
    std::array<Cell, N> cells_; // fixed storage, no allocation

    // Producers hammer enqueuePos_, consumers hammer dequeuePos_. Separate cache lines
    // so the two groups don't slow each other down (false sharing, see SPSC version).
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

    // Multi-threaded: several producers and consumers at the same time.
    // Every value must be consumed exactly once - nothing lost, nothing duplicated.
    // (A global order can't be checked: with multiple consumers, "who prints first"
    // depends on scheduling.)
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
            // Producer p pushes the values [p * kPerProducer, (p + 1) * kPerProducer).
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

    // join() synchronizes, so relaxed counters are fully visible here.
    for (int v = 0; v < kTotal; ++v) {
        assert(seen[v].load() == 1);
    }
    assert(!shared.pop());
    std::cout << "Transferred " << kTotal << " values, each exactly once" << std::endl;
}

} // namespace tst
