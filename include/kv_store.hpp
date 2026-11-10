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

// 单机 KV：std::unordered_map（对应 PPT 里的 HashMap）+ TTL。
// 过期策略：
//   1) 惰性删除：GET / DEL / EXPIRE / TTL 碰到已过期键时立刻删掉
//   2) 主动删除：调用方定期执行 active_expire()，从游标起抽样扫描
// 时间轮、hash/list/zset 没有实现。
// map_ 有互斥锁，因此将来可以从别的线程调用；当前服务器仍在事件循环线程里调用。
class KvStore : noncopyable {
public:
    // ttl 为空或 <=0 表示不过期。SET 会覆盖旧值和旧 TTL。
    bool set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);

    // 找不到或已过期返回 nullopt（已过期则惰性删除）
    std::optional<std::string> get(const std::string& key);

    // 只删除尚未过期的键。已过期视为不存在，返回 false，并把键清掉。
    bool del(const std::string& key);

    // ttl <= 0：键存在且未过期则立即删除并返回 true。
    // 键不存在或已过期：返回 false（已过期的会先清掉）。
    bool expire(const std::string& key, std::chrono::milliseconds ttl);

    // -2 不存在或已过期；-1 存在但没有 TTL；>=0 剩余整秒（向下取整）。
    long long ttl_seconds(const std::string& key);

    // 从内部游标起最多检查 max_keys 个键，删除其中已过期的。返回删除个数。
    // max_keys 小于当前键数时，其余过期键留到后续调用。这不是精确时间轮。
    size_t active_expire(size_t max_keys);

    // 字典里的条目数，包含尚未被惰性或主动扫描清掉的过期键。
    size_t size() const;

private:
    bool is_expired_unlocked(const KvEntry& e) const;
    // 若 key 已过期则删除并返回 true。调用方必须已持有 mu_。
    bool lazy_expire_unlocked(const std::string& key);

    mutable std::mutex mu_;
    std::unordered_map<std::string, KvEntry> map_;
    std::string expire_cursor_;
};

}  // namespace kv
