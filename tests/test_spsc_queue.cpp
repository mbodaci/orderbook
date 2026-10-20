#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "orderbook/spsc_queue.hpp"

using namespace orderbook;

TEST(SpscQueue, PopFromEmptyFails) {
    SpscQueue<int, 4> queue;
    int out = 0;
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(SpscQueue, PushFailsOnlyWhenFull) {
    SpscQueue<int, 4> queue;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(queue.try_push(i));
    }
    EXPECT_FALSE(queue.try_push(4));

    int out = 0;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_TRUE(queue.try_push(4));
}

TEST(SpscQueue, PopsInPushOrderAcrossWraparound) {
    SpscQueue<int, 4> queue;
    int next_push = 0;
    int next_pop = 0;
    // 3 in, 2 out per round: the indices pass the end of the buffer many times
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 3 && queue.try_push(next_push); ++i) {
            ++next_push;
        }
        for (int i = 0; i < 2; ++i) {
            int out = -1;
            ASSERT_TRUE(queue.try_pop(out));
            EXPECT_EQ(out, next_pop++);
        }
    }
}

// Two words, so a slot read before the producer's write is visible would show up as a
// mismatched pair, not just a wrong number.
struct Message {
    std::uint64_t seq;
    std::uint64_t check;
};

constexpr std::uint64_t check_for(std::uint64_t seq) {
    return seq * 0x9E3779B97F4A7C15ULL;
}

// The consumer expects exactly 0, 1, 2, ...: a lost message shows up as a jump forward,
// a duplicate as a repeat, and reordering as either. A small capacity keeps the queue
// switching between full and empty, which is where index bugs live.
TEST(SpscQueue, StressNoLossDuplicationOrReordering) {
    constexpr std::uint64_t MESSAGES = 2'000'000;
    SpscQueue<Message, 16> queue;

    std::thread producer([&] {
        for (std::uint64_t seq = 0; seq < MESSAGES; ++seq) {
            while (!queue.try_push({seq, check_for(seq)})) {
            }
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t first_bad_seq = 0;
    while (expected < MESSAGES) {
        Message message{};
        if (!queue.try_pop(message)) {
            continue;
        }
        if (message.seq != expected || message.check != check_for(message.seq)) {
            if (mismatches++ == 0) {
                first_bad_seq = expected;
            }
        }
        ++expected;
    }
    producer.join();

    EXPECT_EQ(mismatches, 0u) << "first mismatch at message " << first_bad_seq;
    Message leftover{};
    EXPECT_FALSE(queue.try_pop(leftover));
}
