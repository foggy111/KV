// 独立 assert 测试（无需 GoogleTest）
#include "buffer.hpp"
#include "protocol.hpp"

#include <cassert>
#include <iostream>
#include <string>

using kv::Buffer;
using kv::Command;
using kv::ParseResult;
using kv::ProtocolParser;

static void expect_ok(ProtocolParser& p, Buffer& buf, const std::string& name, size_t argc) {
    Command cmd;
    std::string err;
    auto r = p.try_parse(buf, &cmd, &err);
    assert(r == ParseResult::Ok);
    assert(cmd.name == name);
    assert(cmd.args.size() == argc);
    (void)cmd;
}

int main() {
    ProtocolParser parser;

    // 1) 完整行协议
    {
        Buffer buf;
        buf.append("SET foo bar\r\n");
        expect_ok(parser, buf, "SET", 2);
        assert(buf.readable_bytes() == 0);
    }

    // 2) 半包：先半行，再补齐
    {
        Buffer buf;
        buf.append("GET he");
        Command cmd;
        std::string err;
        assert(parser.try_parse(buf, &cmd, &err) == ParseResult::NeedMore);
        buf.append("llo\n");
        assert(parser.try_parse(buf, &cmd, &err) == ParseResult::Ok);
        assert(cmd.name == "GET");
        assert(cmd.args.size() == 1);
        assert(cmd.args[0] == "hello");
    }

    // 3) 粘包：一次两条
    {
        Buffer buf;
        buf.append("PING\r\nSET a b\r\n");
        expect_ok(parser, buf, "PING", 0);
        expect_ok(parser, buf, "SET", 2);
        assert(buf.readable_bytes() == 0);
    }

    // 4) RESP-lite
    {
        Buffer buf;
        buf.append("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
        Command cmd;
        std::string err;
        assert(parser.try_parse(buf, &cmd, &err) == ParseResult::Ok);
        assert(cmd.name == "SET");
        assert(cmd.args.size() == 2);
        assert(cmd.args[0] == "k");
        assert(cmd.args[1] == "v");
    }

    // 5) RESP 半包
    {
        Buffer buf;
        buf.append("*2\r\n$3\r\nGET\r\n$3\r\nke");
        Command cmd;
        std::string err;
        assert(parser.try_parse(buf, &cmd, &err) == ParseResult::NeedMore);
        buf.append("y\r\n");
        assert(parser.try_parse(buf, &cmd, &err) == ParseResult::Ok);
        assert(cmd.name == "GET");
        assert(cmd.args[0] == "key");
    }

    // 6) 编码
    assert(ProtocolParser::encode_simple("OK") == "+OK\r\n");
    assert(ProtocolParser::encode_null_bulk() == "$-1\r\n");
    assert(ProtocolParser::encode_bulk("hi") == "$2\r\nhi\r\n");

    std::cout << "All protocol tests passed.\n";
    return 0;
}
