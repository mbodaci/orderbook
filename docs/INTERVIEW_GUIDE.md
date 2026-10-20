# Interview guide

This guide is for defending the project out loud. For each component it covers how it works,
the decision behind it, the alternatives, the question an interviewer is likely to ask, and an
answer grounded in what the code actually does.

- **Where the numbers come from.** Every benchmark figure is copied from
  [results/benchmarks.md](../results/benchmarks.md). Test outcomes are as recorded in the
  [README](../README.md).
- **Unmeasured claims are marked.** Where something is reasoning rather than measurement, the
  guide says so. Section 7 lists what not to claim.

## 1. The 60-second summary

A single-instrument limit order book and matching engine in C++20.

- **Orders:** limit and market orders, cancel, modify, and self-trade prevention. Matching is
  at price-time priority.
- **The book:** two `std::map`s of price levels, one per side. Each level holds a `std::list`
  of orders in arrival order. A hash map from order ID to a list iterator gives cancels
  without searching the level.
- **The pipeline:** a feed thread sends order messages to a single matching thread through a
  lock-free single-producer, single-consumer ring buffer. It uses acquire-release atomics, a
  cached copy of the other thread's index, and cache-line padding.
- **Verification:**
  - 46 GoogleTest tests, run under ASan/UBSan, with a debug-only invariant checker after
    every change to the book;
  - ThreadSanitizer on the queue;
  - an end-to-end self-check in the pipeline;
  - an independent Python reference matcher that must produce identical trades;
  - CI that runs all of these on every push.
- **Benchmarks:** I compared `std::map` against a sorted vector and kept the map, because the
  vector's p99 was 5–7× worse on a wide book. I measured the queue with and without padding
  and found that padding only helps once true sharing is removed.

## 2. Numbers worth knowing

All from [results/benchmarks.md](../results/benchmarks.md).

- **Setup:**
  - AMD Ryzen 7 5800H, Ubuntu 22.04 under WSL2, g++ 11.4.0 at `-O3 -DNDEBUG`.
  - The order book benchmark is pinned to one core.
  - Timer overhead is about 20–30 ns per sample and is *included* in every latency figure.
- **Noise:** throughput varies by about ±10% between runs, and p99.9 can move by 50% between
  rounds. Treat differences under about 10% as noise.

**Order book, `std::map` (the chosen container), median-throughput round:**

| | Tight book (mean ~5 ticks from mid) | Wide book (mean ~200 ticks from mid) |
|---|---|---|
| Throughput, M ops/s (3 rounds) | 7.26 / 7.96 / 7.99 | 4.77 / 5.08 / 5.10 |
| add p50 / p99 / p99.9 ns | 91 / 391 / 782 | 150 / 491 / 972 |
| cancel p50 / p99 / p99.9 ns | 140 / 561 / 1163 | 200 / 692 / 1382 |
| modify (price) p50 / p99 / p99.9 ns | 210 / 721 / 1373 | 311 / 882 / 1754 |
| match p50 / p99 / p99.9 ns | 221 / 761 / 1563 | 271 / 862 / 1463 |

**Sorted vector, wide book:**

| | Value |
|---|---|
| Throughput, M ops/s (3 rounds) | 3.87 / 3.80 / 3.88 |
| add p99 | 3336 ns |
| cancel p99 | 3828 ns |
| modify (price) p99 | 4098 ns |

**SPSC queue:** M messages/s, median of 7 rounds, two runs each, threads on separate physical
cores.

| Version | Padded | Unpadded | Padded / unpadded |
|---|---:|---:|---:|
| Naive | 64.1 / 64.5 | 72.0 / 70.7 | 0.89× / 0.91× |
| Cached (current) | 124.0 / 126.2 | 73.3 / 69.6 | 1.69× / 1.81× |

**Tests:** 46 in total. `test_matching.cpp` has 17, `test_edge_cases.cpp` has 25, and
`test_spsc_queue.cpp` has 4.

## 3. How each component works

### 3.1 Types: [order.hpp](../include/orderbook/order.hpp)

- **Plain types.** `OrderId` and `Quantity` are `uint64_t`, `Price` is `int64_t` (integer
  ticks), and `OwnerId` is `uint32_t`. `NO_OWNER = 0`.
- **`Order`:** `id`, `side`, `price`, `quantity` (the *remaining* quantity), `type` (Limit by
  default), and `owner` (`NO_OWNER` by default).
  - `type` and `owner` come last and have defaults, so `{id, side, price, qty}` still
    compiles.
  - `owner` fits in padding that was already there after `type`, so `Order` is 40 bytes.
- **`Trade`:** `aggressor_id`, `passive_id`, `price` and `quantity`, with a defaulted
  `operator==` so tests can compare whole trade lists.

### 3.2 Price level: [price_level.hpp](../include/orderbook/price_level.hpp)

A `PriceLevel` holds a price, a running `total_quantity_`, and a `std::list<Order>` in arrival
order. `Handle` is `std::list<Order>::iterator`.

- **`add`:** appends at the back and returns the handle.
- **`remove(handle)`:** erases the order and subtracts its quantity from the total.
- **`reduce(handle, amount)`:** lowers the order's quantity and the total together.
- **`front()`:** the oldest order, which is the next to fill.

