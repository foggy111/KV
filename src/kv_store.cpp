#include "kv_store.hpp"
#include "async_logger.hpp"

#include <vector>

namespace kv {

bool KvStore::is_expired_unlocked(const KvEntry& e) const {
    if (!e.has_ttl) return false;
    return std::chrono::steady_clock::now() >= e.expire_at;
}

bool KvStore::lazy_expire_unlocked(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end() || !is_expired_unlocked(it->second)) return false;
    map_.erase(it);
    LOG_INFO("TTL 惰性删除 键=", key);
    return true;
}

bool KvStore::set(const std::string& key, const std::string& value,
                  std::optional<std::chrono::milliseconds> ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    KvEntry entry;
    entry.value = value;
    if (ttl && ttl->count() > 0) {
        entry.has_ttl = true;
        entry.expire_at = std::chrono::steady_clock::now() + *ttl;
    }
    map_[key] = std::move(entry);
    return true;
}

std::optional<std::string> KvStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (lazy_expire_unlocked(key)) return std::nullopt;
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second.value;
}

bool KvStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    if (is_expired_unlocked(it->second)) {
        map_.erase(it);
        LOG_INFO("TTL 惰性删除 键=", key);
        return false;
    }
    map_.erase(it);
    return true;
}

bool KvStore::expire(const std::string& key, std::chrono::milliseconds ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    if (is_expired_unlocked(it->second)) {
        map_.erase(it);
        LOG_INFO("TTL 惰性删除 键=", key);
        return false;
    }
    if (ttl.count() <= 0) {
        map_.erase(it);
        return true;
    }
    it->second.has_ttl = true;
    it->second.expire_at = std::chrono::steady_clock::now() + ttl;
    return true;
}

long long KvStore::ttl_seconds(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return -2;
    if (is_expired_unlocked(it->second)) {
        map_.erase(it);
        LOG_INFO("TTL 惰性删除 键=", key);
        return -2;
    }
    if (!it->second.has_ttl) return -1;
    auto remain = std::chrono::duration_cast<std::chrono::seconds>(
        it->second.expire_at - std::chrono::steady_clock::now());
    if (remain.count() < 0) return 0;
    return remain.count();
}

size_t KvStore::active_expire(size_t max_keys) {
    std::vector<std::string> removed;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (map_.empty() || max_keys == 0) {
            expire_cursor_.clear();
            return 0;
        }

        auto it = map_.find(expire_cursor_);
        if (it == map_.end()) it = map_.begin();

        size_t scanned = 0;
        while (scanned < max_keys && !map_.empty()) {
            if (it == map_.end()) it = map_.begin();
            ++scanned;
            if (is_expired_unlocked(it->second)) {
                removed.push_back(it->first);
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
        if (it == map_.end() && !map_.empty()) it = map_.begin();
        expire_cursor_ = (it == map_.end()) ? std::string{} : it->first;
    }
    for (const auto& key : removed) {
        LOG_INFO("TTL 主动删除 键=", key);
    }
    return removed.size();
}

size_t KvStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
}

}  // namespace kv
