#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <optional>
#include <thread>

namespace tst {

// RingBufferSpsc: A fixed-size circular buffer (ring buffer) implementation, FIFO.
// Designed for single-producer, single-consumer (SPSC) scenarios.
// This means that one thread can safely push elements into the buffer while
// another thread pops elements from it without additional synchronization mechanisms.
//
// Lock-free: head_ wird nur vom Producer geschrieben, tail_ nur vom Consumer.
// Statt eines full_-Flags (das beide Threads schreiben müssten -> Race) hat der
// Puffer N + 1 Slots; ein Slot bleibt immer frei, um "voll" von "leer" zu unterscheiden.

template <typename T, std::size_t N>
class RingBufferSpsc {
    static_assert(N > 0, "capacity must be > 0");
    static constexpr std::size_t kSlots = N + 1;
    static constexpr std::size_t kCacheLine = 64;

public:
    // Nur vom Producer-Thread aufrufen. Gibt false zurück, wenn voll.
    bool push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed); // eigener Index
        const std::size_t next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) return false; // voll
        buffer_[head] = value;
        head_.store(next, std::memory_order_release); // Element für Consumer sichtbar machen
        return true;
    }

    // Nur vom Consumer-Thread aufrufen. Leer -> nullopt.
    std::optional<T> pop() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed); // eigener Index
        if (tail == head_.load(std::memory_order_acquire)) return std::nullopt; // leer
        T value = std::move(buffer_[tail]);
        tail_.store(increment(tail), std::memory_order_release); // Slot für Producer freigeben
        return value;
    }

    // Nur Momentaufnahmen: Kann schon veraltet sein, wenn der andere Thread aktiv ist.
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    bool full() const {
        return increment(head_.load(std::memory_order_acquire)) ==
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return N; }

    // Nicht thread-safe: Nur aufrufen, wenn kein anderer Thread zugreift.
    void clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t increment(std::size_t i) { return (i + 1) % kSlots; } // Wrap-around

    std::array<T, kSlots> buffer_; // fester Speicher, keine Allokation
    // Eigene Cache-Lines, damit Producer und Consumer sich nicht gegenseitig
    // die Cache-Line invalidieren (False Sharing).
    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // wo das nächste push hinschreibt
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // wo das nächste pop herliest
};

inline void testRingBufferSpsc() {
    std::cout << "[RingBufferSpsc] Testing RingBufferSpsc<int, 5>" << std::endl;

    RingBufferSpsc<int, 5> rb;
    for (int i = 0; i < 5; ++i) {
        assert(rb.push(i));
    }
    std::cout << "Buffer full: " << rb.full() << std::endl;
    assert(!rb.push(5)); // voll

    for (int i = 0; i < 5; ++i) {
        auto val = rb.pop();
        assert(val && *val == i);
    }
    std::cout << "Buffer empty: " << rb.empty() << std::endl;
    assert(!rb.pop()); // leer

    // Ein Producer- und ein Consumer-Thread: Alle Werte müssen in Reihenfolge ankommen.
    std::cout << "[RingBufferSpsc] Testing producer/consumer threads" << std::endl;
    constexpr int kCount = 1'000'000;
    RingBufferSpsc<int, 1024> shared;

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!shared.push(i)) std::this_thread::yield(); // voll -> warten
        }
    });

    std::thread consumer([&] {
        for (int expected = 0; expected < kCount;) {
            if (auto val = shared.pop()) {
                assert(*val == expected);
                ++expected;
            } else {
                std::this_thread::yield(); // leer -> warten
            }
        }
    });

    producer.join();
    consumer.join();
    assert(shared.empty());
    std::cout << "Transferred " << kCount << " values in order" << std::endl;
}

} // namespace tst
