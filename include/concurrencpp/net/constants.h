#ifndef CONCURRENCPP_NET_CONSTANTS_H
#define CONCURRENCPP_NET_CONSTANTS_H

#include <cstdint>
#include <cstddef>
#include <chrono>

namespace concurrencpp::net::constants {

inline constexpr const char* default_address = "0.0.0.0";
inline constexpr std::uint16_t default_port = 8080;

// 会话读缓冲区大小（栈上缓冲区），与示例保持一致以提高局部性
inline constexpr std::size_t default_read_buffer_bytes = 4096; // 4KB

inline constexpr std::chrono::milliseconds default_header_read_timeout{5000};
inline constexpr std::chrono::milliseconds default_body_read_timeout{20000};
inline constexpr std::chrono::milliseconds default_write_timeout{5000};

// I/O 线程池默认线程数
inline constexpr std::size_t default_io_threads = 4;

// HTTP 层限额
inline constexpr std::size_t http_max_header_lines = 100;           // 头部最多行数
inline constexpr std::size_t http_max_header_bytes = 64 * 1024;      // 头部最大总字节数（约）
inline constexpr std::size_t http_max_body_bytes   = 8 * 1024 * 1024;// 正文最大字节数（8MB）
inline constexpr std::size_t default_max_requests_per_connection = 100; // 每连接最大请求数

// 读写超时（缺省值）
inline constexpr std::chrono::milliseconds default_header_read_timeout{5000}; // 5s
inline constexpr std::chrono::milliseconds default_body_read_timeout{20000};   // 20s
inline constexpr std::chrono::milliseconds default_write_timeout{5000};        // 5s

// Keep-Alive 参数（缺省值）
inline constexpr std::size_t default_keep_alive_timeout_seconds = 5; // 通常 5s

} // namespace concurrencpp::net::constants

#endif // CONCURRENCPP_NET_CONSTANTS_H
