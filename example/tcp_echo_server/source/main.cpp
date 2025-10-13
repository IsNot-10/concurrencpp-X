#include <iostream>
#include <string>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>

#include "concurrencpp/concurrencpp.h"

using concurrencpp::lazy_result;
namespace net = concurrencpp::net;

// 用户会话协程：回显示例，用户自行决定读写策略
lazy_result<void> echo_session(std::shared_ptr<asio::ip::tcp::socket> socket) {
    constexpr std::size_t max_length = net::constants::default_read_buffer_bytes;
    char data[max_length];

    for (;;) {
        auto [error, length] = co_await net::async_read_some(*socket, asio::buffer(data, max_length));
        if (error == asio::error::eof) {
            break; // 对端关闭
        } else if (error) {
            co_return; // 读错误
        }

        auto [werr, written] = co_await net::async_write(*socket, asio::buffer(data, length));
        (void)written;
        if (werr) {
            co_return; // 写错误
        }
    }

    std::error_code ec;
    socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket->close(ec);
}


std::atomic<bool> should_stop{false};
concurrencpp::net::tcp_server* server_ptr = nullptr;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived signal " << signal << ", stopping server..." << std::endl;
        should_stop = true;
        if (server_ptr) {
            server_ptr->stop();
        }
    }
}

int main(int argc, char** argv) {
    uint16_t port = concurrencpp::net::constants::default_port;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::cout << "Starting echo server on port " << port << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    try {
        net::tcp_server server(port);
        server_ptr = &server;

        // 设置信号处理
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        
        // 设置用户会话协程（读写逻辑由用户编写）并启动服务器
        server.set_session_handler(echo_session);
        server.start();
        
        // 等待停止信号
        while (!should_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // 停止服务器
        std::cout << "Server stopped." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}