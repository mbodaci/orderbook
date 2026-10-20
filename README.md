[![CI](https://github.com/mbodaci/orderbook/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mbodaci/orderbook/actions/workflows/ci.yml)

# Order Book

A single-instrument limit order book and matching engine in C++20. It supports limit and
market orders, cancel, modify, and self-trade prevention, and matches at price-time priority.

## Build and run

Requires CMake 3.20+ and GCC or Clang (the warning flags are not MSVC syntax; on Windows,
build under WSL). GoogleTest is downloaded at configure time.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure    # tests
./matching_engine [messages]             # feed thread -> SPSC queue -> matching thread
./matching_engine --input flow.csv --trades trades.csv   # same, fed from a flow file
./bench_orderbook                        # benchmarks
```

Debug build with sanitizers. This also enables an invariant checker that walks the whole
book after every add, cancel and modify:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-debug && cd build-debug && ctest --output-on-failure
```

## Design

### Data structures

```cpp
std::map<Price, PriceLevel, std::greater<Price>> bids_;
std::map<Price, PriceLevel> asks_;
std::unordered_map<OrderId, PriceLevel::Handle> orders_;

class PriceLevel {
    Price price_;
    Quantity total_quantity_;
    std::list<Order> orders_;    // oldest first
};
```

**Prices are integer ticks.** Floating-point prices cannot be compared for equality reliably,
and the book keys on price equality.

**Bids use `std::greater`, asks the default `std::less`.** With these comparators, `begin()` is
the best price on both sides. Matching always starts at `opposite.begin()` and walks inward,
and the matching loop is identical for both sides apart from the map type.

**Each level holds a `std::list` for time priority.** New orders go to the back and fills take
from the front, so a level is a FIFO queue. A `std::list` iterator stays valid when other
elements are inserted or erased, and a `std::map` never moves its nodes. So an iterator into
a level's list, called a `Handle`, stays valid for as long as the order rests.

**Cancel is O(1) inside the level.** `orders_` maps each order ID to its `Handle`. The
handle points at the resting `Order`, which carries its own side and price. A cancel is:

1. a hash lookup for the handle;
2. an O(log L) map lookup for its level, where L is the number of levels on that side;
3. an O(1) list erase.

If the level is then empty, it is erased too.

### What I considered

The price-level container was the main open choice. I compared two options:

1. **`std::map`:** a balanced tree, with O(log L) insert, lookup and erase. Existing levels
   are never moved.
2. **A sorted `std::vector` of levels:** stored worst-to-best so the best level sits at the
   back, where inserts and erases shift the fewest elements, with binary search for lookup.
   It is kept for reference in [bench/sorted_levels.hpp](bench/sorted_levels.hpp).

Both exposed the same interface, so the matching code was identical for both. Both passed
the full test suite under ASan/UBSan, and both produced identical results over two million
benchmarked operations (the checksums matched).

### What I chose

`std::map`.

### What the benchmark showed

The benchmark used a synthetic stream: 10,000 resting orders, then 1M operations (50% add,
25% cancel, 15% modify, 10% market order). It ran on two book shapes:

- **Tight book:** orders a mean of about 5 ticks from mid.
- **Wide book:** orders a mean of about 200 ticks from mid.

Setup, method and caveats are in [results/benchmarks.md](results/benchmarks.md). Latencies are
per operation and include about 25 ns of timer overhead.

**Tight book**

| | std::map | sorted vector |
|---|---:|---:|
| Throughput, M ops/s (3 rounds) | 7.26 / 7.96 / 7.99 | 8.17 / 8.21 / 8.07 |
| add p50 / p99 / p99.9 ns | 91 / 391 / 782 | 100 / 350 / 711 |
| cancel p50 / p99 / p99.9 ns | 140 / 561 / 1163 | 150 / 471 / 1032 |
| modify (price) p50 / p99 / p99.9 ns | 210 / 721 / 1373 | 211 / 631 / 1302 |
| match p50 / p99 / p99.9 ns | 221 / 761 / 1563 | 230 / 681 / 1242 |

**Wide book**

| | std::map | sorted vector |
|---|---:|---:|
| Throughput, M ops/s (3 rounds) | 4.77 / 5.08 / 5.10 | 3.87 / 3.80 / 3.88 |
| add p50 / p99 / p99.9 ns | 150 / 491 / 972 | 131 / 3336 / 4999 |
| cancel p50 / p99 / p99.9 ns | 200 / 692 / 1382 | 191 / 3828 / 6423 |
| modify (price) p50 / p99 / p99.9 ns | 311 / 882 / 1754 | 301 / 4098 / 7905 |
| match p50 / p99 / p99.9 ns | 271 / 862 / 1463 | 241 / 801 / 1503 |

**Tight book.** The two are effectively tied. The vector's throughput is a few percent
higher, which is within the run-to-run noise of this environment (about ±10%).

**Wide book.** The vector's median is 5–13% better, but its p99 is 5–7× worse for add,
cancel and modify, and its throughput is about 24% lower.

The likely cause is how the vector inserts and removes levels. Adding or removing a level
away from the back shifts every level between it and the best price. Each shifted
`PriceLevel` contains a `std::list`. Moving a list does not move its nodes, but it does
re-link the first and last nodes to the new list object. Those nodes are scattered on the
heap, so each shifted level costs extra memory accesses that are likely cache misses. A
deep book has many levels to shift, so the cost lands in the tail. `std::map` never moves an
existing level.

This explanation fits the data: the slowdown appears only on the wide book, and only for
operations that create or remove levels. Market orders, which consume levels from the back,
are unaffected. I have not confirmed it with hardware cache-miss counters.

### What I gave up

- **Locality.** Both `std::map` and `std::list` allocate one heap node per level and per
  order. Walking levels or orders is pointer chasing, with no contiguous memory for the
  prefetcher. The vector's slightly better median on the wide book is consistent with this.
- **Allocation on the hot path.** Each new order, and each new price level, calls the
  general-purpose allocator.
- **Constant factors on small books.** A balanced tree carries overhead (node headers,
  rebalancing) that a small, contiguous structure avoids.

### When this choice would be wrong

I would expect a sorted vector to win for a book with only a handful of active levels, say
5–10, where the whole level array fits in L1 cache and shifts are short. I have not measured
this. The closest data point is the tight book, where the vector was a few percent faster but
within noise.

It would also be wrong if prices were confined to a known, narrow band. A flat array indexed
by tick gives O(1) level lookup without a tree or a search. This is also untested here.

## Concurrency

### The SPSC queue

[include/orderbook/spsc_queue.hpp](include/orderbook/spsc_queue.hpp) is a lock-free,
bounded, single-producer single-consumer ring buffer. It is meant to carry messages from a
feed thread to the matching thread. The interface is:

```cpp
template <typename T, std::size_t Capacity, bool Padded = true>
class SpscQueue {
public:
    bool try_push(const T& value);   // producer thread only; false if full
    bool try_pop(T& out);            // consumer thread only; false if empty
};
```

- **Indexing.** Capacity must be a power of two, so `index & (Capacity - 1)` replaces a
  modulo.
- **Indices only increase.** The fill level is `write - read`, so all `Capacity` slots are
  usable with no wasted "full" slot. The subtraction stays correct even after the 64-bit
  counters wrap, because `Capacity` divides 2^64.
- **Neither call blocks.** Each returns `false` instead, and the caller decides whether to
  spin, back off or drop.

### The pipeline: src/main.cpp

[src/main.cpp](src/main.cpp) connects a feed thread to the matching thread through a
`SpscQueue<Message, 1024>`. The two threads are pinned to separate physical cores, and both
busy-spin while the queue is full or empty.

**The message** is a tagged struct: `enum class MsgType { Add, Cancel, Modify, Shutdown }` plus
an `Order`.

- A cancel reads only the order ID.
- A modify reads the ID, plus the price and quantity as the new values.
- A market order is an `Add` whose order has `type = Market`.

The matching thread dispatches with a `switch` that has no `default` case. Because the build
uses `-Wall -Werror`, a message type without a `case` is a compile error.

**Shutdown is a message on the queue.** The feed pushes `Shutdown` after its last order. The
queue is first-in first-out, so every earlier message is applied before the matching thread
reaches it. No separate flag, and no extra memory-ordering argument, is needed.

**The orders** come either from a CSV flow file (`--input`, see Tools) or from a built-in seeded
generator: 50% limit adds, 25% cancels, 15% modifies and 10% market orders. The generator cannot see the book, so some cancels and modifies target orders
that have already filled. The book rejects those, and the program counts them.

**At shutdown the program prints** how many messages were applied and rejected, the number of
trades, the traded volume, and the resting order count.

**It also checks itself.** It applies the same messages again on a single thread, with no
queue, and requires identical results. That means every trade in the same order, the same counts,
and the same resting order count and best prices. The program exits with 1 if anything differs.

I tested this check by building a version whose feed skips one message. It reported a mismatch
and exited with 1.

### Why single-producer is enough

The matching engine is single-threaded by design. Price-time priority needs one total order
of events, and a single thread applying them in sequence gives that order for free. So there
is exactly one consumer. With one feed thread there is also exactly one producer.

That means each index has exactly one writer. Each index is therefore a plain atomic load and
store. Neither thread needs a compare-and-swap or `fetch_add`, which are read-modify-write
operations that contend for the line under concurrent writers.

### Memory ordering: acquire-release, not seq_cst

The guarantee needed is pairwise, and it has two halves.

1. **Producer to consumer: a slot's contents must be visible before its index is.**
   - The producer writes the slot, then stores `write_idx_` with `release`.
   - The consumer loads `write_idx_` with `acquire` before reading the slot.
   - The release/acquire pair creates a happens-before edge, so the consumer sees the
     finished write.
2. **Consumer to producer: a slot must not be reused while it is still being read.**
   - The consumer reads the slot, then stores `read_idx_` with `release`.
   - The producer loads `read_idx_` with `acquire` before overwriting that slot.

Index loads by the thread that owns the index are `relaxed`, because no other thread writes
that index.

`seq_cst` would add a single global order across all `seq_cst` operations on all variables.
Nothing here needs that: there are only two threads, and each edge runs between one store
and the load that reads it. `seq_cst` would also cost more on x86. A `seq_cst` store compiles
to a locked instruction (`xchg`) that acts as a full barrier, while a release store is a plain
`mov`. I checked this in GCC 11's x86-64 output, but have not measured the timing difference
in this project.

**Verified with ThreadSanitizer.** The queue tests pass under TSan with no reports. I also
weakened each of the four acquire/release operations to `relaxed`, one at a time, and TSan
reported a data race on the slot every time.

The stress test alone could not have caught these. Built without TSan, the broken queues
passed it 10 times out of 10. x86's hardware memory model is strong enough to hide the
missing ordering, but the C++ memory model does not guarantee it, and a compiler or a weaker
CPU (ARM) could break it.

### Index caching

In the naive version, every `try_push` reads the consumer's index and every `try_pop` reads
the producer's. Each operation therefore pulls the other core's cache line.

In the current version each thread keeps a plain `std::size_t` copy of the other's index:

- **Producer:** keeps `cached_read_`, and reloads `read_idx_` only when the copy says the
  queue is full.
- **Consumer:** keeps `cached_write_`, and reloads `write_idx_` only when the copy says it is
  empty.

**A stale copy is always safe.** The real `read_idx_` only grows, so a stale copy is never
higher than it. The producer can therefore only see the queue as *fuller* than it really is,
never emptier. The same argument applies to the consumer, which can only see the queue as
emptier than it is. The worst case is a spurious reload, never an overwrite or a read of an
unfilled slot.

### False sharing: measured

Each thread's line holds its own index plus its private copy of the other thread's index.
With `Padded = true`, these sit on separate 64-byte lines, and the buffer starts on a third.
`Padded = false` packs them together. The two variants are identical code, so the benchmark
measures layout alone.

Throughput in M messages/s, median of 7 rounds, two runs each. The threads run on separate
physical cores. Full data is in [results/benchmarks.md](results/benchmarks.md).

| Version | Padded | Unpadded | Padded / unpadded |
|---|---:|---:|---:|
| Naive (reads the other index every op) | 64.1 / 64.5 | 72.0 / 70.7 | 0.89× / 0.91× |
| Cached (current) | 124.0 / 126.2 | 73.3 / 69.6 | 1.69× / 1.81× |

**Padding only helps once true sharing is removed.** The naive queue really does share both
indices on every operation, so padding just splits one contended line into two and is slightly
slower.

Caching removes most of that true sharing. Only false sharing is then left, and padding
removes it. Together they roughly double throughput.

Either change alone gains nothing measurable. Caching without padding runs at 70–73 vs
71–72 M msgs/s, because each thread's private copy still shares a line with the other
thread's index.

This explanation fits all four numbers, but I have not confirmed it with hardware cache-miss
counters.

### With multiple producers

Several changes would be needed with more than one producer:

1. **Claiming a slot.** A producer could no longer just store `write_idx_`. It would claim a
   slot with a read-modify-write (`fetch_add` or a compare-and-swap loop) on a shared index.
   Every producer contends for that cache line.
2. **Knowing a slot is filled.** Slots can be claimed in one order and filled in another, so
   `write_idx_` no longer says which slots are ready. The usual fix is a sequence number in
   each slot, which the consumer checks before reading (Vyukov's bounded MPMC queue). Index
   caching as done here no longer works directly.
3. **The alternative.** Give each producer its own SPSC queue and have the matching thread
   poll them all. The fast path stays as it is, but merging events from several queues into
   one sequence becomes the consumer's job, and so does fairness between them.

Neither option is implemented or measured here.

## Testing

46 GoogleTest tests, run with `ctest`:

| File | Tests | Covers |
|---|---:|---|
| [tests/test_matching.cpp](tests/test_matching.cpp) | 17 | add and cancel basics; best bid/ask; rejections (zero quantity, duplicate ID); full and partial fills; fill at the resting order's price; sweeping several levels; time priority within a level; market orders (full fill, partial fill with the remainder dropped, empty book) |
| [tests/test_edge_cases.cpp](tests/test_edge_cases.cpp) | 25 | cancel after partial or full fill; cancelling a market order's ID; unknown IDs; modify (quantity down keeps priority, quantity up or a price change loses it, a modify that crosses matches immediately, rejections); self-trade prevention (incoming order cancelled, earlier fills kept, applies to market orders and to modify, different or absent owners still trade); order IDs (no reuse after a fill or a cancel, no unused ID below the highest, gaps allowed, a rejected add doesn't advance the mark, a modify keeps its ID) |
| [tests/test_spsc_queue.cpp](tests/test_spsc_queue.cpp) | 4 | empty, full, FIFO order across wraparound; two-thread stress test |

**The stress test** passes 2M messages through a 16-slot queue, so the queue is constantly
full or empty.

- The consumer requires exactly 0, 1, 2, …, so a lost message shows as a jump forward, a
  duplicate as a repeat, and reordering as either.
- Each message is two words that must match, which catches a slot read before it was fully
  written.

**Sanitizers and assertions**

- **Debug builds** run every test under AddressSanitizer and UndefinedBehaviorSanitizer.
- **The invariant checker** (`OrderBook::assert_invariants`) runs after every successful add,
  cancel and modify. It checks five things:
  1. best bid < best ask;
  2. there are no empty levels;
  3. every order sits on the correct side and price, with a positive quantity;
  4. each level's total equals the sum of its orders;
  5. the ID index and the levels contain exactly the same orders.

  It is compiled out in Release builds.
- **The checks are known to fire.** For each one below I built a deliberately broken copy of
  the code and confirmed the check catches it:
  - The invariant checker: a partial fill that skips the level total, and a full fill that
    leaves a stale ID-index entry.
  - ThreadSanitizer: each of the four queue orderings weakened to `relaxed` (see Concurrency).

**ThreadSanitizer** runs on the queue tests only. The TSan runtime cannot start under the
address-space randomisation of recent kernels.

A plain TSan build fails to build, not just to run, because CMake's test discovery runs the
test binary at build time. `CMAKE_CROSSCOMPILING_EMULATOR` fixes this: CMake then runs every
test binary through `setarch -R`, which disables randomisation, both during discovery and under
`ctest`.

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
  "-DCMAKE_CROSSCOMPILING_EMULATOR=setarch;x86_64;-R"
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure -R "^SpscQueue\."
```

GoogleTest still prints PASSED when TSan finds a race. The test fails anyway, because TSan
makes the process exit with code 66.

**CI** ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs three jobs on every push and
pull request to `main`:

1. Release build and tests, then `matching_engine` with 1M messages. It then generates a
   100k-message flow, replays it through the engine, and runs `analyse.py --no-chart`, which
   fails the job if a consistency check fails or the reference matcher disagrees.
2. Debug build with all tests under ASan and UBSan. `UBSAN_OPTIONS=halt_on_error=1` is set,
   because UBSan otherwise only prints a warning and the test would still pass.
3. Debug build with the queue tests under TSan, then `matching_engine` with 5k messages under
   TSan. The count is small because the invariant checker runs after every message.

Both `matching_engine` runs fail the job if the threaded pipeline's result differs from a
single-threaded replay of the same messages.

**Benchmarks as a cross-check**

- The order book benchmark prints a checksum of every operation's result. The `std::map` and
  sorted-vector books produced identical checksums over the same 2M operations. Both also ran
  that stream under ASan/UBSan without errors.
- The queue benchmark aborts if the consumer's sum is wrong.

## Tools

Three Python scripts in [tools/](tools/) run the engine on a flow file and check its output
independently. `generate_flow.py` and `replay.py` use only the standard library. `analyse.py`
needs matplotlib for the chart only (`pip install -r tools/requirements.txt`); use
`--no-chart` to skip it.

```bash
python3 tools/generate_flow.py flow.csv --messages 200000 --seed 42
python3 tools/replay.py flow.csv              # writes flow.trades.csv
python3 tools/analyse.py flow.csv flow.trades.csv
```

**File format.** CSV with a header row, chosen so that the C++ reader needs no library.

- The flow file has the columns `type,id,side,price,quantity,order_type,owner`.
  - Fields a message type does not use are left empty: a cancel uses only `id`, and a modify
    uses `id`, `price` and `quantity`.
  - The engine rejects a wrong header or a malformed row with the file and line number, and
    exits with 2.
- The trades file has the columns `message,aggressor_id,passive_id,price,quantity`. `message`
  is the 0-based index of the flow message that caused the trade.

**`generate_flow.py`** writes a seeded mix: 50% limit adds, 25% cancels, 15% modifies and 10%
market orders.

- Unlike the engine's built-in generator, some limit prices cross the mid, so limit orders and
  modifies also trade when they arrive.
- Owners are drawn from 0–10, so self-trade prevention is exercised.

**`replay.py`** runs `matching_engine --input <flow> --trades <flow>.trades.csv` and passes on
the engine's exit code, so the engine's own pipeline self-check still applies.

**`analyse.py`** checks the trades in two independent ways, then charts trade price over the
flow and traded volume by price:

1. **Consistency checks**, which don't re-run any matching. Every trade must:
   - come from the order being processed;
   - have its buyer and seller on opposite sides;
   - trade at the resting order's price, and not through the incoming order's limit;
   - not overfill either order;
   - not involve an order after it was cancelled;
   - not have the same owner on both sides.
2. **A reference matcher.** This is a second, deliberately plain implementation of the
   engine's rules in Python, about 100 lines. Its trades must match the engine's exactly, in
   the same order.

This is the only check that tests the matching logic itself. The engine's self-check compares
the engine with itself, so it proves the pipeline is lossless but would agree with a logic bug.

I tested both checks against deliberately broken engines, on a 200k-message flow:

| Bug introduced into the engine | Engine self-check | Consistency checks | Reference matcher |
|---|---|---|---|
| Self-trade prevention disabled | passes | 4,500 failures | — |
| Newest order at a price filled first | passes | pass | first differing trade reported |

## Limitations

**Out of scope by design**

- One instrument only.
- Limit and market orders only. No iceberg, stop, IOC, FOK or other order types.
- No persistence, recovery, networking or GUI.

**Engine behaviour**

- **The feed is a file or a generator, not a live source.** `matching_engine` reads its
  messages up front, from a CSV file or its built-in generator, and the feed thread replays
  them. There is no market-data or network input. Trades are recorded and optionally written
  to a file after the run, not published while it happens; a real engine would send
  executions out through a second queue.
- **Some outcomes are silent.** Unfilled market-order quantity is discarded, and an incoming
  order cancelled by self-trade prevention just stops. In both cases the caller has to infer
  the result from the returned trades.
- **Order IDs must increase.**
  - IDs are unique for the book's lifetime: `add_order` rejects any ID at or below the highest
    one accepted so far. So an ID can't be reused after its order fills or is cancelled, and the
    trade log never shows two orders under one ID.
  - A modify that re-adds an order keeps its ID.
  - The caller has to assign IDs in increasing order, as a venue assigning its own order IDs
    would. A book that takes IDs chosen by clients directly would need a set of every ID it has
    seen instead.
- **Self-trade prevention has one policy: cancel the incoming order.** Orders with
  `owner == NO_OWNER` (0) are exempt.
- **No price validation.** Any integer tick is accepted, including zero and negative prices. A
  market order's price is ignored.
- **Allocation on the hot path.** New orders and new price levels allocate heap nodes in
  `std::list`, `std::map` and `std::unordered_map`. Any operation that trades allocates a
  `std::vector<Trade>`.

**Measurement and tooling**

- **Benchmarks were run under WSL2 on one laptop.** Throughput varies by about ±10% between
  runs.
- **Latency figures include about 25 ns of timer overhead.**
- **The order flow is synthetic.**
- **The cache-coherence explanations are untested.** Both the sorted-vector result and the
  false-sharing result are explained by cache behaviour that is consistent with the timings but
  not confirmed with hardware counters.
- **ThreadSanitizer covers only the queue tests.** The order book is single-threaded.
