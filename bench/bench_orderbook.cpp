#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "orderbook/order_book.hpp"

using namespace orderbook;

namespace {

constexpr Price MID = 100'000;
constexpr std::size_t RESTING_TARGET = 10'000;
constexpr std::size_t MEASURED_OPS = 1'000'000;
constexpr int THROUGHPUT_RUNS = 5;

enum class OpKind : std::uint8_t { Add, Cancel, ModifyQuantity, ModifyPrice, Match };
constexpr std::size_t OP_KINDS = 5;
constexpr std::array<const char*, OP_KINDS> OP_NAMES = {
    "add", "cancel", "modify (qty down)", "modify (price)", "match (market)"};

struct Op {
    OpKind kind;
    Order order;  // Cancel uses only id; Modify uses id, price and quantity as the new values
};

struct Workload {
    const char* name;
    double depth_p;  // geometric parameter for distance from mid; mean distance ~ 1/p ticks
};

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

// Returns something derived from every result so the calls cannot be optimised away.
std::size_t apply(OrderBook& book, const Op& op) {
    switch (op.kind) {
    case OpKind::Add:
    case OpKind::Match: {
        auto trades = book.add_order(op.order);
        return trades ? trades->size() : 0;
    }
    case OpKind::Cancel:
        return book.cancel_order(op.order.id);
    case OpKind::ModifyQuantity:
    case OpKind::ModifyPrice: {
        auto trades = book.modify_order(op.order.id, op.order.price, op.order.quantity);
        return trades ? trades->size() : 0;
    }
    }
    return 0;
}

// Builds the op stream by running it against a real book, so every cancel and modify
// targets an order that is resting at that point. Replays on a fresh book then see
// exactly the same states, and none of this bookkeeping lands inside a timed region.
std::vector<Op> generate(double depth_p, std::size_t& prefill) {
    std::mt19937_64 rng(42);
    std::geometric_distribution<Price> distance(depth_p);
    std::uniform_int_distribution<Quantity> resting_qty(1, 100);
    std::uniform_int_distribution<Quantity> market_qty(1, 200);
    std::uniform_int_distribution<int> percent(0, 99);
    std::bernoulli_distribution coin;

    OrderBook book;
    std::vector<Op> ops;
    std::vector<Order> live;  // may still hold filled orders; pruned when picked
    OrderId next_id = 1;

    auto record = [&](const Op& op) {
        apply(book, op);
        ops.push_back(op);
    };
    // Bids strictly below MID and asks strictly above, so passive orders never cross
    auto passive_price = [&](Side side) {
        Price offset = 1 + distance(rng);
        return side == Side::Buy ? MID - offset : MID + offset;
    };
    auto pick_live = [&] {
        while (true) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t i = pick(rng);
            if (book.remaining_quantity(live[i].id)) {
                return i;
            }
            live[i] = live.back();
            live.pop_back();
        }
    };

    auto add = [&] {
        Side side = coin(rng) ? Side::Buy : Side::Sell;
        Order order{next_id++, side, passive_price(side), resting_qty(rng)};
        record({OpKind::Add, order});
        live.push_back(order);
    };
    auto cancel = [&] {
        std::size_t i = pick_live();
        record({OpKind::Cancel, live[i]});
        live[i] = live.back();
        live.pop_back();
    };
    auto modify = [&] {
        std::size_t i = pick_live();
        Order& order = live[i];
        Quantity quantity = *book.remaining_quantity(order.id);
        if (quantity > 1 && coin(rng)) {
            record({OpKind::ModifyQuantity, {order.id, order.side, order.price, quantity / 2}});
            return;
        }
        // A new price equal to the old one would take the in-place path instead
        Price price = order.price;
        while (price == order.price) {
            price = passive_price(order.side);
        }
        order.price = price;
        record({OpKind::ModifyPrice, {order.id, order.side, price, quantity}});
    };
    auto match = [&] {
        Side side = coin(rng) ? Side::Buy : Side::Sell;
        record({OpKind::Match, {next_id++, side, 0, market_qty(rng), OrderType::Market}});
    };

    while (book.order_count() < RESTING_TARGET) {
        add();
    }
    prefill = ops.size();

