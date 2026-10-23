#include "trz/pattern/bus/ringbuffer.hpp"

#include <array>
#include <cstddef>
#include "gtest/gtest.h"

namespace
{
template <class Queue> void checkSingleElementOperations(Queue &queue)
{
    int value = -1;
    ASSERT_FALSE(queue.dequeue(&value));
    EXPECT_EQ(-1, value);

    // Repeatedly fill and drain a capacity-four queue to exercise wraparound.
    for (int round = 0; round < 8; ++round)
    {
        SCOPED_TRACE(round);
        for (int i = 0; i < 4; ++i)
        {
            ASSERT_TRUE(queue.enqueue(round * 4 + i));
        }
        ASSERT_FALSE(queue.enqueue(-1));
        for (int i = 0; i < 4; ++i)
        {
            ASSERT_TRUE(queue.dequeue(&value));
            EXPECT_EQ(round * 4 + i, value);
        }
        ASSERT_FALSE(queue.dequeue(&value));
    }
}

template <class Queue> void checkBulkOperations(Queue &queue)
{
    const std::array<int, 5> input{{1, 2, 3, 4, 5}};
    std::array<int, 4> output{};

    EXPECT_EQ(std::size_t{0}, queue.dequeue(output.data(), output.size()));
    EXPECT_EQ(std::size_t{0}, queue.enqueue(input.data(), input.size()));
    ASSERT_EQ(std::size_t{4}, queue.template enqueue<false>(input.data(), input.size()));
    EXPECT_EQ(std::size_t{0}, queue.template enqueue<false>(input.data(), 1));

    ASSERT_EQ(std::size_t{2}, queue.dequeue(output.data(), 2));
    EXPECT_EQ(1, output[0]);
    EXPECT_EQ(2, output[1]);
    ASSERT_EQ(std::size_t{1}, queue.enqueue(input.data() + 4, 1));

    bool callbackCalled = false;
    const auto consume = [&](const int *values, std::size_t count)
    {
        callbackCalled = true;
        ASSERT_EQ(std::size_t{3}, count);
        EXPECT_EQ(3, values[0]);
        EXPECT_EQ(4, values[1]);
        EXPECT_EQ(5, values[2]);
    };
    EXPECT_EQ(std::size_t{3}, queue.dequeue(consume, output.data(), output.size()));
    EXPECT_TRUE(callbackCalled);

    callbackCalled = false;
    EXPECT_EQ(std::size_t{0}, queue.dequeue(consume, output.data(), output.size()));
    EXPECT_FALSE(callbackCalled);
}
} // namespace

TEST(RingBuffer, FixedCapacitySingleElement)
{
    xthreadbus::ringbuffer<int, 4> queue;
    checkSingleElementOperations(queue);
}

TEST(RingBuffer, DynamicCapacitySingleElement)
{
    xthreadbus::ringbuffer<int, 0> queue(4);
    checkSingleElementOperations(queue);
}

TEST(RingBuffer, FixedCapacityBulk)
{
    xthreadbus::ringbuffer<int, 4> queue;
    checkBulkOperations(queue);
}

TEST(RingBuffer, DynamicCapacityBulk)
{
    xthreadbus::ringbuffer<int, 0> queue(4);
    checkBulkOperations(queue);
}