### 3.3 Order book: [order_book.hpp](../include/orderbook/order_book.hpp) and [order_book.cpp](../src/order_book.cpp)

State:

- `bids_`: `std::map<Price, PriceLevel, std::greater<Price>>`.
- `asks_`: `std::map<Price, PriceLevel>`.
- `orders_`: `std::unordered_map<OrderId, PriceLevel::Handle>`, covering resting orders only.

**`add_order(order)`**

1. Rejects the order (returns `nullopt`) if its quantity is 0, or if its ID is not above
   `highest_id_`, the highest ID accepted so far.
   - This is checked in that order.
   - Only an accepted order advances `highest_id_`, so a rejected add doesn't use up its ID.
   - Every resting ID is at or below the mark, so this also rejects resting duplicates.
   - Everything after this step is the private `place()`, which `modify_order` also uses.
2. Copies the order, then matches the copy against the opposite side.
3. If quantity remains and it's a limit order, rests it:
   - `try_emplace(price, price)` finds or creates the level;
   - `PriceLevel::add` appends the order;
   - the handle goes into `orders_`.
4. Returns the trades.

Unfilled market-order quantity is dropped.

**`cancel_order(id)`**

1. Hash lookup in `orders_`; returns `false` if the ID isn't there.
2. `remove_resting(handle)` chooses the side from `handle->side`, then `unrest()`:
   - finds the level by `handle->price`;
   - removes the order;
   - erases the level if it's now empty.
3. Erases the index entry and returns `true`.

**`best_bid()` / `best_ask()`:** `begin()->first` on the side's map, or `nullopt` if it's empty.

**Complexity**, where L is the number of levels on a side:

| Operation | Cost |
|---|---|
| add, no fill | O(1) average hash lookup + O(log L) `try_emplace` + O(1) list append + O(1) average index insert |
| cancel | O(1) average hash lookup + O(log L) level lookup + O(1) list erase; erasing an empty level by iterator is amortised O(1) |
| each fill during matching | O(1); removing an emptied level from the front of the map is amortised O(1) |
| best bid / ask | O(1); `begin()` is constant time for every standard container |

### 3.4 Matching: `OrderBook::match` in [order_book.cpp](../src/order_book.cpp)

`match` is a member template, so the same code runs on `bids_` and `asks_`, whose types
differ.

1. **Outer loop.** While the incoming order has quantity left and the opposite side isn't
   empty, take `opposite.begin()`, the best level. Stop if `crosses()` is false.
   - A market order always crosses.
   - A buy crosses a level priced at or below its limit; a sell crosses one at or above it.
2. **Inner loop.** While quantity is left and the level isn't empty, take `level.front()`.
   - **Self-trade check first.** If it's a self-trade, set the incoming quantity to 0 and
     break.
   - Otherwise `fill = min(incoming, resting)`, and record a trade at the **level's price**,
     which is the resting order's price.
   - **Full fill:** erase the order from `orders_` *first*, then `level.remove(resting)`.
     `remove` frees the list node, and the erase reads `resting->id` through the handle.
   - **Partial fill:** `level.reduce(resting, fill)`.
3. **After the inner loop,** erase the level if it's empty.

### 3.5 Modify: `OrderBook::modify_order`

1. Rejects an unknown ID or `new_quantity == 0`.
2. **Same price, and new quantity <= remaining:** reduce in place with `PriceLevel::reduce`.
   The order keeps its place in the queue. The level is found with `bids_.at` or `asks_.at`.
3. **Anything else** (a different price, or a larger quantity):
   - copy the resting order (before removal invalidates the handle);
   - set the new price and quantity;
   - remove the original and erase its index entry;
   - call the private `place()` with the copy.

   `place()` skips the ID check. Going through `add_order` would reject the re-add, because the
   order's own ID is already at or below `highest_id_`.

   The order goes to the back of the queue. If the new price crosses, it matches immediately
   as the aggressor, under its own ID and owner.
4. **What `new_quantity` means:** the new *remaining* quantity, not the original size.

### 3.6 Self-trade prevention

`is_self_trade(incoming, resting)` is `incoming.owner != NO_OWNER && incoming.owner ==
resting.owner`. It's checked against each resting order just before filling it.

- **Policy: cancel the incoming order.**
  - The incoming order's remaining quantity is set to 0 and matching stops.
  - Fills already made against other owners stand.
  - The resting order is untouched, and the remainder never rests.
- **Where it applies:** to market orders, and to a modify that re-enters through `add_order`.
- **Exemption:** an order with `owner == 0` never self-trades.

### 3.7 Invariant checker: `assert_invariants()`

`assert_invariants()` runs at the end of every successful add, cancel and in-place modify. A
modify that re-adds the order is checked inside `add_order`.

- **What it checks:**
  1. best bid < best ask whenever both exist;
  2. no level is empty;
  3. each level's key equals `level.price()`;
  4. every order is on the right side, at the level's price, with quantity > 0;
  5. each level's total equals the sum of its orders;
  6. the ID index and the levels contain exactly the same orders (see below).
- **How check 6 works:**
  - For every order it finds in the levels, the `orders_` entry for that ID must point at
    *that exact element*: same address, not just same ID.
  - Then the number of orders found in the levels must equal `orders_.size()`.
  - Each index entry can match only one element, so equal counts mean every index entry is
    reachable through the levels. That's one O(n) pass instead of O(n²).
