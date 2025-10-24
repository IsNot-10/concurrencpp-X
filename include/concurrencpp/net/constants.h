#ifndef CONCURRENCPP_NET_CONSTANTS_H
#define CONCURRENCPP_NET_CONSTANTS_H

#include <cstdint>
#include <cstddef>

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

} // namespace concurrencpp::net::constants

#endif // CONCURRENCPP_NET_CONSTANTS_H
