#include "protocol.hpp"

#include <cctype>
#include <sstream>

namespace kv {

namespace {
std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split_ws(const std::string& line) {
    std::vector<std::string> parts;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) parts.push_back(tok);
    return parts;
}
}  // namespace

ParseResult ProtocolParser::try_parse(Buffer& buf, Command* out, std::string* err) {
    if (buf.readable_bytes() == 0) return ParseResult::NeedMore;

    // 以 '*' 开头走 RESP；否则走 inline 行协议
    if (buf.peek()[0] == '*') {
        return parse_resp(buf, out, err);
    }
    return parse_inline(buf, out, err);
}

ParseResult ProtocolParser::parse_inline(Buffer& buf, Command* out, std::string* err) {
    const char* eol = buf.find_eol();
    if (!eol) return ParseResult::NeedMore;

    size_t line_len = static_cast<size_t>(eol - buf.peek());
    // 去掉可选的 \r
    size_t content_len = line_len;
    if (content_len > 0 && buf.peek()[content_len - 1] == '\r') {
        --content_len;
    }

    std::string line(buf.peek(), content_len);
    buf.retrieve(line_len + 1);  // 含 '\n'

    if (line.empty()) return ParseResult::NeedMore;

    auto parts = split_ws(line);
    if (parts.empty()) {
        *err = "empty command";
        return ParseResult::Error;
    }

    out->name = to_upper(parts[0]);
    out->args.assign(parts.begin() + 1, parts.end());
    return ParseResult::Ok;
}

ParseResult ProtocolParser::parse_resp(Buffer& buf, Command* out, std::string* err) {
    // 最小 RESP 数组解析骨架：*N\r\n$len\r\n...\r\n ...
    // 教育 starter：能解析常见 SET/GET；边界情况留给学生补全。
    const char* crlf = buf.find_crlf();
    if (!crlf) return ParseResult::NeedMore;

    std::string header(buf.peek(), crlf);
    if (header.size() < 2 || header[0] != '*') {
        *err = "bad RESP array header";
        return ParseResult::Error;
    }

    int argc = 0;
    try {
        argc = std::stoi(header.substr(1));
    } catch (...) {
        *err = "bad argc";
        return ParseResult::Error;
    }
    if (argc <= 0 || argc > 64) {
        *err = "argc out of range";
        return ParseResult::Error;
    }

    // 先探测整条命令是否到齐，不到齐不消费
    size_t offset = static_cast<size_t>(crlf - buf.peek()) + 2;
    std::vector<std::string> parts;
    parts.reserve(argc);

    for (int i = 0; i < argc; ++i) {
        if (buf.readable_bytes() < offset + 1) return ParseResult::NeedMore;
        if (buf.peek()[offset] != '$') {
            *err = "expected bulk string";
            return ParseResult::Error;
        }
        const char* len_crlf = std::search(buf.peek() + offset, buf.peek() + buf.readable_bytes(),
                                           "\r\n", "\r\n" + 2);
        if (len_crlf == buf.peek() + buf.readable_bytes()) return ParseResult::NeedMore;

        int bulk_len = 0;
        try {
            bulk_len = std::stoi(std::string(buf.peek() + offset + 1,
                                             len_crlf - (buf.peek() + offset + 1)));
        } catch (...) {
            *err = "bad bulk len";
            return ParseResult::Error;
        }
        if (bulk_len < 0) {
            *err = "negative bulk len";
            return ParseResult::Error;
        }

        size_t data_start = static_cast<size_t>(len_crlf - buf.peek()) + 2;
        size_t need = data_start + static_cast<size_t>(bulk_len) + 2;
        if (buf.readable_bytes() < need) return ParseResult::NeedMore;

        parts.emplace_back(buf.peek() + data_start, bulk_len);
        offset = need;
    }

    // 全部到齐，一次性消费
    buf.retrieve(offset);
    out->name = to_upper(parts[0]);
    out->args.assign(parts.begin() + 1, parts.end());
    return ParseResult::Ok;
}

std::string ProtocolParser::encode_simple(const std::string& msg) {
    return "+" + msg + "\r\n";
}

std::string ProtocolParser::encode_error(const std::string& msg) {
    return "-ERR " + msg + "\r\n";
}

std::string ProtocolParser::encode_bulk(const std::string& msg) {
    return "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n";
}

std::string ProtocolParser::encode_null_bulk() {
    return "$-1\r\n";
}

std::string ProtocolParser::encode_integer(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

}  // namespace kv