- **Compiled out of Release builds.** Under `#ifndef NDEBUG` the declaration is real; in the
  `#else` branch it's an empty inline function, so the call sites need no `#ifdef`. `nm` showed
  no symbol in the Release library.
- **Known to fire.** In deliberately broken builds it caught a partial fill that bypassed the
  level total, and a full fill that left a stale index entry.

### 3.8 SPSC queue: [spsc_queue.hpp](../include/orderbook/spsc_queue.hpp)

`SpscQueue<T, Capacity, Padded = true>`.

- **Layout and indexing:**
  - `Capacity` must be a power of two (a `static_assert`), so `index & MASK` replaces a
    modulo.
  - The indices only ever count up; the fill level is `write - read`. All `Capacity` slots are
    usable, and the unsigned subtraction stays correct even after a wrap, because `Capacity`
    divides 2^64.
  - `static_assert(std::atomic<std::size_t>::is_always_lock_free)`.
- **`try_push`:**
  1. Relaxed load of its own `write_idx_`.
  2. If `write - cached_read_ == Capacity`, reload `cached_read_` from `read_idx_` with
     `acquire`. If it's still full, return `false`.
  3. Write the slot.
  4. Store `write_idx_ + 1` with `release`.
- **`try_pop`** mirrors it: relaxed load of `read_idx_`, then `cached_write_` refreshed with
  `acquire` only when the queue looks empty, then read the slot, then a `release` store of
  `read_idx_`.
- **Layout:** `alignas(ALIGN)` puts `write_idx_` and `cached_read_` (both producer-owned) on
  one line, `read_idx_` and `cached_write_` (both consumer-owned) on another, and the buffer on
  a third. `ALIGN` is 64 when `Padded`, and the atomic's natural alignment otherwise.
- **Why 64:** GCC 11 doesn't provide `std::hardware_destructive_interference_size`, so the
  constant is hard-coded.

### 3.9 Pipeline: [main.cpp](../src/main.cpp)

- **The message:** `struct Message { MsgType type; Order order; }`, with
  `MsgType { Add, Cancel, Modify, Shutdown }`. The queue is `SpscQueue<Message, 1024>`.
- **Input:** messages are loaded up front, either from the built-in seeded generator or from a
  CSV file (`--input`).
- **The feed thread:** pinned to CPU 2. It pushes every message, spinning while the queue is
  full, then pushes `Shutdown`.
- **The matching thread:** pinned to CPU 4. It spins while the queue is empty, and `apply`
  dispatches each message with a `switch` that has no `default` case.
  - It records every trade together with the index of the message that caused it.
  - It stops when `apply` sees `Shutdown`.
- **The self-check:** after both threads join, the same messages are applied again on one
  thread with no queue. The program requires the same `Results` (applied and rejected counts,
  plus every trade with its message index), the same resting order count and the same best
  prices.
- **Exit codes:** 0 for success, 1 if the self-check fails, 2 for bad arguments or a file
  error.
- **Tested:** a version whose feed skips one message printed `NO` and exited with 1.

### 3.10 Python tools: [tools/](../tools/)

**`generate_flow.py`** writes a seeded CSV flow:

- 50% limit adds, 25% cancels, 15% modifies and 10% market orders.
- Limit prices use an offset of -2..20 from mid, so some cross and trade when they arrive.
- Owners are drawn from 0..10.

**`replay.py`** runs `matching_engine --input <flow> --trades <flow>.trades.csv` and passes on
its exit code.

**`analyse.py`** checks the trades in two independent ways, then charts them.

1. **Consistency checks** that don't re-run any matching:
   - trades are in message order;
   - a cancel produces no trades;
   - the aggressor is the order being processed;
   - the passive order exists and wasn't cancelled earlier;
   - the two sides are opposite;
   - the price is the resting order's current price, and not through the aggressor's limit;
   - the owners differ (unless 0);
   - neither order is overfilled.
2. **`ReferenceBook`**, an independent Python implementation of the same rules. Its trades
   must equal the engine's exactly, in order.
   - Levels are a dict from price to a `deque`, with a sorted price list per side maintained
     with `bisect`.
   - A cancelled order is marked dead (remaining = 0) instead of being removed from the
     middle of a deque.
   - When the best level is looked up, dead entries at its front are discarded, and a level
     left with nothing is deleted.
3. **Charts:** trade price over the flow, and traded volume by price. matplotlib is imported
   only inside `chart()`, so `--no-chart` needs only the standard library.

**Tested against broken engines,** on a 200k-message flow:

| Bug in the engine | Engine self-check | Consistency checks | Reference matcher |
|---|---|---|---|
| Self-trade prevention disabled | passes | 4,500 failures | n/a |
| Newest order filled first | passes | pass | reports the first differing trade |

## 4. Design decisions, alternatives, and the questions they invite

### 4.1 Integer tick prices

- **Decision:** `Price` is `int64_t`, in ticks.
- **Alternatives:** `double`, or a decimal type.

**Q: Why not `double`?**
**A:** The book keys levels on price equality and compares prices constantly. Floating-point
values computed in different ways can differ in the last bit, so two orders at "the same" price
could land on different levels. Integer ticks make equality exact and comparisons cheap. A real
venue has a fixed tick size anyway, so converting to and from display prices is a boundary
concern.

