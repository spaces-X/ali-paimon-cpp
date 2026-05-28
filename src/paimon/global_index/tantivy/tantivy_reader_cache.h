/*
 * Copyright 2026-present Alibaba Inc.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace paimon::tantivy {

class TantivyGlobalIndexReader;

/// Thread-safe LRU cache. Header-only so unit tests can specialize with
/// stub value types (e.g. shared_ptr<int>) instead of building real
/// TantivyGlobalIndexReader archives.
template <typename K, typename V>
class LruCache {
 public:
    explicit LruCache(std::size_t capacity = 256) : capacity_(capacity) {}

    /// Returns a default-constructed V (e.g. nullptr shared_ptr) on miss.
    V Lookup(const K& key) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = idx_.find(key);
        if (it == idx_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return V{};
        }
        lru_.splice(lru_.begin(), lru_, it->second);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second->second;
    }

    /// First inserter wins on race; later attempts no-op but bump LRU.
    void Insert(const K& key, V value) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = idx_.find(key);
        if (it != idx_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }
        lru_.emplace_front(key, std::move(value));
        idx_[key] = lru_.begin();
        while (lru_.size() > capacity_) {
            idx_.erase(lru_.back().first);
            lru_.pop_back();
        }
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> g(mu_);
        return lru_.size();
    }

    void SetCapacity(std::size_t n) {
        std::lock_guard<std::mutex> g(mu_);
        capacity_ = n;
        while (lru_.size() > capacity_) {
            idx_.erase(lru_.back().first);
            lru_.pop_back();
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> g(mu_);
        lru_.clear();
        idx_.clear();
    }

    std::uint64_t Hits() const { return hits_.load(std::memory_order_relaxed); }
    std::uint64_t Misses() const { return misses_.load(std::memory_order_relaxed); }

 private:
    mutable std::mutex mu_;
    std::list<std::pair<K, V>> lru_;
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> idx_;
    std::size_t capacity_;
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

/// Process-wide singleton cache for TantivyGlobalIndexReader, keyed by
/// archive file path. Paimon $global_index files are immutable per snapshot
/// so the path uniquely identifies content — no etag/mtime needed.
class TantivyReaderCache {
 public:
    static TantivyReaderCache& Instance();

    LruCache<std::string, std::shared_ptr<TantivyGlobalIndexReader>>& Lru() { return lru_; }

 private:
    TantivyReaderCache() = default;
    LruCache<std::string, std::shared_ptr<TantivyGlobalIndexReader>> lru_;
};

}  // namespace paimon::tantivy
