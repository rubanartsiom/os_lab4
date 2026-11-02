#include "SharedQueue.h"
#include <gtest/gtest.h>

TEST(MessageValidation, AcceptsMessageShorterThanTwentyCharacters)
{
    EXPECT_TRUE(MessageQueue::IsValidMessage(L"Hello"));
}

TEST(MessageValidation, RejectsTwentyCharacters)
{
    EXPECT_FALSE(MessageQueue::IsValidMessage(L"12345678901234567890"));
}

TEST(HeaderValidation, AcceptsNewQueueHeader)
{
    const auto header = MessageQueue::MakeInitialHeader(5);

    EXPECT_TRUE(MessageQueue::IsValidHeader(header));
    EXPECT_EQ(header.capacity, 5u);
    EXPECT_EQ(header.head, 0u);
    EXPECT_EQ(header.tail, 0u);
    EXPECT_EQ(header.count, 0u);
}

TEST(CircularQueue, MovesIndexToZeroAfterLastRecord)
{
    EXPECT_EQ(MessageQueue::NextIndex(0, 3), 1u);
    EXPECT_EQ(MessageQueue::NextIndex(1, 3), 2u);
    EXPECT_EQ(MessageQueue::NextIndex(2, 3), 0u);
}

TEST(HeaderValidation, RejectsCountGreaterThanCapacity)
{
    auto header = MessageQueue::MakeInitialHeader(3);
    header.count = 4;

    EXPECT_FALSE(MessageQueue::IsValidHeader(header));
}
