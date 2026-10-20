#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "orderbook/spsc_queue.hpp"

using namespace orderbook;

namespace {

constexpr std::uint64_t MESSAGES = 50'000'000;
constexpr std::size_t CAPACITY = 1024;
constexpr int ROUNDS = 7;

void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "failed to pin to CPU %d\n", cpu);
        std::exit(1);
    }
}

// Returns messages per second. The consumer sums what it receives so the work cannot be
// optimised away, and the sum is checked against the closed form.
template <bool Padded>
double run_once(int producer_cpu, int consumer_cpu) {
    // Heap-allocated: the padded queue is 8 KiB plus padding, and alignment is honoured by new
    auto queue = std::make_unique<SpscQueue<std::uint64_t, CAPACITY, Padded>>();

    std::thread producer([&] {
        pin_to_cpu(producer_cpu);
        for (std::uint64_t i = 0; i < MESSAGES; ++i) {
            while (!queue->try_push(i)) {
            }
        }
    });

    pin_to_cpu(consumer_cpu);
    auto start = std::chrono::steady_clock::now();
    std::uint64_t sum = 0;
    for (std::uint64_t received = 0; received < MESSAGES;) {
        std::uint64_t value = 0;
        if (queue->try_pop(value)) {
            sum += value;
            ++received;
        }
    }
    auto end = std::chrono::steady_clock::now();
    producer.join();

    if (sum != MESSAGES * (MESSAGES - 1) / 2) {
        std::fprintf(stderr, "checksum mismatch: messages lost or duplicated\n");
        std::exit(1);
    }
    double seconds = std::chrono::duration<double>(end - start).count();
    return MESSAGES / seconds;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    // Default CPUs 2 and 4: separate physical cores on a machine where 2k and 2k+1 are
    // hyperthread siblings. Siblings share L1, which would hide cross-core line transfers.
    int producer_cpu = argc > 2 ? std::atoi(argv[1]) : 2;
    int consumer_cpu = argc > 2 ? std::atoi(argv[2]) : 4;

    std::printf("SpscQueue<uint64_t, %zu>, %llu messages per run, producer CPU %d, "
                "consumer CPU %d\n",
                CAPACITY, static_cast<unsigned long long>(MESSAGES), producer_cpu,
                consumer_cpu);
    std::printf("sizeof padded = %zu, unpadded = %zu\n",
                sizeof(SpscQueue<std::uint64_t, CAPACITY, true>),
                sizeof(SpscQueue<std::uint64_t, CAPACITY, false>));

    // Alternate the two layouts so drift in machine state hits both equally
    std::vector<double> padded;
    std::vector<double> unpadded;
    for (int round = 0; round < ROUNDS; ++round) {
        padded.push_back(run_once<true>(producer_cpu, consumer_cpu));
        unpadded.push_back(run_once<false>(producer_cpu, consumer_cpu));
    }

    auto print = [](const char* name, const std::vector<double>& runs) {
        auto sorted = runs;
        std::sort(sorted.begin(), sorted.end());
        std::printf("%-9s median %7.1f M msgs/s   min %7.1f   max %7.1f\n", name,
                    median(runs) / 1e6, sorted.front() / 1e6, sorted.back() / 1e6);
    };
    print("padded", padded);
    print("unpadded", unpadded);
    std::printf("padded / unpadded = %.2fx\n", median(padded) / median(unpadded));
}
