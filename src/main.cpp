#include <pthread.h>
#include <sched.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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

// Flow file: one message per line after this header. Fields a message type does not use
// are left empty: cancel uses only id; modify uses id, price and quantity.
constexpr std::string_view FLOW_HEADER = "type,id,side,price,quantity,order_type,owner";
constexpr std::size_t FLOW_FIELDS = 7;
constexpr std::string_view TRADES_HEADER = "message,aggressor_id,passive_id,price,quantity";

using Queue = SpscQueue<Message, QUEUE_CAPACITY>;

struct TradeRecord {
    std::size_t message;  // index of the message whose processing produced the trade
    Trade trade;

    bool operator==(const TradeRecord&) const = default;
};

struct Results {
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;  // invalid add, or cancel/modify of an order no longer resting
    std::vector<TradeRecord> trades;

    bool operator==(const Results&) const = default;
};

struct Options {
    std::size_t count = DEFAULT_MESSAGES;
    std::string input;   // empty: generate messages in-process
    std::string trades;  // empty: do not write trades
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

template <typename T>
bool parse_number(std::string_view field, T& out) {
    const char* end = field.data() + field.size();
    auto [parsed_to, error] = std::from_chars(field.data(), end, out);
    return error == std::errc{} && parsed_to == end;
}

std::optional<Message> parse_message(std::string_view line) {
    std::array<std::string_view, FLOW_FIELDS> fields;
    std::size_t count = 0;
    while (true) {
        if (count == FLOW_FIELDS) {
            return std::nullopt;
        }
        std::size_t comma = line.find(',');
        fields[count++] = line.substr(0, comma);
        if (comma == std::string_view::npos) {
            break;
        }
        line.remove_prefix(comma + 1);
    }
    if (count != FLOW_FIELDS) {
        return std::nullopt;
    }
    auto [type, id, side, price, quantity, order_type, owner] = fields;

    Message msg{};
    if (!parse_number(id, msg.order.id)) {
        return std::nullopt;
    }
    if (type == "cancel") {
        msg.type = MsgType::Cancel;
        return msg;
    }
    if (!parse_number(price, msg.order.price) || !parse_number(quantity, msg.order.quantity)) {
        return std::nullopt;
    }
    if (type == "modify") {
        msg.type = MsgType::Modify;
        return msg;
    }
    if (type != "add" || !parse_number(owner, msg.order.owner)) {
        return std::nullopt;
    }
    msg.type = MsgType::Add;
    if (side == "buy") {
        msg.order.side = Side::Buy;
    } else if (side == "sell") {
        msg.order.side = Side::Sell;
    } else {
        return std::nullopt;
    }
    if (order_type == "limit") {
        msg.order.type = OrderType::Limit;
    } else if (order_type == "market") {
        msg.order.type = OrderType::Market;
    } else {
        return std::nullopt;
    }
    return msg;
}

std::optional<std::vector<Message>> read_messages(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return std::nullopt;
    }
    std::vector<Message> messages;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        // Tolerate files written with Windows line endings
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line_number == 1) {
            if (line != FLOW_HEADER) {
                std::fprintf(stderr, "%s:1: expected header %s\n", path.c_str(),
                             std::string(FLOW_HEADER).c_str());
                return std::nullopt;
            }
            continue;
        }
        auto msg = parse_message(line);
        if (!msg) {
            std::fprintf(stderr, "%s:%zu: malformed message: %s\n", path.c_str(), line_number,
                         line.c_str());
            return std::nullopt;
        }
        messages.push_back(*msg);
    }
    if (line_number == 0) {
        std::fprintf(stderr, "%s: empty file\n", path.c_str());
        return std::nullopt;
    }
    return messages;
}

