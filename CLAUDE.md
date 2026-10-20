# CLAUDE.md — Order Book Project

## What this is
A single-instrument C++ limit order book and matching engine, built as a portfolio
project for trading-firm internship applications. Every design choice must survive
an interviewer digging into it.

## Hard rule
I must be able to explain every line aloud without notes. Do not write code I
cannot defend. Prefer small guided pieces over large paste-in blocks.

## Build
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Debug + sanitizers
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build .
```

## Compiler flags
Always compile with `-Wall -Wextra -Werror`. These are set in CMakeLists.txt.

## Testing
Tests use GoogleTest, fetched at configure time via CMake FetchContent.
Run with: `cd build && ctest --output-on-failure`

## Code style

### Comments
Sparse and purposeful. Follow these rules:
- **Section markers** for logical blocks inside functions: `// Match incoming order against resting orders`
- **Why-comments** when the reason is not obvious from the code: `// acquire: pairs with producer's release on write_idx`
- **No comments** on self-explanatory lines. `orders_.erase(id);` does not need a comment.
- **No Doxygen or docstrings.** Function names and parameter names should be clear enough.
- **No per-line narration.** If a block of five lines each has a comment, something is wrong.
- **No commented-out code** in committed files. Use Git history.

### Naming
- Types: `PascalCase` (`OrderBook`, `PriceLevel`)
- Functions and methods: `snake_case` (`add_order`, `cancel_order`)
- Member variables: trailing underscore (`orders_`, `bids_`)
- Constants: `UPPER_SNAKE` (`MAX_PRICE`, `QUEUE_CAPACITY`)

### Formatting
- Braces on same line for functions and control flow
- 4-space indentation, no tabs
- Max line length: 100 characters

## Scope control
If I am adding something not in the spec, push back. Features are cut before
tests, benchmarks or the README.

## Repo layout
```
orderbook/
  README.md
  CLAUDE.md
  CMakeLists.txt
  include/orderbook/     order.hpp, price_level.hpp, order_book.hpp, spsc_queue.hpp
  src/                   order_book.cpp, main.cpp
  tests/                 test_matching.cpp, test_edge_cases.cpp, test_spsc_queue.cpp
  bench/                 bench_orderbook.cpp, bench_spsc.cpp
  tools/                 generate_flow.py, replay.py, analyse.py
  results/               benchmarks.md, flamegraph.svg
  .github/workflows/ci.yml
```

## What is out of scope (and say so in the README)
- Multiple instruments
- Iceberg, stop, and other order types
- Persistence, recovery, networking, GUI
