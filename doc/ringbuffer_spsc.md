# RingBufferSpsc

Source: [ringbuffer_spsc.hpp](../ringbuffer_spsc.hpp)

A fixed-size, lock-free circular buffer (FIFO) for **single-producer,
single-consumer** use: exactly one thread calls `push()` and exactly one (other) thread
calls `pop()`. With more producers or consumers it breaks - use
[RingBufferMpmc](ringbuffer_mpmc.md) for that.

## API

```cpp
template <typename T, std::size_t N>
class RingBufferSpsc {
public:
    bool push(const T& value);      // producer only, false if full
    std::optional<T> pop();         // consumer only, nullopt if empty
    bool empty() const;             // snapshot
    bool full() const;              // snapshot
    void clear();                   // consumer only
    static constexpr std::size_t capacity();  // N
};
```

## Usage

```cpp
#include "ringbuffer_spsc.hpp"
#include <iostream>
#include <thread>

int main() {
    tst::RingBufferSpsc<int, 1024> queue;

    std::thread producer([&] {
        for (int i = 0; i < 100; ++i) {
            while (!queue.push(i)) std::this_thread::yield(); // full -> wait
        }
    });

    std::thread consumer([&] {
        for (int received = 0; received < 100;) {
            if (auto val = queue.pop()) {
                std::cout << *val << '\n';  // 0, 1, 2, ... in order
                ++received;
            } else {
                std::this_thread::yield(); // empty -> wait
            }
        }
    });

    producer.join();
    consumer.join();
}
```

## Why no lock is needed

- `head_` is written only by the producer, `tail_` only by the consumer. Every
  variable has exactly one writer, so there is nothing to fight over.
- The other thread only *reads* that index, to check "is there space?" /
  "is there data?".

## Why one slot always stays empty

The non-threaded [RingBuffer](../ringbuffer.hpp) uses a `full_` flag to tell "full"
apart from "empty" (both have `head == tail`). Here both threads would have to write
that flag -> data race.

Instead the buffer has `N + 1` slots and one slot always stays empty:

```
head == tail      -> empty
head + 1 == tail  -> full    (modulo N + 1)
```

Example with `N = 3` (4 slots):

```
empty          [ . . . . ]   head = tail = 0
push a, b      [ a b . . ]   tail = 0, head = 2
push c         [ a b c . ]   tail = 0, head = 3   -> head + 1 == 4 % 4 == tail: full
pop  -> a      [ . b c . ]   tail = 1, head = 3
push d         [ . b c d ]   tail = 1, head = 0   (wrapped around)
```

## Memory orders

See [memory_order.md](memory_order.md) for background.

### push()

```cpp
const std::size_t head = head_.load(std::memory_order_relaxed);
```
`relaxed`: `head_` is the producer's own index, nobody else writes it.

```cpp
if (next == tail_.load(std::memory_order_acquire)) return false;
```
`acquire`: pairs with the release store of `tail_` in `pop()`. If the producer sees
that the consumer moved `tail_` past a slot, the consumer has finished reading that
slot - it can be overwritten safely.

```cpp
buffer_[head] = value;
head_.store(next, std::memory_order_release);
```
`release`: the write to `buffer_[head]` must be visible before the new `head_`.
Pairs with the acquire load of `head_` in `pop()`.

### pop()

```cpp
const std::size_t tail = tail_.load(std::memory_order_relaxed);
```
`relaxed`: `tail_` is the consumer's own index.

```cpp
if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;
```
`acquire`: pairs with the release store of `head_` in `push()`. Seeing the new `head_`
guarantees seeing the data written before it.

```cpp
T value = std::move(buffer_[tail]);
tail_.store(increment(tail), std::memory_order_release);
```
`release`: the read of `buffer_[tail]` must be finished before the producer sees the
slot as free - otherwise it could overwrite the slot while it's being read.

### empty() / full()

Return snapshots. While the other thread is active the answer may already be outdated
when you look at it. Fine for "probably empty?" checks, not for decisions that must be
exact. Use the return values of `push()` / `pop()` instead.

### clear()

```cpp
tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
```

Resetting both indices to 0 would mean the consumer writes `head_` -> two writers ->
race. Instead the consumer jumps `tail_` forward to `head_`, as if it popped everything
at once. Acquire/release for the same reasons as in `pop()`. Must only be called from
the consumer thread.

## False sharing

```cpp
alignas(kCacheLine) std::atomic<std::size_t> head_{0};
alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
```

CPUs cache memory in lines of typically 64 bytes. If `head_` and `tail_` lived in the
same line, every `push()` would invalidate the consumer's cached copy and every `pop()`
the producer's - even though they touch different variables. Putting each index on its
own cache line avoids that ("false sharing").
