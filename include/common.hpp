#pragma once

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>

// 公共工具：错误检查宏、不可拷贝基类等

namespace kv {

inline void throw_sys_error(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

// 禁止拷贝，允许移动（Reactor 中的 fd 持有者常用）
class noncopyable {
public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;

protected:
    noncopyable() = default;
    ~noncopyable() = default;
};

}  // namespace kv