**Q: Why signed?**
**A:** It keeps differences and offsets natural; the generators compute `MID - offset`. The
book doesn't validate prices, so zero and negative ticks are accepted; that's listed as a
limitation.

### 4.2 Price-level container: `std::map` vs sorted `std::vector`

- **Decision:** `std::map`.
- **Alternatives:**
  - **Measured:** a sorted vector stored worst-to-best, with binary search. It's kept in
    [bench/sorted_levels.hpp](../bench/sorted_levels.hpp).
  - **Not measured:** a flat array indexed by tick, or a pool of levels plus a sorted vector
    of small `(price, index)` pairs.

**Q: A vector is more cache-friendly. Why a tree?**
**A:** I measured both, behind the same interface, with the matching code unchanged.

- **Tight book:** they were effectively tied. The vector was a few percent faster on
  throughput (8.07–8.21 vs 7.26–7.99 M ops/s), which is within the noise.
- **Wide book:** the vector's median was 5–13% better, but its p99 was 5–7× worse for add,
  cancel and modify (for example, add p99 3336 ns vs 491 ns), and its throughput was about
  24% lower (3.80–3.88 vs 4.77–5.10 M ops/s).

A matching engine is judged on its tail and has to handle whatever book shape the market
produces, so I kept the map.

**Q: Why is the vector's tail so bad?**
**A:** Inserting or removing a level away from the best price shifts every level in between.
Each `PriceLevel` contains a `std::list`, so a shift isn't a memcpy. Moving a list re-links its
first and last nodes to the new list object, and those nodes are scattered on the heap. That
explanation fits the data:

- the slowdown appears only on the wide book;
- it appears only in operations that create or remove levels;
- market orders, which remove levels from the back, are unaffected.

I haven't confirmed it with cache-miss counters, which are generally unavailable under WSL2.

**Q: When would the vector win?**
**A:** I'd expect it to win on a book with only a handful of active levels, where the whole
array fits in L1 and shifts are short. I haven't measured that; the closest data is the tight
book, where it was within noise. A flat array indexed by tick would win if prices were
confined to a known narrow band. That's also untested.

**Q: How do you know both implementations were correct?**
**A:** Both passed the full test suite under ASan/UBSan with the invariant checker on. Both
also produced identical checksums over the same benchmark stream, and ran that stream under
ASan/UBSan without errors.

### 4.3 `std::greater` for bids

- **Decision:** `bids_` uses `std::greater<Price>`, and `asks_` uses the default `std::less`.
- **Alternative:** one comparator for both maps, using `rbegin()` for bids.

**Q: Why two comparators?**
**A:** With these comparators, `begin()` is the best price on both sides. The matching loop
always takes `opposite.begin()` and walks inward, and the code is the same for both sides.

- **The cost:** the two maps are different types. So `match`, `rest`, `unrest` and
  `check_side` are templates, and `remove_resting` and the in-place path of modify branch on
  side.
- **With `rbegin()` instead,** the bid path would need reverse iterators, and erasing through a
  reverse iterator is awkward (`std::next(it).base()`).

### 4.4 `std::list` per level

- **Decision:** each level is a `std::list<Order>`.
- **Alternatives:** `std::deque`, `std::vector`, or an intrusive list with a pool allocator.

**Q: Why a linked list? They're slow.**
**A:** Two properties matter:

1. Arrival order is kept with O(1) push to the back and O(1) removal from the front, for
   fills.
2. Iterators to the other elements stay valid when any element is inserted or erased.

The second is what allows O(1) cancel from the middle of a level through a stored handle.

- `std::vector` and `std::deque` can't give that. Erasing from the middle invalidates
  iterators, and `deque::push_back` invalidates all of its iterators, although references
  survive.
- **The cost:** one heap allocation per order, and pointer chasing when walking a level.
- **The usual upgrade** is an intrusive doubly-linked list whose nodes live in a preallocated
  pool, which keeps the same properties without calling the general-purpose allocator. I
  haven't built or measured it.

### 4.5 Iterator handles and the ID index

- **Decision:** `orders_` maps each order ID to a `std::list<Order>::iterator`.
- **Alternatives:**
  - store (side, price) and search the level for the ID, which is O(level length);
  - store a pointer;
  - give each order a sequence number.

**Q: How is cancel O(1)?**
**A:** It's O(1) inside the level. The steps:

1. A hash lookup gives the handle.
2. The handle points at the `Order`, which carries its side and price, so nothing else needs
   storing.
3. An O(log L) map lookup finds the level.
4. An O(1) list erase removes the order.

**Q: Can the handle dangle?**
**A:** No, not while the order rests.

- List iterators survive inserts and erases of *other* elements.
- `std::map` never moves its nodes, so the `PriceLevel`, and therefore the list, stays at the
  same address.
- The handle is invalidated only when that order is removed, and every removal path erases the
  index entry too.
- In `match`, the order matters: the index erase reads `resting->id` through the handle, so
  it happens before `level.remove`. The invariant checker verifies that every index entry
  points at the exact element in its level.

**Q: `unordered_map` rehashes. Doesn't that break things?**
**A:** A rehash invalidates the `unordered_map`'s own iterators, but the code never holds those
across an insert. The *values* are list iterators, and copying them during a rehash doesn't
affect them.