    // Mix: 50% add, 25% cancel, 15% modify, 10% match. Adds above the target become
    // cancels so the book stays near RESTING_TARGET instead of drifting.
    while (ops.size() - prefill < MEASURED_OPS) {
        int r = percent(rng);
        if (r < 50) {
            book.order_count() < RESTING_TARGET ? add() : cancel();
        } else if (r < 75) {
            cancel();
        } else if (r < 90) {
            modify();
        } else {
            match();
        }
    }
    return ops;
}

void replay_prefill(OrderBook& book, const std::vector<Op>& ops, std::size_t prefill) {
    for (std::size_t i = 0; i < prefill; ++i) {
        apply(book, ops[i]);
    }
}

double measure_throughput(const std::vector<Op>& ops, std::size_t prefill, std::size_t& sink) {
    std::vector<double> runs;
    for (int run = 0; run < THROUGHPUT_RUNS; ++run) {
        OrderBook book;
        replay_prefill(book, ops, prefill);
        auto start = Clock::now();
        for (std::size_t i = prefill; i < ops.size(); ++i) {
            sink += apply(book, ops[i]);
        }
        auto end = Clock::now();
        runs.push_back((ops.size() - prefill) * 1e9 / elapsed_ns(start, end));
    }
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

std::array<std::vector<std::uint64_t>, OP_KINDS> measure_latency(const std::vector<Op>& ops,
                                                                 std::size_t prefill,
                                                                 std::size_t& sink) {
    std::array<std::vector<std::uint64_t>, OP_KINDS> latencies;
    OrderBook book;
    replay_prefill(book, ops, prefill);
    for (std::size_t i = prefill; i < ops.size(); ++i) {
        auto start = Clock::now();
        sink += apply(book, ops[i]);
        auto end = Clock::now();
        latencies[static_cast<std::size_t>(ops[i].kind)].push_back(elapsed_ns(start, end));
    }
    return latencies;
}

// Nearest-rank percentile; permille = 500 for p50, 999 for p99.9. Input must be sorted.
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::size_t permille) {
    std::size_t rank = (permille * sorted.size() + 999) / 1000;
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

std::uint64_t clock_overhead_ns() {
    std::vector<std::uint64_t> samples(100'000);
    for (auto& sample : samples) {
        auto start = Clock::now();
        auto end = Clock::now();
        sample = elapsed_ns(start, end);
    }
    std::sort(samples.begin(), samples.end());
    return percentile(samples, 500);
}

void run(const Workload& workload) {
    std::size_t prefill = 0;
    std::vector<Op> ops = generate(workload.depth_p, prefill);
    std::size_t sink = 0;

    // Throughput runs first: they also warm caches and the allocator for the latency run
    double ops_per_sec = measure_throughput(ops, prefill, sink);
    auto latencies = measure_latency(ops, prefill, sink);

    std::printf("\n### %s (p = %.3f)\n\n", workload.name, workload.depth_p);
    std::printf("| op | count | p50 ns | p99 ns | p99.9 ns |\n|---|---:|---:|---:|---:|\n");
    for (std::size_t kind = 0; kind < OP_KINDS; ++kind) {
        auto& samples = latencies[kind];
        std::sort(samples.begin(), samples.end());
        std::printf("| %s | %zu | %llu | %llu | %llu |\n", OP_NAMES[kind], samples.size(),
                    static_cast<unsigned long long>(percentile(samples, 500)),
                    static_cast<unsigned long long>(percentile(samples, 990)),
                    static_cast<unsigned long long>(percentile(samples, 999)));
    }
    std::printf("\nThroughput: %.2f M ops/s (median of %d runs, %zu ops each)\n",
                ops_per_sec / 1e6, THROUGHPUT_RUNS, ops.size() - prefill);
    // Must match across containers: same ops on a correct book produce the same results
    std::printf("Checksum: %zu\n", sink);
}

}  // namespace

int main() {
#ifndef NDEBUG
    std::printf("WARNING: assertions enabled; the invariant checker makes these numbers "
                "meaningless. Build with -DCMAKE_BUILD_TYPE=Release.\n");
#endif
    std::printf("Clock overhead p50: %llu ns (included in every latency sample)\n",
                static_cast<unsigned long long>(clock_overhead_ns()));
    run({"Tight book: activity concentrated near the touch", 0.2});
    run({"Wide book: orders spread over hundreds of ticks", 0.005});
}
