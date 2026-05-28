/*
 * Copyright 2026-present Alibaba Inc.
 *
 * Licensed under the Apache License, Version 2.0.
 *
 * L1: covers LRU mechanics (lookup/insert/evict/capacity-change) +
 * counter accounting + concurrent insert dedup. Real
 * TantivyGlobalIndexReader binding is integration-tested via
 * tantivy_reader_test.
 */

#include "paimon/global_index/tantivy/tantivy_reader_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace paimon::tantivy {

using IntCache = LruCache<std::string, std::shared_ptr<int>>;

TEST(LruCacheTest, LookupOnEmptyReturnsNullAndCountsMiss) {
    IntCache c(4);
    EXPECT_EQ(c.Lookup("k"), nullptr);
    EXPECT_EQ(c.Misses(), 1u);
    EXPECT_EQ(c.Hits(), 0u);
    EXPECT_EQ(c.Size(), 0u);
}

TEST(LruCacheTest, InsertThenLookupHits) {
    IntCache c(4);
    auto v = std::make_shared<int>(42);
    c.Insert("k", v);
    auto got = c.Lookup("k");
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(*got, 42);
    EXPECT_EQ(c.Hits(), 1u);
    EXPECT_EQ(c.Misses(), 0u);
    EXPECT_EQ(c.Size(), 1u);
}

TEST(LruCacheTest, DuplicateInsertKeepsFirstValueBumpLru) {
    IntCache c(4);
    auto v1 = std::make_shared<int>(1);
    auto v2 = std::make_shared<int>(2);
    c.Insert("k", v1);
    c.Insert("k", v2);  // first wins
    EXPECT_EQ(*c.Lookup("k"), 1);
    EXPECT_EQ(c.Size(), 1u);
}

TEST(LruCacheTest, EvictsLeastRecentlyUsedAtCapacity) {
    IntCache c(2);
    c.Insert("a", std::make_shared<int>(1));
    c.Insert("b", std::make_shared<int>(2));
    c.Insert("c", std::make_shared<int>(3));  // evict "a"
    EXPECT_EQ(c.Lookup("a"), nullptr);
    EXPECT_NE(c.Lookup("b"), nullptr);
    EXPECT_NE(c.Lookup("c"), nullptr);
}

TEST(LruCacheTest, LookupBumpsRecency) {
    IntCache c(2);
    c.Insert("a", std::make_shared<int>(1));
    c.Insert("b", std::make_shared<int>(2));
    (void)c.Lookup("a");                       // "a" is now newest
    c.Insert("c", std::make_shared<int>(3));   // evicts "b" (LRU), not "a"
    EXPECT_NE(c.Lookup("a"), nullptr);
    EXPECT_EQ(c.Lookup("b"), nullptr);
}

TEST(LruCacheTest, SetCapacityShrinkEvictsLruFirst) {
    IntCache c(4);
    c.Insert("a", std::make_shared<int>(1));
    c.Insert("b", std::make_shared<int>(2));
    c.Insert("c", std::make_shared<int>(3));
    c.Insert("d", std::make_shared<int>(4));
    c.SetCapacity(2);
    EXPECT_EQ(c.Size(), 2u);
    EXPECT_NE(c.Lookup("c"), nullptr);
    EXPECT_NE(c.Lookup("d"), nullptr);
    EXPECT_EQ(c.Lookup("a"), nullptr);
    EXPECT_EQ(c.Lookup("b"), nullptr);
}

TEST(LruCacheTest, ClearWipesAllEntries) {
    IntCache c(4);
    c.Insert("a", std::make_shared<int>(1));
    c.Insert("b", std::make_shared<int>(2));
    c.Clear();
    EXPECT_EQ(c.Size(), 0u);
    EXPECT_EQ(c.Lookup("a"), nullptr);
}

TEST(LruCacheTest, ConcurrentInsertSameKeyKeepsOneEntry) {
    IntCache c(8);
    constexpr int kThreads = 16;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] { c.Insert("shared", std::make_shared<int>(i)); });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(c.Size(), 1u);
    EXPECT_NE(c.Lookup("shared"), nullptr);
}

TEST(LruCacheTest, ConcurrentLookupOnSameEntryAllHit) {
    IntCache c(8);
    c.Insert("k", std::make_shared<int>(99));
    constexpr int kThreads = 32;
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            auto v = c.Lookup("k");
            if (v && *v == 99) ok.fetch_add(1);
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(ok.load(), kThreads);
    EXPECT_EQ(c.Hits(), static_cast<uint64_t>(kThreads));
}

TEST(TantivyReaderCacheSingletonTest, InstanceReturnsSameObject) {
    auto& a = TantivyReaderCache::Instance();
    auto& b = TantivyReaderCache::Instance();
    EXPECT_EQ(&a, &b);
}

}  // namespace paimon::tantivy
