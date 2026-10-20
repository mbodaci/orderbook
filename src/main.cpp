#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/spsc_queue.hpp"

using namespace orderbook;

namespace {

enum class MsgType : std::uint8_t { Add, Cancel, Modify, Shutdown };

struct Message {
    MsgType type;
    Order order;  // Cancel uses only id; Modify uses id, price and quantity as the new values
};

constexpr std::size_t QUEUE_CAPACITY = 1024;
constexpr std::size_t DEFAULT_MESSAGES = 1'000'000;
constexpr std::uint64_t SEED = 42;
constexpr Price MID = 100'000;
// Separate physical cores on a machine where CPUs 2k and 2k+1 are hyperthread siblings
constexpr int FEED_CPU = 2;
constexpr int MATCHING_CPU = 4;

using Queue = SpscQueue<Message, QUEUE_CAPACITY>;

struct Stats {
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;  // invalid add, or cancel/modify of an order no longer resting
    std::uint64_t trades = 0;
    std::uint64_t volume = 0;

    bool operator==(const Stats&) const = default;
};

// The feed cannot see the book, so some cancels and modifies target orders that have
// already filled. The book rejects those, as a real venue would.
std::vector<Message> generate_messages(std::size_t count) {
    std::mt19937_64 rng(SEED);
    std::geometric_distribution<Price> distance(0.2);
    std::uniform_int_distribution<Quantity> quantity(1, 100);
    std::uniform_int_distribution<int> percent(0, 99);
    std::bernoulli_distribution coin;

    std::vector<Message> messages;
    messages.reserve(count);
    std::vector<Order> live;  // limit orders added and not yet cancelled by the feed
    OrderId next_id = 1;

    // Bids below MID and asks above, so limit orders rest and trades come from market orders
    auto passive_price = [&](Side side) {
        Price offset = 1 + distance(rng);
        return side == Side::Buy ? MID - offset : MID + offset;
    };

    // Mix: 50% limit add, 25% cancel, 15% modify, 10% market order
    while (messages.size() < count) {
        int r = percent(rng);
        Side side = coin(rng) ? Side::Buy : Side::Sell;
        if (r < 50 || live.empty()) {
            Order order{next_id++, side, passive_price(side), quantity(rng)};
            messages.push_back({MsgType::Add, order});
            live.push_back(order);
        } else if (r < 90) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t i = pick(rng);
            if (r < 75) {
                messages.push_back({MsgType::Cancel, live[i]});
                live[i] = live.back();
                live.pop_back();
            } else {
                live[i].price = passive_price(live[i].side);
                live[i].quantity = quantity(rng);
                messages.push_back({MsgType::Modify, live[i]});
            }
        } else {
            Order order{next_id++, side, 0, quantity(rng), OrderType::Market};
            messages.push_back({MsgType::Add, order});
        }
    }
    return messages;
}

void record(Stats& stats, const std::optional<std::vector<Trade>>& trades) {
    if (!trades) {
        ++stats.rejected;
        return;
    }
    ++stats.applied;
    stats.trades += trades->size();
    for (const Trade& trade : *trades) {
        stats.volume += trade.quantity;
    }
}

// Returns false on Shutdown. No default case: -Wswitch makes a missing type a compile error.
bool apply(OrderBook& book, const Message& msg, Stats& stats) {
    switch (msg.type) {
    case MsgType::Add:
        record(stats, book.add_order(msg.order));
        return true;
    case MsgType::Cancel:
        if (book.cancel_order(msg.order.id)) {
            ++stats.applied;
        } else {
            ++stats.rejected;
        }
        return true;
    case MsgType::Modify:
        record(stats, book.modify_order(msg.order.id, msg.order.price, msg.order.quantity));
        return true;
    case MsgType::Shutdown:
        return false;
    }
    return false;
}

// Best effort: on a machine without these CPUs the threads just run unpinned
void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "warning: could not pin to CPU %d\n", cpu);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : DEFAULT_MESSAGES;
#ifndef NDEBUG
    std::printf("Debug build: the invariant checker walks the whole book after every "
                "message, so keep the count small (e.g. 20000).\n");
#endif
    std::vector<Message> messages = generate_messages(count);

    auto queue = std::make_unique<Queue>();
    OrderBook book;
    Stats stats;

    auto start = std::chrono::steady_clock::now();
    std::thread matching([&] {
        pin_to_cpu(MATCHING_CPU);
        Message msg{};
        while (true) {
            if (!queue->try_pop(msg)) {
                continue;
            }
            if (!apply(book, msg, stats)) {
                break;
            }
        }
    });
    std::thread feed([&] {
        pin_to_cpu(FEED_CPU);
        for (const Message& msg : messages) {
            while (!queue->try_push(msg)) {
            }
        }
        // FIFO: everything pushed above is applied before the matching thread sees this
        while (!queue->try_push({MsgType::Shutdown, {}})) {
        }
    });
    feed.join();
    matching.join();
    auto end = std::chrono::steady_clock::now();

    // Same messages applied on one thread with no queue: any loss, duplication or
    // reordering in the pipeline would change the result
    OrderBook reference_book;
    Stats reference;
    for (const Message& msg : messages) {
        apply(reference_book, msg, reference);
    }
    bool matches = stats == reference && book.order_count() == reference_book.order_count()
                   && book.best_bid() == reference_book.best_bid()
                   && book.best_ask() == reference_book.best_ask();

    double seconds = std::chrono::duration<double>(end - start).count();
    std::printf("Messages:  %zu (%.2f M/s through feed -> queue -> matching)\n", count,
                count / seconds / 1e6);
    std::printf("Applied:   %llu\nRejected:  %llu\nTrades:    %llu\nVolume:    %llu\n",
                static_cast<unsigned long long>(stats.applied),
                static_cast<unsigned long long>(stats.rejected),
                static_cast<unsigned long long>(stats.trades),
                static_cast<unsigned long long>(stats.volume));
    std::printf("Resting:   %zu orders\n", book.order_count());
    std::printf("Matches single-threaded replay: %s\n", matches ? "yes" : "NO");
    return matches ? 0 : 1;
}
