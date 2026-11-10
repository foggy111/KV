#include "buffer.hpp"
#include "protocol.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

TEST(Buffer, AppendAndRetrieve) {
    kv::Buffer buf;
    buf.append("abcdef");
    EXPECT_EQ(buf.readable_bytes(), 6u);
    EXPECT_EQ(buf.retrieve_as_string(3), "abc");
    EXPECT_EQ(buf.retrieve_all_as_string(), "def");
    EXPECT_EQ(buf.readable_bytes(), 0u);
}

TEST(Buffer, CrlfAppearsAfterSecondAppend) {
    kv::Buffer buf;
    buf.append("SET a b\r");
    EXPECT_EQ(buf.find_crlf(), nullptr);
    EXPECT_EQ(buf.find_eol(), nullptr);
    buf.append("\nGET k\r\n");
    ASSERT_NE(buf.find_crlf(), nullptr);
    kv::ProtocolParser parser;
    kv::Command cmd;
    std::string err;
    EXPECT_EQ(parser.try_parse(buf, &cmd, &err), kv::ParseResult::Ok);
    EXPECT_EQ(cmd.name, "SET");
    EXPECT_EQ(parser.try_parse(buf, &cmd, &err), kv::ParseResult::Ok);
    EXPECT_EQ(cmd.name, "GET");
    EXPECT_EQ(cmd.args.at(0), "k");
}

TEST(Buffer, CompactKeepsUnreadBytes) {
    kv::Buffer buf(8);
    buf.append("01234567");
    buf.retrieve(4);
    buf.append("ABCD");
    EXPECT_EQ(buf.retrieve_all_as_string(), "4567ABCD");
}

TEST(Buffer, ResizeKeepsBytes) {
    kv::Buffer buf(8);
    std::string payload(20, 'x');
    buf.append(payload);
    EXPECT_EQ(buf.retrieve_all_as_string(), payload);
}

TEST(Buffer, ReadFdHalfThenRest) {
    int sv[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    kv::Buffer buf(4);
    ASSERT_EQ(::write(sv[1], "abc", 3), 3);
    int err = 0;
    ASSERT_EQ(buf.read_fd(sv[0], &err), 3);
    EXPECT_EQ(buf.retrieve_all_as_string(), "abc");

    ASSERT_EQ(::write(sv[1], "def", 3), 3);
    ASSERT_EQ(buf.read_fd(sv[0], &err), 3);
    EXPECT_EQ(buf.retrieve_all_as_string(), "def");

    std::string sticky = "PING\r\nGET k\r\n";
    ASSERT_EQ(::write(sv[1], sticky.data(), sticky.size()), static_cast<ssize_t>(sticky.size()));
    ASSERT_EQ(buf.read_fd(sv[0], &err), static_cast<ssize_t>(sticky.size()));

    kv::ProtocolParser parser;
    kv::Command cmd;
    std::string perr;
    EXPECT_EQ(parser.try_parse(buf, &cmd, &perr), kv::ParseResult::Ok);
    EXPECT_EQ(cmd.name, "PING");
    EXPECT_EQ(parser.try_parse(buf, &cmd, &perr), kv::ParseResult::Ok);
    EXPECT_EQ(cmd.name, "GET");
    EXPECT_EQ(buf.readable_bytes(), 0u);

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(Buffer, ReadFdUsesExtraBufferWhenWritableIsSmall) {
    int sv[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    std::string payload(100, 'z');
    ASSERT_EQ(::write(sv[1], payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

    kv::Buffer buf(4);
    int err = 0;
    ASSERT_EQ(buf.read_fd(sv[0], &err), static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(buf.retrieve_all_as_string(), payload);
    ::close(sv[0]);
    ::close(sv[1]);
}