### 4.6 Return type: `std::optional<std::vector<Trade>>`

- **Decision:** `nullopt` means rejected; an empty vector means accepted with no fills.
- **Alternatives:**
  - a plain vector, which can't tell rejection apart from "no fills";
  - an exception for rejection;
  - a result struct;
  - an output buffer passed in by the caller.

**Q: Isn't allocating a vector per call slow?**
**A:** An empty `std::vector` doesn't allocate, so only calls that trade pay. It's still an
allocation on the hot path, and it's listed as a limitation. The usual fix is to pass in a
reusable output buffer, or a callback per trade. I haven't measured the difference.

### 4.7 Modify semantics

- **Decision:**
  - same price with quantity down (or unchanged): change it in place and keep priority;
  - quantity up, or any price change: cancel and re-add, which loses priority and may match;
  - `new_quantity` is the new *remaining* quantity.
- **Alternatives:**
  - always cancel and re-add;
  - reject quantity increases;
  - treat quantity as the total size.

**Q: Why does reducing quantity keep priority but increasing it doesn't?**
**A:** That's the standard exchange convention. Shrinking an order can't hurt anyone behind it
in the queue; growing it would let a trader add size without waiting their turn. The tests
`QuantityDownKeepsTimePriority` and `QuantityUpLosesTimePriority` check exactly this.

**Q: What if the modified price crosses the spread?**
**A:** The order re-enters through `add_order`, so it matches immediately as the aggressor
under its own ID, and self-trade prevention applies. Tested by
`PriceChangeThatCrossesMatchesImmediately` and `AppliesWhenModifyCrosses`.

### 4.8 Self-trade prevention policy and `NO_OWNER`

- **Decision:** cancel the incoming order; owner 0 is exempt.
- **Alternatives:** cancel the resting order, cancel both, or decrement both by the smaller
  quantity.

**Q: Why cancel-incoming?**
**A:** The trader's existing resting order keeps its priority, and the rule is the simplest to
reason about: matching stops at the first self-match. Fills already made against other owners
stand. Venues generally let the participant choose the policy; I implemented one.

**Q: Why the `NO_OWNER = 0` sentinel instead of a required owner?**
**A:** It lets orders without an owner always trade. If 0 counted as an owner, every order
without one would self-match against every other, and every existing test, all written with
owner 0, would stop matching. The alternative is to make the owner required everywhere. It's a
judgment call, and the sentinel is documented in `order.hpp`.

**Q: How would the caller know self-trade prevention cancelled their order?**
**A:** At the moment they can't, directly; they see fewer trades than expected. It's listed as
a limitation, alongside discarded market-order remainders. A result struct with a "cancelled
quantity" field would fix both.

### 4.9 The invariant checker exists only in debug builds

- **Decision:** a full O(n) check after every change to the book, compiled out under `NDEBUG`.
- **Alternatives:** check only in tests, check incrementally, or check in Release behind a
  flag.

**Q: Isn't O(n) after every operation absurdly slow?**
**A:** Yes, which is why it's debug-only. In Release, `assert_invariants` is an empty inline
function; I confirmed with `nm` that the symbol is absent. In Debug it turns every test, and
every ASan run, into a whole-book consistency check.

The cost is real: the CI TSan job runs the pipeline on only 5k messages because of it.

**Q: How do you know the checker actually catches anything?**
**A:** I broke the engine deliberately in a scratch copy, and the tests aborted on the right
assertion in both cases:

- a partial fill that bypassed the level total tripped the level-total check;
- a full fill that skipped the index erase tripped the count check.

### 4.10 Memory ordering: acquire-release, not seq_cst

- **Decision:**
  - the owner's own index is loaded `relaxed`;
  - the other thread's index is loaded `acquire`;
  - index stores are `release`.
- **Alternatives:** `seq_cst` everywhere, or `relaxed` everywhere, which would be wrong.

**Q: Walk me through why this is correct.**
**A:** There are two happens-before edges.

1. **Producer to consumer.** The producer writes the slot, then release-stores `write_idx_`.
   The consumer acquire-loads `write_idx_`, then reads the slot. So the consumer sees the
   finished write.
2. **Consumer to producer.** The consumer reads the slot, then release-stores `read_idx_`. The
   producer acquire-loads `read_idx_` before overwriting that slot. So the producer never
   overwrites a slot that's still being read.

Each index has exactly one writer, so the owner can read its own index `relaxed`.

**Q: Why not seq_cst?**
**A:** `seq_cst` adds a single global order across all `seq_cst` operations on all variables.
Two threads with pairwise edges don't need that.

It also costs more on x86. I checked GCC 11's output: a `seq_cst` store is `xchg`, a locked
instruction that acts as a full barrier, while a release store is a plain `mov`. I haven't
measured the timing difference here.

**Q: How did you verify the ordering? Stress tests pass on x86 regardless.**
**A:** Exactly. I weakened each of the four acquire and release operations to `relaxed`, one
at a time.

- **Without TSan,** the broken queues passed the stress test 10 times out of 10, because x86's
  strong hardware ordering hides the bug.
- **Under ThreadSanitizer,** each of the four was reported as a data race on the slot.

So TSan in CI is the actual proof; the stress test proves no loss, duplication or reordering
on this hardware.

