#include "async_logger.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    // 测试看断言，不看演示日志。服务器二进制本身的日志不受这里影响。
    kv::AsyncLogger::instance().set_min_level(kv::AsyncLogger::Level::ERROR);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
