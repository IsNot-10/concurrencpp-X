#ifndef CONCURRENCPP_NET_SERVER_HPP
#define CONCURRENCPP_NET_SERVER_HPP

#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <optional>
#include <iostream>
#include "concurrencpp/concurrencpp.h"
#include "concurrencpp/net/asio.hpp"
#include "concurrencpp/net/io_context_pool.hpp"
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/net/constants.h"

namespace concurrencpp::net {

using asio::ip::tcp;

volatile std::sig_atomic_t stop = 0;

class tcp_server {
public:
    // 构造：创建 io_context 池与端口参数
    tcp_server(std::size_t pool_size, unsigned short port)
        : pool_(pool_size), port_(port) {}

    // 使用默认 I/O 线程数的构造（来自 constants::default_io_threads）
    explicit tcp_server(unsigned short port)
        : pool_(concurrencpp::net::constants::default_io_threads), port_(port) {}

    // 设置消息处理回调：用户可就地修改数据并返回新的长度
    // 回调签名：size_t(char* data, size_t length)
    // 要求：返回值不应超过输入缓冲区大小（4096），否则将被截断
    void set_on_message(std::function<std::size_t(char*, std::size_t)> cb) {
        on_message_ = std::move(cb);
    }

    void set_read_timeout(std::chrono::milliseconds timeout) { read_timeout_ = timeout; }
    void set_write_timeout(std::chrono::milliseconds timeout) { write_timeout_ = timeout; }
    void set_rw_timeout(std::chrono::milliseconds read_timeout, std::chrono::milliseconds write_timeout) {
        read_timeout_ = read_timeout;
        write_timeout_ = write_timeout;
    }

    concurrencpp::lazy_result<void> call(std::string host, std::string port){
        auto &io_context = pool_.get_client_io_context();
        asio::ip::tcp::socket socket(io_context);
        auto ec = co_await concurrencpp::net::async_connect(io_context, socket, host, port);
        if (ec) {
            std::cout << "Connect error: " << ec.message() << '\n';
            throw asio::system_error(ec);
        }
        std::cout << "Connect to " << host << ":" << port << " successfully.\n";
        const int max_length = 1024;
        char write_buf[max_length] = {"hello concurrencpp"};
        char read_buf[max_length];

        co_await concurrencpp::net::async_write(socket, asio::buffer(write_buf, max_length));
        auto [error, reply_length] = co_await concurrencpp::net::async_read_some(
            socket, asio::buffer(read_buf, max_length));
        if (error == asio::error::eof) {
            std::cout << "eof at message "<<'\n';
            co_return;
        } else if (error) {
            std::cout << "error: " << error.message()<< '\n';
            throw asio::system_error(error);
        }
        std::cout << "client will close.\n";
        std::error_code ignore_ec;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
        io_context.stop();
    }

    // 会话处理：简单回显
    concurrencpp::lazy_result<void> session(tcp::socket sock) {
        constexpr size_t max_length = 4096;
        char data[max_length]; // 栈上缓冲区，减少堆内存分配

        for (;;) {
            auto [error, length] = 
                co_await concurrencpp::net::async_read_some(sock, asio::buffer(data, max_length));
            
            if (error == asio::error::eof) {
                break; // 连接关闭，简洁退出
            } else if (error) {
                throw asio::system_error(error);
            }
            std::size_t out_len = length;
                if (on_message_) {
                    try {
                        out_len = on_message_(data, length);
                        if (out_len > max_length) {
                            out_len = max_length;
                        }
                    } catch (...) {
                        out_len = length;
                    }
                }
            

            co_await concurrencpp::net::async_write(sock, asio::buffer(data, length));
        }

        std::error_code ec;
        sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        sock.close(ec);
    }

    // 启动：运行 io_context 池并进入接受循环（阻塞直到关闭）
    concurrencpp::lazy_result<void> start() {
        // 运行 io_context 池
        pool_thread_ = std::thread([&,this] { pool_.run(); });
        auto& io_context = pool_.get_io_context();
        tcp::acceptor a(io_context, tcp::endpoint(tcp::v4(), port_));

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&,this](auto, auto) {
            std::error_code ec;
            std::cout << "Received signal, closing acceptor..." << std::endl;
            a.close(ec);
            stop = 1; // 设置标志变量
        });

        for (;;) {
            if (stop) {
                break;
            }

            auto& conn_io_context = pool_.get_io_context();
            tcp::socket socket(conn_io_context);

            auto error = co_await concurrencpp::net::async_accept(a, socket);
            if (error) {
                if (error == asio::error::operation_aborted) {
                    break; // 服务器关闭，退出循环
                }
                continue;
            }

            auto session_task = session(std::move(socket));
            auto result = session_task.run();
        }
    }

    // 析构：确保关闭与清理
    ~tcp_server() {
        pool_.stop();
        if (pool_thread_.joinable()) {
            pool_thread_.join();
        }
    }

private:
    io_context_pool pool_;
    unsigned short port_{concurrencpp::net::constants::default_port};
    std::chrono::milliseconds read_timeout_ { std::chrono::milliseconds(0) };
    std::chrono::milliseconds write_timeout_ { std::chrono::milliseconds(0) };

    std::thread pool_thread_;

    // 用户设定的消息处理回调（可选）
    std::function<std::size_t(char*, std::size_t)> on_message_{};
};

} // namespace concurrencpp::net

#endif