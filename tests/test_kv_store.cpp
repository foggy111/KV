#include "kv_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(KvStore, SetGetDel) {
    kv::KvStore store;
    EXPECT_FALSE(store.get("missing").has_value());
    EXPECT_TRUE(store.set("a", "1"));
    ASSERT_TRUE(store.get("a").has_value());
    EXPECT_EQ(*store.get("a"), "1");
    EXPECT_TRUE(store.del("a"));
    EXPECT_FALSE(store.del("a"));
    EXPECT_FALSE(store.get("a").has_value());
    EXPECT_EQ(store.size(), 0u);
}

TEST(KvStore, SetWithoutTtlClearsOldTtl) {
    kv::KvStore store;
    store.set("a", "1", 5s);
    EXPECT_GT(store.ttl_seconds("a"), 0);
    store.set("a", "2");
    EXPECT_EQ(store.ttl_seconds("a"), -1);
    EXPECT_EQ(*store.get("a"), "2");
}

TEST(KvStore, LazyExpireOnGet) {
    kv::KvStore store;
    store.set("a", "1", 40ms);
    EXPECT_EQ(*store.get("a"), "1");
    std::this_thread::sleep_for(80ms);
    EXPECT_FALSE(store.get("a").has_value());
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.ttl_seconds("a"), -2);
}

TEST(KvStore, DelExpiredIsMiss) {
    kv::KvStore store;
    store.set("a", "1", 30ms);
    std::this_thread::sleep_for(60ms);
    EXPECT_FALSE(store.del("a"));
    EXPECT_EQ(store.size(), 0u);
}

TEST(KvStore, TtlSecondsFloor) {
    kv::KvStore store;
    store.set("a", "1", 2500ms);
    long long ttl = store.ttl_seconds("a");
    EXPECT_GE(ttl, 1);
    EXPECT_LE(ttl, 2);
    store.set("b", "2");
    EXPECT_EQ(store.ttl_seconds("b"), -1);
    EXPECT_EQ(store.ttl_seconds("missing"), -2);

    store.set("c", "3", 400ms);
    EXPECT_EQ(store.ttl_seconds("c"), 0);
    EXPECT_EQ(*store.get("c"), "3");
}

TEST(KvStore, ExpireZeroDeletesLiveKey) {
    kv::KvStore store;
    store.set("a", "1");
    EXPECT_TRUE(store.expire("a", 0ms));
    EXPECT_FALSE(store.get("a").has_value());
}

TEST(KvStore, ExpireMissingOrAlreadyExpired) {
    kv::KvStore store;
    EXPECT_FALSE(store.expire("nope", 1s));
    store.set("a", "1", 30ms);
    std::this_thread::sleep_for(60ms);
    EXPECT_FALSE(store.expire("a", 5s));
    EXPECT_EQ(store.size(), 0u);
}

TEST(KvStore, ExpireOverridesTtl) {
    kv::KvStore store;
    store.set("a", "1", 5s);
    EXPECT_TRUE(store.expire("a", 40ms));
    std::this_thread::sleep_for(80ms);
    EXPECT_FALSE(store.get("a").has_value());
}

TEST(KvStore, ActiveExpireRemovesWithoutGet) {
    kv::KvStore store;
    store.set("gone", "1", 30ms);
    store.set("stay", "2");
    std::this_thread::sleep_for(60ms);
    // 没有 GET。过期键还在表里，直到主动扫描。
    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.active_expire(16), 1u);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(*store.get("stay"), "2");
    EXPECT_FALSE(store.get("gone").has_value());
}

TEST(KvStore, ActiveExpireSamplesAndContinues) {
    kv::KvStore store;
    for (int i = 0; i < 10; ++i) {
        store.set("k" + std::to_string(i), "v", 20ms);
    }
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(store.size(), 10u);
    EXPECT_EQ(store.active_expire(3), 3u);
    EXPECT_EQ(store.size(), 7u);
    EXPECT_EQ(store.active_expire(100), 7u);
    EXPECT_EQ(store.size(), 0u);
}

TEST(KvStore, ActiveExpireSkipsLiveKeys) {
    kv::KvStore store;
    store.set("live", "ok", 5s);
    EXPECT_EQ(store.active_expire(8), 0u);
    EXPECT_EQ(*store.get("live"), "ok");
}

TEST(KvStore, ZeroTtlArgumentDoesNotExpire) {
    kv::KvStore store;
    store.set("a", "1", 0ms);
    EXPECT_EQ(store.ttl_seconds("a"), -1);
    EXPECT_EQ(store.active_expire(4), 0u);
    EXPECT_EQ(*store.get("a"), "1");
}
