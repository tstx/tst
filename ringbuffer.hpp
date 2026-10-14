#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace tst {

// RingBuffer: A fixed-size circular buffer (ring buffer) implementation, FIFO.

template <typename T, std::size_t N>
class RingBuffer {
public:
    // Element hinten einfügen. Gibt false zurück, wenn voll.
    bool push(const T& value) {
        if (full_) return false;
        buffer_[head_] = value;
        head_ = (head_ + 1) % N;          // Wrap-around
        if (head_ == tail_) full_ = true; // jetzt randvoll
        return true;
    }

    // Element vorne entnehmen. Leer -> nullopt.
    std::optional<T> pop() {
        if (empty()) return std::nullopt;
        T value = buffer_[tail_];
        tail_ = (tail_ + 1) % N;          // Wrap-around
        full_ = false;                    // nach pop nie mehr voll
        return value;
    }

    bool empty() const { return !full_ && head_ == tail_; }
    bool full()  const { return full_; }
    void clear() { head_ = tail_ = 0; full_ = false; }

private:
    std::array<T, N> buffer_;   // fester Speicher, keine Allokation
    std::size_t head_ = 0;      // wo das nächste push hinschreibt
    std::size_t tail_ = 0;      // wo das nächste pop herliest
    bool full_ = false;
};

void testRingBuffer() {
    std::cout << "[RingBuffer] Testing RingBuffer<int, 5>" << std::endl;

    RingBuffer<int, 5> rb;
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
}

} // namespace tst