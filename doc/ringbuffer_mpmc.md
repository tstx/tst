# RingBufferMpmc

Source: [ringbuffer_mpmc.hpp](../ringbuffer_mpmc.hpp)

A fixed-size, lock-free circular buffer (FIFO), safe for **multiple producers and
multiple consumers** at the same time. Based on Dmitry Vyukov's
[bounded MPMC queue](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue).

Read [ringbuffer_spsc.md](ringbuffer_spsc.md) and [memory_order.md](memory_order.md)
first.

## API

```cpp
template <typename T, std::size_t N>   // N must be a power of 2, >= 2
class RingBufferMpmc {
public:
    bool push(const T& value);         // any thread, false if full
    std::optional<T> pop();            // any thread, nullopt if empty
    static constexpr std::size_t capacity();  // N
};
```

Copying is deleted: copying a buffer while other threads use it makes no sense.

## Usage

```cpp
#include "ringbuffer_mpmc.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1000;
    constexpr int kTotal = kProducers * kPerProducer;

    tst::RingBufferMpmc<int, 1024> queue;
    std::atomic<int> consumed{0};
    std::atomic<long long> sum{0};

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                while (!queue.push(p * kPerProducer + i)) std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < 4; ++c) {
        threads.emplace_back([&] {
            while (consumed.load() < kTotal) {
                if (auto val = queue.pop()) {
                    sum += *val;
                    ++consumed;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    std::cout << "sum = " << sum << '\n';  // 0 + 1 + ... + 3999 = 7998000
}
```

Note: with several consumers there is no global order of *processing*. Values leave
the queue in FIFO order, but which consumer handles them first depends on scheduling.

## Why the SPSC trick doesn't work anymore

In SPSC every index had exactly one writer. Now several producers share the write
position. If two producers both read `head == 7`, both write slot 7 and one value is
lost. So:

1. **Claiming a position must be atomic** -> `compare_exchange` (CAS) on the
   position. Exactly one thread wins position 7, the others retry with 8, 9, ...

2. **Claiming a slot and filling it are two separate steps.** Between them, a consumer
   must not read the half-written slot. And a producer that is one lap ahead must not
   overwrite a slot whose consumer is still reading it. A shared head/tail can't
   express this -> every slot gets its own state: a **sequence number**.

## The sequence number idea

Positions are counters that only grow: 0, 1, 2, 3, ... (they don't wrap at `N`).
Position `pos` lives in slot `pos % N`. Each slot stores `seq`, which says which
position the slot is ready for:

```
seq == pos      -> slot is free, ready to be WRITTEN for position pos
seq == pos + 1  -> slot contains the data for position pos, ready to be READ
```

Lifecycle of slot 1 with `N = 4`:

| Event | `seq` | Meaning |
|---|---|---|
| initial | 1 | free for position 1 |
| producer writes pos 1 | 2 | `pos + 1`: readable |
| consumer reads pos 1 | 5 | `pos + N`: free for position 5 (next lap) |
| producer writes pos 5 | 6 | readable |
| consumer reads pos 5 | 9 | free for position 9 |

A thread at position `pos` looks at its slot's `seq` and compares:

**Producer** (wants `seq == pos`):

| Condition | Meaning | Action |
|---|---|---|
| `seq == pos` | slot free | try to claim `pos` with CAS |
| `seq < pos` | slot still holds data from the previous lap | return `false` (full) |
| `seq > pos` | another producer already took `pos` | reload position, retry |

**Consumer** (wants `seq == pos + 1`):

| Condition | Meaning | Action |
|---|---|---|
| `seq == pos + 1` | data ready | try to claim `pos` with CAS |
| `seq < pos + 1` | nobody has written `pos` yet | return `nullopt` (empty) |
| `seq > pos + 1` | another consumer already took `pos` | reload position, retry |

"Full" / "empty" also cover a slot that was claimed but isn't finished yet (a producer
claimed it but hasn't written the data). That's fine: the caller just sees "try again
later" for a moment.

### Signed difference

```cpp
const std::intptr_t diff =
    static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
```

The comparison uses a signed difference instead of `seq < pos` so it keeps working when
the counters overflow after 2^64 operations (wrap-around arithmetic).

### Power-of-2 capacity

```cpp
static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of 2");
```

`pos % N` needs a division, which is slow. With `N` a power of 2, `pos & (N - 1)`
gives the same result with a single AND.

## Walkthrough of push()

```cpp
std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);
```
`relaxed`: just a first guess. If it's outdated, the `seq` check or the CAS below
notices and we retry. No data is read based on this value alone.

```cpp
const std::size_t seq = cell->seq.load(std::memory_order_acquire);
```
`acquire`: pairs with the release store of `seq` in `pop()`. If a consumer freed the
slot (`seq == pos`), its read of the old data is guaranteed to be finished -> we may
overwrite it.

```cpp
if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
    break;
}
```
"If `enqueuePos_` is still `pos`, set it to `pos + 1`." Only one producer succeeds.

- `compare_exchange_weak` may fail spuriously (even when the value matches) on some
  CPUs. Fine, we're in a loop anyway, and it's cheaper than `_strong`.
- `relaxed`: the CAS only hands out "tickets". It doesn't protect any data; that's the
  job of the `seq` acquire/release pair. Atomicity alone guarantees that no two
  producers get the same `pos`.
- On failure, the CAS writes the current value of `enqueuePos_` into `pos`, so the next
  iteration automatically tries the newer position.

```cpp
cell->data = value;
cell->seq.store(pos + 1, std::memory_order_release);
```
After a successful CAS nobody else touches the slot: other producers want a different
`seq`, consumers wait for `seq == pos + 1`.
`release`: the data write must be visible before the new `seq`. A consumer that sees
`pos + 1` also sees the data.

## Walkthrough of pop()

Mirror image of `push()`:

```cpp
const std::size_t seq = cell->seq.load(std::memory_order_acquire);
```
`acquire`: pairs with the release store in `push()`. Seeing `seq == pos + 1` means the
producer's data write is visible.

```cpp
T value = std::move(cell->data);
cell->seq.store(pos + N, std::memory_order_release);
```
Marks the slot as free for the next lap (position `pos + N`).
`release`: the read of the data must be finished before a producer sees the slot as
free and overwrites it.

## Constructor

```cpp
for (std::size_t i = 0; i < N; ++i) {
    cells_[i].seq.store(i, std::memory_order_relaxed);
}
```

Slot `i` starts as "free for position `i`". `relaxed` is enough: the constructor runs
before any other thread can see the object, and handing it to a thread (e.g. via
`std::thread`) already synchronizes.

## False sharing

Producers hammer `enqueuePos_`, consumers hammer `dequeuePos_`. Both get their own
cache line so the two groups don't slow each other down. See
[ringbuffer_spsc.md](ringbuffer_spsc.md#false-sharing).
