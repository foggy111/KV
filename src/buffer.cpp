#include "buffer.hpp"

#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>

namespace kv {

ssize_t Buffer::read_fd(int fd, int* saved_errno) {
    // 栈上额外缓冲，减少系统调用次数（muduo 经典技巧）
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = writable_bytes();
    vec[0].iov_base = begin_write();
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *saved_errno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writer_index_ += n;
    } else {
        writer_index_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
}

}  // namespace kv