**Q: Why not `memory_order_consume`?**
**A:** It's discouraged, and compilers implement it as `acquire` anyway.

### 4.11 Cached indices

- **Decision:** each thread keeps a plain `std::size_t` copy of the other thread's index, and
  reloads the atomic only when the queue looks full (producer) or empty (consumer).
- **Alternative:** the naive queue, which reads the other index on every operation. It's kept
  in [bench/spsc_queue_naive.hpp](../bench/spsc_queue_naive.hpp).

**Q: Isn't a stale copy dangerous?**
**A:** No. The real `read_idx_` only grows, so a stale copy is never higher than it. That means
the producer can only think the queue is *fuller* than it really is, never emptier. The same
argument applies to the consumer, which can only see the queue as emptier. The worst case is a
spurious reload, never an overwrite or a read of an unfilled slot.

**Q: Why is `cached_read_` not atomic?**
**A:** Only the producer ever reads or writes it, and only the consumer touches
`cached_write_`. No two threads access the same plain variable, so there's no data race.

**Q: What did caching buy?**
**A:** Padded: 124.0 / 126.2 M msgs/s cached, vs 64.1 / 64.5 naive, roughly double. But see
4.12: caching without padding gained nothing.

### 4.12 Cache-line padding

- **Decision:** `alignas(64)` separates the producer's line, the consumer's line, and the
  buffer.
- **Alternative:** packed (`Padded = false`), which is the same code with a different layout.

**Q: Did padding help?**
**A:** It depends on whether true sharing is still there.

- **Naive queue:** padding was 0.89–0.91× (slightly slower). Every operation reads the other
  thread's index, so the line moves between cores regardless; padding just splits one
  contended line into two.
- **Cached queue:** padding was 1.69–1.81× faster, because the common case touches only the
  thread's own line. Unpadded, the cached queue ran at 73.3 / 69.6, about the same as the naive
  one, because each thread's private copy still shared a line with the other thread's index.

So padding only helps once true sharing is removed, and caching only helps once false sharing
is removed. The coherence explanation fits all four numbers, but hasn't been confirmed with
counters.

**Q: Why 64 bytes and not 128?**
**A:** 64 is the line size on this x86-64 machine. Some libraries use 128, because some Intel
CPUs prefetch adjacent lines in pairs. I only measured 64, on AMD.

**Q: How did you make sure the threads were really on different cores?**
**A:** I pinned them with `pthread_setaffinity_np` to CPUs 2 and 4. `lscpu` showed that CPUs
2k and 2k+1 are hyperthreads of the same physical core, and hyperthreads share L1, which would
hide transfers between cores.

### 4.13 Message format

- **Decision:** a tagged struct, `MsgType` plus an `Order`; 48 bytes.
- **Alternatives:** `std::variant<AddOrder, CancelOrder, ModifyOrder>`, a hand-written tagged
  union, or pointers into a shared pool.

**Q: Why not `std::variant`? It's more type-safe.**
**A:** The tagged struct is the same size, and dispatch is one `switch`. With `-Wall -Werror`,
`-Wswitch` makes a missing `case` a compile error, which is the same exhaustiveness check a
variant visitor gives. The variant's real advantage is that a cancel can't carry a
meaningless price. I judged that not worth the extra machinery (`std::visit` and the
overload-set trick) for three message kinds.

A pool of pointers would bring back memory management across threads, which a by-value queue
avoids.

**Q: 48 bytes doesn't fit a cache line evenly. Does it matter?**
**A:** Possibly; some messages straddle two lines. I haven't measured message size or alignment
effects.

### 4.14 Shutdown handshake

- **Decision:** a `Shutdown` message pushed through the queue after the last order.
- **Alternatives:** an `std::atomic<bool> done` flag, or a message count known in advance.

**Q: Why a message rather than a flag?**
**A:** The queue is first-in first-out, so every message pushed before `Shutdown` is applied
before the matching thread sees it. No extra atomic, and no extra ordering argument.

**Q: What's the bug you'd have to avoid with a flag?**
**A:** The matching thread can find the queue empty, then see `done`, while an order pushed just
before `done` was set is still waiting. The fix is to drain the queue once more after seeing
`done` and before exiting. That's safe because the acquire load of `done` pairs with the feed's
release store, which comes after its last push.

**Q: Downside of your choice?**
**A:** It can't be triggered from outside the feed thread, for example by a signal handler.

### 4.15 Pipeline self-check and the independent reference matcher

- **Decision:** two layers of checking.
  1. The engine replays its own input on one thread and compares the results.
  2. A separate Python matcher must produce identical trades.
- **Alternatives:** consistency checks only, or comparing against the C++ engine only.

**Q: Your self-check compares the engine with itself. What does that prove?**
**A:** Only that the threaded pipeline is lossless: no message lost, duplicated or reordered. It
would happily agree with a matching-logic bug, because both runs use the same `OrderBook`. A
version whose feed dropped one message failed it.

**Q: So how do you test the matching logic end to end?**
**A:** With `analyse.py`'s reference matcher, a second, deliberately plain implementation of
the rules in Python. Against two deliberately broken engines, on a 200k-message flow:

- **Self-trade prevention disabled:** the engine's self-check passed; the consistency checks
  found 4,500 self-trades.
