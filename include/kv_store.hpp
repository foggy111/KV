#pragma once

#include "common.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace kv {

struct KvEntry {
    std::string value;
    // 0 表示永不过期；否则为绝对过期时间点（steady_clock）
    std::chrono::steady_clock::time_point expire_at{};
    bool has_ttl{false};
};

// 单机 KV：unordered_map + 可选 TTL
// TODO(学生):
//   1) 真正的过期删除（惰性 + 定期）
//   2) 过期键的主动清理线程 / 时间轮
//   3) 更多数据结构（hash / list / zset）
class KvStore : noncopyable {
public:
    bool set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);

    // 找不到或已过期返回 nullopt
    std::optional<std::string> get(const std::string& key);

    bool del(const std::string& key);

    size_t size() const;

private:
    bool is_expired_unlocked(const KvEntry& e) const;
    // TODO(学生): 惰性删除钩子
    void lazy_expire_unlocked(const std::string& key);

    mutable std::mutex mu_;
    std::unordered_map<std::string, KvEntry> map_;
};

}  // namespace kv
