#ifndef CONCURRENCPP_NET_CLIENT_HPP
#define CONCURRENCPP_NET_CLIENT_HPP

#include <string>
#include <iostream>
#include <chrono>
#include "concurrencpp/results/lazy_result.h"
#include "concurrencpp/net/asio.hpp"
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/runtime/io_context_pool.hpp"

namespace concurrencpp::net {

using asio::ip::tcp;

class tcp_client {
public:

    explicit tcp_client(io_context_pool& pool) : pool_(&pool) {}

    concurrencpp::lazy_result<std::string> call(std::string host, std::string port, std::string message) {
        auto& io_context = pool_->get_client_io_context();
        tcp::socket socket(io_context);

        asio::steady_timer timer(io_context);
        constexpr auto timeout = std::chrono::seconds(10);
        timer.expires_after(timeout);
        timer.async_wait([&socket](const std::error_code& ec) {
            if (!ec) {
                std::error_code ignore_ec;
                socket.cancel(ignore_ec);
            }
        });

        
            auto ec = co_await async_connect(io_context, socket, host, port);
            if (ec) {
                if (ec == asio::error::operation_aborted) {
                    throw asio::system_error(asio::error::timed_out);
                }
                throw asio::system_error(ec);
            }
        

        
            auto [wec, wlen] = co_await async_write(socket, asio::buffer(message));
            (void)wlen;
            if (wec) {
                if (wec == asio::error::operation_aborted) {
                    throw asio::system_error(asio::error::timed_out);
                }
                throw asio::system_error(wec);
            }
        

        std::string response;
        asio::streambuf read_buf;
        for (;;) {
            auto prepared = read_buf.prepare(4096);
            auto [rec, rlen] = co_await async_read_some(socket, prepared);
            read_buf.commit(rlen);

            if (rec) {
                if (rec == asio::error::eof) {
                    auto buffers = read_buf.data();
                    response.append(asio::buffers_begin(buffers), asio::buffers_end(buffers));
                    break;
                }
                if (rec == asio::error::operation_aborted) {
                    throw asio::system_error(asio::error::timed_out);
                }
                throw asio::system_error(rec);
            }

            auto buffers = read_buf.data();
            const std::string chunk(asio::buffers_begin(buffers), asio::buffers_end(buffers));
            if (!chunk.empty()) {
                response.append(chunk);
                read_buf.consume(chunk.size());
            }
        }

        std::error_code ignore_timer_ec;
        timer.cancel();

        std::error_code ignore_ec;
        socket.shutdown(tcp::socket::shutdown_both, ignore_ec);
        socket.close(ignore_ec);
        co_return response;
    }

private:
    io_context_pool* pool_;
};

}

#endif
