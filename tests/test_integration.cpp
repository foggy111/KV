#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>

using kvtest::Client;
using kvtest::TestServer;

TEST(Integration, PingSetGetDel) {
    TestServer server;
    Client client(server.port());
    EXPECT_EQ(client.command({"PING"}), "+PONG\r\n");
    EXPECT_EQ(client.command({"PING", "hi"}), "$2\r\nhi\r\n");
    EXPECT_EQ(client.command({"SET", "hello", "world"}), "+OK\r\n");
    EXPECT_EQ(client.command({"GET", "hello"}), "$5\r\nworld\r\n");
    EXPECT_EQ(client.command({"TTL", "hello"}), ":-1\r\n");
    EXPECT_EQ(client.command({"DEL", "hello"}), ":1\r\n");
    EXPECT_EQ(client.command({"DEL", "hello"}), ":0\r\n");
    EXPECT_EQ(client.command({"GET", "hello"}), "$-1\r\n");
    EXPECT_EQ(client.command({"DBSIZE"}), ":0\r\n");
}

TEST(Integration, StickyWriteAndByteAtATime) {
    TestServer server;
    Client client(server.port());
    client.write_raw("PING\r\nSET a b\r\nGET a\r\n");
    EXPECT_EQ(client.read_one(), "+PONG\r\n");
    EXPECT_EQ(client.read_one(), "+OK\r\n");
    EXPECT_EQ(client.read_one(), "$1\r\nb\r\n");

    std::string raw = "PING\r\nGET missing\r\n";
    for (char c : raw) {
        ASSERT_EQ(::write(client.fd(), &c, 1), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(client.read_one(), "+PONG\r\n");
    EXPECT_EQ(client.read_one(), "$-1\r\n");
}

TEST(Integration, HalfPacketThenComplete) {
    TestServer server;
    Client client(server.port());
    client.write_raw("PIN");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    client.write_raw("G\r\n");
    EXPECT_EQ(client.read_one(), "+PONG\r\n");
}

TEST(Integration, SetExThenGetMissAndDbsizeDrops) {
    TestServer server;
    Client client(server.port());
    EXPECT_EQ(client.command({"SET", "temp", "soon", "EX", "1"}), "+OK\r\n");
    EXPECT_EQ(client.command({"SET", "keep", "yes"}), "+OK\r\n");
    EXPECT_EQ(client.command({"GET", "temp"}), "$4\r\nsoon\r\n");
    std::string ttl = client.command({"TTL", "temp"});
    EXPECT_TRUE(ttl == ":1\r\n" || ttl == ":0\r\n") << ttl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    // 先看 DBSIZE：主动扫描应已经删掉 temp。若只有惰性删除，这里仍是 2。
    EXPECT_EQ(client.command({"DBSIZE"}), ":1\r\n");
    EXPECT_EQ(client.command({"GET", "temp"}), "$-1\r\n");
    EXPECT_EQ(client.command({"GET", "keep"}), "$3\r\nyes\r\n");
    EXPECT_EQ(client.command({"TTL", "temp"}), ":-2\r\n");
}

TEST(Integration, ExpireCommand) {
    TestServer server;
    Client client(server.port());
    EXPECT_EQ(client.command({"EXPIRE", "missing", "1"}), ":0\r\n");
    EXPECT_EQ(client.command({"SET", "p", "v"}), "+OK\r\n");
    EXPECT_EQ(client.command({"EXPIRE", "p", "1"}), ":1\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(client.command({"DBSIZE"}), ":0\r\n");
    EXPECT_EQ(client.command({"GET", "p"}), "$-1\r\n");
}

TEST(Integration, RejectBadExpire) {
    TestServer server;
    Client client(server.port());
    EXPECT_EQ(client.command({"SET", "a", "b", "EX", "0"}),
              "-ERR expire seconds out of range\r\n");
    EXPECT_EQ(client.command({"SET", "a", "b", "PX", "10"}),
              "-ERR unsupported SET option\r\n");
    EXPECT_EQ(client.command({"SET", "a", "b"}), "+OK\r\n");
    EXPECT_EQ(client.command({"EXPIRE", "a", "-1"}),
              "-ERR expire seconds must be an integer\r\n");
    EXPECT_EQ(client.command({"GET", "a"}), "$1\r\nb\r\n");
}

TEST(Integration, UnknownAndArity) {
    TestServer server;
    Client client(server.port());
    EXPECT_EQ(client.command({"FOO"}), "-ERR unknown command 'FOO'\r\n");
    EXPECT_EQ(client.command({"GET"}), "-ERR wrong number of arguments for GET\r\n");
    EXPECT_EQ(client.command({"SET", "onlykey"}),
              "-ERR wrong number of arguments for SET\r\n");
}

TEST(Integration, ProtocolErrorCloses) {
    TestServer server;
    Client client(server.port());
    client.write_raw("*x\r\n");
    EXPECT_EQ(client.read_one(), "-ERR bad argc\r\n");
    EXPECT_TRUE(client.read_eof());
}

TEST(Integration, QuitStopsAccepting) {
    TestServer server;
    uint16_t port = server.port();
    {
        Client client(port);
        EXPECT_EQ(client.command({"PING"}), "+PONG\r\n");
    }
    server.stop();
    EXPECT_THROW(Client{port}, std::runtime_error);
}