- **Newest order filled first:** the engine's self-check *and* the consistency checks passed;
  only the reference matcher caught it, reporting the first differing trade.

That second case is why both layers exist. The consistency checks can't see time priority.

**Q: Couldn't both implementations share the same bug?**
**A:** Yes. If I misunderstood a rule, both would encode the same misunderstanding. The
structures differ (a deque with lazy deletion vs a list with handles), so they don't share
*implementation* bugs. *Specification* bugs are what the unit tests are for: they spell out
each rule as an explicit expected result.

**Q: Why lazy deletion in the Python matcher?**
**A:** Removing from the middle of a `deque` is O(n). Marking a cancelled entry dead and
dropping dead entries when they reach the front gives the same observable behaviour, as long
as the code looking up the best level skips levels that contain only dead entries, which it
does.

### 4.16 CSV for the tools

- **Decision:** CSV with a header row, for both the flow file and the trades file.
- **Alternatives:** JSON Lines, a single JSON array, or binary records.

**Q: Why not JSON?**
**A:** The C++ engine has to read the file. CSV needs no library: `std::getline` plus
`std::from_chars`. JSON would have needed something like nlohmann/json, the first dependency
outside the test framework, plus slower parsing and roughly three times the file size (my
estimate, not measured). The data is flat with a fixed set of fields, which suits CSV. Binary
was rejected because nobody can inspect it.

**Q: How robust is the parser?**
**A:** I tested these by hand; each prints the file and line and exits with 2:

- a wrong header;
- too few or too many fields;
- a bad side or order type;
- a negative quantity;
- trailing junk in a number;
- an unknown message type;
- an empty file;
- a missing file.

Windows line endings are accepted. There are no unit tests for the parser; these were manual
checks.

## 5. Testing and verification questions

**Q: How do you know there are no memory errors?**
**A:** Every test runs under ASan and UBSan in Debug, in CI.

- `UBSAN_OPTIONS=halt_on_error=1` is set; otherwise UBSan only prints a warning and the test
  still passes.
- The order book benchmark stream also ran under ASan/UBSan, for both containers.

**Q: Why are ASan and TSan separate jobs?**
**A:** Their runtimes can't be combined in one binary.

**Q: Did TSan just work?**
**A:** No. The TSan runtime crashes at startup under recent kernels' address-space
randomisation.

- `setarch -R` disables randomisation for one process.
- CMake's test discovery runs the test binary at build time, so a plain TSan build failed to
  *build*.
- `CMAKE_CROSSCOMPILING_EMULATOR="setarch;x86_64;-R"` makes CMake run every test binary through
  the wrapper, both during discovery and under `ctest`.
- When TSan finds a race, GoogleTest still prints PASSED, but TSan exits with code 66, so the
  test fails.

**Q: What does CI run?**
**A:** Three jobs on `ubuntu-24.04`:

1. **Release:** all tests; the pipeline with 1M messages; then generate, replay and analyse on
   100k messages.
2. **Debug + ASan/UBSan:** all tests.
3. **Debug + TSan:** the queue tests, plus the pipeline with 5k messages.

The runner is pinned rather than `ubuntu-latest`, because a newer GCC could add warnings that
fail `-Werror` without any change to the code.

**Q: How was the benchmark protected against measuring nothing?**
**A:** The op stream is generated before timing, by running it against a real book, so every
cancel and modify targets a live order. The replays consume every result into a checksum,
which is printed, so the calls can't be optimised away, and it must match across
implementations. Latency is per operation, with percentiles by nearest rank. Throughput is a
separate replay with no per-operation timers, taking the median of 5 runs.

**Q: Why not Google Benchmark?**
**A:** I needed per-operation latency percentiles over a stream of operations that depend on
the book's state. Google Benchmark reports a mean per iteration of an independent loop.

## 6. Weak spots to be ready for

These are true of the current code. Better to raise them yourself than to be caught by them.

- **Order IDs must increase.** IDs are unique for the book's lifetime. `add_order` rejects any
  ID at or below the highest one accepted so far, so an ID can't be reused after its order
  fills or is cancelled, and the trade log can never show two orders under one ID.
  - **The trade-off:** the caller must assign IDs in increasing order. That suits a venue
    that assigns its own order IDs, with clients' IDs mapped onto them at the gateway. It
    doesn't suit a book that takes IDs chosen by clients directly.
  - **The alternative I rejected:** a set of every ID ever seen. Callers could then use any
    order, but its memory grows without bound.
  - **Cost:** one comparison and 8 bytes of state.
  - **Corollary:** ID 0 can never be used, because the mark starts at 0.
- **No exception safety.** If an allocation throws partway through `add_order`, for example
  `orders_.emplace` after the order has been rested, the book is left inconsistent: an order
  rests with no index entry. The engine assumes allocation succeeds. Fixes would be to reserve
  up front, use a pool, or reorder the steps so the failure-prone one comes first.
- **Allocation on the hot path.** A list node per order, a map node per new level, a hash node
  per resting order, and a vector for any call that trades.
- **Some outcomes are silent.** Discarded market-order quantity and self-trade-prevention
  cancels are never reported explicitly.
- **No price validation.** Zero and negative ticks are accepted, and a market order's price is
  ignored.
