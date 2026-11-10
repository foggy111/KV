#include "buffer.hpp"
#include "protocol.hpp"

#include <gtest/gtest.h>

#include <string>

using kv::Buffer;
using kv::Command;
using kv::ParseResult;
using kv::ProtocolParser;

namespace {

ParseResult parse_one(ProtocolParser& parser, Buffer& buf, Command* cmd, std::string* err) {
    return parser.try_parse(buf, cmd, err);
}

}  // namespace

TEST(Protocol, InlineSet) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("SET foo bar\r\n");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "SET");
    ASSERT_EQ(cmd.args.size(), 2u);
    EXPECT_EQ(cmd.args[0], "foo");
    EXPECT_EQ(cmd.args[1], "bar");
    EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(Protocol, InlineSetEx) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("set k v EX 2\n");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "SET");
    ASSERT_EQ(cmd.args.size(), 4u);
    EXPECT_EQ(cmd.args[2], "EX");
    EXPECT_EQ(cmd.args[3], "2");
}

TEST(Protocol, HalfPacket) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("GET he");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::NeedMore);
    EXPECT_EQ(buf.readable_bytes(), 6u);

    buf.append("llo\r");
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::NeedMore);

    buf.append("\n");
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "GET");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "hello");
    EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(Protocol, StickyTwoCommands) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("PING\r\nSET a b\r\n");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "PING");
    EXPECT_TRUE(cmd.args.empty());
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "SET");
    ASSERT_EQ(cmd.args.size(), 2u);
    EXPECT_EQ(cmd.args[0], "a");
    EXPECT_EQ(cmd.args[1], "b");
    EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(Protocol, EmptyLineDoesNotStallFollowingCommand) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("\r\nPING\r\n");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "PING");
    EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(Protocol, RespSet) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "SET");
    ASSERT_EQ(cmd.args.size(), 2u);
    EXPECT_EQ(cmd.args[0], "k");
    EXPECT_EQ(cmd.args[1], "v");
}

TEST(Protocol, RespHalfPacket) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("*2\r\n$3\r\nGET\r\n$3\r\nke");
    Command cmd;
    std::string err;
    size_t before = buf.readable_bytes();
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::NeedMore);
    EXPECT_EQ(buf.readable_bytes(), before);
    buf.append("y\r\n");
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "GET");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "key");
}

TEST(Protocol, RespStickyTwoCommands) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("*1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nb\r\n");
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "PING");
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Ok);
    EXPECT_EQ(cmd.name, "SET");
    EXPECT_EQ(cmd.args[1], "b");
    EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(Protocol, RespBadArgcDoesNotConsume) {
    ProtocolParser parser;
    Buffer buf;
    buf.append("*x\r\n");
    Command cmd;
    std::string err;
    size_t before = buf.readable_bytes();
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Error);
    EXPECT_EQ(buf.readable_bytes(), before);
    EXPECT_FALSE(err.empty());
}

TEST(Protocol, RespBulkMissingCrlf) {
    ProtocolParser parser;
    Buffer buf;
    // 长度声称 2，但数据后面不是 CRLF。
    buf.append("*1\r\n$2\r\nABXX");
    Command cmd;
    std::string err;
    size_t before = buf.readable_bytes();
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Error);
    EXPECT_EQ(err, "bulk missing CRLF");
    EXPECT_EQ(buf.readable_bytes(), before);
}

TEST(Protocol, InlineTooLong) {
    ProtocolParser parser;
    Buffer buf;
    buf.append(std::string(1024 * 1024 + 1, 'A'));
    Command cmd;
    std::string err;
    EXPECT_EQ(parse_one(parser, buf, &cmd, &err), ParseResult::Error);
    EXPECT_EQ(err, "inline command too long");
}

TEST(Protocol, Encode) {
    EXPECT_EQ(ProtocolParser::encode_simple("OK"), "+OK\r\n");
    EXPECT_EQ(ProtocolParser::encode_simple("PONG"), "+PONG\r\n");
    EXPECT_EQ(ProtocolParser::encode_error("nope"), "-ERR nope\r\n");
    EXPECT_EQ(ProtocolParser::encode_null_bulk(), "$-1\r\n");
    EXPECT_EQ(ProtocolParser::encode_bulk("hi"), "$2\r\nhi\r\n");
    EXPECT_EQ(ProtocolParser::encode_integer(-2), ":-2\r\n");
    EXPECT_EQ(ProtocolParser::encode_integer(1), ":1\r\n");
}
