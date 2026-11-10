#include "event_loop.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

TEST(EventLoop, RunEveryFiresUntilQuit) {
    std::atomic<int> hits{0};
    std::promise<kv::EventLoop*> ready;
    std::thread thread([&] {
        kv::EventLoop loop;
        loop.run_every(std::chrono::milliseconds(20), [&] { hits.fetch_add(1); });
        ready.set_value(&loop);
        loop.loop();
    });

    kv::EventLoop* loop = ready.get_future().get();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    loop->quit();
    thread.join();
    EXPECT_GE(hits.load(), 4);
}
