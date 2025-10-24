#ifndef ASYNC_SIMPLE_HTTP_SERVER_HPP
#define ASYNC_SIMPLE_HTTP_SERVER_HPP

#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <optional>
#include <iostream>

#include "concurrencpp/results/lazy_result.h"
#include "connection.hpp"
#include "concurrencpp/net/asio.hpp"
#include "concurrencpp/runtime/io_context_pool.hpp"
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/net/constants.h"

namespace concurrencpp::net {

using asio::ip::tcp;

class http_server {
public:

    http_server(io_context_pool& pool, unsigned short port)
        : pool_(pool), port_(port) {}

    concurrencpp::lazy_result<void> start_one(asio::ip::tcp::socket socket) {
        connection conn(std::move(socket), "./");
        co_await conn.start();
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

            auto task = start_one(std::move(socket));
            task.run();
        }
    }

    ~http_server() = default;

private:
    io_context_pool& pool_;
    unsigned short port_{concurrencpp::net::constants::default_port};
};

}

#endif
