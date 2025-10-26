#ifndef ASYNC_SIMPLE_HTTP_SERVER_HPP
#define ASYNC_SIMPLE_HTTP_SERVER_HPP

#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <optional>
#include <iostream>
#include <unordered_map>

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
    http_server(io_context_pool& pool, unsigned short port, const std::string& doc_root = "./")
        : pool_(pool), port_(port), doc_root_(doc_root) {}

    // 通用路由注册方法，减少重复代码
    template<typename Handler>
    void register_route(const std::string& method, const std::string& path, Handler&& handler) {
        routes_[method + " " + path] = std::forward<Handler>(handler);
    }

    // REST API 路由方法 - 使用模板函数减少重复
    void GET(const std::string& path, RouteHandler handler) {
        register_route("GET", path, std::move(handler));
    }

    void POST(const std::string& path, RouteHandler handler) {
        register_route("POST", path, std::move(handler));
    }

    void PUT(const std::string& path, RouteHandler handler) {
        register_route("PUT", path, std::move(handler));
    }

    void DELETE(const std::string& path, RouteHandler handler) {
        register_route("DELETE", path, std::move(handler));
    }

    void ROUTE(const std::string& method, const std::string& path, RouteHandler handler) {
        register_route(method, path, std::move(handler));
    }

    void set_default_route(RouteHandler handler) {
        routes_["* *"] = std::move(handler);
    }

    std::vector<std::string> list_routes() const {
        std::vector<std::string> result;
        for (const auto& [route, _] : routes_) {
            result.push_back(route);
        }
        return result;
    }

    concurrencpp::lazy_result<void> start_one(asio::ip::tcp::socket socket) {
        connection conn(std::move(socket), std::move(doc_root_), routes_);
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
    std::string doc_root_;
    std::unordered_map<std::string, RouteHandler> routes_;
};

}

#endif
