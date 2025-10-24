#ifndef CONCURRENCPP_NET_SERVER_HPP
#define CONCURRENCPP_NET_SERVER_HPP

#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <optional>
#include <iostream>

#include "concurrencpp/results/lazy_result.h"
#include "concurrencpp/net/asio.hpp"
#include "concurrencpp/runtime/io_context_pool.hpp"
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/net/constants.h"

namespace concurrencpp::net {

using asio::ip::tcp;

class tcp_server {
public:

    tcp_server(io_context_pool& pool, unsigned short port)
        : pool_(pool), port_(port) {}

    void set_on_message(std::function<std::size_t(char*, std::size_t)> cb) {
        on_message_ = std::move(cb);
    }

    concurrencpp::lazy_result<void> session(tcp::socket sock) {
        asio::streambuf buffer;

        for (;;) {
            auto prepared = buffer.prepare(4096);
            auto [error, length] =
                co_await concurrencpp::net::async_read_some(sock, prepared);

            if (error == asio::error::eof) {
                break;
            } else if (error) {
                throw asio::system_error(error);
            }

            buffer.commit(length);

            std::vector<char> data(length);
            auto readable = buffer.data();
            asio::buffer_copy(asio::buffer(data.data(), data.size()), readable);
            std::size_t out_len = length;
            if (on_message_) {
                try {
                    out_len = on_message_(data.data(), length);
                    if (out_len > data.size()) {
                        out_len = data.size();
                    }
                } catch (...) {
                    out_len = length;
                }
            }

            co_await concurrencpp::net::async_write(sock, asio::buffer(data.data(), out_len));
            buffer.consume(length);
        }

        std::error_code ec;
        sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        sock.close(ec);
    }

    concurrencpp::lazy_result<void> start() {
        auto& io_context = pool_.get_io_context();
        tcp::acceptor a(io_context, tcp::endpoint(tcp::v4(), port_));

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&,this](auto, auto) {
            std::error_code ec;
            std::cout << "Received signal, closing acceptor..." << std::endl;
            a.close(ec);
        });

        for (;;) {
            auto& conn_io_context = pool_.get_io_context();
            tcp::socket socket(conn_io_context);

            auto error = co_await concurrencpp::net::async_accept(a, socket);
            if (error) {
                if (error == asio::error::operation_aborted) {
                    break;
                }
                continue;
            }

            auto session_task = session(std::move(socket));
            auto result = session_task.run();
        }
    }

    ~tcp_server() = default;

private:
    io_context_pool& pool_;
    unsigned short port_{concurrencpp::net::constants::default_port};

    std::function<std::size_t(char*, std::size_t)> on_message_{};
};

}

#endif
