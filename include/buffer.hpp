#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace kv {

// 应用层缓冲：处理 TCP 粘包 / 半包
// 布局: [ prependable | readable | writable ]
class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize = 1024;

    explicit Buffer(size_t initial = kInitialSize)
        : buffer_(kCheapPrepend + initial),
          reader_index_(kCheapPrepend),
          writer_index_(kCheapPrepend) {}

    size_t readable_bytes() const { return writer_index_ - reader_index_; }
    size_t writable_bytes() const { return buffer_.size() - writer_index_; }
    size_t prependable_bytes() const { return reader_index_; }

    const char* peek() const { return begin() + reader_index_; }

    void retrieve(size_t len) {
        if (len < readable_bytes()) {
            reader_index_ += len;
        } else {
            retrieve_all();
        }
    }

    void retrieve_all() {
        reader_index_ = kCheapPrepend;
        writer_index_ = kCheapPrepend;
    }

    std::string retrieve_as_string(size_t len) {
        len = std::min(len, readable_bytes());
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    std::string retrieve_all_as_string() {
        return retrieve_as_string(readable_bytes());
    }

    void append(const char* data, size_t len) {
        ensure_writable(len);
        std::memcpy(begin_write(), data, len);
        writer_index_ += len;
    }

    void append(const std::string& s) { append(s.data(), s.size()); }

    ssize_t read_fd(int fd, int* saved_errno);

    // 查找 CRLF，用于行协议解析；找不到返回 nullptr
    const char* find_crlf() const {
        static const char kCRLF[] = "\r\n";
        const char* crlf = std::search(peek(), begin_write(), kCRLF, kCRLF + 2);
        return crlf == begin_write() ? nullptr : crlf;
    }

    // 查找单个 '\n'（兼容简单行协议）
    const char* find_eol() const {
        const void* p = memchr(peek(), '\n', readable_bytes());
        return static_cast<const char*>(p);
    }

private:
    char* begin() { return buffer_.data(); }
    const char* begin() const { return buffer_.data(); }
    char* begin_write() { return begin() + writer_index_; }
    const char* begin_write() const { return begin() + writer_index_; }

    void ensure_writable(size_t len) {
        if (writable_bytes() < len) {
            make_space(len);
        }
    }

    void make_space(size_t len) {
        if (writable_bytes() + prependable_bytes() < len + kCheapPrepend) {
            buffer_.resize(writer_index_ + len);
        } else {
            // 把可读数据挪到前面腾出空间
            size_t readable = readable_bytes();
            std::copy(begin() + reader_index_, begin() + writer_index_,
                      begin() + kCheapPrepend);
            reader_index_ = kCheapPrepend;
            writer_index_ = reader_index_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t reader_index_;
    size_t writer_index_;
};

}  // namespace kv
