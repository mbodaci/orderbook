# Benchmarks

## Setup

| | |
|---|---|
| CPU | AMD Ryzen 7 5800H, benchmark pinned to one core (`taskset -c 3`) |
| OS | Ubuntu 22.04 under WSL2 (kernel 6.18.54.1-microsoft-standard-WSL2) |
| Compiler | g++ 11.4.0, `-O3 -DNDEBUG` (CMake Release), C++20 |
| Timer | `std::chrono::steady_clock`; ~20–30 ns per sample, included in every latency figure |

Reproduce:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
taskset -c 3 ./build/bench_orderbook
```

## Method

[bench/bench_orderbook.cpp](../bench/bench_orderbook.cpp) generates one op stream per workload
from a fixed seed, by running it against a real book so every cancel and modify targets an
order that is actually resting. The stream is then replayed on fresh books, so generation and
bookkeeping never fall inside a timed region.

- **Book:** prefilled to 10,000 resting orders (untimed), then 1,000,000 measured ops.
- **Mix:** 50% add, 25% cancel, 15% modify (half quantity-down, half price change), 10% market
  order. Adds are converted to cancels while the book is above 10,000 orders, so its size
  stays stable.
- **Prices:** bids sit below a fixed mid and asks above, so adds never cross. Each order's
  distance from mid is geometric with parameter `p`:
  - **Tight book**, `p = 0.2`: mean distance about 5 ticks.
  - **Wide book**, `p = 0.005`: mean distance about 200 ticks, hundreds of populated levels per side.
- **Latency:** each op is timed on its own; percentiles use the nearest rank.
- **Throughput:** the same stream replayed with no per-op timers; the median of 5 runs.
- **Checksum:** sums every result (trade counts, cancel outcomes). It must be identical across
  implementations, since the same ops on a correct book produce the same results.

## Price-level container: `std::map` vs sorted `std::vector`

Both implementations exposed the same interface, so `order_book.cpp` was unchanged between
them. The vector version is kept for reference in
[bench/sorted_levels.hpp](../bench/sorted_levels.hpp); it is not used in production. The vector stored levels worst-to-best, so the best level sat at the back, where inserts
and erases shift the fewest elements, and it found levels by binary search. Both passed all 35
tests under ASan/UBSan with the invariant checker on. Both also ran the full stream under
ASan/UBSan with identical checksums.

Results are from three alternating rounds. Latency comes from the round with the median
throughput.

### Tight book

| | std::map | sorted vector |
|---|---:|---:|
| Throughput, M ops/s (3 rounds) | 7.26 / 7.96 / 7.99 | 8.17 / 8.21 / 8.07 |
| add p50 / p99 / p99.9 ns | 91 / 391 / 782 | 100 / 350 / 711 |
| cancel p50 / p99 / p99.9 ns | 140 / 561 / 1163 | 150 / 471 / 1032 |
| modify (price) p50 / p99 / p99.9 ns | 210 / 721 / 1373 | 211 / 631 / 1302 |
| match p50 / p99 / p99.9 ns | 221 / 761 / 1563 | 230 / 681 / 1242 |

### Wide book

| | std::map | sorted vector |
|---|---:|---:|
| Throughput, M ops/s (3 rounds) | 4.77 / 5.08 / 5.10 | 3.87 / 3.80 / 3.88 |
| add p50 / p99 / p99.9 ns | 150 / 491 / 972 | 131 / 3336 / 4999 |
| cancel p50 / p99 / p99.9 ns | 200 / 692 / 1382 | 191 / 3828 / 6423 |
| modify (price) p50 / p99 / p99.9 ns | 311 / 882 / 1754 | 301 / 4098 / 7905 |
| match p50 / p99 / p99.9 ns | 271 / 862 / 1463 | 241 / 801 / 1503 |

### Decision: keep `std::map`

On the tight book the two are effectively tied. The vector is a few percent faster, about the
same as the run-to-run noise. On the wide book the vector's median is 5–13% better, but its
p99 is 5–7× worse for add, cancel and modify, and its throughput is about 24% lower.

The tail comes from inserting or removing a level away from the back, which shifts every
level between it and the best price. Each `PriceLevel` holds a `std::list`, so shifting one is
not a memcpy. A list's move re-links its first and last nodes to the new list object, which
means one or two cache misses per shifted level. `std::map` never moves existing levels: an
insert costs O(log n) pointer chasing no matter where the price lands.

A matching engine is judged on its tail and has to cope with whatever book shape the market
produces. `std::map` is the safer choice.

Untested ideas that could change this: keep levels in a stable pool and sort a vector of
small `(price, index)` pairs, so shifts move 16 bytes; or index levels directly by tick
inside a bounded price band.

## SPSC queue: padding and index caching

Reproduce: `./build/bench_spsc [producer_cpu consumer_cpu]`.

[bench/bench_spsc.cpp](../bench/bench_spsc.cpp) passes 50M `uint64_t` messages through a
`SpscQueue<uint64_t, 1024>`. The producer and consumer spin on `try_push` and `try_pop`. The
consumer sums what it receives, and the run aborts if the sum is wrong, which would mean a
message was lost or duplicated.

- **Pinning:** the threads run on CPUs 2 and 4. On this machine CPUs 2k and 2k+1 are
  hyperthreads of the same physical core, so 2 and 4 are separate cores. Cache lines
  therefore really cross between cores, through the shared L3.
- **Rounds:** each run does 7 rounds, alternating padded and unpadded, and reports the median.
- **Layout is the only difference.** `Padded` is a template parameter, so padded and unpadded
  run the same code.
  - Padded: each thread's index sits on its own 64-byte line, and the buffer starts on a
    further line.
  - Unpadded: everything is packed together.

I measured two versions of the queue:

1. **Naive:** every `try_push` loads the consumer's `read_idx_`, and every `try_pop` loads the
   producer's `write_idx_`. It is kept for reference in
   [bench/spsc_queue_naive.hpp](../bench/spsc_queue_naive.hpp) and is not used in production.
2. **Cached** (the current code): each thread keeps a plain `std::size_t` copy of the other
   thread's index, on its own cache line. It reloads the real atomic only when its copy says
   the queue is full (for the producer) or empty (for the consumer).

Two runs of each, alternating naive and cached, in M messages/s (median of 7 rounds):

| Version | Padded | Unpadded | Padded / unpadded |
|---|---:|---:|---:|
| Naive | 64.1 / 64.5 | 72.0 / 70.7 | 0.89× / 0.91× |
| Cached | 124.0 / 126.2 | 73.3 / 69.6 | 1.69× / 1.81× |

Earlier sessions gave the same picture: the naive queue at 0.92× and 0.95×, and a
cached prototype at 1.72×.

`sizeof`:

| Version | Padded | Unpadded |
|---|---:|---:|
| Naive | 8320 bytes | 8208 bytes |
| Cached | 8320 bytes | 8224 bytes |

The buffer is 8192 bytes.

### Why padding only helps once true sharing is removed

**Naive version.** Padding does not help, and is 5–11% slower. Every push reads the
consumer's index and every pop reads the producer's, so each operation needs the other
core's cache line. That is true sharing: the data really is shared, and no layout can avoid
the transfer.

Packed onto one line, both indices travel together. Padded onto two lines, each thread
writes one line and reads the other, so two lines move between the cores instead of one.

**Cached version.** In the common case each thread reads and writes only its own line: its
own index plus its private copy of the other's. The other thread's line is fetched only when
the queue looks full or empty. With 1024 slots that happens rarely, so the true sharing is
mostly gone.

What remains is false sharing:

- **Padded:** no unrelated data shares a line, and throughput roughly doubles over the naive
  queue (124–126 vs 64 M msgs/s).
- **Unpadded:** each thread's private variables share a line with the other thread's index,
  so every write by one thread still invalidates the line the other is using. Caching gains
  almost nothing there (70–73 vs 71–72 M msgs/s).

The 1.7–1.8× gap between padded and unpadded cached queues is therefore the cost of false
sharing.

**Caveat on the explanation.** These are throughput measurements. The coherence explanation
fits all four numbers, but I have not confirmed it with cache-miss counters, which are
generally unavailable under WSL2.

## Caveats

- **WSL2 is noisy.** Throughput varies by roughly ±10% between runs; a later `std::map` run
  gave 7.35 and 4.41 M ops/s. p99.9 values can move by 50% between rounds. Compare only
  numbers from the same session, and treat differences under about 10% as noise.
- **Timer overhead is included.** About 25 ns of every latency sample is the clock itself,
  which matters most for the cheapest ops. The clock also appears to tick in roughly 10 ns
  steps.
- **Synthetic flow.** Real order flow differs, for example in cancel rates and in price paths
  that move. Using two book shapes is meant to show sensitivity, not to model a real market.