- **Time priority within a level isn't checked by the invariant checker.** There's no
  timestamp. Priority is tested by the unit tests (`SamePriceFillsOldestFirst` and the modify
  tests) and end to end by the reference matcher.
- **The queue never destroys popped objects.** Slots are default-constructed and overwritten
  by assignment, so a popped value stays in its slot until it's overwritten or the queue is
  destroyed. That's fine for the trivially copyable `Message`; it would matter for a type that
  owns resources.
- **Busy-spinning burns two cores.** That's right for latency on dedicated hardware, and wrong
  on a shared machine. The alternatives are spinning then yielding, or a futex or condition
  variable for the idle case.
- **Backpressure.** When the queue fills, the feed spins. A real market-data feed can't wait,
  so it would need a larger buffer, conflation, or dropping with sequence-gap detection.
- **The pipeline's M/s figure is not a benchmark.** It includes starting the threads, recording
  trades into a vector, and a book that grows without bound with the built-in generator.
  Don't quote it next to `results/benchmarks.md`.
- **The CSV parser has no unit tests.** Its error paths were checked by hand.

### 6a. The three trade-offs, as out-loud answers

Three of the items above are deliberate engineering choices, not oversights. The goal when one
comes up is to show you reasoned about it and chose, not that you missed it. Lead with the
reasoning, name the condition under which you'd decide differently, and stop. Do not apologise.

**No exception safety.**

> The only places that can throw are the allocations: a `std::map` node when a new price level
> appears, a `std::list` node per order, and a hash-map node per resting order. In a
> single-threaded engine processing a trusted feed, a `bad_alloc` means the machine is out of
> memory, and there's no meaningful recovery from that in-process — the engine is done either
> way. So I didn't pay for the strong exception guarantee, because here it buys nothing: the
> complexity of making every operation roll back cleanly would protect against a failure mode
> that already means the process dies. If this were a library other code depended on, or if it
> had to survive allocation failure and keep serving, I'd add it — reserve the containers up
> front, or reorder each operation so the allocating step happens before any state is mutated,
> so a throw leaves the book untouched.

The interviewer is testing whether you *understand* exception safety, not whether you sprayed
`try`/`catch` everywhere. The strong answer reasons about the guarantee and the cost; the weak
answer is "I'd wrap it in try/catch."

If pushed on "so your book can be left inconsistent?" — agree plainly, and be specific: the one
window is an order resting in a level with no matching entry in the ID index, if the index
insert throws after the rest. Name the fix (do the index insert first, then rest), and note it's
a few lines. Knowing the exact window is the point.

**The queue never runs destructors on popped values.**

> The ring buffer's slots are default-constructed once, and a pop reads the value out and leaves
> the slot as-is until the next write overwrites it by assignment. That's correct for `Message`,
> which is trivially copyable and owns nothing — there's nothing to destroy. It's a documented
> constraint on `T`, not a leak: the queue requires a trivially copyable element type. If I
> needed it to carry a type that owns a resource — a buffer, a handle — I'd run the destructor
> on pop, or store the slots as raw storage and placement-new / explicit-destroy each element.
> For a message type that's deliberately a flat POD, the simpler design is the right one.

This is the cleanest of the three, because it's genuinely not a bug — it's a precondition on
`T` that `Message` satisfies. Say that it's a constraint, not a defect, and that you know what
would change the decision (a resource-owning element type).

If pushed on "how would you enforce the constraint?" — a `static_assert(std::is_trivially_
copyable_v<T>)` at the top of the queue makes the precondition a compile error instead of a
convention. If you haven't added one, say you'd add one; it's one line and it's the honest fix.

**The CSV parser's error paths were checked by hand, not by unit tests.**

The honest position depends on whether you've added the tests. If you have not, this is the
weakest of the three to defend, because the fix is cheap — the eight error paths (wrong header,
wrong field count, bad side, bad order type, negative quantity, trailing junk, unknown message
type, empty and missing file) are eight small tests. "I tested it by hand" is a thin answer when
a day's work closes it, and an interviewer who hears it may wonder what else was checked by hand.

> I wrote the parser defensively — it rejects a wrong header, a bad field count, an unknown side
> or type, a negative quantity, trailing junk, and an empty or missing file, each with the file
> and line number and a non-zero exit. I verified each path by hand. The gap I'd close first is
> turning those into unit tests: they're cheap and they'd belong in CI alongside the matcher.

If you *have* added the tests, this item leaves the weak-spots list entirely and becomes a
strength: an independent parser with full error-path coverage. That asymmetry — twenty minutes
of work turns a soft target into a point in your favour — is why the recommendation is to write
the tests rather than rehearse the answer.

## 7. Things not to claim

- **Any cache-miss explanation as fact.** Both the sorted-vector tail and the false-sharing
  result are consistent with the timings, but not confirmed with hardware counters.
- **That the vector wins on tiny books.** It's an expectation, not a measurement.
- **That `seq_cst` would be measurably slower here.** The codegen difference (`xchg` vs `mov`)
  is verified; the timing isn't.
- **Any latency figure without its context.** The ~25 ns of timer overhead is included, it was
  measured under WSL2, and the noise is about ±10%.
- **That 128-byte padding would or wouldn't help.** Only 64 was measured.
- **Production readiness.** It's one instrument, two order types, no persistence, recovery or
  networking, and a synthetic feed.
