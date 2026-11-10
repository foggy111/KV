#pragma once

#include "buffer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace kv {

// 解析结果
struct Command {
    std::string name;               // 大写：SET / GET / DEL / PING ...
    std::vector<std::string> args;
};

enum class ParseResult {
    Ok,          // 解析出一条完整命令
    NeedMore,    // 半包，等更多数据
    Error,       // 协议错误
};

// Redis-like 简易协议（子集，不是完整 RESP）：
//   1) 行协议:  SET key value\r\n  /  SET key value EX 2\r\n
//   2) RESP 数组：仅支持顶层 *N + bulk string（$len）。不支持嵌套数组、
//      简单字符串请求、整块协议错误后的自动重同步。
//
// 粘包/半包：数据不够时返回 NeedMore，且不消费已读字节。
// Error：不消费字节。调用方必须停下来（本项目选择关闭连接），
// 不能原样再调一次，否则会在同一段坏数据上忙循环。
class ProtocolParser {
public:
    // 尝试从 buf 解析一条命令；成功时会 retrieve 已消费字节。
    // 空行会被跳过并继续看下一条。
    ParseResult try_parse(Buffer& buf, Command* out, std::string* err);

    // 编码响应（简易行协议 / RESP 简单字符串）
    static std::string encode_simple(const std::string& msg);   // +OK\r\n
    static std::string encode_error(const std::string& msg);    // -ERR ...\r\n
    static std::string encode_bulk(const std::string& msg);     // $n\r\n...\r\n
    static std::string encode_null_bulk();                      // $-1\r\n
    static std::string encode_integer(long long n);             // :n\r\n

private:
    ParseResult parse_inline(Buffer& buf, Command* out, std::string* err);
    // TODO(学生): 实现完整 RESP 数组解析（以 '*' 开头）
    ParseResult parse_resp(Buffer& buf, Command* out, std::string* err);
};

}  // namespace kv
