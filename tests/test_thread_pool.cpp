#include "thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>

TEST(ThreadPool, RunsQueuedTasks) {
    kv::ThreadPool pool(2);
    std::atomic<int> n{0};
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(pool.enqueue([&n] { n.fetch_add(1); }));
    }
    pool.stop();
    EXPECT_EQ(n.load(), 20);
}

TEST(ThreadPool, EnqueueAfterStop) {
    kv::ThreadPool pool(1);
    pool.stop();
    EXPECT_FALSE(pool.enqueue([] {}));
}
