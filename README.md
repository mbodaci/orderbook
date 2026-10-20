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

_Week 4: to be written._

## Testing

_To be written._

## Limitations

_To be written._
