#ifndef CONCURRENCPP_NET_CONSTANTS_H
#define CONCURRENCPP_NET_CONSTANTS_H

#include <cstdint>
#include <cstddef>

namespace concurrencpp::net::constants {

// 默认监听地址与端口
inline constexpr const char* default_address = "0.0.0.0";
inline constexpr std::uint16_t default_port = 8080;

// 默认监听队列长度（不依赖asio头，统一常量）
inline constexpr int default_backlog = 1024;

// 套接字默认选项
inline constexpr bool default_reuse_address = true;
inline constexpr bool default_tcp_no_delay = true;
inline constexpr bool default_keep_alive = true;

// 会话读缓冲区大小（栈上缓冲区），与示例保持一致以提高局部性
inline constexpr std::size_t default_read_buffer_bytes = 4096; // 4KB

// I/O 线程池默认线程数
inline constexpr std::size_t default_io_threads = 4;

} // namespace concurrencpp::net::constants

#endif // CONCURRENCPP_NET_CONSTANTS_H