bool write_trades(const std::string& path, const std::vector<TradeRecord>& trades) {
    std::ofstream file(path);
    file << TRADES_HEADER << '\n';
    for (const auto& [message, trade] : trades) {
        file << message << ',' << trade.aggressor_id << ',' << trade.passive_id << ','
             << trade.price << ',' << trade.quantity << '\n';
    }
    file.close();
    return !file.fail();
}

void record(Results& results, std::size_t message,
            const std::optional<std::vector<Trade>>& trades) {
    if (!trades) {
        ++results.rejected;
        return;
    }
    ++results.applied;
    for (const Trade& trade : *trades) {
        results.trades.push_back({message, trade});
    }
}

// Returns false on Shutdown. No default case: -Wswitch makes a missing type a compile error.
bool apply(OrderBook& book, const Message& msg, std::size_t index, Results& results) {
    switch (msg.type) {
    case MsgType::Add:
        record(results, index, book.add_order(msg.order));
        return true;
    case MsgType::Cancel:
        if (book.cancel_order(msg.order.id)) {
            ++results.applied;
        } else {
            ++results.rejected;
        }
        return true;
    case MsgType::Modify:
        record(results, index,
               book.modify_order(msg.order.id, msg.order.price, msg.order.quantity));
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

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            options.input = argv[++i];
        } else if (arg == "--trades" && i + 1 < argc) {
            options.trades = argv[++i];
        } else if (!parse_number(arg, options.count)) {
            return std::nullopt;
        }
    }
    return options;
}

}  // namespace

// Exit codes: 0 success, 1 threaded run differs from the single-threaded replay,
// 2 bad arguments or file error.
int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    if (!options) {
        std::fprintf(stderr, "usage: %s [count] [--input flow.csv] [--trades trades.csv]\n",
                     argv[0]);
        return 2;
    }
#ifndef NDEBUG
    std::printf("Debug build: the invariant checker walks the whole book after every "
                "message, so keep the count small (e.g. 20000).\n");
#endif
    std::vector<Message> messages;
    if (options->input.empty()) {
        messages = generate_messages(options->count);
    } else {
        auto loaded = read_messages(options->input);
        if (!loaded) {
            return 2;
        }
        messages = std::move(*loaded);
    }

    auto queue = std::make_unique<Queue>();
    OrderBook book;
    Results results;

    auto start = std::chrono::steady_clock::now();
    std::thread matching([&] {
        pin_to_cpu(MATCHING_CPU);
        Message msg{};
        for (std::size_t index = 0;; ++index) {
            while (!queue->try_pop(msg)) {
            }
            if (!apply(book, msg, index, results)) {
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
    // reordering in the pipeline would change the trades or the final book
    OrderBook reference_book;
    Results reference;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        apply(reference_book, messages[index], index, reference);
    }
    bool matches = results == reference && book.order_count() == reference_book.order_count()
                   && book.best_bid() == reference_book.best_bid()
                   && book.best_ask() == reference_book.best_ask();

    Quantity volume = 0;
    for (const auto& entry : results.trades) {
        volume += entry.trade.quantity;
    }
    double seconds = std::chrono::duration<double>(end - start).count();
    std::printf("Messages:  %zu from %s (%.2f M/s through feed -> queue -> matching)\n",
                messages.size(), options->input.empty() ? "generator" : options->input.c_str(),
                messages.size() / seconds / 1e6);
    std::printf("Applied:   %llu\nRejected:  %llu\nTrades:    %zu\nVolume:    %llu\n",
                static_cast<unsigned long long>(results.applied),
                static_cast<unsigned long long>(results.rejected), results.trades.size(),
                static_cast<unsigned long long>(volume));
    std::printf("Resting:   %zu orders\n", book.order_count());
    std::printf("Matches single-threaded replay: %s\n", matches ? "yes" : "NO");

    if (!options->trades.empty() && !write_trades(options->trades, results.trades)) {
        std::fprintf(stderr, "cannot write %s\n", options->trades.c_str());
        return 2;
    }
    return matches ? 0 : 1;
}
