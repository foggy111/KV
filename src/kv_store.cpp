#include "kv_store.hpp"

namespace kv {

bool KvStore::is_expired_unlocked(const KvEntry& e) const {
    if (!e.has_ttl) return false;
    return std::chrono::steady_clock::now() >= e.expire_at;
}

void KvStore::lazy_expire_unlocked(const std::string& key) {
    auto it = map_.find(key);
    if (it != map_.end() && is_expired_unlocked(it->second)) {
        map_.erase(it);
    }
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
    lazy_expire_unlocked(key);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second.value;
}

bool KvStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.erase(key) > 0;
}

size_t KvStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
}

}  // namespace kv
