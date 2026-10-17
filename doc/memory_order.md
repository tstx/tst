# Memory order crash course

Both lock-free ring buffers ([SPSC](ringbuffer_spsc.md), [MPMC](ringbuffer_mpmc.md))
use explicit `std::memory_order` arguments on their atomics. This page explains what
they mean.

## The problem: reordering

The compiler and the CPU may reorder memory accesses as long as the *current* thread
can't tell the difference. Another thread **can** tell the difference:

```cpp
buffer_[head] = value;   // (1) write data
head_ = next;            // (2) publish "there is new data"
```

If (2) becomes visible to the consumer before (1), the consumer reads a slot that
hasn't been written yet. `memory_order` tells the compiler/CPU which orderings it must
keep.

## The orders used here

| Order | Used on | Guarantee |
|---|---|---|
| `relaxed` | load / store / RMW | Only the operation itself is atomic. No ordering for other memory. |
| `release` | store | Every read/write **before** this store becomes visible to other threads no later than the store itself. *"I'm done preparing the data, now I publish it."* |
| `acquire` | load | Every read/write **after** this load stays after it. If the load sees a value written by a release store, everything the other thread did before that store is visible here. *"I saw the flag, so I may now look at the data."* |
| `seq_cst` | all (default) | Like acquire/release, plus one global order of all `seq_cst` operations. Stronger and slower. Not needed here. |

`seq_cst` is what you get when you write `head_ = x` or `head_.load()` without an
argument.

## Release/acquire pairs

A release store and an acquire load **on the same atomic variable** form a pair
("synchronizes-with"):

```
Producer                              Consumer
buffer_[h] = value;                   if (head_.load(acquire) != t)   // sees h+1
head_.store(h + 1, release);  ---->       value = buffer_[t];         // sees value
```

If the consumer's acquire load observes `h + 1`, it is guaranteed to also observe
`value` in `buffer_[h]`.

## When is `relaxed` enough?

- Reading a variable that only your own thread writes (e.g. the producer reading
  `head_` in the SPSC buffer). You always see your own latest write.
- Counters/tickets where only atomicity matters and no other data depends on the value
  (e.g. the position CAS in the MPMC buffer, statistics counters).
- Initialization before the object is shared with other threads (creating a
  `std::thread` or calling `join()` already synchronizes).

## Example

```cpp
#include <atomic>
#include <cassert>
#include <thread>

int data = 0;
std::atomic<bool> ready{false};

int main() {
    std::thread producer([] {
        data = 42;                                     // plain write
        ready.store(true, std::memory_order_release);  // publish
    });
    std::thread consumer([] {
        while (!ready.load(std::memory_order_acquire)) {} // wait for publish
        assert(data == 42);                               // guaranteed
    });
    producer.join();
    consumer.join();
}
```

With `relaxed` on both sides the `assert` could fail on weakly ordered CPUs (ARM,
POWER), and the compiler may reorder on any CPU.
